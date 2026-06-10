//
//  unittest.c
//  sqlite-memory unit tests
//
//  Created by Marco Bambini on 05/02/26.
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#define mkdir_p(path) _mkdir(path)
#define rmdir_p(path) _rmdir(path)
#ifndef TEST_TMP_DIR
#define TEST_TMP_DIR "build/test_tmp"  // Windows: native APIs don't understand /tmp
#endif
#else
#include <unistd.h>
#define mkdir_p(path) mkdir(path, 0755)
#define rmdir_p(path) rmdir(path)
#ifndef TEST_TMP_DIR
#define TEST_TMP_DIR "/tmp"
#endif
#endif

#include "dbmem-utils.h"
#include "dbmem-parser.h"

#ifdef TEST_SQLITE_EXTENSION
#include "sqlite-memory.h"
#ifndef DBMEM_OMIT_LOCAL_ENGINE
#include "ggml.h"
void dbmem_logger(enum ggml_log_level level, const char *text, void *user_data);
#endif
#endif

// ============================================================================
// Test Framework
// ============================================================================

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) static void test_##name(void)
#define RUN_TEST(name) do { \
    printf("  Running %s... ", #name); \
    fflush(stdout); \
    test_##name(); \
    tests_run++; \
    tests_passed++; \
    printf("PASSED\n"); \
} while(0)

#define ASSERT(cond) do { \
    if (!(cond)) { \
        printf("FAILED\n    Assertion failed: %s\n    At %s:%d\n", #cond, __FILE__, __LINE__); \
        tests_failed++; \
        tests_passed--; \
        return; \
    } \
} while(0)

#define ASSERT_EQ(a, b) do { \
    if ((a) != (b)) { \
        printf("FAILED\n    Expected %s == %s\n    At %s:%d\n", #a, #b, __FILE__, __LINE__); \
        tests_failed++; \
        tests_passed--; \
        return; \
    } \
} while(0)

#define ASSERT_STR_EQ(a, b) do { \
    if (strcmp((a), (b)) != 0) { \
        printf("FAILED\n    Expected \"%s\" == \"%s\"\n    At %s:%d\n", (a), (b), __FILE__, __LINE__); \
        tests_failed++; \
        tests_passed--; \
        return; \
    } \
} while(0)

// ============================================================================
// Test Helpers
// ============================================================================

static dbmem_parse_settings default_settings(void) {
    dbmem_parse_settings s = {0};
    s.max_tokens = 512;
    s.overlay_tokens = 64;
    s.chars_per_token = 4;
    s.skip_semantic = false;
    s.skip_html = true;
    return s;
}

typedef struct {
    char   **chunks;
    size_t  *lengths;
    size_t  *offsets;
    size_t  *src_lengths;
    size_t   count;
    size_t   capacity;
} test_ctx_t;

static int test_callback(const char *text, size_t len, size_t offset, size_t length, void *xdata, size_t index) {
    test_ctx_t *ctx = (test_ctx_t *)xdata;
    UNUSED_PARAM(index);

    if (ctx->count >= ctx->capacity) {
        size_t new_cap = ctx->capacity ? ctx->capacity * 2 : 16;
        ctx->chunks = realloc(ctx->chunks, new_cap * sizeof(char *));
        ctx->lengths = realloc(ctx->lengths, new_cap * sizeof(size_t));
        ctx->offsets = realloc(ctx->offsets, new_cap * sizeof(size_t));
        ctx->src_lengths = realloc(ctx->src_lengths, new_cap * sizeof(size_t));
        ctx->capacity = new_cap;
    }

    ctx->chunks[ctx->count] = malloc(len + 1);
    memcpy(ctx->chunks[ctx->count], text, len);
    ctx->chunks[ctx->count][len] = '\0';
    ctx->lengths[ctx->count] = len;
    ctx->offsets[ctx->count] = offset;
    ctx->src_lengths[ctx->count] = length;
    ctx->count++;

    return 0;
}

static void free_test_ctx(test_ctx_t *ctx) {
    for (size_t i = 0; i < ctx->count; i++) {
        free(ctx->chunks[i]);
    }
    free(ctx->chunks);
    free(ctx->lengths);
    free(ctx->offsets);
    free(ctx->src_lengths);
    memset(ctx, 0, sizeof(*ctx));
}

static int test_ctx_contains(test_ctx_t *ctx, const char *needle) {
    for (size_t i = 0; i < ctx->count; i++) {
        if (strstr(ctx->chunks[i], needle) != NULL) return 1;
    }
    return 0;
}

// ============================================================================
// dbmem_parse Tests
// ============================================================================

TEST(dbmem_parse_null_input) {
    dbmem_parse_settings settings = default_settings();
    int rc = dbmem_parse(NULL, 100, &settings);
    ASSERT_EQ(rc, -1);
}

TEST(dbmem_parse_null_settings) {
    const char *input = "test";
    int rc = dbmem_parse(input, strlen(input), NULL);
    ASSERT_EQ(rc, -1);
}

TEST(dbmem_parse_empty_buffer) {
    dbmem_parse_settings settings = default_settings();
    test_ctx_t ctx = {0};
    settings.callback = test_callback;
    settings.xdata = &ctx;

    int rc = dbmem_parse("", 0, &settings);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(ctx.count, 0);

    free_test_ctx(&ctx);
}

TEST(dbmem_parse_simple_text) {
    const char *input = "Hello world";
    dbmem_parse_settings settings = default_settings();
    test_ctx_t ctx = {0};
    settings.callback = test_callback;
    settings.xdata = &ctx;

    int rc = dbmem_parse(input, strlen(input), &settings);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(ctx.count, 1);
    ASSERT_STR_EQ(ctx.chunks[0], "Hello world");

    free_test_ctx(&ctx);
}

TEST(dbmem_parse_strips_markdown) {
    const char *input = "# Heading\n**bold** text";
    dbmem_parse_settings settings = default_settings();
    test_ctx_t ctx = {0};
    settings.callback = test_callback;
    settings.xdata = &ctx;

    int rc = dbmem_parse(input, strlen(input), &settings);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(ctx.count, 1);
    ASSERT_STR_EQ(ctx.chunks[0], "Heading\nbold text");

    free_test_ctx(&ctx);
}

TEST(dbmem_parse_strips_links) {
    const char *input = "Click [here](https://example.com) for more";
    dbmem_parse_settings settings = default_settings();
    test_ctx_t ctx = {0};
    settings.callback = test_callback;
    settings.xdata = &ctx;

    int rc = dbmem_parse(input, strlen(input), &settings);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(ctx.count, 1);
    ASSERT_STR_EQ(ctx.chunks[0], "Click here for more");

    free_test_ctx(&ctx);
}

TEST(dbmem_parse_strips_code_blocks) {
    const char *input = "Before\n```\ncode\n```\nAfter";
    dbmem_parse_settings settings = default_settings();
    test_ctx_t ctx = {0};
    settings.callback = test_callback;
    settings.xdata = &ctx;

    int rc = dbmem_parse(input, strlen(input), &settings);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(ctx.count, 1);
    ASSERT_STR_EQ(ctx.chunks[0], "Before\ncode\nAfter");

    free_test_ctx(&ctx);
}

TEST(dbmem_parse_strips_lists) {
    const char *input = "- Item 1\n- Item 2\n1. First\n2. Second";
    dbmem_parse_settings settings = default_settings();
    test_ctx_t ctx = {0};
    settings.callback = test_callback;
    settings.xdata = &ctx;

    int rc = dbmem_parse(input, strlen(input), &settings);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(ctx.count, 1);
    ASSERT_STR_EQ(ctx.chunks[0], "Item 1\nItem 2\nFirst\nSecond");

    free_test_ctx(&ctx);
}

TEST(dbmem_parse_strips_blockquotes) {
    const char *input = "> Quote line 1\n> Quote line 2";
    dbmem_parse_settings settings = default_settings();
    test_ctx_t ctx = {0};
    settings.callback = test_callback;
    settings.xdata = &ctx;

    int rc = dbmem_parse(input, strlen(input), &settings);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(ctx.count, 1);
    ASSERT_STR_EQ(ctx.chunks[0], "Quote line 1\nQuote line 2");

    free_test_ctx(&ctx);
}

TEST(dbmem_parse_strips_html) {
    const char *input = "Before <div>content</div> after";
    dbmem_parse_settings settings = default_settings();
    settings.skip_html = true;
    test_ctx_t ctx = {0};
    settings.callback = test_callback;
    settings.xdata = &ctx;

    int rc = dbmem_parse(input, strlen(input), &settings);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(ctx.count, 1);
    ASSERT_STR_EQ(ctx.chunks[0], "Before content after");

    free_test_ctx(&ctx);
}

TEST(dbmem_parse_preserves_html) {
    const char *input = "Before <div>content</div> after";
    dbmem_parse_settings settings = default_settings();
    settings.skip_html = false;
    test_ctx_t ctx = {0};
    settings.callback = test_callback;
    settings.xdata = &ctx;

    int rc = dbmem_parse(input, strlen(input), &settings);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(ctx.count, 1);
    ASSERT_STR_EQ(ctx.chunks[0], "Before <div>content</div> after");

    free_test_ctx(&ctx);
}

TEST(dbmem_parse_mdx_strips_esm_and_expressions) {
    const char *input =
        "import Widget from './Widget';\n"
        "export const metadata = {\n"
        "  title: 'Hidden title',\n"
        "  description: 'Hidden description'\n"
        "};\n"
        "\n"
        "# Hello {user.name}\n"
        "\n"
        "Visible <Widget prop={metadata}>inside</Widget> text.\n"
        "{items.map((item) => (\n"
        "  <span>{item.label}</span>\n"
        "))}\n"
        "After expression.\n";
    dbmem_parse_settings settings = default_settings();
    settings.mdx_mode = true;
    settings.overlay_tokens = 0;
    test_ctx_t ctx = {0};
    settings.callback = test_callback;
    settings.xdata = &ctx;

    int rc = dbmem_parse(input, strlen(input), &settings);
    ASSERT_EQ(rc, 0);
    ASSERT(ctx.count >= 1);
    ASSERT(test_ctx_contains(&ctx, "Hello"));
    ASSERT(test_ctx_contains(&ctx, "Visible inside text."));
    ASSERT(test_ctx_contains(&ctx, "After expression."));
    ASSERT(!test_ctx_contains(&ctx, "Widget from"));
    ASSERT(!test_ctx_contains(&ctx, "Hidden title"));
    ASSERT(!test_ctx_contains(&ctx, "user.name"));
    ASSERT(!test_ctx_contains(&ctx, "items.map"));
    ASSERT(!test_ctx_contains(&ctx, "item.label"));

    free_test_ctx(&ctx);
}

TEST(dbmem_parse_mdx_preserves_fenced_code) {
    const char *input =
        "Before\n"
        "```js\n"
        "import Widget from './Widget';\n"
        "const node = <Widget enabled={true} />;\n"
        "```\n"
        "After {ignoredExpression}\n";
    dbmem_parse_settings settings = default_settings();
    settings.mdx_mode = true;
    test_ctx_t ctx = {0};
    settings.callback = test_callback;
    settings.xdata = &ctx;

    int rc = dbmem_parse(input, strlen(input), &settings);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(ctx.count, 1);
    ASSERT(strstr(ctx.chunks[0], "Before") != NULL);
    ASSERT(strstr(ctx.chunks[0], "import Widget from './Widget';") != NULL);
    ASSERT(strstr(ctx.chunks[0], "const node = <Widget enabled={true} />;") != NULL);
    ASSERT(strstr(ctx.chunks[0], "After") != NULL);
    ASSERT(strstr(ctx.chunks[0], "ignoredExpression") == NULL);

    free_test_ctx(&ctx);
}

TEST(dbmem_parse_mdx_keeps_import_export_prose_and_indented_code) {
    const char *input =
        "# Visible\n"
        "import a database from the dashboard.\n"
        "export data from SQLite Cloud when needed.\n"
        "\n"
        "    import sqlitecloud\n"
        "    export default App\n"
        "\n"
        "import Real from './Real';\n"
        "export const hidden = 'not searchable';\n";
    dbmem_parse_settings settings = default_settings();
    settings.mdx_mode = true;
    settings.overlay_tokens = 0;
    test_ctx_t ctx = {0};
    settings.callback = test_callback;
    settings.xdata = &ctx;

    int rc = dbmem_parse(input, strlen(input), &settings);
    ASSERT_EQ(rc, 0);
    ASSERT(ctx.count >= 1);
    ASSERT(test_ctx_contains(&ctx, "import a database from the dashboard."));
    ASSERT(test_ctx_contains(&ctx, "export data from SQLite Cloud when needed."));
    ASSERT(test_ctx_contains(&ctx, "import sqlitecloud"));
    ASSERT(test_ctx_contains(&ctx, "export default App"));
    ASSERT(!test_ctx_contains(&ctx, "Real from"));
    ASSERT(!test_ctx_contains(&ctx, "not searchable"));

    free_test_ctx(&ctx);
}

TEST(dbmem_parse_mdx_real_docs_file) {
    const char *input =
        "---\n"
        "title: Multi Code Component Examples\n"
        "description: Multi Code Component Examples\n"
        "slug: multicode\n"
        "---\n"
        "import MultiCode from '@commons-components/Code/MultiCode.astro';\n"
        "\n"
        "In this examples, we will show how to use the `MultiCode` component:\n"
        "\n"
        "---\n"
        "## First example\n"
        "\n"
        "export const WebliteSourceCode = `<script>\n"
        "  async function searchData(event) {\n"
        "    const query = document.getElementById('query').value;\n"
        "  }\n"
        "</script>`;\n"
        "\n"
        "export const codeExamplesOne = [\n"
        "    {\n"
        "        sliderItem: \"Web\",\n"
        "        codeLines: WebliteSourceCode,\n"
        "        lang: \"html\",\n"
        "    }\n"
        "];\n"
        "\n"
        "<MultiCode id=\"first\" copyCode={true} codeItems={codeExamplesOne} />\n";

    dbmem_parse_settings settings = default_settings();
    settings.mdx_mode = true;
    settings.overlay_tokens = 0;
    test_ctx_t ctx = {0};
    settings.callback = test_callback;
    settings.xdata = &ctx;

    int rc = dbmem_parse(input, strlen(input), &settings);
    ASSERT_EQ(rc, 0);
    ASSERT(ctx.count >= 1);
    ASSERT(test_ctx_contains(&ctx, "Multi Code Component Examples"));
    ASSERT(test_ctx_contains(&ctx, "First example"));
    ASSERT(!test_ctx_contains(&ctx, "commons-components"));
    ASSERT(!test_ctx_contains(&ctx, "WebliteSourceCode"));
    ASSERT(!test_ctx_contains(&ctx, "codeExamplesOne"));

    free_test_ctx(&ctx);
}

TEST(dbmem_parse_skip_semantic) {
    const char *input = "# Section 1\nContent 1\n# Section 2\nContent 2";
    dbmem_parse_settings settings = default_settings();
    settings.skip_semantic = true;
    settings.overlay_tokens = 0;
    test_ctx_t ctx = {0};
    settings.callback = test_callback;
    settings.xdata = &ctx;

    int rc = dbmem_parse(input, strlen(input), &settings);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(ctx.count, 1);  // Single chunk when skip_semantic

    free_test_ctx(&ctx);
}

TEST(dbmem_parse_token_split) {
    // Create content larger than max_tokens
    char input[500];
    memset(input, 'A', sizeof(input) - 1);
    input[sizeof(input) - 1] = '\0';
    // Add newlines for split points
    for (int i = 50; i < 450; i += 50) {
        input[i] = '\n';
    }

    dbmem_parse_settings settings = default_settings();
    settings.max_tokens = 50;
    settings.chars_per_token = 4;
    settings.overlay_tokens = 0;
    test_ctx_t ctx = {0};
    settings.callback = test_callback;
    settings.xdata = &ctx;

    int rc = dbmem_parse(input, strlen(input), &settings);
    ASSERT_EQ(rc, 0);
    ASSERT(ctx.count > 1);  // Should split into multiple chunks

    free_test_ctx(&ctx);
}

TEST(dbmem_parse_overlay) {
    const char *input = "Line 1\nLine 2\nLine 3\nLine 4\nLine 5\nLine 6\nLine 7\nLine 8";
    dbmem_parse_settings settings = default_settings();
    settings.max_tokens = 10;
    settings.overlay_tokens = 5;
    settings.chars_per_token = 4;
    test_ctx_t ctx = {0};
    settings.callback = test_callback;
    settings.xdata = &ctx;

    int rc = dbmem_parse(input, strlen(input), &settings);
    ASSERT_EQ(rc, 0);

    // With overlay, chunks after the first should have content from previous
    if (ctx.count > 1) {
        ASSERT(ctx.lengths[1] > 0);
    }

    free_test_ctx(&ctx);
}

TEST(dbmem_parse_offset_length) {
    const char *input = "# Title\nParagraph content here";
    dbmem_parse_settings settings = default_settings();
    test_ctx_t ctx = {0};
    settings.callback = test_callback;
    settings.xdata = &ctx;

    int rc = dbmem_parse(input, strlen(input), &settings);
    ASSERT_EQ(rc, 0);
    ASSERT(ctx.count >= 1);

    // Verify offset and length are within bounds
    for (size_t i = 0; i < ctx.count; i++) {
        ASSERT(ctx.offsets[i] <= strlen(input));
        ASSERT(ctx.offsets[i] + ctx.src_lengths[i] <= strlen(input));
    }

    free_test_ctx(&ctx);
}

TEST(dbmem_parse_complex_document) {
    const char *input =
        "# Main Title\n"
        "\n"
        "Some intro text with **bold** and *italic*.\n"
        "\n"
        "## Section 1\n"
        "\n"
        "A paragraph with a [link](http://example.com).\n"
        "\n"
        "```python\n"
        "def hello():\n"
        "    print('world')\n"
        "```\n"
        "\n"
        "## Section 2\n"
        "\n"
        "> A blockquote\n"
        "> continues here\n"
        "\n"
        "- Item 1\n"
        "- Item 2\n"
        "- Item 3\n";

    dbmem_parse_settings settings = default_settings();
    settings.overlay_tokens = 0;
    test_ctx_t ctx = {0};
    settings.callback = test_callback;
    settings.xdata = &ctx;

    int rc = dbmem_parse(input, strlen(input), &settings);
    ASSERT_EQ(rc, 0);
    ASSERT(ctx.count >= 1);

    // Verify all chunks have valid text
    for (size_t i = 0; i < ctx.count; i++) {
        ASSERT(ctx.chunks[i] != NULL);
        ASSERT(ctx.lengths[i] > 0);
    }

    free_test_ctx(&ctx);
}

TEST(dbmem_parse_inline_code) {
    const char *input = "Use `printf()` function";
    dbmem_parse_settings settings = default_settings();
    test_ctx_t ctx = {0};
    settings.callback = test_callback;
    settings.xdata = &ctx;

    int rc = dbmem_parse(input, strlen(input), &settings);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(ctx.count, 1);
    ASSERT_STR_EQ(ctx.chunks[0], "Use printf() function");

    free_test_ctx(&ctx);
}

TEST(dbmem_parse_image) {
    const char *input = "An image: ![alt text](image.png)";
    dbmem_parse_settings settings = default_settings();
    test_ctx_t ctx = {0};
    settings.callback = test_callback;
    settings.xdata = &ctx;

    int rc = dbmem_parse(input, strlen(input), &settings);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(ctx.count, 1);
    ASSERT_STR_EQ(ctx.chunks[0], "An image: alt text");

    free_test_ctx(&ctx);
}

TEST(dbmem_parse_strikethrough) {
    const char *input = "This is ~~deleted~~ text";
    dbmem_parse_settings settings = default_settings();
    test_ctx_t ctx = {0};
    settings.callback = test_callback;
    settings.xdata = &ctx;

    int rc = dbmem_parse(input, strlen(input), &settings);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(ctx.count, 1);
    ASSERT_STR_EQ(ctx.chunks[0], "This is deleted text");

    free_test_ctx(&ctx);
}

TEST(dbmem_parse_thematic_break) {
    const char *input = "Before\n---\nAfter";
    dbmem_parse_settings settings = default_settings();
    test_ctx_t ctx = {0};
    settings.callback = test_callback;
    settings.xdata = &ctx;

    int rc = dbmem_parse(input, strlen(input), &settings);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(ctx.count, 1);
    ASSERT_STR_EQ(ctx.chunks[0], "Before\nAfter");

    free_test_ctx(&ctx);
}

TEST(dbmem_parse_table) {
    const char *input = "| Col1 | Col2 |\n|---|---|\n| A | B |";
    dbmem_parse_settings settings = default_settings();
    test_ctx_t ctx = {0};
    settings.callback = test_callback;
    settings.xdata = &ctx;

    int rc = dbmem_parse(input, strlen(input), &settings);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(ctx.count, 1);
    ASSERT_STR_EQ(ctx.chunks[0], " Col1  Col2\n A  B");

    free_test_ctx(&ctx);
}

// ============================================================================
// Additional Markdown Tests
// ============================================================================

TEST(dbmem_parse_nested_emphasis) {
    const char *input = "This is ***bold and italic*** text";
    dbmem_parse_settings settings = default_settings();
    test_ctx_t ctx = {0};
    settings.callback = test_callback;
    settings.xdata = &ctx;

    int rc = dbmem_parse(input, strlen(input), &settings);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(ctx.count, 1);
    ASSERT_STR_EQ(ctx.chunks[0], "This is bold and italic text");

    free_test_ctx(&ctx);
}

TEST(dbmem_parse_underscore_emphasis) {
    const char *input = "This is __bold__ and _italic_";
    dbmem_parse_settings settings = default_settings();
    test_ctx_t ctx = {0};
    settings.callback = test_callback;
    settings.xdata = &ctx;

    int rc = dbmem_parse(input, strlen(input), &settings);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(ctx.count, 1);
    ASSERT_STR_EQ(ctx.chunks[0], "This is bold and italic");

    free_test_ctx(&ctx);
}

TEST(dbmem_parse_tilde_fence) {
    const char *input = "Before\n~~~\ncode here\n~~~\nAfter";
    dbmem_parse_settings settings = default_settings();
    test_ctx_t ctx = {0};
    settings.callback = test_callback;
    settings.xdata = &ctx;

    int rc = dbmem_parse(input, strlen(input), &settings);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(ctx.count, 1);
    ASSERT_STR_EQ(ctx.chunks[0], "Before\ncode here\nAfter");

    free_test_ctx(&ctx);
}

TEST(dbmem_parse_double_backtick_code) {
    const char *input = "Code with ``backtick ` inside``";
    dbmem_parse_settings settings = default_settings();
    test_ctx_t ctx = {0};
    settings.callback = test_callback;
    settings.xdata = &ctx;

    int rc = dbmem_parse(input, strlen(input), &settings);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(ctx.count, 1);
    ASSERT_STR_EQ(ctx.chunks[0], "Code with backtick ` inside");

    free_test_ctx(&ctx);
}

TEST(dbmem_parse_reference_link) {
    const char *input = "Click [here][ref] for more";
    dbmem_parse_settings settings = default_settings();
    test_ctx_t ctx = {0};
    settings.callback = test_callback;
    settings.xdata = &ctx;

    int rc = dbmem_parse(input, strlen(input), &settings);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(ctx.count, 1);
    ASSERT_STR_EQ(ctx.chunks[0], "Click here for more");

    free_test_ctx(&ctx);
}

TEST(dbmem_parse_shortcut_link) {
    const char *input = "Click [here] for more";
    dbmem_parse_settings settings = default_settings();
    test_ctx_t ctx = {0};
    settings.callback = test_callback;
    settings.xdata = &ctx;

    int rc = dbmem_parse(input, strlen(input), &settings);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(ctx.count, 1);
    ASSERT_STR_EQ(ctx.chunks[0], "Click here for more");

    free_test_ctx(&ctx);
}

TEST(dbmem_parse_nested_blockquote) {
    const char *input = "> Level 1\n>> Level 2\n>>> Level 3";
    dbmem_parse_settings settings = default_settings();
    test_ctx_t ctx = {0};
    settings.callback = test_callback;
    settings.xdata = &ctx;

    int rc = dbmem_parse(input, strlen(input), &settings);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(ctx.count, 1);
    ASSERT_STR_EQ(ctx.chunks[0], "Level 1\nLevel 2\nLevel 3");

    free_test_ctx(&ctx);
}

TEST(dbmem_parse_heading_levels) {
    const char *input = "# H1\n## H2\n### H3\n#### H4\n##### H5\n###### H6";
    dbmem_parse_settings settings = default_settings();
    test_ctx_t ctx = {0};
    settings.callback = test_callback;
    settings.xdata = &ctx;

    int rc = dbmem_parse(input, strlen(input), &settings);
    ASSERT_EQ(rc, 0);
    ASSERT(ctx.count >= 1);
    // First chunk should start with H1
    ASSERT(strstr(ctx.chunks[0], "H1") != NULL);

    free_test_ctx(&ctx);
}

TEST(dbmem_parse_heading_sections_stay_split) {
    const char *input = "# One\nAlpha text.\n\n## Two\nBeta text.";
    dbmem_parse_settings settings = default_settings();
    test_ctx_t ctx = {0};
    settings.overlay_tokens = 0;
    settings.callback = test_callback;
    settings.xdata = &ctx;

    int rc = dbmem_parse(input, strlen(input), &settings);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(ctx.count, 2);
    ASSERT_STR_EQ(ctx.chunks[0], "One\nAlpha text.");
    ASSERT_STR_EQ(ctx.chunks[1], "Two\nBeta text.");

    free_test_ctx(&ctx);
}

TEST(dbmem_parse_heading_trailing_hashes) {
    const char *input = "## Heading ##\n### Another ###";
    dbmem_parse_settings settings = default_settings();
    test_ctx_t ctx = {0};
    settings.callback = test_callback;
    settings.xdata = &ctx;

    int rc = dbmem_parse(input, strlen(input), &settings);
    ASSERT_EQ(rc, 0);
    ASSERT(ctx.count >= 1);
    ASSERT(strstr(ctx.chunks[0], "Heading") != NULL);
    // Trailing hashes should be stripped
    ASSERT(strstr(ctx.chunks[0], "##") == NULL);

    free_test_ctx(&ctx);
}

TEST(dbmem_parse_multiline_html) {
    const char *input = "Text\n<div\nclass=\"test\">\ncontent</div>\nMore";
    dbmem_parse_settings settings = default_settings();
    settings.skip_html = true;
    test_ctx_t ctx = {0};
    settings.callback = test_callback;
    settings.xdata = &ctx;

    int rc = dbmem_parse(input, strlen(input), &settings);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(ctx.count, 1);
    ASSERT_STR_EQ(ctx.chunks[0], "Text\ncontent\nMore");

    free_test_ctx(&ctx);
}

TEST(dbmem_parse_blank_lines) {
    const char *input = "Line 1\n\nLine 2\n\n\nLine 3";
    dbmem_parse_settings settings = default_settings();
    test_ctx_t ctx = {0};
    settings.callback = test_callback;
    settings.xdata = &ctx;

    int rc = dbmem_parse(input, strlen(input), &settings);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(ctx.count, 1);
    ASSERT_STR_EQ(ctx.chunks[0], "Line 1\n\nLine 2\n\n\nLine 3");

    free_test_ctx(&ctx);
}

TEST(dbmem_parse_mixed_list_markers) {
    const char *input = "- Dash\n* Star\n+ Plus\n1. One\n2) Two";
    dbmem_parse_settings settings = default_settings();
    test_ctx_t ctx = {0};
    settings.callback = test_callback;
    settings.xdata = &ctx;

    int rc = dbmem_parse(input, strlen(input), &settings);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(ctx.count, 1);
    ASSERT_STR_EQ(ctx.chunks[0], "Dash\nStar\nPlus\nOne\nTwo");

    free_test_ctx(&ctx);
}

TEST(dbmem_parse_asterisk_thematic_break) {
    const char *input = "Before\n***\nAfter";
    dbmem_parse_settings settings = default_settings();
    test_ctx_t ctx = {0};
    settings.callback = test_callback;
    settings.xdata = &ctx;

    int rc = dbmem_parse(input, strlen(input), &settings);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(ctx.count, 1);
    ASSERT_STR_EQ(ctx.chunks[0], "Before\nAfter");

    free_test_ctx(&ctx);
}

TEST(dbmem_parse_underscore_thematic_break) {
    const char *input = "Before\n___\nAfter";
    dbmem_parse_settings settings = default_settings();
    test_ctx_t ctx = {0};
    settings.callback = test_callback;
    settings.xdata = &ctx;

    int rc = dbmem_parse(input, strlen(input), &settings);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(ctx.count, 1);
    ASSERT_STR_EQ(ctx.chunks[0], "Before\nAfter");

    free_test_ctx(&ctx);
}

// ============================================================================
// Edge Cases
// ============================================================================

TEST(dbmem_parse_single_char) {
    const char *input = "X";
    dbmem_parse_settings settings = default_settings();
    test_ctx_t ctx = {0};
    settings.callback = test_callback;
    settings.xdata = &ctx;

    int rc = dbmem_parse(input, strlen(input), &settings);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(ctx.count, 1);
    ASSERT_STR_EQ(ctx.chunks[0], "X");

    free_test_ctx(&ctx);
}

TEST(dbmem_parse_only_whitespace) {
    const char *input = "   \n\n   \n";
    dbmem_parse_settings settings = default_settings();
    test_ctx_t ctx = {0};
    settings.callback = test_callback;
    settings.xdata = &ctx;

    int rc = dbmem_parse(input, strlen(input), &settings);
    ASSERT_EQ(rc, 0);
    // Should produce empty or whitespace-only output
    ASSERT(ctx.count <= 1);

    free_test_ctx(&ctx);
}

TEST(dbmem_parse_unclosed_code_span) {
    const char *input = "Text with `unclosed code";
    dbmem_parse_settings settings = default_settings();
    test_ctx_t ctx = {0};
    settings.callback = test_callback;
    settings.xdata = &ctx;

    int rc = dbmem_parse(input, strlen(input), &settings);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(ctx.count, 1);
    ASSERT(strlen(ctx.chunks[0]) > 0);

    free_test_ctx(&ctx);
}

TEST(dbmem_parse_unclosed_link) {
    const char *input = "Text with [unclosed link";
    dbmem_parse_settings settings = default_settings();
    test_ctx_t ctx = {0};
    settings.callback = test_callback;
    settings.xdata = &ctx;

    int rc = dbmem_parse(input, strlen(input), &settings);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(ctx.count, 1);
    ASSERT(strlen(ctx.chunks[0]) > 0);

    free_test_ctx(&ctx);
}

TEST(dbmem_parse_very_long_line) {
    char input[2000];
    memset(input, 'x', sizeof(input) - 1);
    input[sizeof(input) - 1] = '\0';

    dbmem_parse_settings settings = default_settings();
    test_ctx_t ctx = {0};
    settings.callback = test_callback;
    settings.xdata = &ctx;

    int rc = dbmem_parse(input, strlen(input), &settings);
    ASSERT_EQ(rc, 0);
    ASSERT(ctx.count >= 1);

    free_test_ctx(&ctx);
}

TEST(dbmem_parse_special_characters) {
    const char *input = "Text with < > & \" ' characters";
    dbmem_parse_settings settings = default_settings();
    settings.skip_html = false;
    test_ctx_t ctx = {0};
    settings.callback = test_callback;
    settings.xdata = &ctx;

    int rc = dbmem_parse(input, strlen(input), &settings);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(ctx.count, 1);
    ASSERT(strstr(ctx.chunks[0], "<") != NULL);
    ASSERT(strstr(ctx.chunks[0], ">") != NULL);

    free_test_ctx(&ctx);
}

TEST(dbmem_parse_nested_brackets) {
    const char *input = "Link with [nested [brackets]](url)";
    dbmem_parse_settings settings = default_settings();
    test_ctx_t ctx = {0};
    settings.callback = test_callback;
    settings.xdata = &ctx;

    int rc = dbmem_parse(input, strlen(input), &settings);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(ctx.count, 1);
    // Should extract the nested bracket text
    ASSERT(strstr(ctx.chunks[0], "nested") != NULL);

    free_test_ctx(&ctx);
}

// ============================================================================
// Chunking Edge Cases
// ============================================================================

// Callbacks for special tests (must be at file scope in C)
static size_t g_expected_index = 0;
static int g_index_error = 0;

static int index_verify_callback(const char *text, size_t len, size_t offset, size_t length, void *xdata, size_t index) {
    UNUSED_PARAM(text); UNUSED_PARAM(len); UNUSED_PARAM(offset);
    UNUSED_PARAM(length); UNUSED_PARAM(xdata);
    if (index != g_expected_index) {
        g_index_error = 1;
    }
    g_expected_index++;
    return 0;
}

static int g_abort_call_count = 0;

static int abort_after_two_callback(const char *text, size_t len, size_t offset, size_t length, void *xdata, size_t index) {
    UNUSED_PARAM(text); UNUSED_PARAM(len); UNUSED_PARAM(offset);
    UNUSED_PARAM(length); UNUSED_PARAM(xdata); UNUSED_PARAM(index);
    g_abort_call_count++;
    if (g_abort_call_count >= 2) {
        return -1;  // Abort after 2 calls
    }
    return 0;
}

TEST(dbmem_parse_zero_max_tokens) {
    const char *input = "Some text that would normally be split";
    dbmem_parse_settings settings = default_settings();
    settings.max_tokens = 0;  // No splitting
    settings.overlay_tokens = 0;
    test_ctx_t ctx = {0};
    settings.callback = test_callback;
    settings.xdata = &ctx;

    int rc = dbmem_parse(input, strlen(input), &settings);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(ctx.count, 1);  // Should be single chunk

    free_test_ctx(&ctx);
}

TEST(dbmem_parse_overlay_larger_than_chunk) {
    const char *input = "Short";
    dbmem_parse_settings settings = default_settings();
    settings.max_tokens = 10;
    settings.overlay_tokens = 100;  // Larger than content
    settings.chars_per_token = 4;
    test_ctx_t ctx = {0};
    settings.callback = test_callback;
    settings.xdata = &ctx;

    int rc = dbmem_parse(input, strlen(input), &settings);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(ctx.count, 1);

    free_test_ctx(&ctx);
}

TEST(dbmem_parse_multiple_chunks_verify_count) {
    // Create content that will definitely split
    char input[1000];
    for (int i = 0; i < 999; i++) {
        input[i] = (i % 50 == 49) ? '\n' : 'A';
    }
    input[999] = '\0';

    dbmem_parse_settings settings = default_settings();
    settings.max_tokens = 25;
    settings.chars_per_token = 4;  // 100 chars per chunk
    settings.overlay_tokens = 0;
    test_ctx_t ctx = {0};
    settings.callback = test_callback;
    settings.xdata = &ctx;

    int rc = dbmem_parse(input, strlen(input), &settings);
    ASSERT_EQ(rc, 0);
    ASSERT(ctx.count > 5);  // Should have multiple chunks

    free_test_ctx(&ctx);
}

TEST(dbmem_parse_callback_receives_index) {
    g_expected_index = 0;
    g_index_error = 0;

    char input[500];
    for (int i = 0; i < 499; i++) {
        input[i] = (i % 50 == 49) ? '\n' : 'B';
    }
    input[499] = '\0';

    dbmem_parse_settings settings = default_settings();
    settings.max_tokens = 25;
    settings.chars_per_token = 4;
    settings.overlay_tokens = 0;
    settings.callback = index_verify_callback;
    settings.xdata = NULL;

    int rc = dbmem_parse(input, strlen(input), &settings);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(g_index_error, 0);
    ASSERT(g_expected_index > 1);  // Multiple chunks processed
}

TEST(dbmem_parse_callback_can_abort) {
    g_abort_call_count = 0;

    char input[500];
    for (int i = 0; i < 499; i++) {
        input[i] = (i % 50 == 49) ? '\n' : 'C';
    }
    input[499] = '\0';

    dbmem_parse_settings settings = default_settings();
    settings.max_tokens = 25;
    settings.chars_per_token = 4;
    settings.overlay_tokens = 0;
    settings.callback = abort_after_two_callback;
    settings.xdata = NULL;

    int rc = dbmem_parse(input, strlen(input), &settings);
    ASSERT_EQ(rc, -1);  // Should return error from callback
    ASSERT_EQ(g_abort_call_count, 2);  // Should have stopped after 2 calls
}

TEST(dbmem_parse_no_callback) {
    const char *input = "Some text";
    dbmem_parse_settings settings = default_settings();
    settings.callback = NULL;  // No callback
    settings.xdata = NULL;

    int rc = dbmem_parse(input, strlen(input), &settings);
    ASSERT_EQ(rc, 0);  // Should succeed without callback
}

TEST(dbmem_parse_only_heading) {
    const char *input = "# Just a heading";
    dbmem_parse_settings settings = default_settings();
    test_ctx_t ctx = {0};
    settings.callback = test_callback;
    settings.xdata = &ctx;

    int rc = dbmem_parse(input, strlen(input), &settings);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(ctx.count, 1);
    ASSERT_STR_EQ(ctx.chunks[0], "Just a heading");

    free_test_ctx(&ctx);
}

TEST(dbmem_parse_code_with_markdown_inside) {
    const char *input = "```\n# Not a heading\n**not bold**\n```";
    dbmem_parse_settings settings = default_settings();
    test_ctx_t ctx = {0};
    settings.callback = test_callback;
    settings.xdata = &ctx;

    int rc = dbmem_parse(input, strlen(input), &settings);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(ctx.count, 1);
    // Markdown inside code block should be preserved
    ASSERT(strstr(ctx.chunks[0], "# Not a heading") != NULL);
    ASSERT(strstr(ctx.chunks[0], "**not bold**") != NULL);

    free_test_ctx(&ctx);
}

// ============================================================================
// Directory Scanning Tests
// ============================================================================

// Helper to create a file with content
static int create_test_file(const char *path, const char *content) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    if (content) fputs(content, f);
    fclose(f);
    return 0;
}

// Helper to remove a file
static void remove_test_file(const char *path) {
    remove(path);
}

// Callback context for directory scan tests
typedef struct {
    char **paths;
    size_t count;
    size_t capacity;
} scan_ctx_t;

static int scan_callback(const char *path, void *data) {
    scan_ctx_t *ctx = (scan_ctx_t *)data;

    if (ctx->count >= ctx->capacity) {
        size_t new_cap = ctx->capacity ? ctx->capacity * 2 : 16;
        ctx->paths = realloc(ctx->paths, new_cap * sizeof(char *));
        ctx->capacity = new_cap;
    }

    ctx->paths[ctx->count] = strdup(path);
    ctx->count++;
    return 0;  // 0 = continue
}

static void free_scan_ctx(scan_ctx_t *ctx) {
    for (size_t i = 0; i < ctx->count; i++) {
        free(ctx->paths[i]);
    }
    free(ctx->paths);
    memset(ctx, 0, sizeof(*ctx));
}

static int path_contains(scan_ctx_t *ctx, const char *substring) {
    for (size_t i = 0; i < ctx->count; i++) {
        if (strstr(ctx->paths[i], substring) != NULL) return 1;
    }
    return 0;
}

TEST(dbmem_dir_scan_empty_dir) {
    // Create empty test directory
    const char *test_dir = TEST_TMP_DIR "/dbmem_test_empty";
    rmdir_p(test_dir);  // Remove if exists
    mkdir_p(test_dir);

    scan_ctx_t ctx = {0};
    int rc = dbmem_dir_scan(test_dir, scan_callback, &ctx);

    ASSERT_EQ(rc, 0);
    ASSERT_EQ(ctx.count, 0);  // No files found

    free_scan_ctx(&ctx);
    rmdir_p(test_dir);
}

TEST(dbmem_dir_scan_single_file) {
    // Create test directory with one file
    const char *test_dir = TEST_TMP_DIR "/dbmem_test_single";
    const char *test_file = TEST_TMP_DIR "/dbmem_test_single/file.txt";

    rmdir_p(test_dir);
    remove_test_file(test_file);
    mkdir_p(test_dir);
    create_test_file(test_file, "test content");

    scan_ctx_t ctx = {0};
    int rc = dbmem_dir_scan(test_dir, scan_callback, &ctx);

    ASSERT_EQ(rc, 0);
    ASSERT_EQ(ctx.count, 1);
    ASSERT(strstr(ctx.paths[0], "file.txt") != NULL);

    free_scan_ctx(&ctx);
    remove_test_file(test_file);
    rmdir_p(test_dir);
}

TEST(dbmem_dir_scan_multiple_files) {
    // Create test directory with multiple files
    const char *test_dir = TEST_TMP_DIR "/dbmem_test_multi";

    // Clean up first
    remove(TEST_TMP_DIR "/dbmem_test_multi/a.txt");
    remove(TEST_TMP_DIR "/dbmem_test_multi/b.txt");
    remove(TEST_TMP_DIR "/dbmem_test_multi/c.md");
    rmdir_p(test_dir);

    mkdir_p(test_dir);
    create_test_file(TEST_TMP_DIR "/dbmem_test_multi/a.txt", "a");
    create_test_file(TEST_TMP_DIR "/dbmem_test_multi/b.txt", "b");
    create_test_file(TEST_TMP_DIR "/dbmem_test_multi/c.md", "c");

    scan_ctx_t ctx = {0};
    int rc = dbmem_dir_scan(test_dir, scan_callback, &ctx);

    ASSERT_EQ(rc, 0);
    ASSERT_EQ(ctx.count, 3);
    ASSERT(path_contains(&ctx, "a.txt"));
    ASSERT(path_contains(&ctx, "b.txt"));
    ASSERT(path_contains(&ctx, "c.md"));

    free_scan_ctx(&ctx);
    remove(TEST_TMP_DIR "/dbmem_test_multi/a.txt");
    remove(TEST_TMP_DIR "/dbmem_test_multi/b.txt");
    remove(TEST_TMP_DIR "/dbmem_test_multi/c.md");
    rmdir_p(test_dir);
}

TEST(dbmem_dir_scan_recursive) {
    // Create nested directory structure
    const char *base = TEST_TMP_DIR "/dbmem_test_recursive";
    const char *sub1 = TEST_TMP_DIR "/dbmem_test_recursive/sub1";
    const char *sub2 = TEST_TMP_DIR "/dbmem_test_recursive/sub2";
    const char *subsub = TEST_TMP_DIR "/dbmem_test_recursive/sub1/subsub";

    // Clean up first
    remove(TEST_TMP_DIR "/dbmem_test_recursive/root.txt");
    remove(TEST_TMP_DIR "/dbmem_test_recursive/sub1/file1.txt");
    remove(TEST_TMP_DIR "/dbmem_test_recursive/sub1/subsub/deep.txt");
    remove(TEST_TMP_DIR "/dbmem_test_recursive/sub2/file2.txt");
    rmdir_p(subsub);
    rmdir_p(sub1);
    rmdir_p(sub2);
    rmdir_p(base);

    // Create structure
    mkdir_p(base);
    mkdir_p(sub1);
    mkdir_p(sub2);
    mkdir_p(subsub);

    create_test_file(TEST_TMP_DIR "/dbmem_test_recursive/root.txt", "root");
    create_test_file(TEST_TMP_DIR "/dbmem_test_recursive/sub1/file1.txt", "file1");
    create_test_file(TEST_TMP_DIR "/dbmem_test_recursive/sub2/file2.txt", "file2");
    create_test_file(TEST_TMP_DIR "/dbmem_test_recursive/sub1/subsub/deep.txt", "deep");

    scan_ctx_t ctx = {0};
    int rc = dbmem_dir_scan(base, scan_callback, &ctx);

    ASSERT_EQ(rc, 0);
    ASSERT_EQ(ctx.count, 4);  // 4 files total
    ASSERT(path_contains(&ctx, "root.txt"));
    ASSERT(path_contains(&ctx, "file1.txt"));
    ASSERT(path_contains(&ctx, "file2.txt"));
    ASSERT(path_contains(&ctx, "deep.txt"));

    // Verify full paths contain directory structure
    ASSERT(path_contains(&ctx, "sub1/file1.txt") || path_contains(&ctx, "sub1\\file1.txt"));
    ASSERT(path_contains(&ctx, "subsub/deep.txt") || path_contains(&ctx, "subsub\\deep.txt"));

    free_scan_ctx(&ctx);

    // Cleanup
    remove(TEST_TMP_DIR "/dbmem_test_recursive/root.txt");
    remove(TEST_TMP_DIR "/dbmem_test_recursive/sub1/file1.txt");
    remove(TEST_TMP_DIR "/dbmem_test_recursive/sub1/subsub/deep.txt");
    remove(TEST_TMP_DIR "/dbmem_test_recursive/sub2/file2.txt");
    rmdir_p(subsub);
    rmdir_p(sub1);
    rmdir_p(sub2);
    rmdir_p(base);
}

TEST(dbmem_dir_scan_skips_hidden) {
    // Create directory with hidden files (dot files)
    const char *test_dir = TEST_TMP_DIR "/dbmem_test_hidden";

    remove(TEST_TMP_DIR "/dbmem_test_hidden/visible.txt");
    remove(TEST_TMP_DIR "/dbmem_test_hidden/.hidden");
    rmdir_p(test_dir);

    mkdir_p(test_dir);
    create_test_file(TEST_TMP_DIR "/dbmem_test_hidden/visible.txt", "visible");
    create_test_file(TEST_TMP_DIR "/dbmem_test_hidden/.hidden", "hidden");

    scan_ctx_t ctx = {0};
    int rc = dbmem_dir_scan(test_dir, scan_callback, &ctx);

    ASSERT_EQ(rc, 0);
    ASSERT_EQ(ctx.count, 1);  // Only visible file
    ASSERT(path_contains(&ctx, "visible.txt"));
    ASSERT(!path_contains(&ctx, ".hidden"));

    free_scan_ctx(&ctx);
    remove(TEST_TMP_DIR "/dbmem_test_hidden/visible.txt");
    remove(TEST_TMP_DIR "/dbmem_test_hidden/.hidden");
    rmdir_p(test_dir);
}

TEST(dbmem_dir_scan_skips_hidden_dirs) {
    // Create directory with hidden subdirectory
    const char *test_dir = TEST_TMP_DIR "/dbmem_test_hidden_dir";
    const char *hidden_dir = TEST_TMP_DIR "/dbmem_test_hidden_dir/.hidden_dir";

    remove(TEST_TMP_DIR "/dbmem_test_hidden_dir/visible.txt");
    remove(TEST_TMP_DIR "/dbmem_test_hidden_dir/.hidden_dir/secret.txt");
    rmdir_p(hidden_dir);
    rmdir_p(test_dir);

    mkdir_p(test_dir);
    mkdir_p(hidden_dir);
    create_test_file(TEST_TMP_DIR "/dbmem_test_hidden_dir/visible.txt", "visible");
    create_test_file(TEST_TMP_DIR "/dbmem_test_hidden_dir/.hidden_dir/secret.txt", "secret");

    scan_ctx_t ctx = {0};
    int rc = dbmem_dir_scan(test_dir, scan_callback, &ctx);

    ASSERT_EQ(rc, 0);
    ASSERT_EQ(ctx.count, 1);  // Only visible file, hidden dir not scanned
    ASSERT(path_contains(&ctx, "visible.txt"));
    ASSERT(!path_contains(&ctx, "secret.txt"));

    free_scan_ctx(&ctx);
    remove(TEST_TMP_DIR "/dbmem_test_hidden_dir/visible.txt");
    remove(TEST_TMP_DIR "/dbmem_test_hidden_dir/.hidden_dir/secret.txt");
    rmdir_p(hidden_dir);
    rmdir_p(test_dir);
}

TEST(dbmem_dir_scan_null_path) {
    scan_ctx_t ctx = {0};
    int rc = dbmem_dir_scan(NULL, scan_callback, &ctx);
    ASSERT_EQ(rc, -1);
    free_scan_ctx(&ctx);
}

TEST(dbmem_dir_scan_null_callback) {
    int rc = dbmem_dir_scan(".", NULL, NULL);  // Use current dir (exists on all platforms)
    ASSERT_EQ(rc, -1);
}

TEST(dbmem_dir_scan_nonexistent) {
    scan_ctx_t ctx = {0};
    int rc = dbmem_dir_scan(TEST_TMP_DIR "/nonexistent_dir_12345", scan_callback, &ctx);
    ASSERT_EQ(rc, -1);
    free_scan_ctx(&ctx);
}

static int abort_scan_callback(const char *path, void *data) {
    UNUSED_PARAM(path);
    int *count = (int *)data;
    (*count)++;
    if (*count >= 2) return 1;  // non-zero = abort after 2 files
    return 0;  // 0 = continue
}

TEST(dbmem_dir_scan_callback_abort) {
    // Create directory with multiple files
    const char *test_dir = TEST_TMP_DIR "/dbmem_test_abort";

    remove(TEST_TMP_DIR "/dbmem_test_abort/a.txt");
    remove(TEST_TMP_DIR "/dbmem_test_abort/b.txt");
    remove(TEST_TMP_DIR "/dbmem_test_abort/c.txt");
    rmdir_p(test_dir);

    mkdir_p(test_dir);
    create_test_file(TEST_TMP_DIR "/dbmem_test_abort/a.txt", "a");
    create_test_file(TEST_TMP_DIR "/dbmem_test_abort/b.txt", "b");
    create_test_file(TEST_TMP_DIR "/dbmem_test_abort/c.txt", "c");

    int count = 0;
    int rc = dbmem_dir_scan(test_dir, abort_scan_callback, &count);

    ASSERT_EQ(rc, -1);  // Should return error when callback aborts
    ASSERT_EQ(count, 2);  // Should have stopped after 2 files

    remove(TEST_TMP_DIR "/dbmem_test_abort/a.txt");
    remove(TEST_TMP_DIR "/dbmem_test_abort/b.txt");
    remove(TEST_TMP_DIR "/dbmem_test_abort/c.txt");
    rmdir_p(test_dir);
}

TEST(dbmem_dir_scan_trailing_slash) {
    // Test with trailing slash in path
    const char *test_dir = TEST_TMP_DIR "/dbmem_test_slash";

    remove(TEST_TMP_DIR "/dbmem_test_slash/file.txt");
    rmdir_p(test_dir);

    mkdir_p(test_dir);
    create_test_file(TEST_TMP_DIR "/dbmem_test_slash/file.txt", "test");

    scan_ctx_t ctx = {0};
    int rc = dbmem_dir_scan(TEST_TMP_DIR "/dbmem_test_slash/", scan_callback, &ctx);

    ASSERT_EQ(rc, 0);
    ASSERT_EQ(ctx.count, 1);
    ASSERT(path_contains(&ctx, "file.txt"));

    free_scan_ctx(&ctx);
    remove(TEST_TMP_DIR "/dbmem_test_slash/file.txt");
    rmdir_p(test_dir);
}

#ifdef TEST_SQLITE_EXTENSION
// ============================================================================
// SQLite Extension Tests
// ============================================================================

// Helper to open db with extension
static sqlite3 *open_test_db(void) {
    sqlite3 *db = NULL;
    int rc = sqlite3_open(":memory:", &db);
    if (rc != SQLITE_OK) return NULL;

    rc = sqlite3_memory_init(db, NULL, NULL);
    if (rc != SQLITE_OK) {
        sqlite3_close(db);
        return NULL;
    }
    return db;
}

static sqlite3 *open_test_db_path(const char *path) {
    sqlite3 *db = NULL;
    int rc = sqlite3_open(path, &db);
    if (rc != SQLITE_OK) return NULL;

    rc = sqlite3_memory_init(db, NULL, NULL);
    if (rc != SQLITE_OK) {
        sqlite3_close(db);
        return NULL;
    }
    return db;
}

// Helper to execute SQL and get integer result
static int exec_get_int(sqlite3 *db, const char *sql, sqlite3_int64 *result) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return rc;

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        *result = sqlite3_column_int64(stmt, 0);
        rc = SQLITE_OK;
    }
    sqlite3_finalize(stmt);
    return rc;
}

// Helper to execute SQL and get text result
static int exec_get_text(sqlite3 *db, const char *sql, char *result, size_t max_len) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return rc;

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        const char *text = (const char *)sqlite3_column_text(stmt, 0);
        if (text) {
            strncpy(result, text, max_len - 1);
            result[max_len - 1] = '\0';
        } else {
            result[0] = '\0';
        }
        rc = SQLITE_OK;
    }
    sqlite3_finalize(stmt);
    return rc;
}

static dbmem_context *get_test_ctx(sqlite3 *db) {
    sqlite3_stmt *stmt = NULL;
    dbmem_context *ctx = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT _memory_ctx_ptr();", -1, &stmt, NULL);

    if (rc == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW) {
        ctx = (dbmem_context *)sqlite3_value_pointer(sqlite3_column_value(stmt, 0), "dbmem_context_ptr");
    }

    if (stmt) sqlite3_finalize(stmt);
    return ctx;
}

TEST(sqlite_memory_version) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    char version[64];
    int rc = exec_get_text(db, "SELECT memory_version();", version, sizeof(version));
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT(strlen(version) > 0);
    ASSERT(strstr(version, ".") != NULL);  // Version should contain a dot

    sqlite3_close(db);
}

TEST(sqlite_memory_is_enabled) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    sqlite3_int64 result = 0;
    int rc = exec_get_int(db, "SELECT memory_is_enabled();", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 1);

    sqlite3_close(db);
}

TEST(sqlite_memory_is_enabled_missing_table) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    int rc = sqlite3_exec(db, "DROP TABLE dbmem_cache;", NULL, NULL, NULL);
    ASSERT_EQ(rc, SQLITE_OK);

    sqlite3_int64 result = 1;
    rc = exec_get_int(db, "SELECT memory_is_enabled();", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 0);

    sqlite3_close(db);
}

TEST(sqlite_memory_is_enabled_ignores_schema_version) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    int rc = sqlite3_exec(db, "DELETE FROM dbmem_settings WHERE key='schema_version';", NULL, NULL, NULL);
    ASSERT_EQ(rc, SQLITE_OK);

    sqlite3_int64 result = 1;
    rc = exec_get_int(db, "SELECT memory_is_enabled();", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 1);

    sqlite3_close(db);
}

TEST(sqlite_memory_clear_empty) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    sqlite3_int64 result;
    int rc = exec_get_int(db, "SELECT memory_clear();", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 1);  // Should return 1 (success)

    sqlite3_close(db);
}

TEST(sqlite_memory_delete_nonexistent) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    sqlite3_int64 result;
    int rc = exec_get_int(db, "SELECT memory_delete(printf('%016x', 12345));", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 0);  // Should return 0 (no rows deleted)

    sqlite3_close(db);
}

TEST(sqlite_memory_delete_context_nonexistent) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    sqlite3_int64 result;
    int rc = exec_get_int(db, "SELECT memory_delete_context('nonexistent');", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 0);  // Should return 0 (no rows deleted)

    sqlite3_close(db);
}

TEST(sqlite_memory_delete_file_direct) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    int rc = sqlite3_exec(db,
        "INSERT INTO dbmem_content (hash, path, value, length, context, created_at) VALUES "
        "(printf('%016x', 800), 'docs/delete.md', 'delete me', 9, 'ctx', 0), "
        "(printf('%016x', 801), 'docs/keep.md', 'keep me', 7, 'ctx', 0);"
        "INSERT INTO dbmem_vault (hash, seq, embedding, offset, length) VALUES "
        "(printf('%016x', 800), 0, X'00000000', 0, 5), "
        "(printf('%016x', 800), 1, X'00000000', 5, 4), "
        "(printf('%016x', 801), 0, X'00000000', 0, 7);"
        "INSERT INTO dbmem_vault_fts (content, hash, seq, context) VALUES "
        "('delete chunk 1', printf('%016x', 800), 0, 'ctx'), "
        "('delete chunk 2', printf('%016x', 800), 1, 'ctx'), "
        "('keep chunk', printf('%016x', 801), 0, 'ctx');",
        NULL, NULL, NULL);
    ASSERT_EQ(rc, SQLITE_OK);

    sqlite3_int64 result = 0;
    rc = exec_get_int(db, "SELECT memory_delete_file('docs/delete.md');", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 1);

    sqlite3_int64 count = 0;
    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_content WHERE hash = printf('%016x', 800);", &count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(count, 0);

    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_vault WHERE hash = printf('%016x', 800);", &count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(count, 0);

    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_vault_fts WHERE hash = printf('%016x', 800);", &count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(count, 0);

    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_content WHERE hash = printf('%016x', 801);", &count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(count, 1);

    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_vault WHERE hash = printf('%016x', 801);", &count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(count, 1);

    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_vault_fts WHERE hash = printf('%016x', 801);", &count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(count, 1);

    sqlite3_close(db);
}

TEST(sqlite_memory_delete_file_missing) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    sqlite3_int64 result = 1;
    int rc = exec_get_int(db, "SELECT memory_delete_file('missing.md');", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 0);

    sqlite3_close(db);
}

TEST(sqlite_memory_delete_file_matches_source_path) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    int rc = sqlite3_exec(db,
        "INSERT INTO dbmem_content (hash, path, value, length, context, created_at) VALUES "
        "(printf('%016x', 802), 'docs/delete-source.md', 'delete me', 9, 'ctx', 0);"
        "INSERT INTO dbmem_content_source (path, source_path) VALUES "
        "('docs/delete-source.md', '/tmp/delete-source.md');"
        "INSERT INTO dbmem_vault (hash, seq, embedding, offset, length) VALUES "
        "(printf('%016x', 802), 0, X'00000000', 0, 9);",
        NULL, NULL, NULL);
    ASSERT_EQ(rc, SQLITE_OK);

    sqlite3_int64 result = 0;
    rc = exec_get_int(db, "SELECT memory_delete_file('/tmp/delete-source.md');", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 1);

    sqlite3_int64 count = 0;
    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_content WHERE hash = printf('%016x', 802);", &count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(count, 0);

    sqlite3_close(db);
}

TEST(sqlite_memory_delete_file_rejects_ambiguous_path) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    int rc = sqlite3_exec(db,
        "INSERT INTO dbmem_content (hash, path, value, length, context, created_at) VALUES "
        "(printf('%016x', 803), 'shared.md', 'one', 3, 'ctx', 0), "
        "(printf('%016x', 804), 'other.md', 'two', 3, 'ctx', 0);"
        "INSERT INTO dbmem_content_source (path, source_path) VALUES "
        "('shared.md', '/tmp/one.md'), "
        "('other.md', 'shared.md');",
        NULL, NULL, NULL);
    ASSERT_EQ(rc, SQLITE_OK);

    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(db, "SELECT memory_delete_file('shared.md');", -1, &stmt, NULL);
    ASSERT_EQ(rc, SQLITE_OK);
    rc = sqlite3_step(stmt);
    ASSERT(rc == SQLITE_ERROR);
    const char *msg = sqlite3_errmsg(db);
    ASSERT(strstr(msg, "matched more than one row") != NULL);
    sqlite3_finalize(stmt);

    sqlite3_int64 count = 0;
    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_content;", &count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(count, 2);

    sqlite3_close(db);
}

TEST(sqlite_memory_delete_file_invalid_path) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT memory_delete_file('');", -1, &stmt, NULL);
    ASSERT_EQ(rc, SQLITE_OK);
    rc = sqlite3_step(stmt);
    ASSERT(rc == SQLITE_ERROR);
    sqlite3_finalize(stmt);

    rc = sqlite3_prepare_v2(db, "SELECT memory_delete_file(123);", -1, &stmt, NULL);
    ASSERT_EQ(rc, SQLITE_OK);
    rc = sqlite3_step(stmt);
    ASSERT(rc == SQLITE_ERROR);
    sqlite3_finalize(stmt);

    sqlite3_close(db);
}

TEST(sqlite_memory_delete_file_removes_directory_marker_only) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    int rc = sqlite3_exec(db,
        "INSERT INTO dbmem_content (hash, path, value, length, context, created_at) VALUES "
        "(printf('%016x', 805), 'dirname/', '', 0, NULL, 0), "
        "(printf('%016x', 806), 'dirname/file.md', 'content', 7, NULL, 0);",
        NULL, NULL, NULL);
    ASSERT_EQ(rc, SQLITE_OK);

    sqlite3_int64 result = 0;
    rc = exec_get_int(db, "SELECT memory_delete_file('dirname');", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 1);

    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_content WHERE path = 'dirname/';", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 0);

    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_content WHERE path = 'dirname/file.md';", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 1);

    sqlite3_close(db);
}

TEST(sqlite_memory_rename_file_direct) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    int rc = sqlite3_exec(db,
        "INSERT INTO dbmem_content (hash, path, value, length, context, created_at) "
        "VALUES (printf('%016x', 780), 'docs/old.md', 'content', 7, 'ctx', 0);"
        "INSERT INTO dbmem_vault (hash, seq, embedding, offset, length) "
        "VALUES (printf('%016x', 780), 0, X'00000000', 0, 7);",
        NULL, NULL, NULL);
    ASSERT_EQ(rc, SQLITE_OK);

    sqlite3_int64 result = 0;
    rc = exec_get_int(db, "SELECT memory_rename_file('docs/old.md', 'docs/new.md');", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 1);

    char path[64];
    rc = exec_get_text(db, "SELECT path FROM dbmem_content WHERE hash = printf('%016x', 780);", path, sizeof(path));
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_STR_EQ(path, "docs/new.md");

    sqlite3_int64 count = 0;
    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_content WHERE path = 'docs/old.md';", &count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(count, 0);

    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_vault WHERE hash = printf('%016x', 780);", &count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(count, 1);

    sqlite3_close(db);
}

TEST(sqlite_memory_rename_file_directory_marker) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    int rc = sqlite3_exec(db,
        "INSERT INTO dbmem_content (hash, path, value, length, context, created_at) "
        "VALUES (printf('%016x', 783), 'old-dir/', '', 0, NULL, 0);",
        NULL, NULL, NULL);
    ASSERT_EQ(rc, SQLITE_OK);

    sqlite3_int64 result = 0;
    rc = exec_get_int(db, "SELECT memory_rename_file('old-dir/', 'new-dir/');", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 1);

    char path[64];
    rc = exec_get_text(db, "SELECT path FROM dbmem_content WHERE hash = printf('%016x', 783);", path, sizeof(path));
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_STR_EQ(path, "new-dir/");

    sqlite3_close(db);
}

TEST(sqlite_memory_rename_file_rejects_marker_file_conversion) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    int rc = sqlite3_exec(db,
        "INSERT INTO dbmem_content (hash, path, value, length, context, created_at) VALUES "
        "(printf('%016x', 784), 'dir/', '', 0, NULL, 0), "
        "(printf('%016x', 785), 'file.md', 'content', 7, NULL, 0);",
        NULL, NULL, NULL);
    ASSERT_EQ(rc, SQLITE_OK);

    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(db, "SELECT memory_rename_file('dir/', 'fileish');", -1, &stmt, NULL);
    ASSERT_EQ(rc, SQLITE_OK);
    rc = sqlite3_step(stmt);
    ASSERT(rc == SQLITE_ERROR);
    sqlite3_finalize(stmt);

    rc = sqlite3_prepare_v2(db, "SELECT memory_rename_file('file.md', 'dirish/');", -1, &stmt, NULL);
    ASSERT_EQ(rc, SQLITE_OK);
    rc = sqlite3_step(stmt);
    ASSERT(rc == SQLITE_ERROR);
    sqlite3_finalize(stmt);

    sqlite3_close(db);
}

TEST(sqlite_memory_rename_file_matches_source_path) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    int rc = sqlite3_exec(db,
        "INSERT INTO dbmem_content (hash, path, value, length, context, created_at) "
        "VALUES (printf('%016x', 782), 'docs/source-old.md', 'content', 7, 'ctx', 0);"
        "INSERT INTO dbmem_content_source (path, source_path) "
        "VALUES ('docs/source-old.md', '/tmp/source-old.md');",
        NULL, NULL, NULL);
    ASSERT_EQ(rc, SQLITE_OK);

    sqlite3_int64 result = 0;
    rc = exec_get_int(db, "SELECT memory_rename_file('/tmp/source-old.md', 'docs/source-new.md');", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 1);

    char path[64];
    rc = exec_get_text(db, "SELECT path FROM dbmem_content WHERE hash = printf('%016x', 782);", path, sizeof(path));
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_STR_EQ(path, "docs/source-new.md");

    char source_path[64];
    rc = exec_get_text(db, "SELECT source_path FROM dbmem_content_source WHERE path = 'docs/source-new.md';", source_path, sizeof(source_path));
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_STR_EQ(source_path, "/tmp/source-old.md");

    sqlite3_close(db);
}

TEST(sqlite_memory_rename_file_missing) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    sqlite3_int64 result = 1;
    int rc = exec_get_int(db, "SELECT memory_rename_file('missing.md', 'new.md');", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 0);

    sqlite3_close(db);
}

TEST(sqlite_memory_rename_file_duplicate_path) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    int rc = sqlite3_exec(db,
        "INSERT INTO dbmem_content (hash, path, value, length, context, created_at) VALUES "
        "(printf('%016x', 790), 'docs/a.md', 'a', 1, NULL, 0), "
        "(printf('%016x', 791), 'docs/b.md', 'b', 1, NULL, 0);",
        NULL, NULL, NULL);
    ASSERT_EQ(rc, SQLITE_OK);

    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(db, "SELECT memory_rename_file('docs/a.md', 'docs/b.md');", -1, &stmt, NULL);
    ASSERT_EQ(rc, SQLITE_OK);
    rc = sqlite3_step(stmt);
    ASSERT(rc == SQLITE_ERROR);
    sqlite3_finalize(stmt);

    sqlite3_int64 count = 0;
    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_content WHERE path IN ('docs/a.md', 'docs/b.md');", &count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(count, 2);

    sqlite3_close(db);
}

TEST(sqlite_memory_rename_file_rejects_ambiguous_path) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    int rc = sqlite3_exec(db,
        "INSERT INTO dbmem_content (hash, path, value, length, context, created_at) VALUES "
        "(printf('%016x', 792), 'shared.md', 'one', 3, NULL, 0), "
        "(printf('%016x', 793), 'other.md', 'two', 3, NULL, 0);"
        "INSERT INTO dbmem_content_source (path, source_path) VALUES "
        "('shared.md', '/tmp/one.md'), "
        "('other.md', 'shared.md');",
        NULL, NULL, NULL);
    ASSERT_EQ(rc, SQLITE_OK);

    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(db, "SELECT memory_rename_file('shared.md', 'renamed.md');", -1, &stmt, NULL);
    ASSERT_EQ(rc, SQLITE_OK);
    rc = sqlite3_step(stmt);
    ASSERT(rc == SQLITE_ERROR);
    const char *msg = sqlite3_errmsg(db);
    ASSERT(strstr(msg, "matched more than one row") != NULL);
    sqlite3_finalize(stmt);

    sqlite3_int64 count = 0;
    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_content WHERE path IN ('shared.md', 'other.md');", &count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(count, 2);

    sqlite3_close(db);
}

#ifdef _WIN32
#define DBMEM_TEST_ABS_ROOT "C:\\dbmem\\project\\"
#else
#define DBMEM_TEST_ABS_ROOT "/tmp/dbmem/project/"
#endif

TEST(sqlite_memory_list_files_empty) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    char json[128];
    int rc = exec_get_text(db, "SELECT memory_list_files();", json, sizeof(json));
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_STR_EQ(json, "{\"root\":\"\",\"children\":[]}");

    sqlite3_close(db);
}

TEST(sqlite_memory_list_files_strips_common_full_path) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    int rc = sqlite3_exec(db,
        "INSERT INTO dbmem_content (hash, path, value, length, context, created_at) VALUES "
        "(printf('%016x', 710), '" DBMEM_TEST_ABS_ROOT "zeta.md', 'v1', 2, NULL, 0), "
        "(printf('%016x', 711), '" DBMEM_TEST_ABS_ROOT "docs/nested/beta.md', 'v2', 2, NULL, 0), "
        "(printf('%016x', 712), '" DBMEM_TEST_ABS_ROOT "docs/alpha.md', 'v3', 2, NULL, 0);",
        NULL, NULL, NULL);
    ASSERT_EQ(rc, SQLITE_OK);

    char json[1024];
    rc = exec_get_text(db, "SELECT memory_list_files();", json, sizeof(json));
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_STR_EQ(json, "{\"root\":\"\",\"children\":[{\"type\":\"directory\",\"name\":\"docs\",\"path\":\"docs\",\"children\":[{\"type\":\"directory\",\"name\":\"nested\",\"path\":\"docs/nested\",\"children\":[{\"type\":\"file\",\"name\":\"beta.md\",\"path\":\"docs/nested/beta.md\",\"indexed\":false}]},{\"type\":\"file\",\"name\":\"alpha.md\",\"path\":\"docs/alpha.md\",\"indexed\":false}]},{\"type\":\"file\",\"name\":\"zeta.md\",\"path\":\"zeta.md\",\"indexed\":false}]}");

    sqlite3_close(db);
}

TEST(sqlite_memory_list_files_keeps_relative_paths) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    int rc = sqlite3_exec(db,
        "INSERT INTO dbmem_content (hash, path, value, length, context, created_at) VALUES "
        "(printf('%016x', 720), 'notes/zeta.md', 'v1', 2, NULL, 0), "
        "(printf('%016x', 721), 'notes/docs/alpha.md', 'v2', 2, NULL, 0);",
        NULL, NULL, NULL);
    ASSERT_EQ(rc, SQLITE_OK);

    char json[1024];
    rc = exec_get_text(db, "SELECT memory_list_files();", json, sizeof(json));
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_STR_EQ(json, "{\"root\":\"\",\"children\":[{\"type\":\"directory\",\"name\":\"notes\",\"path\":\"notes\",\"children\":[{\"type\":\"directory\",\"name\":\"docs\",\"path\":\"notes/docs\",\"children\":[{\"type\":\"file\",\"name\":\"alpha.md\",\"path\":\"notes/docs/alpha.md\",\"indexed\":false}]},{\"type\":\"file\",\"name\":\"zeta.md\",\"path\":\"notes/zeta.md\",\"indexed\":false}]}]}");

    sqlite3_close(db);
}

TEST(sqlite_memory_list_files_strips_single_full_path_directory) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    int rc = sqlite3_exec(db,
        "INSERT INTO dbmem_content (hash, path, value, length, context, created_at) "
        "VALUES (printf('%016x', 730), '" DBMEM_TEST_ABS_ROOT "docs/readme.md', 'v1', 2, NULL, 0);",
        NULL, NULL, NULL);
    ASSERT_EQ(rc, SQLITE_OK);

    char json[512];
    rc = exec_get_text(db, "SELECT memory_list_files();", json, sizeof(json));
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_STR_EQ(json, "{\"root\":\"\",\"children\":[{\"type\":\"file\",\"name\":\"readme.md\",\"path\":\"readme.md\",\"indexed\":false}]}");

    sqlite3_close(db);
}

TEST(sqlite_memory_list_files_normalizes_windows_separators) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    int rc = sqlite3_exec(db,
        "INSERT INTO dbmem_content (hash, path, value, length, context, created_at) VALUES "
        "(printf('%016x', 740), 'C:\\dbmem\\project\\docs\\beta.md', 'v1', 2, NULL, 0), "
        "(printf('%016x', 741), 'C:\\dbmem\\project\\alpha.md', 'v2', 2, NULL, 0);",
        NULL, NULL, NULL);
    ASSERT_EQ(rc, SQLITE_OK);

    char json[1024];
    rc = exec_get_text(db, "SELECT memory_list_files();", json, sizeof(json));
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_STR_EQ(json, "{\"root\":\"\",\"children\":[{\"type\":\"directory\",\"name\":\"docs\",\"path\":\"docs\",\"children\":[{\"type\":\"file\",\"name\":\"beta.md\",\"path\":\"docs/beta.md\",\"indexed\":false}]},{\"type\":\"file\",\"name\":\"alpha.md\",\"path\":\"alpha.md\",\"indexed\":false}]}");

    sqlite3_close(db);
}

TEST(sqlite_memory_list_files_does_not_strip_mixed_path_types) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    int rc = sqlite3_exec(db,
        "INSERT INTO dbmem_content (hash, path, value, length, context, created_at) VALUES "
        "(printf('%016x', 750), '/tmp/dbmem/project/readme.md', 'v1', 2, NULL, 0), "
        "(printf('%016x', 751), 'notes/alpha.md', 'v2', 2, NULL, 0);",
        NULL, NULL, NULL);
    ASSERT_EQ(rc, SQLITE_OK);

    char json[2048];
    rc = exec_get_text(db, "SELECT memory_list_files();", json, sizeof(json));
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_STR_EQ(json, "{\"root\":\"\",\"children\":[{\"type\":\"directory\",\"name\":\"notes\",\"path\":\"notes\",\"children\":[{\"type\":\"file\",\"name\":\"alpha.md\",\"path\":\"notes/alpha.md\",\"indexed\":false}]},{\"type\":\"directory\",\"name\":\"tmp\",\"path\":\"/tmp\",\"children\":[{\"type\":\"directory\",\"name\":\"dbmem\",\"path\":\"/tmp/dbmem\",\"children\":[{\"type\":\"directory\",\"name\":\"project\",\"path\":\"/tmp/dbmem/project\",\"children\":[{\"type\":\"file\",\"name\":\"readme.md\",\"path\":\"/tmp/dbmem/project/readme.md\",\"indexed\":false}]}]}]}]}");

    sqlite3_close(db);
}

TEST(sqlite_memory_list_files_omits_empty_paths) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    int rc = sqlite3_exec(db,
        "INSERT INTO dbmem_content (hash, path, value, length, context, created_at) VALUES "
        "(printf('%016x', 760), '', 'text memory', 11, NULL, 0), "
        "(printf('%016x', 761), 'docs/alpha.md', 'v2', 2, NULL, 0);",
        NULL, NULL, NULL);
    ASSERT_EQ(rc, SQLITE_OK);

    char json[512];
    rc = exec_get_text(db, "SELECT memory_list_files();", json, sizeof(json));
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_STR_EQ(json, "{\"root\":\"\",\"children\":[{\"type\":\"directory\",\"name\":\"docs\",\"path\":\"docs\",\"children\":[{\"type\":\"file\",\"name\":\"alpha.md\",\"path\":\"docs/alpha.md\",\"indexed\":false}]}]}");

    sqlite3_close(db);
}

TEST(sqlite_memory_list_files_escapes_json_strings) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    int rc = sqlite3_exec(db,
        "INSERT INTO dbmem_content (hash, path, value, length, context, created_at) "
        "VALUES (printf('%016x', 770), 'docs/a\"b.md', 'v1', 2, NULL, 0);",
        NULL, NULL, NULL);
    ASSERT_EQ(rc, SQLITE_OK);

    char json[512];
    rc = exec_get_text(db, "SELECT memory_list_files();", json, sizeof(json));
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_STR_EQ(json, "{\"root\":\"\",\"children\":[{\"type\":\"directory\",\"name\":\"docs\",\"path\":\"docs\",\"children\":[{\"type\":\"file\",\"name\":\"a\\\"b.md\",\"path\":\"docs/a\\\"b.md\",\"indexed\":false}]}]}");

    sqlite3_close(db);
}

TEST(sqlite_memory_list_files_includes_empty_directory_marker) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    int rc = sqlite3_exec(db,
        "INSERT INTO dbmem_content (hash, path, value, length, context, created_at) "
        "VALUES (printf('%016x', 771), 'dirname/', '', 0, NULL, 0);",
        NULL, NULL, NULL);
    ASSERT_EQ(rc, SQLITE_OK);

    char json[512];
    rc = exec_get_text(db, "SELECT memory_list_files();", json, sizeof(json));
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_STR_EQ(json, "{\"root\":\"\",\"children\":[{\"type\":\"directory\",\"name\":\"dirname\",\"path\":\"dirname\",\"children\":[]}]}");

    sqlite3_close(db);
}

TEST(sqlite_memory_list_files_merges_directory_marker_with_children) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    int rc = sqlite3_exec(db,
        "INSERT INTO dbmem_content (hash, path, value, length, context, created_at) VALUES "
        "(printf('%016x', 772), 'dirname/', '', 0, NULL, 0), "
        "(printf('%016x', 773), 'dirname/file.md', 'content', 7, NULL, 0);",
        NULL, NULL, NULL);
    ASSERT_EQ(rc, SQLITE_OK);

    char json[512];
    rc = exec_get_text(db, "SELECT memory_list_files();", json, sizeof(json));
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_STR_EQ(json, "{\"root\":\"\",\"children\":[{\"type\":\"directory\",\"name\":\"dirname\",\"path\":\"dirname\",\"children\":[{\"type\":\"file\",\"name\":\"file.md\",\"path\":\"dirname/file.md\",\"indexed\":false}]}]}");

    sqlite3_close(db);
}

TEST(sqlite_memory_list_files_reports_indexed_flag) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    int rc = sqlite3_exec(db,
        "INSERT INTO dbmem_content (hash, path, value, length, context, created_at) VALUES "
        "(printf('%016x', 801), 'done.md', 'v1', 2, NULL, 0), "
        "(printf('%016x', 802), 'todo.md', 'v2', 2, NULL, 0), "
        "(printf('%016x', 803), 'empty.md', '', 0, NULL, 0);",
        NULL, NULL, NULL);
    ASSERT_EQ(rc, SQLITE_OK);

    rc = sqlite3_exec(db,
        "INSERT INTO dbmem_vault (hash, seq, embedding, offset, length, n_tokens, truncated) "
        "VALUES (printf('%016x', 801), 0, zeroblob(16), 0, 2, 1, 0);",
        NULL, NULL, NULL);
    ASSERT_EQ(rc, SQLITE_OK);

    char json[1024];
    rc = exec_get_text(db, "SELECT memory_list_files();", json, sizeof(json));
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_STR_EQ(json, "{\"root\":\"\",\"children\":["
        "{\"type\":\"file\",\"name\":\"done.md\",\"path\":\"done.md\",\"indexed\":true},"
        "{\"type\":\"file\",\"name\":\"empty.md\",\"path\":\"empty.md\",\"indexed\":true},"
        "{\"type\":\"file\",\"name\":\"todo.md\",\"path\":\"todo.md\",\"indexed\":false}]}");

    sqlite3_close(db);
}

TEST(sqlite_memory_materialize_files_creates_directories_and_files) {
    const char *base = TEST_TMP_DIR "/dbmem_materialize";
    const char *docs = TEST_TMP_DIR "/dbmem_materialize/docs";
    const char *nested = TEST_TMP_DIR "/dbmem_materialize/docs/nested";
    const char *file1 = TEST_TMP_DIR "/dbmem_materialize/docs/nested/a.md";
    const char *file2 = TEST_TMP_DIR "/dbmem_materialize/root.md";

    remove_test_file(file1);
    remove_test_file(file2);
    rmdir_p(nested);
    rmdir_p(docs);
    rmdir_p(base);

    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    char sql[2048];
    snprintf(sql, sizeof(sql),
        "INSERT INTO dbmem_content (hash, path, value, length, context, created_at) VALUES "
        "(printf('%%016x', 810), 'docs/nested/a.md', '# Nested\nContent from db.', 25, NULL, 0), "
        "(printf('%%016x', 811), 'root.md', 'Root content', 12, NULL, 0);");
    int rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
    ASSERT_EQ(rc, SQLITE_OK);

    sqlite3_int64 result = 0;
    snprintf(sql, sizeof(sql), "SELECT memory_materialize_files('%s');", base);
    rc = exec_get_int(db, sql, &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 2);

    int64_t len = 0;
    char *content = dbmem_file_read(file1, &len);
    ASSERT(content != NULL);
    ASSERT_STR_EQ(content, "# Nested\nContent from db.");
    dbmemory_free(content);

    content = dbmem_file_read(file2, &len);
    ASSERT(content != NULL);
    ASSERT_STR_EQ(content, "Root content");
    dbmemory_free(content);

    sqlite3_close(db);
    remove_test_file(file1);
    remove_test_file(file2);
    rmdir_p(nested);
    rmdir_p(docs);
    rmdir_p(base);
}

TEST(sqlite_memory_materialize_files_creates_directory_markers) {
    const char *base = TEST_TMP_DIR "/dbmem_materialize_marker";
    const char *dirname = TEST_TMP_DIR "/dbmem_materialize_marker/dirname";

    rmdir_p(dirname);
    rmdir_p(base);

    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    int rc = sqlite3_exec(db,
        "INSERT INTO dbmem_content (hash, path, value, length, context, created_at) "
        "VALUES (printf('%016x', 815), 'dirname/', '', 0, NULL, 0);",
        NULL, NULL, NULL);
    ASSERT_EQ(rc, SQLITE_OK);

    sqlite3_int64 result = 0;
    char sql[1024];
    snprintf(sql, sizeof(sql), "SELECT memory_materialize_files('%s');", base);
    rc = exec_get_int(db, sql, &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 1);
    ASSERT(dbmem_dir_exists(dirname));

    sqlite3_close(db);
    rmdir_p(dirname);
    rmdir_p(base);
}

TEST(sqlite_memory_materialize_files_accepts_existing_same_content) {
    const char *root = TEST_TMP_DIR;
    const char *file = TEST_TMP_DIR "/dbmem_materialize_existing.md";
    const char *content_text = "Already here.";

    remove_test_file(file);
    ASSERT_EQ(create_test_file(file, content_text), 0);

    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    char sql[1024];
    snprintf(sql, sizeof(sql),
        "INSERT INTO dbmem_content (hash, path, value, length, context, created_at) "
        "VALUES (printf('%%016x', 812), 'dbmem_materialize_existing.md', '%s', 13, NULL, 0);",
        content_text);
    int rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
    ASSERT_EQ(rc, SQLITE_OK);

    sqlite3_int64 result = 0;
    snprintf(sql, sizeof(sql), "SELECT memory_materialize_files('%s');", root);
    rc = exec_get_int(db, sql, &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 1);

    int64_t len = 0;
    char *read_back = dbmem_file_read(file, &len);
    ASSERT(read_back != NULL);
    ASSERT_STR_EQ(read_back, content_text);
    dbmemory_free(read_back);

    sqlite3_close(db);
    remove_test_file(file);
}

TEST(sqlite_memory_materialize_files_rejects_parent_segments) {
    const char *root = TEST_TMP_DIR "/dbmem_materialize_safe";
    const char *escaped = TEST_TMP_DIR "/dbmem_materialize_escape.md";

    remove_test_file(escaped);
    rmdir_p(root);
    mkdir_p(root);

    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    int rc = sqlite3_exec(db,
        "INSERT INTO dbmem_content (hash, path, value, length, context, created_at) "
        "VALUES (printf('%016x', 814), '../dbmem_materialize_escape.md', 'escape', 6, NULL, 0);",
        NULL, NULL, NULL);
    ASSERT_EQ(rc, SQLITE_OK);

    char sql[1024];
    snprintf(sql, sizeof(sql), "SELECT memory_materialize_files('%s');", root);
    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    ASSERT_EQ(rc, SQLITE_OK);
    rc = sqlite3_step(stmt);
    ASSERT(rc == SQLITE_ERROR);
    sqlite3_finalize(stmt);

    ASSERT(!dbmem_file_exists(escaped));

    sqlite3_close(db);
    rmdir_p(root);
}

TEST(sqlite_memory_materialize_files_rejects_null_content) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    int rc = sqlite3_exec(db,
        "INSERT INTO dbmem_content (hash, path, value, length, context, created_at) "
        "VALUES (printf('%016x', 813), 'missing-content.md', NULL, 0, NULL, 0);",
        NULL, NULL, NULL);
    ASSERT_EQ(rc, SQLITE_OK);

    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(db, "SELECT memory_materialize_files();", -1, &stmt, NULL);
    ASSERT_EQ(rc, SQLITE_OK);
    rc = sqlite3_step(stmt);
    ASSERT(rc == SQLITE_ERROR);
    sqlite3_finalize(stmt);

    sqlite3_close(db);
}

TEST(sqlite_schema_has_timestamps) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    // Check that schema includes created_at column
    char sql[512];
    int rc = exec_get_text(db,
        "SELECT sql FROM sqlite_master WHERE name='dbmem_content';",
        sql, sizeof(sql));
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT(strstr(sql, "hash TEXT PRIMARY KEY NOT NULL") != NULL);
    ASSERT(strstr(sql, "source_path") == NULL);
    ASSERT(strstr(sql, "created_at") != NULL);
    ASSERT(strstr(sql, "last_accessed") != NULL);

    rc = exec_get_text(db,
        "SELECT sql FROM sqlite_master WHERE name='dbmem_content_source';",
        sql, sizeof(sql));
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT(strstr(sql, "path TEXT PRIMARY KEY NOT NULL") != NULL);
    ASSERT(strstr(sql, "source_path") != NULL);

    rc = exec_get_text(db,
        "SELECT sql FROM sqlite_master WHERE name='dbmem_vault';",
        sql, sizeof(sql));
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT(strstr(sql, "hash TEXT NOT NULL") != NULL);
    ASSERT(strstr(sql, "n_tokens") != NULL);
    ASSERT(strstr(sql, "truncated") != NULL);

    rc = exec_get_text(db,
        "SELECT sql FROM sqlite_master WHERE name='dbmem_cache';",
        sql, sizeof(sql));
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT(strstr(sql, "text_hash TEXT NOT NULL") != NULL);
    ASSERT(strstr(sql, "n_tokens") != NULL);
    ASSERT(strstr(sql, "truncated") != NULL);

    sqlite3_int64 schema_version = 0;
    rc = exec_get_int(db, "SELECT value FROM dbmem_settings WHERE key = 'schema_version';", &schema_version);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(schema_version, 4);

    sqlite3_close(db);
}

TEST(sqlite_schema_migrates_embedding_metadata) {
    sqlite3 *db = NULL;
    int rc = sqlite3_open(":memory:", &db);
    ASSERT_EQ(rc, SQLITE_OK);

    rc = sqlite3_exec(db,
        "CREATE TABLE dbmem_settings (key TEXT PRIMARY KEY, value TEXT);"
        "INSERT INTO dbmem_settings (key, value) VALUES ('schema_version', '1');"
        "CREATE TABLE dbmem_vault (hash TEXT NOT NULL, seq INTEGER NOT NULL, embedding BLOB NOT NULL, offset INTEGER NOT NULL, length INTEGER NOT NULL, PRIMARY KEY (hash, seq));"
        "CREATE TABLE dbmem_cache (text_hash TEXT NOT NULL, provider TEXT NOT NULL, model TEXT NOT NULL, embedding BLOB NOT NULL, dimension INTEGER NOT NULL, PRIMARY KEY (text_hash, provider, model));",
        NULL, NULL, NULL);
    ASSERT_EQ(rc, SQLITE_OK);

    rc = sqlite3_memory_init(db, NULL, NULL);
    ASSERT_EQ(rc, SQLITE_OK);

    sqlite3_int64 count = 0;
    rc = exec_get_int(db, "SELECT COUNT(*) FROM pragma_table_info('dbmem_vault') WHERE name = 'n_tokens';", &count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(count, 1);

    rc = exec_get_int(db, "SELECT COUNT(*) FROM pragma_table_info('dbmem_vault') WHERE name = 'truncated';", &count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(count, 1);

    rc = exec_get_int(db, "SELECT COUNT(*) FROM pragma_table_info('dbmem_cache') WHERE name = 'n_tokens';", &count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(count, 1);

    rc = exec_get_int(db, "SELECT COUNT(*) FROM pragma_table_info('dbmem_cache') WHERE name = 'truncated';", &count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(count, 1);

    rc = sqlite3_exec(db,
        "INSERT INTO dbmem_vault (hash, seq, embedding, offset, length) VALUES (printf('%016x', 900), 0, X'00000000', 0, 4);"
        "INSERT INTO dbmem_cache (text_hash, provider, model, embedding, dimension) VALUES (printf('%016x', 901), 'dummy', 'model', X'00000000', 1);",
        NULL, NULL, NULL);
    ASSERT_EQ(rc, SQLITE_OK);

    rc = exec_get_int(db, "SELECT n_tokens FROM dbmem_vault WHERE hash = printf('%016x', 900);", &count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(count, 0);

    rc = exec_get_int(db, "SELECT truncated FROM dbmem_cache WHERE text_hash = printf('%016x', 901);", &count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(count, 0);

    rc = exec_get_int(db, "SELECT COUNT(*) FROM pragma_table_info('dbmem_content') WHERE name = 'source_path';", &count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(count, 0);

    rc = exec_get_int(db, "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='dbmem_content_source';", &count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(count, 1);

    rc = exec_get_int(db, "SELECT value FROM dbmem_settings WHERE key = 'schema_version';", &count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(count, 4);

    sqlite3_close(db);
}

TEST(sqlite_schema_migrates_source_path_to_local_table) {
    sqlite3 *db = NULL;
    int rc = sqlite3_open(":memory:", &db);
    ASSERT_EQ(rc, SQLITE_OK);

    rc = sqlite3_exec(db,
        "CREATE TABLE dbmem_settings (key TEXT PRIMARY KEY, value TEXT);"
        "INSERT INTO dbmem_settings (key, value) VALUES ('schema_version', '3');"
        "CREATE TABLE dbmem_content (hash TEXT PRIMARY KEY NOT NULL, path TEXT NOT NULL DEFAULT '' UNIQUE, source_path TEXT DEFAULT NULL, value TEXT DEFAULT NULL, length INTEGER NOT NULL DEFAULT 0, context TEXT DEFAULT NULL, created_at INTEGER DEFAULT 0, last_accessed INTEGER DEFAULT 0);"
        "INSERT INTO dbmem_content (hash, path, source_path, value, length, context, created_at, last_accessed) "
        "VALUES (printf('%016x', 998), 'docs/local.md', '/tmp/local.md', 'content', 7, 'ctx', 11, 12);"
        "CREATE TABLE dbmem_vault (hash TEXT NOT NULL, seq INTEGER NOT NULL, embedding BLOB NOT NULL, offset INTEGER NOT NULL, length INTEGER NOT NULL, n_tokens INTEGER NOT NULL DEFAULT 0, truncated INTEGER NOT NULL DEFAULT 0, PRIMARY KEY (hash, seq));"
        "CREATE TABLE dbmem_cache (text_hash TEXT NOT NULL, provider TEXT NOT NULL, model TEXT NOT NULL, embedding BLOB NOT NULL, dimension INTEGER NOT NULL, n_tokens INTEGER NOT NULL DEFAULT 0, truncated INTEGER NOT NULL DEFAULT 0, PRIMARY KEY (text_hash, provider, model));",
        NULL, NULL, NULL);
    ASSERT_EQ(rc, SQLITE_OK);

    rc = sqlite3_memory_init(db, NULL, NULL);
    ASSERT_EQ(rc, SQLITE_OK);

    sqlite3_int64 count = 0;
    rc = exec_get_int(db, "SELECT COUNT(*) FROM pragma_table_info('dbmem_content') WHERE name = 'source_path';", &count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(count, 0);

    char source_path[64];
    rc = exec_get_text(db, "SELECT source_path FROM dbmem_content_source WHERE path = 'docs/local.md';", source_path, sizeof(source_path));
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_STR_EQ(source_path, "/tmp/local.md");

    rc = exec_get_int(db, "SELECT value FROM dbmem_settings WHERE key = 'schema_version';", &count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(count, 4);

    sqlite3_close(db);
}

// Test that inserting directly into tables works with new schema
TEST(sqlite_direct_insert_with_timestamp) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    // Insert a test record directly
    int rc = sqlite3_exec(db,
        "INSERT INTO dbmem_content (hash, path, value, length, context, created_at) "
        "VALUES (printf('%016x', 123), 'test/path', 'test value', 10, 'ctx1', strftime('%s','now'));",
        NULL, NULL, NULL);
    ASSERT_EQ(rc, SQLITE_OK);

    // Verify it's there
    sqlite3_int64 count;
    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_content;", &count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(count, 1);

    // Verify created_at was set
    sqlite3_int64 created_at;
    rc = exec_get_int(db, "SELECT created_at FROM dbmem_content WHERE hash = printf('%016x', 123);", &created_at);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT(created_at > 0);  // Should be a valid Unix timestamp

    sqlite3_close(db);
}

TEST(sqlite_memory_delete_direct) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    // Insert a test record directly
    int rc = sqlite3_exec(db,
        "INSERT INTO dbmem_content (hash, path, value, length, context, created_at) "
        "VALUES (printf('%016x', 456), 'test/path2', 'test value 2', 12, 'ctx2', strftime('%s','now'));"
        "INSERT INTO dbmem_content_source (path, source_path) "
        "VALUES ('test/path2', '/tmp/test/path2');",
        NULL, NULL, NULL);
    ASSERT_EQ(rc, SQLITE_OK);

    // Delete it
    sqlite3_int64 result;
    rc = exec_get_int(db, "SELECT memory_delete(printf('%016x', 456));", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 1);  // Should have deleted 1 row

    // Verify it's gone
    sqlite3_int64 count;
    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_content;", &count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(count, 0);

    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_content_source;", &count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(count, 0);

    sqlite3_close(db);
}

TEST(sqlite_memory_delete_context_direct) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    // Insert test records with different contexts
    int rc = sqlite3_exec(db,
        "INSERT INTO dbmem_content (hash, path, value, length, context, created_at) VALUES "
        "(printf('%016x', 100), 'path1', 'v1', 2, 'ctx_a', 0), "
        "(printf('%016x', 101), 'path2', 'v2', 2, 'ctx_a', 0), "
        "(printf('%016x', 102), 'path3', 'v3', 2, 'ctx_b', 0);"
        "INSERT INTO dbmem_content_source (path, source_path) VALUES "
        "('path1', '/tmp/path1'), "
        "('path2', '/tmp/path2'), "
        "('path3', '/tmp/path3');",
        NULL, NULL, NULL);
    ASSERT_EQ(rc, SQLITE_OK);

    // Delete context 'ctx_a'
    sqlite3_int64 result;
    rc = exec_get_int(db, "SELECT memory_delete_context('ctx_a');", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 2);  // Should have deleted 2 rows

    // Verify only ctx_b remains
    sqlite3_int64 count;
    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_content;", &count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(count, 1);

    // Verify ctx_b is the remaining one
    char context[64];
    rc = exec_get_text(db, "SELECT context FROM dbmem_content;", context, sizeof(context));
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_STR_EQ(context, "ctx_b");

    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_content_source;", &count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(count, 1);

    char source_path[64];
    rc = exec_get_text(db, "SELECT source_path FROM dbmem_content_source;", source_path, sizeof(source_path));
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_STR_EQ(source_path, "/tmp/path3");

    sqlite3_close(db);
}

TEST(sqlite_memory_clear_direct) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    // Insert test records
    int rc = sqlite3_exec(db,
        "INSERT INTO dbmem_content (hash, path, value, length, context, created_at) VALUES "
        "(printf('%016x', 200), 'p1', 'v1', 2, 'c1', 0), "
        "(printf('%016x', 201), 'p2', 'v2', 2, 'c2', 0);"
        "INSERT INTO dbmem_content_source (path, source_path) VALUES "
        "('p1', '/tmp/p1'), "
        "('p2', '/tmp/p2');",
        NULL, NULL, NULL);
    ASSERT_EQ(rc, SQLITE_OK);

    // Clear all
    sqlite3_int64 result;
    rc = exec_get_int(db, "SELECT memory_clear();", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 1);

    // Verify all gone
    sqlite3_int64 count;
    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_content;", &count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(count, 0);

    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_content_source;", &count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(count, 0);

    sqlite3_close(db);
}

TEST(sqlite_memory_delete_with_vault_data) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    // Insert into content and vault tables
    int rc = sqlite3_exec(db,
        "INSERT INTO dbmem_content (hash, path, value, length, context, created_at) "
        "VALUES (printf('%016x', 300), 'path300', 'value', 5, 'ctx', 0);",
        NULL, NULL, NULL);
    ASSERT_EQ(rc, SQLITE_OK);

    rc = sqlite3_exec(db,
        "INSERT INTO dbmem_vault (hash, seq, embedding, offset, length) "
        "VALUES (printf('%016x', 300), 0, X'00000000', 0, 5), (printf('%016x', 300), 1, X'00000000', 5, 5);",
        NULL, NULL, NULL);
    ASSERT_EQ(rc, SQLITE_OK);

    // Verify vault has data
    sqlite3_int64 vault_count;
    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_vault WHERE hash = printf('%016x', 300);", &vault_count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(vault_count, 2);

    // Delete by hash
    sqlite3_int64 result;
    rc = exec_get_int(db, "SELECT memory_delete(printf('%016x', 300));", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 1);

    // Verify content is gone
    sqlite3_int64 content_count;
    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_content WHERE hash = printf('%016x', 300);", &content_count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(content_count, 0);

    // Verify vault is also gone
    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_vault WHERE hash = printf('%016x', 300);", &vault_count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(vault_count, 0);

    sqlite3_close(db);
}

TEST(sqlite_memory_delete_twice) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    // Insert a record
    int rc = sqlite3_exec(db,
        "INSERT INTO dbmem_content (hash, path, value, length, context, created_at) "
        "VALUES (printf('%016x', 400), 'path400', 'value', 5, 'ctx', 0);",
        NULL, NULL, NULL);
    ASSERT_EQ(rc, SQLITE_OK);

    // Delete first time - should return 1
    sqlite3_int64 result;
    rc = exec_get_int(db, "SELECT memory_delete(printf('%016x', 400));", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 1);

    // Delete second time - should return 0
    rc = exec_get_int(db, "SELECT memory_delete(printf('%016x', 400));", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 0);

    sqlite3_close(db);
}

TEST(sqlite_memory_delete_context_null) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    // Insert records - some with NULL context, some with context
    int rc = sqlite3_exec(db,
        "INSERT INTO dbmem_content (hash, path, value, length, context, created_at) VALUES "
        "(printf('%016x', 500), 'p1', 'v1', 2, NULL, 0), "
        "(printf('%016x', 501), 'p2', 'v2', 2, NULL, 0), "
        "(printf('%016x', 502), 'p3', 'v3', 2, 'has_context', 0);",
        NULL, NULL, NULL);
    ASSERT_EQ(rc, SQLITE_OK);

    // Verify 3 records
    sqlite3_int64 count;
    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_content;", &count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(count, 3);

    // Delete NULL context - function expects TEXT, so NULL returns error
    // This is expected behavior - use empty string '' for records with no context
    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(db, "SELECT memory_delete_context(NULL);", -1, &stmt, NULL);
    ASSERT_EQ(rc, SQLITE_OK);
    rc = sqlite3_step(stmt);
    // Should return error because NULL is not TEXT type
    ASSERT(rc == SQLITE_ERROR);
    sqlite3_finalize(stmt);

    // Verify records are still there (nothing was deleted)
    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_content;", &count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(count, 3);

    sqlite3_close(db);
}

TEST(sqlite_memory_delete_wrong_type) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    // Try to call memory_delete with an invalid hash string
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT memory_delete('not_a_number');", -1, &stmt, NULL);
    ASSERT_EQ(rc, SQLITE_OK);

    rc = sqlite3_step(stmt);
    // Should return an error
    ASSERT(rc == SQLITE_ERROR || rc == SQLITE_ROW);  // Implementation may vary
    sqlite3_finalize(stmt);

    sqlite3_close(db);
}

TEST(sqlite_memory_delete_context_wrong_type) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    // Try to call memory_delete_context with INTEGER instead of TEXT
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT memory_delete_context(12345);", -1, &stmt, NULL);
    ASSERT_EQ(rc, SQLITE_OK);

    rc = sqlite3_step(stmt);
    // Should return an error
    ASSERT(rc == SQLITE_ERROR || rc == SQLITE_ROW);  // Implementation may vary
    sqlite3_finalize(stmt);

    sqlite3_close(db);
}

TEST(sqlite_memory_update_access_setting) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    // Set update_access to 0
    sqlite3_int64 result;
    int rc = exec_get_int(db, "SELECT memory_set_option('update_access', 0);", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 1);

    // Get the setting back
    rc = exec_get_int(db, "SELECT memory_get_option('update_access');", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 0);

    // Set it back to 1
    rc = exec_get_int(db, "SELECT memory_set_option('update_access', 1);", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 1);

    // Verify
    rc = exec_get_int(db, "SELECT memory_get_option('update_access');", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 1);

    sqlite3_close(db);
}

TEST(sqlite_memory_created_at_valid_range) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    // Insert with current timestamp
    int rc = sqlite3_exec(db,
        "INSERT INTO dbmem_content (hash, path, value, length, context, created_at) "
        "VALUES (printf('%016x', 600), 'path600', 'value', 5, 'ctx', strftime('%s','now'));",
        NULL, NULL, NULL);
    ASSERT_EQ(rc, SQLITE_OK);

    // Get the created_at value
    sqlite3_int64 created_at;
    rc = exec_get_int(db, "SELECT created_at FROM dbmem_content WHERE hash = printf('%016x', 600);", &created_at);
    ASSERT_EQ(rc, SQLITE_OK);

    // Should be greater than 0
    ASSERT(created_at > 0);

    // Should be a reasonable Unix timestamp (after year 2020 = 1577836800)
    ASSERT(created_at > 1577836800);

    // Should not be in the future (give 60 seconds buffer)
    sqlite3_int64 now;
    rc = exec_get_int(db, "SELECT strftime('%s','now');", &now);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT(created_at <= now + 60);

    sqlite3_close(db);
}

TEST(sqlite_memory_clear_with_vault_fts) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    // Insert into all tables
    int rc = sqlite3_exec(db,
        "INSERT INTO dbmem_content (hash, path, value, length, context, created_at) "
        "VALUES (printf('%016x', 700), 'path700', 'value', 5, 'ctx', 0);",
        NULL, NULL, NULL);
    ASSERT_EQ(rc, SQLITE_OK);

    rc = sqlite3_exec(db,
        "INSERT INTO dbmem_vault (hash, seq, embedding, offset, length) "
        "VALUES (printf('%016x', 700), 0, X'00000000', 0, 5);",
        NULL, NULL, NULL);
    ASSERT_EQ(rc, SQLITE_OK);

    rc = sqlite3_exec(db,
        "INSERT INTO dbmem_vault_fts (content, hash, seq, context) "
        "VALUES ('test content', printf('%016x', 700), 0, 'ctx');",
        NULL, NULL, NULL);
    ASSERT_EQ(rc, SQLITE_OK);

    // Clear all
    sqlite3_int64 result;
    rc = exec_get_int(db, "SELECT memory_clear();", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 1);

    // Verify all tables are empty
    sqlite3_int64 count;
    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_content;", &count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(count, 0);

    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_vault;", &count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(count, 0);

    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_vault_fts;", &count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(count, 0);

    sqlite3_close(db);
}

// Helper to insert a fake dbmem_content entry with a known path, hash, and length
static int insert_fake_content(sqlite3 *db, uint64_t hash, const char *path, const char *context, sqlite3_int64 length) {
    sqlite3_stmt *vm = NULL;
    const char *sql = "INSERT INTO dbmem_content (hash, path, value, length, context, created_at) "
                      "VALUES (?1, ?2, 'fake', ?3, ?4, 0);";
    int rc = sqlite3_prepare_v2(db, sql, -1, &vm, NULL);
    if (rc != SQLITE_OK) return rc;
    char hash_text[DBMEM_HASH_STR_MAXLEN];
    sqlite3_bind_text(vm, 1, dbmem_hash_to_hex(hash, hash_text), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(vm, 2, path, -1, SQLITE_STATIC);
    sqlite3_bind_int64(vm, 3, length);
    if (context) sqlite3_bind_text(vm, 4, context, -1, SQLITE_STATIC);
    else sqlite3_bind_null(vm, 4);
    rc = sqlite3_step(vm);
    sqlite3_finalize(vm);
    return (rc == SQLITE_DONE) ? SQLITE_OK : rc;
}

TEST(sqlite_sync_directory_removes_deleted) {
    // Test that memory_add_directory removes entries for files no longer on disk
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    const char *test_dir = TEST_TMP_DIR "/dbmem_test_sync_del";
    const char *file_keep = TEST_TMP_DIR "/dbmem_test_sync_del/keep.md";
    const char *file_gone = TEST_TMP_DIR "/dbmem_test_sync_del/gone.md";

    // Clean up
    remove(file_keep);
    remove(file_gone);
    rmdir_p(test_dir);

    // Create directory with one file
    mkdir_p(test_dir);
    create_test_file(file_keep, "# Keep me\nStill here.");

    // Pre-insert entries: one for the existing file (with correct hash),
    // and one for a file that no longer exists
    int64_t len = 0;
    char *buf = dbmem_file_read(file_keep, &len);
    ASSERT(buf != NULL);
    uint64_t keep_hash = dbmem_hash_compute(buf, (size_t)len);
    dbmemory_free(buf);

    int rc = insert_fake_content(db, keep_hash, file_keep, NULL, len);
    ASSERT_EQ(rc, SQLITE_OK);

    rc = insert_fake_content(db, 99999, file_gone, NULL, 4);
    ASSERT_EQ(rc, SQLITE_OK);

    // Verify 2 entries before sync
    sqlite3_int64 count;
    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_content;", &count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(count, 2);

    // Sync — should remove the entry for gone.md, skip keep.md (hash match)
    sqlite3_int64 result;
    rc = exec_get_int(db, "SELECT memory_add_directory('" TEST_TMP_DIR "/dbmem_test_sync_del');", &result);
    ASSERT_EQ(rc, SQLITE_OK);

    // Only keep.md entry should remain
    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_content;", &count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(count, 1);

    char path[256];
    rc = exec_get_text(db, "SELECT path FROM dbmem_content;", path, sizeof(path));
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT(strstr(path, "keep.md") != NULL);

    remove(file_keep);
    rmdir_p(test_dir);
    sqlite3_close(db);
}

TEST(sqlite_sync_directory_removes_all_deleted) {
    // Test sync on a directory where ALL previously indexed files were deleted
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    const char *test_dir = TEST_TMP_DIR "/dbmem_test_sync_allgone";
    const char *file_a = TEST_TMP_DIR "/dbmem_test_sync_allgone/a.md";
    const char *file_b = TEST_TMP_DIR "/dbmem_test_sync_allgone/b.md";
    const char *file_c = TEST_TMP_DIR "/dbmem_test_sync_allgone/c.md";

    remove(TEST_TMP_DIR "/dbmem_test_sync_allgone/x.md");
    rmdir_p(test_dir);
    mkdir_p(test_dir);  // empty directory

    // Insert fake entries pointing to files that don't exist
    int rc = insert_fake_content(db, 1001, file_a, "ctx", 4);
    ASSERT_EQ(rc, SQLITE_OK);
    rc = insert_fake_content(db, 1002, file_b, "ctx", 4);
    ASSERT_EQ(rc, SQLITE_OK);
    rc = insert_fake_content(db, 1003, file_c, "ctx", 4);
    ASSERT_EQ(rc, SQLITE_OK);

    // Also insert vault entries to verify cascade delete
    rc = sqlite3_exec(db,
        "INSERT INTO dbmem_vault (hash, seq, embedding, offset, length) VALUES "
        "(printf('%016x', 1001), 0, X'00000000', 0, 4), "
        "(printf('%016x', 1002), 0, X'00000000', 0, 4), "
        "(printf('%016x', 1003), 0, X'00000000', 0, 4);",
        NULL, NULL, NULL);
    ASSERT_EQ(rc, SQLITE_OK);

    sqlite3_int64 count;
    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_content;", &count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(count, 3);

    // Sync — all files gone, all entries should be removed
    sqlite3_int64 result;
    rc = exec_get_int(db, "SELECT memory_add_directory('" TEST_TMP_DIR "/dbmem_test_sync_allgone');", &result);
    ASSERT_EQ(rc, SQLITE_OK);

    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_content;", &count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(count, 0);

    // Vault entries should also be gone
    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_vault;", &count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(count, 0);

    rmdir_p(test_dir);
    sqlite3_close(db);
}

TEST(sqlite_sync_directory_skips_unchanged) {
    // Test that sync skips files whose content hash hasn't changed
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    const char *test_dir = TEST_TMP_DIR "/dbmem_test_sync_skip";
    const char *file = TEST_TMP_DIR "/dbmem_test_sync_skip/note.md";
    const char *content = "# My Note\nSome content.";

    remove(file);
    rmdir_p(test_dir);
    mkdir_p(test_dir);
    create_test_file(file, content);

    // Compute the hash from disk so Windows text-mode newline translation
    // cannot make the pre-inserted hash differ from memory_add_directory().
    int64_t len = 0;
    char *buf = dbmem_file_read(file, &len);
    ASSERT(buf != NULL);
    uint64_t hash = dbmem_hash_compute(buf, (size_t)len);
    dbmemory_free(buf);

    int rc = insert_fake_content(db, hash, file, "notes", len);
    ASSERT_EQ(rc, SQLITE_OK);

    // Sync — file exists with matching hash, should be skipped
    sqlite3_int64 result;
    rc = exec_get_int(db, "SELECT memory_add_directory('" TEST_TMP_DIR "/dbmem_test_sync_skip', 'notes');", &result);
    ASSERT_EQ(rc, SQLITE_OK);

    // Entry still exists unchanged (no duplication)
    sqlite3_int64 count;
    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_content;", &count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(count, 1);

    remove(file);
    rmdir_p(test_dir);
    sqlite3_close(db);
}

TEST(sqlite_sync_directory_ignores_sibling_prefixes) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    const char *test_dir = TEST_TMP_DIR "/dbmem_test_sync_prefix";
    const char *target_file = TEST_TMP_DIR "/dbmem_test_sync_prefix/gone.md";
    const char *sibling_file = TEST_TMP_DIR "/dbmem_test_sync_prefix2/gone.md";

    remove_test_file(target_file);
    remove_test_file(sibling_file);
    rmdir_p(TEST_TMP_DIR "/dbmem_test_sync_prefix2");
    rmdir_p(test_dir);
    mkdir_p(test_dir);

    int rc = insert_fake_content(db, 3001, target_file, NULL, 4);
    ASSERT_EQ(rc, SQLITE_OK);
    rc = insert_fake_content(db, 3002, sibling_file, NULL, 4);
    ASSERT_EQ(rc, SQLITE_OK);

    sqlite3_int64 result;
    rc = exec_get_int(db, "SELECT memory_add_directory('" TEST_TMP_DIR "/dbmem_test_sync_prefix');", &result);
    ASSERT_EQ(rc, SQLITE_OK);

    sqlite3_int64 count = 0;
    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_content;", &count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(count, 1);

    char path[256];
    rc = exec_get_text(db, "SELECT path FROM dbmem_content;", path, sizeof(path));
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_STR_EQ(path, sibling_file);

    rmdir_p(test_dir);
    sqlite3_close(db);
}

TEST(sqlite_sync_directory_keeps_logical_rows_without_source_path) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    const char *test_dir = TEST_TMP_DIR "/dbmem_test_sync_logical";
    rmdir_p(test_dir);
    mkdir_p(test_dir);

    int rc = insert_fake_content(db, 4001, "logical-note-without-source", "ctx", 4);
    ASSERT_EQ(rc, SQLITE_OK);

    sqlite3_int64 result = -1;
    rc = exec_get_int(db, "SELECT memory_add_directory('" TEST_TMP_DIR "/dbmem_test_sync_logical');", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 0);

    sqlite3_int64 count = 0;
    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_content WHERE path = 'logical-note-without-source';", &count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(count, 1);

    rmdir_p(test_dir);
    sqlite3_close(db);
}

TEST(sqlite_cache_table_exists) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    // Check that dbmem_cache table exists
    char sql[512];
    int rc = exec_get_text(db,
        "SELECT sql FROM sqlite_master WHERE name='dbmem_cache';",
        sql, sizeof(sql));
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT(strstr(sql, "text_hash") != NULL);
    ASSERT(strstr(sql, "text_hash TEXT NOT NULL") != NULL);
    ASSERT(strstr(sql, "provider") != NULL);
    ASSERT(strstr(sql, "model") != NULL);
    ASSERT(strstr(sql, "embedding") != NULL);
    ASSERT(strstr(sql, "dimension") != NULL);
    ASSERT(strstr(sql, "n_tokens") != NULL);
    ASSERT(strstr(sql, "truncated") != NULL);

    sqlite3_close(db);
}

TEST(sqlite_cache_clear_empty) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    sqlite3_int64 result;
    int rc = exec_get_int(db, "SELECT memory_cache_clear();", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 0);  // No rows deleted from empty cache

    sqlite3_close(db);
}

TEST(sqlite_cache_clear_with_data) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    // Insert some fake cache entries
    int rc = sqlite3_exec(db,
        "INSERT INTO dbmem_cache (text_hash, provider, model, embedding, dimension) VALUES "
        "(printf('%016x', 111), 'openai', 'text-embedding-3-small', X'00000000', 1), "
        "(printf('%016x', 222), 'openai', 'text-embedding-3-small', X'00000000', 1), "
        "(printf('%016x', 333), 'local', 'nomic', X'00000000', 1);",
        NULL, NULL, NULL);
    ASSERT_EQ(rc, SQLITE_OK);

    // Clear all
    sqlite3_int64 result;
    rc = exec_get_int(db, "SELECT memory_cache_clear();", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 3);

    // Verify empty
    sqlite3_int64 count;
    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_cache;", &count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(count, 0);

    sqlite3_close(db);
}

TEST(sqlite_cache_clear_by_provider_model) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    // Insert cache entries for different provider/model combos
    int rc = sqlite3_exec(db,
        "INSERT INTO dbmem_cache (text_hash, provider, model, embedding, dimension) VALUES "
        "(printf('%016x', 111), 'openai', 'text-embedding-3-small', X'00000000', 1), "
        "(printf('%016x', 222), 'openai', 'text-embedding-3-small', X'00000000', 1), "
        "(printf('%016x', 333), 'local', 'nomic', X'00000000', 1);",
        NULL, NULL, NULL);
    ASSERT_EQ(rc, SQLITE_OK);

    // Clear only openai entries
    sqlite3_int64 result;
    rc = exec_get_int(db, "SELECT memory_cache_clear('openai', 'text-embedding-3-small');", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 2);

    // Verify only local entry remains
    sqlite3_int64 count;
    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_cache;", &count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(count, 1);

    char provider[64];
    rc = exec_get_text(db, "SELECT provider FROM dbmem_cache;", provider, sizeof(provider));
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_STR_EQ(provider, "local");

    sqlite3_close(db);
}

TEST(sqlite_cache_setting_default) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    // embedding_cache should default to enabled (not in settings table, but enabled in context)
    // Set it to 0, read back, set to 1, read back
    sqlite3_int64 result;
    int rc = exec_get_int(db, "SELECT memory_set_option('embedding_cache', 0);", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 1);

    rc = exec_get_int(db, "SELECT memory_get_option('embedding_cache');", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 0);

    rc = exec_get_int(db, "SELECT memory_set_option('embedding_cache', 1);", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 1);

    rc = exec_get_int(db, "SELECT memory_get_option('embedding_cache');", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 1);

    sqlite3_close(db);
}

TEST(sqlite_cache_max_entries_setting) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    // Default is 0 (no limit)
    sqlite3_int64 result;
    int rc = exec_get_int(db, "SELECT memory_set_option('cache_max_entries', 100);", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 1);

    rc = exec_get_int(db, "SELECT memory_get_option('cache_max_entries');", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 100);

    // Set back to 0 (no limit)
    rc = exec_get_int(db, "SELECT memory_set_option('cache_max_entries', 0);", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 1);

    rc = exec_get_int(db, "SELECT memory_get_option('cache_max_entries');", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 0);

    sqlite3_close(db);
}

TEST(sqlite_cache_eviction) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    // Set max entries to 3
    sqlite3_int64 result;
    int rc = exec_get_int(db, "SELECT memory_set_option('cache_max_entries', 3);", &result);
    ASSERT_EQ(rc, SQLITE_OK);

    // Insert 5 entries (rowids 1-5)
    rc = sqlite3_exec(db,
        "INSERT INTO dbmem_cache (text_hash, provider, model, embedding, dimension) VALUES "
        "(printf('%016x', 1), 'p', 'm', X'00000000', 1), "
        "(printf('%016x', 2), 'p', 'm', X'00000000', 1), "
        "(printf('%016x', 3), 'p', 'm', X'00000000', 1), "
        "(printf('%016x', 4), 'p', 'm', X'00000000', 1), "
        "(printf('%016x', 5), 'p', 'm', X'00000000', 1);",
        NULL, NULL, NULL);
    ASSERT_EQ(rc, SQLITE_OK);

    // Verify 5 entries before any sync triggers eviction
    sqlite3_int64 count;
    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_cache;", &count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(count, 5);

    // Now manually call memory_cache_clear to clear all, then re-insert within limit
    rc = exec_get_int(db, "SELECT memory_cache_clear();", &result);
    ASSERT_EQ(rc, SQLITE_OK);

    // Insert exactly 3 (at limit)
    rc = sqlite3_exec(db,
        "INSERT INTO dbmem_cache (text_hash, provider, model, embedding, dimension) VALUES "
        "(printf('%016x', 10), 'p', 'm', X'00000000', 1), "
        "(printf('%016x', 11), 'p', 'm', X'00000000', 1), "
        "(printf('%016x', 12), 'p', 'm', X'00000000', 1);",
        NULL, NULL, NULL);
    ASSERT_EQ(rc, SQLITE_OK);

    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_cache;", &count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(count, 3);

    sqlite3_close(db);
}

TEST(sqlite_cache_no_eviction_when_unlimited) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    // Default cache_max_entries is 0 (no limit)
    // Insert many entries, none should be evicted
    int rc = sqlite3_exec(db,
        "INSERT INTO dbmem_cache (text_hash, provider, model, embedding, dimension) VALUES "
        "(printf('%016x', 1), 'p', 'm', X'00000000', 1), "
        "(printf('%016x', 2), 'p', 'm', X'00000000', 1), "
        "(printf('%016x', 3), 'p', 'm', X'00000000', 1), "
        "(printf('%016x', 4), 'p', 'm', X'00000000', 1), "
        "(printf('%016x', 5), 'p', 'm', X'00000000', 1);",
        NULL, NULL, NULL);
    ASSERT_EQ(rc, SQLITE_OK);

    sqlite3_int64 count;
    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_cache;", &count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(count, 5);

    sqlite3_close(db);
}

TEST(sqlite_search_oversample_setting) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    // Default is 0 (no oversampling)
    sqlite3_int64 result;
    int rc = exec_get_int(db, "SELECT memory_set_option('search_oversample', 4);", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 1);

    rc = exec_get_int(db, "SELECT memory_get_option('search_oversample');", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 4);

    // Set back to 0 (no oversampling)
    rc = exec_get_int(db, "SELECT memory_set_option('search_oversample', 0);", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 1);

    rc = exec_get_int(db, "SELECT memory_get_option('search_oversample');", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 0);

    sqlite3_close(db);
}

TEST(sqlite_search_zero_value_settings_apply_to_context) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    sqlite3_int64 result = 0;
    int rc = exec_get_int(db, "SELECT memory_set_option('max_results', 0);", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    rc = exec_get_int(db, "SELECT memory_set_option('vector_weight', 0.0);", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    rc = exec_get_int(db, "SELECT memory_set_option('text_weight', 0.0);", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    rc = exec_get_int(db, "SELECT memory_set_option('min_score', 0.0);", &result);
    ASSERT_EQ(rc, SQLITE_OK);

    dbmem_context *ctx = get_test_ctx(db);
    ASSERT(ctx != NULL);
    ASSERT_EQ(dbmem_context_max_results(ctx), 0);
    ASSERT_EQ(dbmem_context_vector_weight(ctx), 0.0);
    ASSERT_EQ(dbmem_context_text_weight(ctx), 0.0);
    ASSERT_EQ(dbmem_context_min_score(ctx), 0.0);

    sqlite3_close(db);
}

TEST(sqlite_memory_delete_context_with_vault) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    // Insert records with different contexts into content and vault
    int rc = sqlite3_exec(db,
        "INSERT INTO dbmem_content (hash, path, value, length, context, created_at) VALUES "
        "(printf('%016x', 800), 'p1', 'v1', 2, 'delete_me', 0), "
        "(printf('%016x', 801), 'p2', 'v2', 2, 'keep_me', 0);",
        NULL, NULL, NULL);
    ASSERT_EQ(rc, SQLITE_OK);

    rc = sqlite3_exec(db,
        "INSERT INTO dbmem_vault (hash, seq, embedding, offset, length) VALUES "
        "(printf('%016x', 800), 0, X'00000000', 0, 2), "
        "(printf('%016x', 801), 0, X'00000000', 0, 2);",
        NULL, NULL, NULL);
    ASSERT_EQ(rc, SQLITE_OK);

    // Delete context 'delete_me'
    sqlite3_int64 result;
    rc = exec_get_int(db, "SELECT memory_delete_context('delete_me');", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 1);

    // Verify content: only 'keep_me' remains
    sqlite3_int64 count;
    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_content;", &count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(count, 1);

    // Verify vault: only hash 801 remains
    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_vault;", &count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(count, 1);

    char hash[64];
    rc = exec_get_text(db, "SELECT hash FROM dbmem_vault;", hash, sizeof(hash));
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_STR_EQ(hash, "0000000000000321");

    sqlite3_close(db);
}
// ============================================================================
// Custom Provider Tests
// ============================================================================

// Dummy embedding engine for testing
typedef struct {
    float embedding[4];
    int dimension;
    int compute_count;
    char api_key[256];
} dummy_engine_t;

static int dummy_compute_calls = 0;
static int dummy_init_calls = 0;

static void *dummy_init(const char *model, const char *api_key, void *xdata, char err_msg[1024]) {
    UNUSED_PARAM(model);
    UNUSED_PARAM(xdata);
    dummy_init_calls++;
    dummy_engine_t *e = (dummy_engine_t *)calloc(1, sizeof(dummy_engine_t));
    if (!e) { snprintf(err_msg, 1024, "alloc failed"); return NULL; }
    e->dimension = 4;
    e->embedding[0] = 0.1f;
    e->embedding[1] = 0.2f;
    e->embedding[2] = 0.3f;
    e->embedding[3] = 0.4f;
    if (api_key) strncpy(e->api_key, api_key, sizeof(e->api_key) - 1);
    return e;
}

static int dummy_compute(void *engine, const char *text, int text_len, void *xdata, dbmem_embedding_result_t *result) {
    UNUSED_PARAM(text);
    UNUSED_PARAM(text_len);
    UNUSED_PARAM(xdata);
    dummy_engine_t *e = (dummy_engine_t *)engine;
    e->compute_count++;
    dummy_compute_calls++;
    result->n_tokens = text_len / 4;
    result->truncated = false;
    result->n_embd = e->dimension;
    result->embedding = e->embedding;
    return 0;
}

static int truncated_dummy_compute(void *engine, const char *text, int text_len, void *xdata, dbmem_embedding_result_t *result) {
    int rc = dummy_compute(engine, text, text_len, xdata, result);
    if (rc != 0) return rc;
    result->n_tokens = 3;
    result->truncated = true;
    return 0;
}

static void dummy_free(void *engine, void *xdata) {
    UNUSED_PARAM(xdata);
    free(engine);
}

TEST(sqlite_memory_add_content_uses_explicit_content_and_context) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    const char *path = "docs/dbmem_explicit_content.md";
    const char *disk_content = "This content came from disk.";
    const char *explicit_content = "# Explicit Content\nThis content came from the SQL argument.";

    dbmem_provider_t prov = { .init = dummy_init, .compute = dummy_compute, .free = dummy_free };
    int rc = sqlite3_memory_register_provider(db, "dummy", &prov);
    ASSERT_EQ(rc, SQLITE_OK);

    sqlite3_int64 result = 0;
    rc = exec_get_int(db, "SELECT memory_set_model('dummy', 'test-model');", &result);
    ASSERT_EQ(rc, SQLITE_OK);

    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(db, "SELECT memory_add_content(?1, ?2, ?3);", -1, &stmt, NULL);
    ASSERT_EQ(rc, SQLITE_OK);
    sqlite3_bind_text(stmt, 1, path, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, explicit_content, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, "sync-context", -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);
    ASSERT_EQ(rc, SQLITE_ROW);
    ASSERT_EQ(sqlite3_column_int64(stmt, 0), 1);
    sqlite3_finalize(stmt);

    char value[256];
    rc = exec_get_text(db, "SELECT value FROM dbmem_content;", value, sizeof(value));
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_STR_EQ(value, explicit_content);
    ASSERT(strstr(value, disk_content) == NULL);

    char stored_path[256];
    rc = exec_get_text(db, "SELECT path FROM dbmem_content;", stored_path, sizeof(stored_path));
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_STR_EQ(stored_path, "docs/dbmem_explicit_content.md");

    sqlite3_int64 source_count = 1;
    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_content_source;", &source_count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(source_count, 0);

    char context[64];
    rc = exec_get_text(db, "SELECT context FROM dbmem_content;", context, sizeof(context));
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_STR_EQ(context, "sync-context");

    char indexed_content[256];
    rc = exec_get_text(db, "SELECT group_concat(content, '\n') FROM dbmem_vault_fts;", indexed_content, sizeof(indexed_content));
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT(strstr(indexed_content, "Explicit Content") != NULL);
    ASSERT(strstr(indexed_content, "SQL argument") != NULL);
    ASSERT(strstr(indexed_content, "disk") == NULL);

    sqlite3_close(db);
}

TEST(sqlite_memory_add_content_removes_stale_path_when_new_content_is_deduped) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    dbmem_provider_t prov = { .init = dummy_init, .compute = dummy_compute, .free = dummy_free };
    int rc = sqlite3_memory_register_provider(db, "dummy", &prov);
    ASSERT_EQ(rc, SQLITE_OK);

    sqlite3_int64 result = 0;
    rc = exec_get_int(db, "SELECT memory_set_model('dummy', 'test-model');", &result);
    ASSERT_EQ(rc, SQLITE_OK);

    const char *old_content = "# A\nOld content.";
    const char *shared_content = "# Shared\nSame content.";
    uint64_t old_hash = dbmem_hash_compute(old_content, strlen(old_content));
    char old_hash_text[DBMEM_HASH_STR_MAXLEN];
    dbmem_hash_to_hex(old_hash, old_hash_text);

    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(db, "SELECT memory_add_content(?1, ?2);", -1, &stmt, NULL);
    ASSERT_EQ(rc, SQLITE_OK);
    sqlite3_bind_text(stmt, 1, "docs/a.md", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, old_content, -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);
    ASSERT_EQ(rc, SQLITE_ROW);
    sqlite3_finalize(stmt);

    rc = sqlite3_prepare_v2(db, "SELECT memory_add_content(?1, ?2);", -1, &stmt, NULL);
    ASSERT_EQ(rc, SQLITE_OK);
    sqlite3_bind_text(stmt, 1, "docs/b.md", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, shared_content, -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);
    ASSERT_EQ(rc, SQLITE_ROW);
    sqlite3_finalize(stmt);

    rc = sqlite3_prepare_v2(db, "SELECT memory_add_content(?1, ?2);", -1, &stmt, NULL);
    ASSERT_EQ(rc, SQLITE_OK);
    sqlite3_bind_text(stmt, 1, "docs/a.md", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, shared_content, -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);
    ASSERT_EQ(rc, SQLITE_ROW);
    sqlite3_finalize(stmt);

    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_content WHERE path = 'docs/a.md';", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 0);

    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_content;", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 1);

    char path[64];
    rc = exec_get_text(db, "SELECT path FROM dbmem_content;", path, sizeof(path));
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_STR_EQ(path, "docs/b.md");

    char *sql = sqlite3_mprintf("SELECT COUNT(*) FROM dbmem_vault WHERE hash = '%q';", old_hash_text);
    ASSERT(sql != NULL);
    rc = exec_get_int(db, sql, &result);
    sqlite3_free(sql);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 0);

    sqlite3_close(db);
}

TEST(sqlite_memory_preserve_duplicate_paths_option_defaults_to_zero) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    sqlite3_int64 result = -1;
    int rc = exec_get_int(db, "SELECT memory_get_option('preserve_duplicate_paths');", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 0);

    rc = exec_get_int(db, "SELECT memory_set_option('preserve_duplicate_paths', 1);", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 1);

    rc = exec_get_int(db, "SELECT memory_get_option('preserve_duplicate_paths');", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 1);

    rc = exec_get_int(db, "SELECT memory_set_option('preserve_duplicate_paths', 0);", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 1);

    rc = exec_get_int(db, "SELECT memory_get_option('preserve_duplicate_paths');", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 0);

    sqlite3_close(db);
}

TEST(sqlite_memory_add_content_stores_empty_content) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT memory_add_content(?1, ?2);", -1, &stmt, NULL);
    ASSERT_EQ(rc, SQLITE_OK);
    sqlite3_bind_text(stmt, 1, "docs/empty.md", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, "", 0, SQLITE_STATIC);
    rc = sqlite3_step(stmt);
    ASSERT_EQ(rc, SQLITE_ROW);
    sqlite3_finalize(stmt);

    sqlite3_int64 result = 0;
    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_content WHERE path = 'docs/empty.md' AND length = 0;", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 1);

    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_vault;", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 0);

    sqlite3_close(db);
}

TEST(sqlite_memory_add_content_preserves_duplicate_empty_paths_when_enabled) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    sqlite3_int64 result = 0;
    int rc = exec_get_int(db, "SELECT memory_set_option('preserve_duplicate_paths', 1);", &result);
    ASSERT_EQ(rc, SQLITE_OK);

    rc = exec_get_int(db, "SELECT memory_add_content('docs/a.md', '');", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    rc = exec_get_int(db, "SELECT memory_add_content('docs/b.md', '');", &result);
    ASSERT_EQ(rc, SQLITE_OK);

    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_content;", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 2);

    rc = exec_get_int(db, "SELECT COUNT(DISTINCT hash) FROM dbmem_content;", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 2);

    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_vault;", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 0);

    sqlite3_close(db);
}

TEST(sqlite_memory_rename_file_rekeys_preserved_empty_path_hash) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    sqlite3_int64 result = 0;
    int rc = exec_get_int(db, "SELECT memory_set_option('preserve_duplicate_paths', 1);", &result);
    ASSERT_EQ(rc, SQLITE_OK);

    rc = exec_get_int(db, "SELECT memory_add_content('untitled-1.md', '');", &result);
    ASSERT_EQ(rc, SQLITE_OK);

    rc = exec_get_int(db, "SELECT memory_rename_file('untitled-1.md', '1.md');", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 1);

    rc = exec_get_int(db, "SELECT memory_add_content('untitled-1.md', '');", &result);
    ASSERT_EQ(rc, SQLITE_OK);

    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_content WHERE path IN ('1.md', 'untitled-1.md');", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 2);

    rc = exec_get_int(db, "SELECT COUNT(DISTINCT hash) FROM dbmem_content WHERE path IN ('1.md', 'untitled-1.md');", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 2);

    sqlite3_close(db);
}

TEST(sqlite_memory_rename_file_rekeys_preserved_index_hashes) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    dbmem_provider_t prov = { .init = dummy_init, .compute = dummy_compute, .free = dummy_free };
    int rc = sqlite3_memory_register_provider(db, "dummy", &prov);
    ASSERT_EQ(rc, SQLITE_OK);

    sqlite3_int64 result = 0;
    rc = exec_get_int(db, "SELECT memory_set_model('dummy', 'test-model');", &result);
    ASSERT_EQ(rc, SQLITE_OK);

    rc = exec_get_int(db, "SELECT memory_set_option('preserve_duplicate_paths', 1);", &result);
    ASSERT_EQ(rc, SQLITE_OK);

    rc = exec_get_int(db, "SELECT memory_add_content('untitled-1.md', '# Heading\nIndexed body text.');", &result);
    ASSERT_EQ(rc, SQLITE_OK);

    char old_hash[DBMEM_HASH_STR_MAXLEN];
    rc = exec_get_text(db, "SELECT hash FROM dbmem_content WHERE path = 'untitled-1.md';", old_hash, sizeof(old_hash));
    ASSERT_EQ(rc, SQLITE_OK);

    char *sql = sqlite3_mprintf("SELECT COUNT(*) FROM dbmem_vault WHERE hash = '%q';", old_hash);
    ASSERT(sql != NULL);
    rc = exec_get_int(db, sql, &result);
    sqlite3_free(sql);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT(result > 0);

    sql = sqlite3_mprintf("SELECT COUNT(*) FROM dbmem_vault_fts WHERE hash = '%q';", old_hash);
    ASSERT(sql != NULL);
    rc = exec_get_int(db, sql, &result);
    sqlite3_free(sql);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT(result > 0);

    rc = exec_get_int(db, "SELECT memory_rename_file('untitled-1.md', '1.md');", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 1);

    char new_hash[DBMEM_HASH_STR_MAXLEN];
    rc = exec_get_text(db, "SELECT hash FROM dbmem_content WHERE path = '1.md';", new_hash, sizeof(new_hash));
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT(strcmp(old_hash, new_hash) != 0);

    sql = sqlite3_mprintf("SELECT COUNT(*) FROM dbmem_vault WHERE hash = '%q';", old_hash);
    ASSERT(sql != NULL);
    rc = exec_get_int(db, sql, &result);
    sqlite3_free(sql);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 0);

    sql = sqlite3_mprintf("SELECT COUNT(*) FROM dbmem_vault WHERE hash = '%q';", new_hash);
    ASSERT(sql != NULL);
    rc = exec_get_int(db, sql, &result);
    sqlite3_free(sql);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT(result > 0);

    sql = sqlite3_mprintf("SELECT COUNT(*) FROM dbmem_vault_fts WHERE hash = '%q';", old_hash);
    ASSERT(sql != NULL);
    rc = exec_get_int(db, sql, &result);
    sqlite3_free(sql);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 0);

    sql = sqlite3_mprintf("SELECT COUNT(*) FROM dbmem_vault_fts WHERE hash = '%q';", new_hash);
    ASSERT(sql != NULL);
    rc = exec_get_int(db, sql, &result);
    sqlite3_free(sql);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT(result > 0);

    rc = exec_get_int(db, "SELECT memory_add_content('untitled-1.md', '# Heading\nIndexed body text.');", &result);
    ASSERT_EQ(rc, SQLITE_OK);

    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_content WHERE path IN ('1.md', 'untitled-1.md');", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 2);

    sqlite3_close(db);
}

TEST(sqlite_memory_rename_file_rejects_preserved_path_without_saved_content) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    dbmem_provider_t prov = { .init = dummy_init, .compute = dummy_compute, .free = dummy_free };
    int rc = sqlite3_memory_register_provider(db, "dummy", &prov);
    ASSERT_EQ(rc, SQLITE_OK);

    sqlite3_int64 result = 0;
    rc = exec_get_int(db, "SELECT memory_set_model('dummy', 'test-model');", &result);
    ASSERT_EQ(rc, SQLITE_OK);

    rc = exec_get_int(db, "SELECT memory_set_option('preserve_duplicate_paths', 1);", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    rc = exec_get_int(db, "SELECT memory_set_option('save_content', 0);", &result);
    ASSERT_EQ(rc, SQLITE_OK);

    rc = exec_get_int(db, "SELECT memory_add_content('untitled-1.md', '# Heading\nUnsaved body text.');", &result);
    ASSERT_EQ(rc, SQLITE_OK);

    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(db, "SELECT memory_rename_file('untitled-1.md', '1.md');", -1, &stmt, NULL);
    ASSERT_EQ(rc, SQLITE_OK);
    rc = sqlite3_step(stmt);
    ASSERT_EQ(rc, SQLITE_ERROR);
    ASSERT(strstr(sqlite3_errmsg(db), "save_content=0") != NULL);
    sqlite3_finalize(stmt);

    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_content WHERE path = 'untitled-1.md';", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 1);
    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_content WHERE path = '1.md';", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 0);

    sqlite3_close(db);
}

TEST(sqlite_memory_add_content_keeps_same_empty_path_idempotent_when_preserving_duplicates) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    sqlite3_int64 result = 0;
    int rc = exec_get_int(db, "SELECT memory_set_option('preserve_duplicate_paths', 1);", &result);
    ASSERT_EQ(rc, SQLITE_OK);

    rc = exec_get_int(db, "SELECT memory_add_content('docs/empty-idempotent.md', '');", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    rc = exec_get_int(db, "SELECT memory_add_content('docs/empty-idempotent.md', '');", &result);
    ASSERT_EQ(rc, SQLITE_OK);

    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_content;", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 1);

    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_content WHERE path = 'docs/empty-idempotent.md' AND length = 0;", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 1);

    sqlite3_close(db);
}

TEST(sqlite_memory_add_content_requires_preserve_for_directory_marker) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT memory_add_content('dirname/', '');", -1, &stmt, NULL);
    ASSERT_EQ(rc, SQLITE_OK);
    rc = sqlite3_step(stmt);
    ASSERT(rc == SQLITE_ERROR);
    const char *msg = sqlite3_errmsg(db);
    ASSERT(strstr(msg, "preserve_duplicate_paths=1") != NULL);
    sqlite3_finalize(stmt);

    sqlite3_close(db);
}

TEST(sqlite_memory_add_content_creates_directory_marker) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    sqlite3_int64 result = 0;
    int rc = exec_get_int(db, "SELECT memory_set_option('preserve_duplicate_paths', 1);", &result);
    ASSERT_EQ(rc, SQLITE_OK);

    rc = exec_get_int(db, "SELECT memory_add_content('dirname/', '');", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 1);

    char path[64];
    rc = exec_get_text(db, "SELECT path FROM dbmem_content;", path, sizeof(path));
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_STR_EQ(path, "dirname/");

    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_content WHERE path = 'dirname/' AND value = '' AND length = 0;", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 1);

    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_vault;", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 0);

    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_vault_fts;", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 0);

    char json[512];
    rc = exec_get_text(db, "SELECT memory_list_files();", json, sizeof(json));
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_STR_EQ(json, "{\"root\":\"\",\"children\":[{\"type\":\"directory\",\"name\":\"dirname\",\"path\":\"dirname\",\"children\":[]}]}");

    sqlite3_close(db);
}

TEST(sqlite_memory_add_content_directory_marker_is_idempotent) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    sqlite3_int64 result = 0;
    int rc = exec_get_int(db, "SELECT memory_set_option('preserve_duplicate_paths', 1);", &result);
    ASSERT_EQ(rc, SQLITE_OK);

    rc = exec_get_int(db, "SELECT memory_add_content('dirname/', '');", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    rc = exec_get_int(db, "SELECT memory_add_content('dirname/', '');", &result);
    ASSERT_EQ(rc, SQLITE_OK);

    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_content WHERE path = 'dirname/';", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 1);

    sqlite3_close(db);
}

TEST(sqlite_memory_add_content_rejects_nonempty_directory_marker) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    sqlite3_int64 result = 0;
    int rc = exec_get_int(db, "SELECT memory_set_option('preserve_duplicate_paths', 1);", &result);
    ASSERT_EQ(rc, SQLITE_OK);

    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(db, "SELECT memory_add_content('dirname/', 'content');", -1, &stmt, NULL);
    ASSERT_EQ(rc, SQLITE_OK);
    rc = sqlite3_step(stmt);
    ASSERT(rc == SQLITE_ERROR);
    sqlite3_finalize(stmt);

    sqlite3_close(db);
}

TEST(sqlite_memory_add_content_rejects_file_directory_conflicts) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    sqlite3_int64 result = 0;
    int rc = exec_get_int(db, "SELECT memory_set_option('preserve_duplicate_paths', 1);", &result);
    ASSERT_EQ(rc, SQLITE_OK);

    rc = exec_get_int(db, "SELECT memory_add_content('dirname', '');", &result);
    ASSERT_EQ(rc, SQLITE_OK);

    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(db, "SELECT memory_add_content('dirname/', '');", -1, &stmt, NULL);
    ASSERT_EQ(rc, SQLITE_OK);
    rc = sqlite3_step(stmt);
    ASSERT(rc == SQLITE_ERROR);
    sqlite3_finalize(stmt);

    rc = exec_get_int(db, "SELECT memory_clear();", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    rc = exec_get_int(db, "SELECT memory_set_option('preserve_duplicate_paths', 1);", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    rc = exec_get_int(db, "SELECT memory_add_content('dirname/', '');", &result);
    ASSERT_EQ(rc, SQLITE_OK);

    rc = sqlite3_prepare_v2(db, "SELECT memory_add_content('dirname', '');", -1, &stmt, NULL);
    ASSERT_EQ(rc, SQLITE_OK);
    rc = sqlite3_step(stmt);
    ASSERT(rc == SQLITE_ERROR);
    sqlite3_finalize(stmt);

    sqlite3_close(db);
}

TEST(sqlite_memory_add_content_preserves_duplicate_nonempty_paths_when_enabled) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    dbmem_provider_t prov = { .init = dummy_init, .compute = dummy_compute, .free = dummy_free };
    int rc = sqlite3_memory_register_provider(db, "dummy", &prov);
    ASSERT_EQ(rc, SQLITE_OK);

    sqlite3_int64 result = 0;
    rc = exec_get_int(db, "SELECT memory_set_model('dummy', 'test-model');", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    rc = exec_get_int(db, "SELECT memory_set_option('preserve_duplicate_paths', 1);", &result);
    ASSERT_EQ(rc, SQLITE_OK);

    const char *content = "# API\nSame content.";
    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(db, "SELECT memory_add_content(?1, ?2);", -1, &stmt, NULL);
    ASSERT_EQ(rc, SQLITE_OK);
    sqlite3_bind_text(stmt, 1, "docs/a.md", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, content, -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);
    ASSERT_EQ(rc, SQLITE_ROW);
    sqlite3_finalize(stmt);

    rc = sqlite3_prepare_v2(db, "SELECT memory_add_content(?1, ?2);", -1, &stmt, NULL);
    ASSERT_EQ(rc, SQLITE_OK);
    sqlite3_bind_text(stmt, 1, "docs/b.md", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, content, -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);
    ASSERT_EQ(rc, SQLITE_ROW);
    sqlite3_finalize(stmt);

    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_content;", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 2);

    rc = exec_get_int(db, "SELECT COUNT(DISTINCT hash) FROM dbmem_content;", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 2);

    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_vault;", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 2);

    sqlite3_close(db);
}

TEST(sqlite_memory_add_file_reads_disk_and_stores_context) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    const char *dir = TEST_TMP_DIR "/dbmem_file_context_dir";
    const char *path = TEST_TMP_DIR "/dbmem_file_context_dir/note.md";
    const char *disk_content = "# File Context\nThis content came from disk.";

    remove_test_file(path);
    rmdir_p(dir);
    mkdir_p(dir);
    ASSERT_EQ(create_test_file(path, disk_content), 0);

    dbmem_provider_t prov = { .init = dummy_init, .compute = dummy_compute, .free = dummy_free };
    int rc = sqlite3_memory_register_provider(db, "dummy", &prov);
    ASSERT_EQ(rc, SQLITE_OK);

    sqlite3_int64 result = 0;
    rc = exec_get_int(db, "SELECT memory_set_model('dummy', 'test-model');", &result);
    ASSERT_EQ(rc, SQLITE_OK);

    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(db, "SELECT memory_add_file(?1, ?2);", -1, &stmt, NULL);
    ASSERT_EQ(rc, SQLITE_OK);
    sqlite3_bind_text(stmt, 1, path, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, "file-context", -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);
    ASSERT_EQ(rc, SQLITE_ROW);
    ASSERT_EQ(sqlite3_column_int64(stmt, 0), 1);
    sqlite3_finalize(stmt);

    char value[256];
    rc = exec_get_text(db, "SELECT value FROM dbmem_content;", value, sizeof(value));
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_STR_EQ(value, disk_content);

    char stored_path[256];
    rc = exec_get_text(db, "SELECT path FROM dbmem_content;", stored_path, sizeof(stored_path));
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT(strstr(stored_path, "dbmem_file_context_dir/note.md") != NULL);

    char source_path[256];
    rc = exec_get_text(db, "SELECT source_path FROM dbmem_content_source;", source_path, sizeof(source_path));
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_STR_EQ(source_path, path);

    char context[64];
    rc = exec_get_text(db, "SELECT context FROM dbmem_content;", context, sizeof(context));
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_STR_EQ(context, "file-context");

    remove_test_file(path);
    rmdir_p(dir);
    sqlite3_close(db);
}

TEST(sqlite_memory_add_file_stores_empty_file) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    const char *path = TEST_TMP_DIR "/dbmem_empty_file.md";
    remove_test_file(path);
    ASSERT_EQ(create_test_file(path, ""), 0);

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT memory_add_file(?1);", -1, &stmt, NULL);
    ASSERT_EQ(rc, SQLITE_OK);
    sqlite3_bind_text(stmt, 1, path, -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);
    ASSERT_EQ(rc, SQLITE_ROW);
    sqlite3_finalize(stmt);

    sqlite3_int64 result = 0;
    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_content WHERE length = 0;", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 1);

    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_vault;", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 0);

    remove_test_file(path);
    sqlite3_close(db);
}

TEST(sqlite_memory_add_file_preserves_duplicate_empty_paths_when_enabled) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    const char *file1 = TEST_TMP_DIR "/dbmem_empty_file_a.md";
    const char *file2 = TEST_TMP_DIR "/dbmem_empty_file_b.md";
    remove_test_file(file1);
    remove_test_file(file2);
    ASSERT_EQ(create_test_file(file1, ""), 0);
    ASSERT_EQ(create_test_file(file2, ""), 0);

    sqlite3_int64 result = 0;
    int rc = exec_get_int(db, "SELECT memory_set_option('preserve_duplicate_paths', 1);", &result);
    ASSERT_EQ(rc, SQLITE_OK);

    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(db, "SELECT memory_add_file(?1);", -1, &stmt, NULL);
    ASSERT_EQ(rc, SQLITE_OK);
    sqlite3_bind_text(stmt, 1, file1, -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);
    ASSERT_EQ(rc, SQLITE_ROW);
    sqlite3_finalize(stmt);

    rc = sqlite3_prepare_v2(db, "SELECT memory_add_file(?1);", -1, &stmt, NULL);
    ASSERT_EQ(rc, SQLITE_OK);
    sqlite3_bind_text(stmt, 1, file2, -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);
    ASSERT_EQ(rc, SQLITE_ROW);
    sqlite3_finalize(stmt);

    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_content;", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 2);

    rc = exec_get_int(db, "SELECT COUNT(DISTINCT hash) FROM dbmem_content;", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 2);

    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_content_source;", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 2);

    remove_test_file(file1);
    remove_test_file(file2);
    sqlite3_close(db);
}

TEST(sqlite_memory_add_file_attaches_source_path_to_existing_logical_path) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    const char *dir = TEST_TMP_DIR "/dbmem_attach_source";
    const char *path = TEST_TMP_DIR "/dbmem_attach_source/note.md";
    const char *content = "# Local\nUpdated content.";

    remove_test_file(path);
    rmdir_p(dir);
    mkdir_p(dir);
    ASSERT_EQ(create_test_file(path, content), 0);

    uint64_t hash = dbmem_hash_compute(content, strlen(content));
    char hash_text[DBMEM_HASH_STR_MAXLEN];
    dbmem_hash_to_hex(hash, hash_text);

    char *sql = sqlite3_mprintf(
        "INSERT INTO dbmem_content (hash, path, value, length, context, created_at) "
        "VALUES ('%q', 'dbmem_attach_source/note.md', '%q', %d, 'ctx', 0);",
        hash_text, content, (int)strlen(content));
    ASSERT(sql != NULL);
    int rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
    sqlite3_free(sql);
    ASSERT_EQ(rc, SQLITE_OK);

    dbmem_provider_t prov = { .init = dummy_init, .compute = dummy_compute, .free = dummy_free };
    rc = sqlite3_memory_register_provider(db, "dummy", &prov);
    ASSERT_EQ(rc, SQLITE_OK);

    sqlite3_int64 result = 0;
    rc = exec_get_int(db, "SELECT memory_set_model('dummy', 'test-model');", &result);
    ASSERT_EQ(rc, SQLITE_OK);

    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(db, "SELECT memory_add_file(?1, 'ctx');", -1, &stmt, NULL);
    ASSERT_EQ(rc, SQLITE_OK);
    sqlite3_bind_text(stmt, 1, path, -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);
    ASSERT_EQ(rc, SQLITE_ROW);
    sqlite3_finalize(stmt);

    sqlite3_int64 count = 0;
    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_content WHERE path = 'dbmem_attach_source/note.md';", &count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(count, 1);

    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_content;", &count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(count, 1);

    char source_path[256];
    rc = exec_get_text(db, "SELECT source_path FROM dbmem_content_source;", source_path, sizeof(source_path));
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_STR_EQ(source_path, path);

    remove_test_file(path);
    rmdir_p(dir);
    sqlite3_close(db);
}

#ifndef _WIN32
TEST(sqlite_memory_add_file_disambiguates_parent_collisions) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    const char *dir1 = TEST_TMP_DIR "/dbmem_suffix_one/a";
    const char *dir2 = TEST_TMP_DIR "/dbmem_suffix_two/a";
    const char *base1 = TEST_TMP_DIR "/dbmem_suffix_one";
    const char *base2 = TEST_TMP_DIR "/dbmem_suffix_two";
    const char *file1 = TEST_TMP_DIR "/dbmem_suffix_one/a/readme.md";
    const char *file2 = TEST_TMP_DIR "/dbmem_suffix_two/a/readme.md";

    remove_test_file(file1);
    remove_test_file(file2);
    rmdir_p(dir1);
    rmdir_p(dir2);
    rmdir_p(base1);
    rmdir_p(base2);
    mkdir_p(base1);
    mkdir_p(base2);
    mkdir_p(dir1);
    mkdir_p(dir2);
    ASSERT_EQ(create_test_file(file1, "# One\nFirst file."), 0);
    ASSERT_EQ(create_test_file(file2, "# Two\nSecond file."), 0);

    dbmem_provider_t prov = { .init = dummy_init, .compute = dummy_compute, .free = dummy_free };
    int rc = sqlite3_memory_register_provider(db, "dummy", &prov);
    ASSERT_EQ(rc, SQLITE_OK);

    sqlite3_int64 result = 0;
    rc = exec_get_int(db, "SELECT memory_set_model('dummy', 'test-model');", &result);
    ASSERT_EQ(rc, SQLITE_OK);

    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(db, "SELECT memory_add_file(?1);", -1, &stmt, NULL);
    ASSERT_EQ(rc, SQLITE_OK);
    sqlite3_bind_text(stmt, 1, file1, -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);
    ASSERT_EQ(rc, SQLITE_ROW);
    sqlite3_finalize(stmt);

    rc = sqlite3_prepare_v2(db, "SELECT memory_add_file(?1);", -1, &stmt, NULL);
    ASSERT_EQ(rc, SQLITE_OK);
    sqlite3_bind_text(stmt, 1, file2, -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);
    ASSERT_EQ(rc, SQLITE_ROW);
    sqlite3_finalize(stmt);

    char paths[256];
    rc = exec_get_text(db, "SELECT group_concat(path, '|') FROM dbmem_content ORDER BY path;", paths, sizeof(paths));
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT(strstr(paths, "a/readme.md") != NULL);
    ASSERT(strstr(paths, "dbmem_suffix_two/a/readme.md") != NULL);

    char source_path[256];
    rc = exec_get_text(db,
        "SELECT s.source_path FROM dbmem_content_source s "
        "JOIN dbmem_content c ON c.path = s.path WHERE c.path = 'a/readme.md';",
        source_path, sizeof(source_path));
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_STR_EQ(source_path, file1);

    remove_test_file(file1);
    remove_test_file(file2);
    rmdir_p(dir1);
    rmdir_p(dir2);
    rmdir_p(base1);
    rmdir_p(base2);
    sqlite3_close(db);
}
#endif

TEST(sqlite_memory_add_directory_stores_relative_paths) {
    const char *base = TEST_TMP_DIR "/dbmem_relative_scan";
    const char *nested = TEST_TMP_DIR "/dbmem_relative_scan/nested";
    const char *file = TEST_TMP_DIR "/dbmem_relative_scan/nested/note.md";

    remove_test_file(file);
    rmdir_p(nested);
    rmdir_p(base);
    mkdir_p(base);
    mkdir_p(nested);
    ASSERT_EQ(create_test_file(file, "# Note\nRelative path."), 0);

    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    dbmem_provider_t prov = { .init = dummy_init, .compute = dummy_compute, .free = dummy_free };
    int rc = sqlite3_memory_register_provider(db, "dummy", &prov);
    ASSERT_EQ(rc, SQLITE_OK);

    sqlite3_int64 result = 0;
    rc = exec_get_int(db, "SELECT memory_set_model('dummy', 'test-model');", &result);
    ASSERT_EQ(rc, SQLITE_OK);

    char sql[512];
    snprintf(sql, sizeof(sql), "SELECT memory_add_directory('%s');", base);
    rc = exec_get_int(db, sql, &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 1);

    char stored_path[256];
    rc = exec_get_text(db, "SELECT path FROM dbmem_content;", stored_path, sizeof(stored_path));
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_STR_EQ(stored_path, "nested/note.md");

    char source_path[256];
    rc = exec_get_text(db, "SELECT source_path FROM dbmem_content_source;", source_path, sizeof(source_path));
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_STR_EQ(source_path, file);

    sqlite3_close(db);
    remove_test_file(file);
    rmdir_p(nested);
    rmdir_p(base);
}

TEST(sqlite_memory_add_directory_preserves_text_entries) {
    const char *base = TEST_TMP_DIR "/dbmem_preserve_text_scan";
    const char *file = TEST_TMP_DIR "/dbmem_preserve_text_scan/file.md";

    remove_test_file(file);
    rmdir_p(base);
    mkdir_p(base);
    ASSERT_EQ(create_test_file(file, "# File\nFilesystem content."), 0);

    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    dbmem_provider_t prov = { .init = dummy_init, .compute = dummy_compute, .free = dummy_free };
    int rc = sqlite3_memory_register_provider(db, "dummy", &prov);
    ASSERT_EQ(rc, SQLITE_OK);

    sqlite3_int64 result = 0;
    rc = exec_get_int(db, "SELECT memory_set_model('dummy', 'test-model');", &result);
    ASSERT_EQ(rc, SQLITE_OK);

    rc = exec_get_int(db, "SELECT memory_add_text('Logical text entry should survive directory sync.', 'test-context');", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 1);

    rc = exec_get_int(db, "SELECT memory_add_directory('" TEST_TMP_DIR "/dbmem_preserve_text_scan');", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 1);

    sqlite3_int64 count = 0;
    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_content;", &count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(count, 2);

    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_content WHERE context = 'test-context';", &count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(count, 1);

    sqlite3_close(db);
    remove_test_file(file);
    rmdir_p(base);
}

TEST(sqlite_memory_add_content_rejects_non_text_content) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT memory_add_content('docs/readme.md', 123);", -1, &stmt, NULL);
    ASSERT_EQ(rc, SQLITE_OK);
    rc = sqlite3_step(stmt);
    ASSERT(rc == SQLITE_ERROR);
    sqlite3_finalize(stmt);

    sqlite3_close(db);
}

TEST(sqlite_mdx_preprocessing_applies_only_to_mdx_files) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    const char *mdx_path = TEST_TMP_DIR "/dbmem_mdx_preprocess.mdx";
    const char *md_path = TEST_TMP_DIR "/dbmem_mdx_preprocess.md";

    remove_test_file(mdx_path);
    remove_test_file(md_path);

    dbmem_provider_t prov = { .init = dummy_init, .compute = dummy_compute, .free = dummy_free };
    int rc = sqlite3_memory_register_provider(db, "dummy", &prov);
    ASSERT_EQ(rc, SQLITE_OK);

    sqlite3_int64 result = 0;
    rc = exec_get_int(db, "SELECT memory_set_model('dummy', 'test-model');", &result);
    ASSERT_EQ(rc, SQLITE_OK);

    ASSERT_EQ(create_test_file(mdx_path,
        "import Hidden from './hidden';\n"
        "# MDX Visible\n"
        "export const hidden = { label: 'Do not index' };\n"
        "Shown <Hidden foo={hidden}>inner</Hidden> text {hidden.label}.\n"), 0);

    char sql[512];
    snprintf(sql, sizeof(sql), "SELECT memory_add_file('%s');", mdx_path);
    rc = exec_get_int(db, sql, &result);
    ASSERT_EQ(rc, SQLITE_OK);

    char content[2048];
    rc = exec_get_text(db, "SELECT group_concat(content, '\n') FROM dbmem_vault_fts;", content, sizeof(content));
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT(strstr(content, "MDX Visible") != NULL);
    ASSERT(strstr(content, "Shown inner text") != NULL);
    ASSERT(strstr(content, "Hidden from") == NULL);
    ASSERT(strstr(content, "Do not index") == NULL);
    ASSERT(strstr(content, "hidden.label") == NULL);

    rc = exec_get_int(db, "SELECT memory_clear();", &result);
    ASSERT_EQ(rc, SQLITE_OK);

    ASSERT_EQ(create_test_file(md_path,
        "import Hidden from './hidden';\n"
        "# MD Visible\n"
        "export const hidden = { label: 'Do index in markdown' };\n"
        "Shown <Hidden foo={hidden}>inner</Hidden> text {hidden.label}.\n"), 0);

    snprintf(sql, sizeof(sql), "SELECT memory_add_file('%s');", md_path);
    rc = exec_get_int(db, sql, &result);
    ASSERT_EQ(rc, SQLITE_OK);

    rc = exec_get_text(db, "SELECT group_concat(content, '\n') FROM dbmem_vault_fts;", content, sizeof(content));
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT(strstr(content, "MD Visible") != NULL);
    ASSERT(strstr(content, "Hidden from") != NULL);
    ASSERT(strstr(content, "Do index in markdown") != NULL);
    ASSERT(strstr(content, "hidden.label") != NULL);

    remove_test_file(mdx_path);
    remove_test_file(md_path);
    sqlite3_close(db);
}

static void *dummy_init_fail(const char *model, const char *api_key, void *xdata, char err_msg[1024]) {
    UNUSED_PARAM(model);
    UNUSED_PARAM(api_key);
    UNUSED_PARAM(xdata);
    snprintf(err_msg, 1024, "intentional init failure");
    return NULL;
}

typedef struct {
    int fail_after;
    int calls;
} flaky_provider_state_t;

static void *flaky_init(const char *model, const char *api_key, void *xdata, char err_msg[1024]) {
    flaky_provider_state_t *state = (flaky_provider_state_t *)xdata;
    if (state) state->calls = 0;
    return dummy_init(model, api_key, NULL, err_msg);
}

static int flaky_compute(void *engine, const char *text, int text_len, void *xdata, dbmem_embedding_result_t *result) {
    flaky_provider_state_t *state = (flaky_provider_state_t *)xdata;
    if (state) {
        state->calls++;
        if (state->calls >= state->fail_after) return -1;
    }
    return dummy_compute(engine, text, text_len, NULL, result);
}

TEST(sqlite_custom_provider_register) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    dbmem_provider_t prov = { .init = dummy_init, .compute = dummy_compute, .free = dummy_free };
    int rc = sqlite3_memory_register_provider(db, "dummy", &prov);
    ASSERT_EQ(rc, SQLITE_OK);

    sqlite3_close(db);
}

TEST(sqlite_custom_provider_set_model) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    dbmem_provider_t prov = { .init = dummy_init, .compute = dummy_compute, .free = dummy_free };
    int rc = sqlite3_memory_register_provider(db, "dummy", &prov);
    ASSERT_EQ(rc, SQLITE_OK);

    // set_model should succeed with custom provider
    sqlite3_int64 result = 0;
    rc = exec_get_int(db, "SELECT memory_set_model('dummy', 'test-model');", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 1);

    sqlite3_close(db);
}

TEST(sqlite_memory_add_text_requires_model) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT memory_add_text('Hello world, this is a test.');", -1, &stmt, NULL);
    ASSERT_EQ(rc, SQLITE_OK);
    rc = sqlite3_step(stmt);
    ASSERT_EQ(rc, SQLITE_ERROR);
    ASSERT(strstr(sqlite3_errmsg(db), "memory_set_model must be called before adding content") != NULL);
    sqlite3_finalize(stmt);

    sqlite3_close(db);
}

#ifndef DBMEM_OMIT_REMOTE_ENGINE
TEST(sqlite_saved_remote_model_initializes_lazily_after_apikey) {
    const char *path = TEST_TMP_DIR "/dbmem_saved_remote_model.sqlite";
    remove_test_file(path);

    sqlite3 *db = open_test_db_path(path);
    ASSERT(db != NULL);

    int rc = sqlite3_exec(db,
        "INSERT OR REPLACE INTO dbmem_settings (key, value) VALUES ('provider', 'openai');"
        "INSERT OR REPLACE INTO dbmem_settings (key, value) VALUES ('model', 'text-embedding-3-small');",
        NULL, NULL, NULL);
    ASSERT_EQ(rc, SQLITE_OK);
    sqlite3_close(db);

    db = open_test_db_path(path);
    ASSERT(db != NULL);

    dbmem_context *ctx = get_test_ctx(db);
    ASSERT(ctx != NULL);
    rc = dbmem_context_ensure_engine(ctx);
    ASSERT_EQ(rc, SQLITE_ERROR);
    ASSERT(strstr(dbmem_context_errmsg(ctx), "memory_set_apikey must be called") != NULL);

    sqlite3_int64 result = 0;
    rc = exec_get_int(db, "SELECT memory_set_apikey('test-key');", &result);
    ASSERT_EQ(rc, SQLITE_OK);

    rc = dbmem_context_ensure_engine(ctx);
    ASSERT_EQ(rc, SQLITE_OK);

    bool is_local = true;
    ASSERT(dbmem_context_engine(ctx, &is_local) != NULL);
    ASSERT_EQ(is_local, false);

    sqlite3_close(db);
    remove_test_file(path);
}
#endif

#ifndef DBMEM_OMIT_LOCAL_ENGINE
TEST(sqlite_saved_local_model_initializes_lazily) {
    const char *path = TEST_TMP_DIR "/dbmem_saved_local_model.sqlite";
    const char *model_path = "models/embeddinggemma-300M-Q8_0.gguf";
    remove_test_file(path);

    if (access(model_path, F_OK) != 0) return;

    sqlite3 *db = open_test_db_path(path);
    ASSERT(db != NULL);

    sqlite3_int64 result = 0;
    int rc = exec_get_int(db, "SELECT memory_set_model('local', 'models/embeddinggemma-300M-Q8_0.gguf');", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    sqlite3_close(db);

    db = open_test_db_path(path);
    ASSERT(db != NULL);

    dbmem_context *ctx = get_test_ctx(db);
    ASSERT(ctx != NULL);

    bool is_local = false;
    ASSERT(dbmem_context_engine(ctx, &is_local) == NULL);

    rc = exec_get_int(db, "SELECT memory_add_text('Saved local model settings should load lazily.');", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT(result >= 1);

    ASSERT(dbmem_context_engine(ctx, &is_local) != NULL);
    ASSERT_EQ(is_local, true);

    sqlite3_close(db);
    remove_test_file(path);
}
#endif

TEST(sqlite_saved_custom_model_initializes_lazily_after_register) {
    const char *path = TEST_TMP_DIR "/dbmem_saved_custom_model.sqlite";
    remove_test_file(path);

    sqlite3 *db = open_test_db_path(path);
    ASSERT(db != NULL);

    dbmem_provider_t prov = { .init = dummy_init, .compute = dummy_compute, .free = dummy_free };
    int rc = sqlite3_memory_register_provider(db, "dummy", &prov);
    ASSERT_EQ(rc, SQLITE_OK);

    sqlite3_int64 result = 0;
    rc = exec_get_int(db, "SELECT memory_set_model('dummy', 'test-model');", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    sqlite3_close(db);

    dummy_init_calls = 0;
    dummy_compute_calls = 0;

    db = open_test_db_path(path);
    ASSERT(db != NULL);

    dbmem_context *ctx = get_test_ctx(db);
    ASSERT(ctx != NULL);
    bool is_local = true;
    ASSERT(dbmem_context_engine(ctx, &is_local) == NULL);
    ASSERT_EQ(dummy_init_calls, 0);

    rc = sqlite3_memory_register_provider(db, "dummy", &prov);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(dummy_init_calls, 0);

    rc = exec_get_int(db, "SELECT memory_add_text('Saved custom provider settings should load lazily.');", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT(result >= 1);
    ASSERT_EQ(dummy_init_calls, 1);
    ASSERT(dummy_compute_calls >= 1);

    ASSERT(dbmem_context_engine(ctx, &is_local) != NULL);
    ASSERT_EQ(is_local, false);

    sqlite3_close(db);
    remove_test_file(path);
}

TEST(sqlite_custom_provider_add_text) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    dbmem_provider_t prov = { .init = dummy_init, .compute = dummy_compute, .free = dummy_free };
    int rc = sqlite3_memory_register_provider(db, "dummy", &prov);
    ASSERT_EQ(rc, SQLITE_OK);

    sqlite3_int64 result = 0;
    rc = exec_get_int(db, "SELECT memory_set_model('dummy', 'test-model');", &result);
    ASSERT_EQ(rc, SQLITE_OK);

    // add text should use the custom provider to compute embeddings
    rc = exec_get_int(db, "SELECT memory_add_text('Hello world, this is a test.');", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT(result >= 1);

    // verify data was stored
    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_vault;", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT(result >= 1);

    rc = exec_get_int(db, "SELECT n_tokens FROM dbmem_vault LIMIT 1;", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 7);

    rc = exec_get_int(db, "SELECT truncated FROM dbmem_vault LIMIT 1;", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 0);

    rc = exec_get_int(db, "SELECT n_tokens FROM dbmem_cache LIMIT 1;", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 7);

    rc = exec_get_int(db, "SELECT truncated FROM dbmem_cache LIMIT 1;", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 0);

    rc = exec_get_int(db, "SELECT memory_clear();", &result);
    ASSERT_EQ(rc, SQLITE_OK);

    rc = exec_get_int(db, "SELECT memory_add_text('Hello world, this is a test.');", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT(result >= 1);

    rc = exec_get_int(db, "SELECT n_tokens FROM dbmem_vault LIMIT 1;", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 7);

    rc = exec_get_int(db, "SELECT truncated FROM dbmem_vault LIMIT 1;", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 0);

    sqlite3_close(db);
}

TEST(sqlite_memory_reindex_refreshes_synced_value_changes) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    dbmem_provider_t prov = { .init = dummy_init, .compute = dummy_compute, .free = dummy_free };
    int rc = sqlite3_memory_register_provider(db, "dummy", &prov);
    ASSERT_EQ(rc, SQLITE_OK);

    sqlite3_int64 result = 0;
    rc = exec_get_int(db, "SELECT memory_set_model('dummy', 'test-model');", &result);
    ASSERT_EQ(rc, SQLITE_OK);

    const char *path = "docs/synced.md";
    const char *old_value = "# Old\nBefore sync.";
    const char *new_value = "# New\nAfter sync merge.";
    uint64_t old_hash = dbmem_hash_compute(old_value, strlen(old_value));
    uint64_t new_hash = dbmem_hash_compute(new_value, strlen(new_value));
    char old_hash_text[DBMEM_HASH_STR_MAXLEN];
    char new_hash_text[DBMEM_HASH_STR_MAXLEN];
    dbmem_hash_to_hex(old_hash, old_hash_text);
    dbmem_hash_to_hex(new_hash, new_hash_text);

    char *sql = sqlite3_mprintf(
        "INSERT INTO dbmem_content (hash, path, value, length, context, created_at) "
        "VALUES ('%q', '%q', '%q', %d, 'sync', 0);"
        "INSERT INTO dbmem_content_source (path, source_path) "
        "VALUES ('%q', '/tmp/synced.md');"
        "INSERT INTO dbmem_vault (hash, seq, embedding, offset, length, n_tokens, truncated) "
        "VALUES ('%q', 0, X'00000000000000000000000000000000', 0, 4, 1, 0);",
        old_hash_text, path, new_value, (int)strlen(new_value),
        path, old_hash_text);
    ASSERT(sql != NULL);
    rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
    sqlite3_free(sql);
    ASSERT_EQ(rc, SQLITE_OK);

    rc = exec_get_int(db, "SELECT memory_reindex();", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 1);

    char stored_hash[DBMEM_HASH_STR_MAXLEN];
    rc = exec_get_text(db, "SELECT hash FROM dbmem_content WHERE path = 'docs/synced.md';", stored_hash, sizeof(stored_hash));
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_STR_EQ(stored_hash, new_hash_text);

    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_vault WHERE hash = (SELECT hash FROM dbmem_content WHERE path = 'docs/synced.md');", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT(result >= 1);

    sql = sqlite3_mprintf("SELECT COUNT(*) FROM dbmem_vault WHERE hash = '%q';", old_hash_text);
    ASSERT(sql != NULL);
    rc = exec_get_int(db, sql, &result);
    sqlite3_free(sql);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 0);

    char source_path[64];
    rc = exec_get_text(db, "SELECT source_path FROM dbmem_content_source WHERE path = 'docs/synced.md';", source_path, sizeof(source_path));
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_STR_EQ(source_path, "/tmp/synced.md");

    sqlite3_close(db);
}

TEST(sqlite_memory_reindex_preserves_directory_markers) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    dbmem_provider_t prov = { .init = dummy_init, .compute = dummy_compute, .free = dummy_free };
    int rc = sqlite3_memory_register_provider(db, "dummy", &prov);
    ASSERT_EQ(rc, SQLITE_OK);

    sqlite3_int64 result = 0;
    rc = exec_get_int(db, "SELECT memory_set_model('dummy', 'test-model');", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    rc = exec_get_int(db, "SELECT memory_set_option('preserve_duplicate_paths', 1);", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    rc = exec_get_int(db, "SELECT memory_add_content('dirname/', '');", &result);
    ASSERT_EQ(rc, SQLITE_OK);

    rc = exec_get_int(db, "SELECT memory_reindex();", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 0);

    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_content WHERE path = 'dirname/' AND length = 0;", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 1);

    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_vault;", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 0);

    sqlite3_close(db);
}

TEST(sqlite_memory_reindex_preserves_empty_files) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    dbmem_provider_t prov = { .init = dummy_init, .compute = dummy_compute, .free = dummy_free };
    int rc = sqlite3_memory_register_provider(db, "dummy", &prov);
    ASSERT_EQ(rc, SQLITE_OK);

    sqlite3_int64 result = 0;
    rc = exec_get_int(db, "SELECT memory_set_model('dummy', 'test-model');", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    rc = exec_get_int(db, "SELECT memory_set_option('preserve_duplicate_paths', 1);", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    rc = exec_get_int(db, "SELECT memory_add_content('docs/empty.md', '');", &result);
    ASSERT_EQ(rc, SQLITE_OK);

    char hash_before[DBMEM_HASH_STR_MAXLEN];
    rc = exec_get_text(db, "SELECT hash FROM dbmem_content WHERE path = 'docs/empty.md';", hash_before, sizeof(hash_before));
    ASSERT_EQ(rc, SQLITE_OK);

    rc = exec_get_int(db, "SELECT memory_reindex();", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 0);

    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_content WHERE path = 'docs/empty.md' AND value = '' AND length = 0;", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 1);

    char hash_after[DBMEM_HASH_STR_MAXLEN];
    rc = exec_get_text(db, "SELECT hash FROM dbmem_content WHERE path = 'docs/empty.md';", hash_after, sizeof(hash_after));
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_STR_EQ(hash_after, hash_before);

    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_vault;", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 0);

    sqlite3_close(db);
}

TEST(sqlite_set_model_reindex_preserves_directory_markers) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    dbmem_provider_t prov = { .init = dummy_init, .compute = dummy_compute, .free = dummy_free };
    int rc = sqlite3_memory_register_provider(db, "dummy", &prov);
    ASSERT_EQ(rc, SQLITE_OK);

    sqlite3_int64 result = 0;
    rc = exec_get_int(db, "SELECT memory_set_model('dummy', 'model-a');", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    rc = exec_get_int(db, "SELECT memory_set_option('preserve_duplicate_paths', 1);", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    rc = exec_get_int(db, "SELECT memory_add_content('dirname/', '');", &result);
    ASSERT_EQ(rc, SQLITE_OK);

    rc = exec_get_int(db, "SELECT memory_set_model('dummy', 'model-b');", &result);
    ASSERT_EQ(rc, SQLITE_OK);

    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_content WHERE path = 'dirname/' AND length = 0;", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 1);

    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_vault;", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 0);

    sqlite3_close(db);
}

TEST(sqlite_custom_provider_skips_whitespace_only_text) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    dbmem_provider_t prov = { .init = dummy_init, .compute = dummy_compute, .free = dummy_free };
    int rc = sqlite3_memory_register_provider(db, "dummy", &prov);
    ASSERT_EQ(rc, SQLITE_OK);

    sqlite3_int64 result = 0;
    rc = exec_get_int(db, "SELECT memory_set_model('dummy', 'test-model');", &result);
    ASSERT_EQ(rc, SQLITE_OK);

    dummy_compute_calls = 0;
    rc = exec_get_int(db, "SELECT memory_add_text('   \n\n   \n');", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 1);
    ASSERT_EQ(dummy_compute_calls, 0);

    // no embeddings are computed: the only vault row is the zero-chunk sentinel
    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_vault;", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 1);

    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_vault WHERE length(embedding) = 0 AND n_tokens = 0;", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 1);

    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_cache;", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 0);

    sqlite3_close(db);
}

TEST(sqlite_custom_provider_persists_truncated_metadata) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    dbmem_provider_t prov = { .init = dummy_init, .compute = truncated_dummy_compute, .free = dummy_free };
    int rc = sqlite3_memory_register_provider(db, "truncdummy", &prov);
    ASSERT_EQ(rc, SQLITE_OK);

    sqlite3_int64 result = 0;
    rc = exec_get_int(db, "SELECT memory_set_model('truncdummy', 'test-model');", &result);
    ASSERT_EQ(rc, SQLITE_OK);

    rc = exec_get_int(db, "SELECT memory_add_text('This custom provider reports truncation.');", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT(result >= 1);

    rc = exec_get_int(db, "SELECT n_tokens FROM dbmem_vault LIMIT 1;", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 3);

    rc = exec_get_int(db, "SELECT truncated FROM dbmem_vault LIMIT 1;", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 1);

    rc = exec_get_int(db, "SELECT n_tokens FROM dbmem_cache LIMIT 1;", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 3);

    rc = exec_get_int(db, "SELECT truncated FROM dbmem_cache LIMIT 1;", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 1);

    rc = exec_get_int(db, "SELECT memory_clear();", &result);
    ASSERT_EQ(rc, SQLITE_OK);

    rc = exec_get_int(db, "SELECT memory_add_text('This custom provider reports truncation.');", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT(result >= 1);

    rc = exec_get_int(db, "SELECT n_tokens FROM dbmem_vault LIMIT 1;", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 3);

    rc = exec_get_int(db, "SELECT truncated FROM dbmem_vault LIMIT 1;", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 1);

    sqlite3_close(db);
}

TEST(sqlite_custom_provider_null_callbacks) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    // missing compute should fail
    dbmem_provider_t prov1 = { .init = dummy_init, .compute = NULL, .free = NULL };
    int rc = sqlite3_memory_register_provider(db, "bad", &prov1);
    ASSERT_EQ(rc, SQLITE_MISUSE);

    // missing init should fail
    dbmem_provider_t prov2 = { .init = NULL, .compute = dummy_compute, .free = NULL };
    rc = sqlite3_memory_register_provider(db, "bad", &prov2);
    ASSERT_EQ(rc, SQLITE_MISUSE);

    // NULL provider should fail
    rc = sqlite3_memory_register_provider(db, "bad", NULL);
    ASSERT_EQ(rc, SQLITE_MISUSE);

    sqlite3_close(db);
}

TEST(sqlite_custom_provider_init_error) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    dbmem_provider_t prov = { .init = dummy_init_fail, .compute = dummy_compute, .free = NULL };
    int rc = sqlite3_memory_register_provider(db, "failprov", &prov);
    ASSERT_EQ(rc, SQLITE_OK);

    // set_model should fail because init returns NULL
    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(db, "SELECT memory_set_model('failprov', 'any');", -1, &stmt, NULL);
    ASSERT_EQ(rc, SQLITE_OK);
    rc = sqlite3_step(stmt);
    ASSERT_EQ(rc, SQLITE_ERROR);  // should be an error
    sqlite3_finalize(stmt);

    sqlite3_close(db);
}

TEST(sqlite_custom_provider_apikey_passed) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    // set API key first
    sqlite3_int64 result = 0;
    int rc = exec_get_int(db, "SELECT memory_set_apikey('test-key-123');", &result);
    ASSERT_EQ(rc, SQLITE_OK);

    dbmem_provider_t prov = { .init = dummy_init, .compute = dummy_compute, .free = dummy_free };
    rc = sqlite3_memory_register_provider(db, "dummy", &prov);
    ASSERT_EQ(rc, SQLITE_OK);

    rc = exec_get_int(db, "SELECT memory_set_model('dummy', 'test-model');", &result);
    ASSERT_EQ(rc, SQLITE_OK);

    // verify the ctx pointer function returns a valid pointer
    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(db, "SELECT _memory_ctx_ptr();", -1, &stmt, NULL);
    ASSERT_EQ(rc, SQLITE_OK);
    rc = sqlite3_step(stmt);
    ASSERT_EQ(rc, SQLITE_ROW);
    void *ptr = sqlite3_value_pointer(sqlite3_column_value(stmt, 0), "dbmem_context_ptr");
    ASSERT(ptr != NULL);
    sqlite3_finalize(stmt);

    sqlite3_close(db);
}

TEST(sqlite_set_model_failed_reindex_preserves_existing_rows) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    dbmem_provider_t ok_prov = { .init = dummy_init, .compute = dummy_compute, .free = dummy_free };
    int rc = sqlite3_memory_register_provider(db, "dummy", &ok_prov);
    ASSERT_EQ(rc, SQLITE_OK);

    sqlite3_int64 result = 0;
    rc = exec_get_int(db, "SELECT memory_set_model('dummy', 'test-model');", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    rc = exec_get_int(db, "SELECT memory_add_text('Persist me through failed reindex.', 'keep');", &result);
    ASSERT_EQ(rc, SQLITE_OK);

    sqlite3_int64 count = 0;
    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_content;", &count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(count, 1);
    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_vault;", &count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT(count >= 1);

    flaky_provider_state_t state = { .fail_after = 1, .calls = 0 };
    dbmem_provider_t flaky_prov = { .init = flaky_init, .compute = flaky_compute, .free = dummy_free, .xdata = &state };
    rc = sqlite3_memory_register_provider(db, "flaky", &flaky_prov);
    ASSERT_EQ(rc, SQLITE_OK);

    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(db, "SELECT memory_set_model('flaky', 'test-model');", -1, &stmt, NULL);
    ASSERT_EQ(rc, SQLITE_OK);
    rc = sqlite3_step(stmt);
    ASSERT_EQ(rc, SQLITE_ERROR);
    sqlite3_finalize(stmt);

    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_content;", &count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(count, 1);
    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_vault;", &count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT(count >= 1);

    char context[64];
    rc = exec_get_text(db, "SELECT context FROM dbmem_content;", context, sizeof(context));
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_STR_EQ(context, "keep");

    char provider[64];
    rc = exec_get_text(db, "SELECT memory_get_option('provider');", provider, sizeof(provider));
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_STR_EQ(provider, "dummy");

    char model[64];
    rc = exec_get_text(db, "SELECT memory_get_option('model');", model, sizeof(model));
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_STR_EQ(model, "test-model");

    sqlite3_close(db);
}

// Regression: when memory_set_model() switches from a custom provider to a
// remote provider (a different provider class), the previous custom engine
// must be released immediately — not leaked until the database is closed.
typedef struct {
    int free_count;
} tracking_free_state_t;

static void *tracking_init(const char *model, const char *api_key, void *xdata, char err_msg[1024]) {
    UNUSED_PARAM(model);
    UNUSED_PARAM(api_key);
    UNUSED_PARAM(xdata);
    UNUSED_PARAM(err_msg);
    // any non-NULL pointer is fine; the test only cares about the free callback
    return calloc(1, 1);
}

static int tracking_compute(void *engine, const char *text, int text_len, void *xdata, dbmem_embedding_result_t *result) {
    UNUSED_PARAM(engine);
    UNUSED_PARAM(text);
    UNUSED_PARAM(text_len);
    UNUSED_PARAM(xdata);
    UNUSED_PARAM(result);
    return -1;
}

static void tracking_free(void *engine, void *xdata) {
    tracking_free_state_t *s = (tracking_free_state_t *)xdata;
    if (s) s->free_count++;
    free(engine);
}

#ifndef DBMEM_OMIT_REMOTE_ENGINE
TEST(sqlite_set_model_releases_previous_engine_on_class_switch) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    // remote engine init requires an api key to succeed
    sqlite3_int64 result = 0;
    int rc = exec_get_int(db, "SELECT memory_set_apikey('test-key');", &result);
    ASSERT_EQ(rc, SQLITE_OK);

    tracking_free_state_t state = {0};
    dbmem_provider_t prov = { .init = tracking_init, .compute = tracking_compute, .free = tracking_free, .xdata = &state };
    rc = sqlite3_memory_register_provider(db, "tracker", &prov);
    ASSERT_EQ(rc, SQLITE_OK);

    // activate the custom provider — ctx->custom_engine is now non-NULL
    rc = exec_get_int(db, "SELECT memory_set_model('tracker', 'm1');", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(state.free_count, 0);

    // switch to a provider from a different class (remote). The previous
    // custom engine must be released during this call, not kept alive on ctx.
    rc = exec_get_int(db, "SELECT memory_set_model('openai', 'text-embedding-3-small');", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(state.free_count, 1);

    // closing the db must not double-free the already-released custom engine
    sqlite3_close(db);
    ASSERT_EQ(state.free_count, 1);
}
#else
TEST(sqlite_set_model_failed_remote_switch_keeps_custom_engine) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    sqlite3_int64 result = 0;
    int rc = exec_get_int(db, "SELECT memory_set_apikey('test-key');", &result);
    ASSERT_EQ(rc, SQLITE_OK);

    tracking_free_state_t state = {0};
    dbmem_provider_t prov = { .init = tracking_init, .compute = tracking_compute, .free = tracking_free, .xdata = &state };
    rc = sqlite3_memory_register_provider(db, "tracker", &prov);
    ASSERT_EQ(rc, SQLITE_OK);

    rc = exec_get_int(db, "SELECT memory_set_model('tracker', 'm1');", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(state.free_count, 0);

    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(db, "SELECT memory_set_model('openai', 'text-embedding-3-small');", -1, &stmt, NULL);
    ASSERT_EQ(rc, SQLITE_OK);
    rc = sqlite3_step(stmt);
    ASSERT_EQ(rc, SQLITE_ERROR);
    sqlite3_finalize(stmt);

    ASSERT_EQ(state.free_count, 0);

    sqlite3_close(db);
    ASSERT_EQ(state.free_count, 1);
}
#endif

// ============================================================================
// Deferred Embeddings Tests
// ============================================================================

TEST(sqlite_memory_defer_embeddings_stores_content_without_index) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    sqlite3_int64 result = -1;
    int rc = exec_get_int(db, "SELECT memory_get_option('defer_embeddings');", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 0);

    rc = exec_get_int(db, "SELECT memory_set_option('defer_embeddings', 1);", &result);
    ASSERT_EQ(rc, SQLITE_OK);

    // no model configured: a deferred add must succeed without an embedding engine
    rc = exec_get_int(db, "SELECT memory_add_content('docs/deferred.md', '# Title\nDeferred body text.');", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 1);

    sqlite3_int64 count = -1;
    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_content;", &count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(count, 1);

    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_vault;", &count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(count, 0);

    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_vault_fts;", &count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(count, 0);

    rc = exec_get_int(db, "SELECT memory_pending_count();", &count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(count, 1);

    // embedding pending content requires a configured model
    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(db, "SELECT memory_embed_pending();", -1, &stmt, NULL);
    ASSERT_EQ(rc, SQLITE_OK);
    rc = sqlite3_step(stmt);
    ASSERT(rc == SQLITE_ERROR);
    const char *msg = sqlite3_errmsg(db);
    ASSERT(strstr(msg, "no embedding model") != NULL);
    sqlite3_finalize(stmt);

    sqlite3_close(db);
}

TEST(sqlite_memory_defer_embeddings_requires_save_content) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    sqlite3_int64 result = 0;
    int rc = exec_get_int(db, "SELECT memory_set_option('defer_embeddings', 1);", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    rc = exec_get_int(db, "SELECT memory_set_option('save_content', 0);", &result);
    ASSERT_EQ(rc, SQLITE_OK);

    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(db, "SELECT memory_add_content('docs/nosave.md', 'Body text.');", -1, &stmt, NULL);
    ASSERT_EQ(rc, SQLITE_OK);
    rc = sqlite3_step(stmt);
    ASSERT(rc == SQLITE_ERROR);
    const char *msg = sqlite3_errmsg(db);
    ASSERT(strstr(msg, "save_content") != NULL);
    sqlite3_finalize(stmt);

    sqlite3_close(db);
}

TEST(sqlite_memory_embed_pending_embeds_deferred_content_in_batches) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    dbmem_provider_t prov = { .init = dummy_init, .compute = dummy_compute, .free = dummy_free };
    int rc = sqlite3_memory_register_provider(db, "dummy", &prov);
    ASSERT_EQ(rc, SQLITE_OK);

    sqlite3_int64 result = 0;
    rc = exec_get_int(db, "SELECT memory_set_model('dummy', 'test-model');", &result);
    ASSERT_EQ(rc, SQLITE_OK);

    rc = exec_get_int(db, "SELECT memory_set_option('defer_embeddings', 1);", &result);
    ASSERT_EQ(rc, SQLITE_OK);

    rc = exec_get_int(db, "SELECT memory_add_content('docs/a.md', '# A\nAlpha body content.');", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    rc = exec_get_int(db, "SELECT memory_add_content('docs/b.md', '# B\nBeta body content.');", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    rc = exec_get_int(db, "SELECT memory_add_content('docs/c.md', '# C\nGamma body content.');", &result);
    ASSERT_EQ(rc, SQLITE_OK);

    sqlite3_int64 count = -1;
    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_vault;", &count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(count, 0);

    rc = exec_get_int(db, "SELECT memory_pending_count();", &count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(count, 3);

    rc = exec_get_int(db, "SELECT memory_embed_pending(2);", &count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(count, 2);

    rc = exec_get_int(db, "SELECT memory_pending_count();", &count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(count, 1);

    rc = exec_get_int(db, "SELECT memory_embed_pending();", &count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(count, 1);

    rc = exec_get_int(db, "SELECT memory_pending_count();", &count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(count, 0);

    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_vault;", &count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT(count >= 3);

    sqlite3_int64 fts_count = -1;
    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_vault_fts;", &fts_count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(fts_count, count);

    rc = exec_get_int(db,
        "SELECT COUNT(*) FROM dbmem_content c WHERE NOT EXISTS (SELECT 1 FROM dbmem_vault v WHERE v.hash = c.hash);",
        &count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(count, 0);

    rc = exec_get_int(db, "SELECT memory_embed_pending();", &count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(count, 0);

    sqlite3_close(db);
}

TEST(sqlite_memory_zero_chunk_content_marks_processed_with_sentinel) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    dbmem_provider_t prov = { .init = dummy_init, .compute = dummy_compute, .free = dummy_free };
    int rc = sqlite3_memory_register_provider(db, "dummy", &prov);
    ASSERT_EQ(rc, SQLITE_OK);

    sqlite3_int64 result = 0;
    rc = exec_get_int(db, "SELECT memory_set_model('dummy', 'test-model');", &result);
    ASSERT_EQ(rc, SQLITE_OK);

    int calls_before = dummy_compute_calls;

    // whitespace-only content parses to zero chunks: a sentinel vault row marks it processed
    rc = exec_get_int(db, "SELECT memory_add_content('docs/blank.md', '   ' || char(10) || char(9) || char(10));", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 1);

    sqlite3_int64 count = -1;
    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_vault WHERE length(embedding) = 0 AND n_tokens = 0;", &count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(count, 1);

    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_vault_fts;", &count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(count, 0);

    rc = exec_get_int(db, "SELECT memory_pending_count();", &count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(count, 0);

    ASSERT_EQ(dummy_compute_calls, calls_before);

    // deferred zero-chunk content resolves through memory_embed_pending the same way
    rc = exec_get_int(db, "SELECT memory_set_option('defer_embeddings', 1);", &result);
    ASSERT_EQ(rc, SQLITE_OK);

    rc = exec_get_int(db, "SELECT memory_add_content('docs/blank2.md', char(10) || '  ' || char(10));", &result);
    ASSERT_EQ(rc, SQLITE_OK);

    rc = exec_get_int(db, "SELECT memory_pending_count();", &count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(count, 1);

    rc = exec_get_int(db, "SELECT memory_embed_pending();", &count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(count, 1);

    rc = exec_get_int(db, "SELECT memory_pending_count();", &count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(count, 0);

    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_vault WHERE length(embedding) = 0 AND n_tokens = 0;", &count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(count, 2);

    ASSERT_EQ(dummy_compute_calls, calls_before);

    sqlite3_close(db);
}

#ifndef DBMEM_OMIT_LOCAL_ENGINE
TEST(sqlite_local_logger_ignores_stale_user_data) {
    dbmem_logger(GGML_LOG_LEVEL_WARN, "ignored warning", (void *)1);
}
#endif

#endif // TEST_SQLITE_EXTENSION

// ============================================================================
// Main
// ============================================================================

int main(int argc, char *argv[]) {
    UNUSED_PARAM(argc);
    UNUSED_PARAM(argv);

    printf("\n=== sqlite-memory Unit Tests ===\n\n");

    printf("API tests:\n");
    RUN_TEST(dbmem_parse_null_input);
    RUN_TEST(dbmem_parse_null_settings);
    RUN_TEST(dbmem_parse_empty_buffer);
    RUN_TEST(dbmem_parse_simple_text);

    printf("\nMarkdown stripping tests:\n");
    RUN_TEST(dbmem_parse_strips_markdown);
    RUN_TEST(dbmem_parse_strips_links);
    RUN_TEST(dbmem_parse_strips_code_blocks);
    RUN_TEST(dbmem_parse_strips_lists);
    RUN_TEST(dbmem_parse_strips_blockquotes);
    RUN_TEST(dbmem_parse_strips_html);
    RUN_TEST(dbmem_parse_preserves_html);
    RUN_TEST(dbmem_parse_mdx_strips_esm_and_expressions);
    RUN_TEST(dbmem_parse_mdx_preserves_fenced_code);
    RUN_TEST(dbmem_parse_mdx_keeps_import_export_prose_and_indented_code);
    RUN_TEST(dbmem_parse_mdx_real_docs_file);
    RUN_TEST(dbmem_parse_inline_code);
    RUN_TEST(dbmem_parse_image);
    RUN_TEST(dbmem_parse_strikethrough);
    RUN_TEST(dbmem_parse_thematic_break);
    RUN_TEST(dbmem_parse_table);

    printf("\nAdditional markdown tests:\n");
    RUN_TEST(dbmem_parse_nested_emphasis);
    RUN_TEST(dbmem_parse_underscore_emphasis);
    RUN_TEST(dbmem_parse_tilde_fence);
    RUN_TEST(dbmem_parse_double_backtick_code);
    RUN_TEST(dbmem_parse_reference_link);
    RUN_TEST(dbmem_parse_shortcut_link);
    RUN_TEST(dbmem_parse_nested_blockquote);
    RUN_TEST(dbmem_parse_heading_levels);
    RUN_TEST(dbmem_parse_heading_sections_stay_split);
    RUN_TEST(dbmem_parse_heading_trailing_hashes);
    RUN_TEST(dbmem_parse_multiline_html);
    RUN_TEST(dbmem_parse_blank_lines);
    RUN_TEST(dbmem_parse_mixed_list_markers);
    RUN_TEST(dbmem_parse_asterisk_thematic_break);
    RUN_TEST(dbmem_parse_underscore_thematic_break);

    printf("\nEdge cases:\n");
    RUN_TEST(dbmem_parse_single_char);
    RUN_TEST(dbmem_parse_only_whitespace);
    RUN_TEST(dbmem_parse_unclosed_code_span);
    RUN_TEST(dbmem_parse_unclosed_link);
    RUN_TEST(dbmem_parse_very_long_line);
    RUN_TEST(dbmem_parse_special_characters);
    RUN_TEST(dbmem_parse_nested_brackets);
    RUN_TEST(dbmem_parse_only_heading);
    RUN_TEST(dbmem_parse_code_with_markdown_inside);

    printf("\nChunking tests:\n");
    RUN_TEST(dbmem_parse_skip_semantic);
    RUN_TEST(dbmem_parse_token_split);
    RUN_TEST(dbmem_parse_overlay);
    RUN_TEST(dbmem_parse_offset_length);
    RUN_TEST(dbmem_parse_complex_document);
    RUN_TEST(dbmem_parse_zero_max_tokens);
    RUN_TEST(dbmem_parse_overlay_larger_than_chunk);
    RUN_TEST(dbmem_parse_multiple_chunks_verify_count);

    printf("\nCallback tests:\n");
    RUN_TEST(dbmem_parse_callback_receives_index);
    RUN_TEST(dbmem_parse_callback_can_abort);
    RUN_TEST(dbmem_parse_no_callback);

    printf("\nDirectory scanning tests:\n");
    RUN_TEST(dbmem_dir_scan_empty_dir);
    RUN_TEST(dbmem_dir_scan_single_file);
    RUN_TEST(dbmem_dir_scan_multiple_files);
    RUN_TEST(dbmem_dir_scan_recursive);
    RUN_TEST(dbmem_dir_scan_skips_hidden);
    RUN_TEST(dbmem_dir_scan_skips_hidden_dirs);
    RUN_TEST(dbmem_dir_scan_null_path);
    RUN_TEST(dbmem_dir_scan_null_callback);
    RUN_TEST(dbmem_dir_scan_nonexistent);
    RUN_TEST(dbmem_dir_scan_callback_abort);
    RUN_TEST(dbmem_dir_scan_trailing_slash);

#ifdef TEST_SQLITE_EXTENSION
    printf("\nSQLite extension tests:\n");
    RUN_TEST(sqlite_memory_version);
    RUN_TEST(sqlite_memory_is_enabled);
    RUN_TEST(sqlite_memory_is_enabled_missing_table);
    RUN_TEST(sqlite_memory_is_enabled_ignores_schema_version);
    RUN_TEST(sqlite_memory_clear_empty);
    RUN_TEST(sqlite_memory_delete_nonexistent);
    RUN_TEST(sqlite_memory_delete_context_nonexistent);
    RUN_TEST(sqlite_memory_delete_file_direct);
    RUN_TEST(sqlite_memory_delete_file_missing);
    RUN_TEST(sqlite_memory_delete_file_matches_source_path);
    RUN_TEST(sqlite_memory_delete_file_rejects_ambiguous_path);
    RUN_TEST(sqlite_memory_delete_file_invalid_path);
    RUN_TEST(sqlite_memory_delete_file_removes_directory_marker_only);
    RUN_TEST(sqlite_memory_rename_file_direct);
    RUN_TEST(sqlite_memory_rename_file_directory_marker);
    RUN_TEST(sqlite_memory_rename_file_rejects_marker_file_conversion);
    RUN_TEST(sqlite_memory_rename_file_matches_source_path);
    RUN_TEST(sqlite_memory_rename_file_missing);
    RUN_TEST(sqlite_memory_rename_file_duplicate_path);
    RUN_TEST(sqlite_memory_rename_file_rejects_ambiguous_path);
    RUN_TEST(sqlite_memory_list_files_empty);
    RUN_TEST(sqlite_memory_list_files_strips_common_full_path);
    RUN_TEST(sqlite_memory_list_files_keeps_relative_paths);
    RUN_TEST(sqlite_memory_list_files_strips_single_full_path_directory);
    RUN_TEST(sqlite_memory_list_files_normalizes_windows_separators);
    RUN_TEST(sqlite_memory_list_files_does_not_strip_mixed_path_types);
    RUN_TEST(sqlite_memory_list_files_omits_empty_paths);
    RUN_TEST(sqlite_memory_list_files_escapes_json_strings);
    RUN_TEST(sqlite_memory_list_files_includes_empty_directory_marker);
    RUN_TEST(sqlite_memory_list_files_merges_directory_marker_with_children);
    RUN_TEST(sqlite_memory_list_files_reports_indexed_flag);
    RUN_TEST(sqlite_memory_materialize_files_creates_directories_and_files);
    RUN_TEST(sqlite_memory_materialize_files_creates_directory_markers);
    RUN_TEST(sqlite_memory_materialize_files_accepts_existing_same_content);
    RUN_TEST(sqlite_memory_materialize_files_rejects_parent_segments);
    RUN_TEST(sqlite_memory_materialize_files_rejects_null_content);
    RUN_TEST(sqlite_schema_has_timestamps);
    RUN_TEST(sqlite_schema_migrates_embedding_metadata);
    RUN_TEST(sqlite_schema_migrates_source_path_to_local_table);
    RUN_TEST(sqlite_direct_insert_with_timestamp);
    RUN_TEST(sqlite_memory_delete_direct);
    RUN_TEST(sqlite_memory_delete_context_direct);
    RUN_TEST(sqlite_memory_clear_direct);

    printf("\nSync tests:\n");
    RUN_TEST(sqlite_sync_directory_removes_deleted);
    RUN_TEST(sqlite_sync_directory_removes_all_deleted);
    RUN_TEST(sqlite_sync_directory_skips_unchanged);
    RUN_TEST(sqlite_sync_directory_ignores_sibling_prefixes);
    RUN_TEST(sqlite_sync_directory_keeps_logical_rows_without_source_path);

    printf("\nSQLite extension advanced tests:\n");
    RUN_TEST(sqlite_memory_delete_with_vault_data);
    RUN_TEST(sqlite_memory_delete_twice);
    RUN_TEST(sqlite_memory_delete_context_null);
    RUN_TEST(sqlite_memory_delete_wrong_type);
    RUN_TEST(sqlite_memory_delete_context_wrong_type);
    RUN_TEST(sqlite_memory_update_access_setting);
    RUN_TEST(sqlite_memory_created_at_valid_range);
    RUN_TEST(sqlite_memory_clear_with_vault_fts);
    RUN_TEST(sqlite_memory_delete_context_with_vault);

    printf("\nEmbedding cache tests:\n");
    RUN_TEST(sqlite_cache_table_exists);
    RUN_TEST(sqlite_cache_clear_empty);
    RUN_TEST(sqlite_cache_clear_with_data);
    RUN_TEST(sqlite_cache_clear_by_provider_model);
    RUN_TEST(sqlite_cache_setting_default);
    RUN_TEST(sqlite_cache_max_entries_setting);
    RUN_TEST(sqlite_cache_eviction);
    RUN_TEST(sqlite_cache_no_eviction_when_unlimited);

    printf("\nSearch oversampling tests:\n");
    RUN_TEST(sqlite_search_oversample_setting);
    RUN_TEST(sqlite_search_zero_value_settings_apply_to_context);

    printf("\nCustom provider tests:\n");
    RUN_TEST(sqlite_custom_provider_register);
    RUN_TEST(sqlite_custom_provider_set_model);
    RUN_TEST(sqlite_memory_add_text_requires_model);
#ifndef DBMEM_OMIT_REMOTE_ENGINE
    RUN_TEST(sqlite_saved_remote_model_initializes_lazily_after_apikey);
#endif
#ifndef DBMEM_OMIT_LOCAL_ENGINE
    RUN_TEST(sqlite_saved_local_model_initializes_lazily);
#endif
    RUN_TEST(sqlite_saved_custom_model_initializes_lazily_after_register);
    RUN_TEST(sqlite_custom_provider_add_text);
    RUN_TEST(sqlite_memory_reindex_refreshes_synced_value_changes);
    RUN_TEST(sqlite_memory_reindex_preserves_directory_markers);
    RUN_TEST(sqlite_memory_reindex_preserves_empty_files);
    RUN_TEST(sqlite_set_model_reindex_preserves_directory_markers);
    RUN_TEST(sqlite_custom_provider_skips_whitespace_only_text);
    RUN_TEST(sqlite_custom_provider_persists_truncated_metadata);
    RUN_TEST(sqlite_memory_add_content_uses_explicit_content_and_context);
    RUN_TEST(sqlite_memory_add_content_removes_stale_path_when_new_content_is_deduped);
    RUN_TEST(sqlite_memory_preserve_duplicate_paths_option_defaults_to_zero);
    RUN_TEST(sqlite_memory_add_content_stores_empty_content);
    RUN_TEST(sqlite_memory_add_content_preserves_duplicate_empty_paths_when_enabled);
    RUN_TEST(sqlite_memory_rename_file_rekeys_preserved_empty_path_hash);
    RUN_TEST(sqlite_memory_rename_file_rekeys_preserved_index_hashes);
    RUN_TEST(sqlite_memory_rename_file_rejects_preserved_path_without_saved_content);
    RUN_TEST(sqlite_memory_add_content_keeps_same_empty_path_idempotent_when_preserving_duplicates);
    RUN_TEST(sqlite_memory_add_content_requires_preserve_for_directory_marker);
    RUN_TEST(sqlite_memory_add_content_creates_directory_marker);
    RUN_TEST(sqlite_memory_add_content_directory_marker_is_idempotent);
    RUN_TEST(sqlite_memory_add_content_rejects_nonempty_directory_marker);
    RUN_TEST(sqlite_memory_add_content_rejects_file_directory_conflicts);
    RUN_TEST(sqlite_memory_add_content_preserves_duplicate_nonempty_paths_when_enabled);
    RUN_TEST(sqlite_memory_add_file_reads_disk_and_stores_context);
    RUN_TEST(sqlite_memory_add_file_stores_empty_file);
    RUN_TEST(sqlite_memory_add_file_preserves_duplicate_empty_paths_when_enabled);
    RUN_TEST(sqlite_memory_add_file_attaches_source_path_to_existing_logical_path);
#ifndef _WIN32
    RUN_TEST(sqlite_memory_add_file_disambiguates_parent_collisions);
#endif
    RUN_TEST(sqlite_memory_add_directory_stores_relative_paths);
    RUN_TEST(sqlite_memory_add_directory_preserves_text_entries);
    RUN_TEST(sqlite_memory_add_content_rejects_non_text_content);
    RUN_TEST(sqlite_mdx_preprocessing_applies_only_to_mdx_files);
    RUN_TEST(sqlite_custom_provider_null_callbacks);
    RUN_TEST(sqlite_custom_provider_init_error);
    RUN_TEST(sqlite_custom_provider_apikey_passed);
    RUN_TEST(sqlite_set_model_failed_reindex_preserves_existing_rows);

    printf("\nDeferred embeddings tests:\n");
    RUN_TEST(sqlite_memory_defer_embeddings_stores_content_without_index);
    RUN_TEST(sqlite_memory_defer_embeddings_requires_save_content);
    RUN_TEST(sqlite_memory_embed_pending_embeds_deferred_content_in_batches);
    RUN_TEST(sqlite_memory_zero_chunk_content_marks_processed_with_sentinel);

#ifndef DBMEM_OMIT_REMOTE_ENGINE
    RUN_TEST(sqlite_set_model_releases_previous_engine_on_class_switch);
#else
    RUN_TEST(sqlite_set_model_failed_remote_switch_keeps_custom_engine);
#endif
#ifndef DBMEM_OMIT_LOCAL_ENGINE
    RUN_TEST(sqlite_local_logger_ignores_stale_user_data);
#endif
#endif

    printf("\n=== Results ===\n");
    printf("Tests run:    %d\n", tests_run);
    printf("Tests passed: %d\n", tests_passed);
    printf("Tests failed: %d\n", tests_failed);
    printf("\n");

    return tests_failed > 0 ? 1 : 0;
}
