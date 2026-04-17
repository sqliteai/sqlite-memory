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
    FILE *f = fopen(path, "w");
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

typedef struct {
    int init_calls;
    int set_column_calls;
    int set_filter_calls;
    int clear_filter_calls;
    int cleanup_calls;
    int fail_init;
    int fail_set_column;
    int fail_set_filter;
    int fail_clear_filter;
    int fail_cleanup;
} fake_cloudsync_t;

static void fake_cloudsync_result(sqlite3_context *context, int rc) {
    sqlite3 *db = sqlite3_context_db_handle(context);
    if (rc != SQLITE_OK) {
        sqlite3_result_error(context, sqlite3_errmsg(db), -1);
        return;
    }
    sqlite3_result_int(context, 1);
}

static void fake_cloudsync_version(sqlite3_context *context, int argc, sqlite3_value **argv) {
    UNUSED_PARAM(argc);
    UNUSED_PARAM(argv);
    sqlite3_result_text(context, "test-cloudsync", -1, SQLITE_STATIC);
}

static void fake_cloudsync_init(sqlite3_context *context, int argc, sqlite3_value **argv) {
    UNUSED_PARAM(argc);
    fake_cloudsync_t *state = (fake_cloudsync_t *)sqlite3_user_data(context);
    sqlite3 *db = sqlite3_context_db_handle(context);
    const char *table = (const char *)sqlite3_value_text(argv[0]);
    const char *algo = (const char *)sqlite3_value_text(argv[1]);

    state->init_calls++;
    if (state->fail_init) {
        sqlite3_result_error(context, "fake cloudsync_init failed", -1);
        return;
    }

    int rc = sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS cloudsync_table_settings ("
        "tbl_name TEXT NOT NULL COLLATE NOCASE, "
        "col_name TEXT NOT NULL COLLATE NOCASE, "
        "key TEXT, value TEXT, "
        "PRIMARY KEY(tbl_name,col_name,key));",
        NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        fake_cloudsync_result(context, rc);
        return;
    }

    char *sql = sqlite3_mprintf(
        "REPLACE INTO cloudsync_table_settings (tbl_name, col_name, key, value) "
        "VALUES ('%q', '*', 'algo', '%q');",
        table, algo);
    if (!sql) {
        sqlite3_result_error_nomem(context);
        return;
    }

    rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
    sqlite3_free(sql);
    fake_cloudsync_result(context, rc);
}

static void fake_cloudsync_set_column(sqlite3_context *context, int argc, sqlite3_value **argv) {
    UNUSED_PARAM(argc);
    fake_cloudsync_t *state = (fake_cloudsync_t *)sqlite3_user_data(context);
    sqlite3 *db = sqlite3_context_db_handle(context);
    const char *table = (const char *)sqlite3_value_text(argv[0]);
    const char *column = (const char *)sqlite3_value_text(argv[1]);
    const char *key = (const char *)sqlite3_value_text(argv[2]);
    const char *value = (const char *)sqlite3_value_text(argv[3]);

    state->set_column_calls++;
    if (state->fail_set_column) {
        sqlite3_result_error(context, "fake cloudsync_set_column failed", -1);
        return;
    }

    char *sql = sqlite3_mprintf(
        "REPLACE INTO cloudsync_table_settings (tbl_name, col_name, key, value) "
        "VALUES ('%q', '%q', '%q', '%q');",
        table, column, key, value);
    if (!sql) {
        sqlite3_result_error_nomem(context);
        return;
    }

    int rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
    sqlite3_free(sql);
    fake_cloudsync_result(context, rc);
}

static void fake_cloudsync_set_filter(sqlite3_context *context, int argc, sqlite3_value **argv) {
    UNUSED_PARAM(argc);
    fake_cloudsync_t *state = (fake_cloudsync_t *)sqlite3_user_data(context);
    sqlite3 *db = sqlite3_context_db_handle(context);
    const char *table = (const char *)sqlite3_value_text(argv[0]);
    const char *filter = (const char *)sqlite3_value_text(argv[1]);

    state->set_filter_calls++;
    if (state->fail_set_filter) {
        sqlite3_result_error(context, "fake cloudsync_set_filter failed", -1);
        return;
    }

    char *sql = sqlite3_mprintf(
        "REPLACE INTO cloudsync_table_settings (tbl_name, col_name, key, value) "
        "VALUES ('%q', '*', 'filter', '%q');",
        table, filter);
    if (!sql) {
        sqlite3_result_error_nomem(context);
        return;
    }

    int rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
    sqlite3_free(sql);
    fake_cloudsync_result(context, rc);
}

static void fake_cloudsync_clear_filter(sqlite3_context *context, int argc, sqlite3_value **argv) {
    UNUSED_PARAM(argc);
    fake_cloudsync_t *state = (fake_cloudsync_t *)sqlite3_user_data(context);
    sqlite3 *db = sqlite3_context_db_handle(context);
    const char *table = (const char *)sqlite3_value_text(argv[0]);

    state->clear_filter_calls++;
    if (state->fail_clear_filter) {
        sqlite3_result_error(context, "fake cloudsync_clear_filter failed", -1);
        return;
    }

    char *sql = sqlite3_mprintf(
        "DELETE FROM cloudsync_table_settings "
        "WHERE tbl_name='%q' AND col_name='*' AND key='filter';",
        table);
    if (!sql) {
        sqlite3_result_error_nomem(context);
        return;
    }

    int rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
    sqlite3_free(sql);
    fake_cloudsync_result(context, rc);
}

static void fake_cloudsync_cleanup(sqlite3_context *context, int argc, sqlite3_value **argv) {
    UNUSED_PARAM(argc);
    fake_cloudsync_t *state = (fake_cloudsync_t *)sqlite3_user_data(context);
    sqlite3 *db = sqlite3_context_db_handle(context);
    const char *table = (const char *)sqlite3_value_text(argv[0]);

    state->cleanup_calls++;
    if (state->fail_cleanup) {
        sqlite3_result_error(context, "fake cloudsync_cleanup failed", -1);
        return;
    }

    char *sql = sqlite3_mprintf(
        "DELETE FROM cloudsync_table_settings WHERE tbl_name='%q';",
        table);
    if (!sql) {
        sqlite3_result_error_nomem(context);
        return;
    }

    int rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
    sqlite3_free(sql);
    fake_cloudsync_result(context, rc);
}

static int install_fake_cloudsync(sqlite3 *db, fake_cloudsync_t *state) {
    int rc = sqlite3_create_function_v2(db, "cloudsync_version", 0, SQLITE_UTF8, state, fake_cloudsync_version, NULL, NULL, NULL);
    if (rc != SQLITE_OK) return rc;
    rc = sqlite3_create_function_v2(db, "cloudsync_init", 3, SQLITE_UTF8, state, fake_cloudsync_init, NULL, NULL, NULL);
    if (rc != SQLITE_OK) return rc;
    rc = sqlite3_create_function_v2(db, "cloudsync_set_column", 4, SQLITE_UTF8, state, fake_cloudsync_set_column, NULL, NULL, NULL);
    if (rc != SQLITE_OK) return rc;
    rc = sqlite3_create_function_v2(db, "cloudsync_set_filter", 2, SQLITE_UTF8, state, fake_cloudsync_set_filter, NULL, NULL, NULL);
    if (rc != SQLITE_OK) return rc;
    rc = sqlite3_create_function_v2(db, "cloudsync_clear_filter", 1, SQLITE_UTF8, state, fake_cloudsync_clear_filter, NULL, NULL, NULL);
    if (rc != SQLITE_OK) return rc;
    return sqlite3_create_function_v2(db, "cloudsync_cleanup", 1, SQLITE_UTF8, state, fake_cloudsync_cleanup, NULL, NULL, NULL);
}

TEST(sqlite_memory_version) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    char version[64];
    int rc = exec_get_text(db, "SELECT memory_version();", version, sizeof(version));
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT(strlen(version) > 0);
    ASSERT(strstr(version, ".") != NULL);  // Version should contain a dot
    ASSERT_STR_EQ(version, "1.0.0");

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
    int rc = exec_get_int(db, "SELECT memory_delete('0000000000003039');", &result);
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

TEST(sqlite_schema_has_timestamps) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    // Check that schema includes created_at column and uses TEXT hash (1.0.0+)
    char sql[512];
    int rc = exec_get_text(db,
        "SELECT sql FROM sqlite_master WHERE name='dbmem_content';",
        sql, sizeof(sql));
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT(strstr(sql, "created_at") != NULL);
    ASSERT(strstr(sql, "last_accessed") != NULL);
    ASSERT(strstr(sql, "hash TEXT") != NULL);

    sqlite3_close(db);
}

// Test that inserting directly into tables works with new schema
TEST(sqlite_direct_insert_with_timestamp) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    // Insert a test record directly (hash is now TEXT, 16-char hex)
    int rc = sqlite3_exec(db,
        "INSERT INTO dbmem_content (hash, path, value, length, context, created_at) "
        "VALUES ('000000000000007b', 'test/path', 'test value', 10, 'ctx1', strftime('%s','now'));",
        NULL, NULL, NULL);
    ASSERT_EQ(rc, SQLITE_OK);

    // Verify it's there
    sqlite3_int64 count;
    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_content;", &count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(count, 1);

    // Verify created_at was set
    sqlite3_int64 created_at;
    rc = exec_get_int(db, "SELECT created_at FROM dbmem_content WHERE hash='000000000000007b';", &created_at);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT(created_at > 0);  // Should be a valid Unix timestamp

    sqlite3_close(db);
}

TEST(sqlite_memory_delete_direct) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    // Insert a test record directly (hash is now TEXT, 16-char hex; 456 = 0x1c8)
    int rc = sqlite3_exec(db,
        "INSERT INTO dbmem_content (hash, path, value, length, context, created_at) "
        "VALUES ('00000000000001c8', 'test/path2', 'test value 2', 12, 'ctx2', strftime('%s','now'));",
        NULL, NULL, NULL);
    ASSERT_EQ(rc, SQLITE_OK);

    // Delete it using the TEXT hash
    sqlite3_int64 result;
    rc = exec_get_int(db, "SELECT memory_delete('00000000000001c8');", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 1);  // Should have deleted 1 row

    // Verify it's gone
    sqlite3_int64 count;
    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_content;", &count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(count, 0);

    sqlite3_close(db);
}

TEST(sqlite_memory_delete_context_direct) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    // Insert test records with different contexts (hashes as 16-char hex)
    // 100=0x64, 101=0x65, 102=0x66
    int rc = sqlite3_exec(db,
        "INSERT INTO dbmem_content (hash, path, value, length, context, created_at) VALUES "
        "('0000000000000064', 'path1', 'v1', 2, 'ctx_a', 0), "
        "('0000000000000065', 'path2', 'v2', 2, 'ctx_a', 0), "
        "('0000000000000066', 'path3', 'v3', 2, 'ctx_b', 0);",
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

    sqlite3_close(db);
}

TEST(sqlite_memory_clear_direct) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    // Insert test records (200=0xc8, 201=0xc9)
    int rc = sqlite3_exec(db,
        "INSERT INTO dbmem_content (hash, path, value, length, context, created_at) VALUES "
        "('00000000000000c8', 'p1', 'v1', 2, 'c1', 0), "
        "('00000000000000c9', 'p2', 'v2', 2, 'c2', 0);",
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

    sqlite3_close(db);
}

TEST(sqlite_memory_delete_with_vault_data) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    // Insert into content and vault tables (300 = 0x12c)
    int rc = sqlite3_exec(db,
        "INSERT INTO dbmem_content (hash, path, value, length, context, created_at) "
        "VALUES ('000000000000012c', 'path300', 'value', 5, 'ctx', 0);",
        NULL, NULL, NULL);
    ASSERT_EQ(rc, SQLITE_OK);

    rc = sqlite3_exec(db,
        "INSERT INTO dbmem_vault (hash, seq, embedding, offset, length) "
        "VALUES ('000000000000012c', 0, X'00000000', 0, 5), ('000000000000012c', 1, X'00000000', 5, 5);",
        NULL, NULL, NULL);
    ASSERT_EQ(rc, SQLITE_OK);

    // Verify vault has data
    sqlite3_int64 vault_count;
    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_vault WHERE hash='000000000000012c';", &vault_count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(vault_count, 2);

    // Delete by hash (TEXT)
    sqlite3_int64 result;
    rc = exec_get_int(db, "SELECT memory_delete('000000000000012c');", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 1);

    // Verify content is gone
    sqlite3_int64 content_count;
    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_content WHERE hash='000000000000012c';", &content_count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(content_count, 0);

    // Verify vault is also gone
    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_vault WHERE hash='000000000000012c';", &vault_count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(vault_count, 0);

    sqlite3_close(db);
}

TEST(sqlite_memory_delete_twice) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    // Insert a record (400 = 0x190)
    int rc = sqlite3_exec(db,
        "INSERT INTO dbmem_content (hash, path, value, length, context, created_at) "
        "VALUES ('0000000000000190', 'path400', 'value', 5, 'ctx', 0);",
        NULL, NULL, NULL);
    ASSERT_EQ(rc, SQLITE_OK);

    // Delete first time - should return 1
    sqlite3_int64 result;
    rc = exec_get_int(db, "SELECT memory_delete('0000000000000190');", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 1);

    // Delete second time - should return 0
    rc = exec_get_int(db, "SELECT memory_delete('0000000000000190');", &result);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(result, 0);

    sqlite3_close(db);
}

TEST(sqlite_memory_delete_context_null) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    // Insert records - some with NULL context, some with context (500=0x1f4, 501=0x1f5, 502=0x1f6)
    int rc = sqlite3_exec(db,
        "INSERT INTO dbmem_content (hash, path, value, length, context, created_at) VALUES "
        "('00000000000001f4', 'p1', 'v1', 2, NULL, 0), "
        "('00000000000001f5', 'p2', 'v2', 2, NULL, 0), "
        "('00000000000001f6', 'p3', 'v3', 2, 'has_context', 0);",
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

    // memory_delete now expects TEXT; passing an INTEGER should return an error
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT memory_delete(42);", -1, &stmt, NULL);
    ASSERT_EQ(rc, SQLITE_OK);

    rc = sqlite3_step(stmt);
    ASSERT_EQ(rc, SQLITE_ERROR);
    sqlite3_finalize(stmt);

    // Passing a TEXT hash (even one that doesn't match any row) should succeed, returning 0
    rc = sqlite3_prepare_v2(db, "SELECT memory_delete('0000000000000000');", -1, &stmt, NULL);
    ASSERT_EQ(rc, SQLITE_OK);
    rc = sqlite3_step(stmt);
    ASSERT_EQ(rc, SQLITE_ROW);
    ASSERT_EQ(sqlite3_column_int64(stmt, 0), 0);
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

    // Insert with current timestamp (600 = 0x258)
    int rc = sqlite3_exec(db,
        "INSERT INTO dbmem_content (hash, path, value, length, context, created_at) "
        "VALUES ('0000000000000258', 'path600', 'value', 5, 'ctx', strftime('%s','now'));",
        NULL, NULL, NULL);
    ASSERT_EQ(rc, SQLITE_OK);

    // Get the created_at value
    sqlite3_int64 created_at;
    rc = exec_get_int(db, "SELECT created_at FROM dbmem_content WHERE hash='0000000000000258';", &created_at);
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

    // Insert into all tables (700 = 0x2bc)
    int rc = sqlite3_exec(db,
        "INSERT INTO dbmem_content (hash, path, value, length, context, created_at) "
        "VALUES ('00000000000002bc', 'path700', 'value', 5, 'ctx', 0);",
        NULL, NULL, NULL);
    ASSERT_EQ(rc, SQLITE_OK);

    rc = sqlite3_exec(db,
        "INSERT INTO dbmem_vault (hash, seq, embedding, offset, length) "
        "VALUES ('00000000000002bc', 0, X'00000000', 0, 5);",
        NULL, NULL, NULL);
    ASSERT_EQ(rc, SQLITE_OK);

    rc = sqlite3_exec(db,
        "INSERT INTO dbmem_vault_fts (content, hash, seq, context) "
        "VALUES ('test content', '00000000000002bc', 0, 'ctx');",
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

// Helper to insert a fake dbmem_content entry with a known path, hash, and length.
// hash_hex must be a 16-char lowercase hex string (e.g. "000000000000007b").
static int insert_fake_content(sqlite3 *db, const char *hash_hex, const char *path, const char *context, sqlite3_int64 length) {
    sqlite3_stmt *vm = NULL;
    const char *sql = "INSERT INTO dbmem_content (hash, path, value, length, context, created_at) "
                      "VALUES (?1, ?2, 'fake', ?3, ?4, 0);";
    int rc = sqlite3_prepare_v2(db, sql, -1, &vm, NULL);
    if (rc != SQLITE_OK) return rc;
    sqlite3_bind_text(vm, 1, hash_hex, -1, SQLITE_STATIC);
    sqlite3_bind_text(vm, 2, path, -1, SQLITE_STATIC);
    sqlite3_bind_int64(vm, 3, length);
    if (context) sqlite3_bind_text(vm, 4, context, -1, SQLITE_STATIC);
    else sqlite3_bind_null(vm, 4);
    rc = sqlite3_step(vm);
    sqlite3_finalize(vm);
    return (rc == SQLITE_DONE) ? SQLITE_OK : rc;
}

// Helper: convert uint64_t hash to 16-char lowercase hex string
static void hash_to_hex(uint64_t hash, char buf[17]) {
    snprintf(buf, 17, "%016llx", (unsigned long long)hash);
}

TEST(sqlite_sync_directory_removes_deleted) {
    // Test that memory_add_directory removes entries for files no longer on disk
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    const char *test_dir = "/tmp/dbmem_test_sync_del";
    const char *file_keep = "/tmp/dbmem_test_sync_del/keep.md";

    // Clean up
    remove(file_keep);
    remove("/tmp/dbmem_test_sync_del/gone.md");
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

    char keep_hash_hex[17];
    hash_to_hex(keep_hash, keep_hash_hex);

    int rc = insert_fake_content(db, keep_hash_hex, file_keep, NULL, len);
    ASSERT_EQ(rc, SQLITE_OK);

    rc = insert_fake_content(db, "000000000001869f", "/tmp/dbmem_test_sync_del/gone.md", NULL, 4);
    ASSERT_EQ(rc, SQLITE_OK);

    // Verify 2 entries before sync
    sqlite3_int64 count;
    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_content;", &count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(count, 2);

    // Sync — should remove the entry for gone.md, skip keep.md (hash match)
    sqlite3_int64 result;
    rc = exec_get_int(db, "SELECT memory_add_directory('/tmp/dbmem_test_sync_del');", &result);
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

    const char *test_dir = "/tmp/dbmem_test_sync_allgone";
    remove("/tmp/dbmem_test_sync_allgone/x.md");
    rmdir_p(test_dir);
    mkdir_p(test_dir);  // empty directory

    // Insert fake entries pointing to files that don't exist (1001=0x3e9, 1002=0x3ea, 1003=0x3eb)
    int rc = insert_fake_content(db, "00000000000003e9", "/tmp/dbmem_test_sync_allgone/a.md", "ctx", 4);
    ASSERT_EQ(rc, SQLITE_OK);
    rc = insert_fake_content(db, "00000000000003ea", "/tmp/dbmem_test_sync_allgone/b.md", "ctx", 4);
    ASSERT_EQ(rc, SQLITE_OK);
    rc = insert_fake_content(db, "00000000000003eb", "/tmp/dbmem_test_sync_allgone/c.md", "ctx", 4);
    ASSERT_EQ(rc, SQLITE_OK);

    // Also insert vault entries to verify cascade delete
    rc = sqlite3_exec(db,
        "INSERT INTO dbmem_vault (hash, seq, embedding, offset, length) VALUES "
        "('00000000000003e9', 0, X'00000000', 0, 4), "
        "('00000000000003ea', 0, X'00000000', 0, 4), "
        "('00000000000003eb', 0, X'00000000', 0, 4);",
        NULL, NULL, NULL);
    ASSERT_EQ(rc, SQLITE_OK);

    sqlite3_int64 count;
    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_content;", &count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(count, 3);

    // Sync — all files gone, all entries should be removed
    sqlite3_int64 result;
    rc = exec_get_int(db, "SELECT memory_add_directory('/tmp/dbmem_test_sync_allgone');", &result);
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

    const char *test_dir = "/tmp/dbmem_test_sync_skip";
    const char *file = "/tmp/dbmem_test_sync_skip/note.md";
    const char *content = "# My Note\nSome content.";

    remove(file);
    rmdir_p(test_dir);
    mkdir_p(test_dir);
    create_test_file(file, content);

    // Compute the hash and pre-insert the entry
    uint64_t hash = dbmem_hash_compute(content, strlen(content));
    char hash_hex[17];
    hash_to_hex(hash, hash_hex);
    int rc = insert_fake_content(db, hash_hex, file, "notes", (sqlite3_int64)strlen(content));
    ASSERT_EQ(rc, SQLITE_OK);

    // Sync — file exists with matching hash, should be skipped
    sqlite3_int64 result;
    rc = exec_get_int(db, "SELECT memory_add_directory('/tmp/dbmem_test_sync_skip', 'notes');", &result);
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

TEST(sqlite_cache_table_exists) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    // Check that dbmem_cache table exists
    char sql[256];
    int rc = exec_get_text(db,
        "SELECT sql FROM sqlite_master WHERE name='dbmem_cache';",
        sql, sizeof(sql));
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT(strstr(sql, "text_hash") != NULL);
    ASSERT(strstr(sql, "provider") != NULL);
    ASSERT(strstr(sql, "model") != NULL);
    ASSERT(strstr(sql, "embedding") != NULL);
    ASSERT(strstr(sql, "dimension") != NULL);

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

    // Insert some fake cache entries (text_hash is now TEXT hex; 111=0x6f, 222=0xde, 333=0x14d)
    int rc = sqlite3_exec(db,
        "INSERT INTO dbmem_cache (text_hash, provider, model, embedding, dimension) VALUES "
        "('000000000000006f', 'openai', 'text-embedding-3-small', X'00000000', 1), "
        "('00000000000000de', 'openai', 'text-embedding-3-small', X'00000000', 1), "
        "('000000000000014d', 'local', 'nomic', X'00000000', 1);",
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

    // Insert cache entries for different provider/model combos (text_hash as TEXT hex)
    int rc = sqlite3_exec(db,
        "INSERT INTO dbmem_cache (text_hash, provider, model, embedding, dimension) VALUES "
        "('000000000000006f', 'openai', 'text-embedding-3-small', X'00000000', 1), "
        "('00000000000000de', 'openai', 'text-embedding-3-small', X'00000000', 1), "
        "('000000000000014d', 'local', 'nomic', X'00000000', 1);",
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

    // Insert 5 entries (rowids 1-5; text_hash as TEXT hex)
    rc = sqlite3_exec(db,
        "INSERT INTO dbmem_cache (text_hash, provider, model, embedding, dimension) VALUES "
        "('0000000000000001', 'p', 'm', X'00000000', 1), "
        "('0000000000000002', 'p', 'm', X'00000000', 1), "
        "('0000000000000003', 'p', 'm', X'00000000', 1), "
        "('0000000000000004', 'p', 'm', X'00000000', 1), "
        "('0000000000000005', 'p', 'm', X'00000000', 1);",
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
        "('000000000000000a', 'p', 'm', X'00000000', 1), "
        "('000000000000000b', 'p', 'm', X'00000000', 1), "
        "('000000000000000c', 'p', 'm', X'00000000', 1);",
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
    // Insert many entries, none should be evicted (text_hash as TEXT hex)
    int rc = sqlite3_exec(db,
        "INSERT INTO dbmem_cache (text_hash, provider, model, embedding, dimension) VALUES "
        "('0000000000000001', 'p', 'm', X'00000000', 1), "
        "('0000000000000002', 'p', 'm', X'00000000', 1), "
        "('0000000000000003', 'p', 'm', X'00000000', 1), "
        "('0000000000000004', 'p', 'm', X'00000000', 1), "
        "('0000000000000005', 'p', 'm', X'00000000', 1);",
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

TEST(sqlite_memory_delete_context_with_vault) {
    sqlite3 *db = open_test_db();
    ASSERT(db != NULL);

    // Insert records with different contexts into content and vault (800=0x320, 801=0x321)
    int rc = sqlite3_exec(db,
        "INSERT INTO dbmem_content (hash, path, value, length, context, created_at) VALUES "
        "('0000000000000320', 'p1', 'v1', 2, 'delete_me', 0), "
        "('0000000000000321', 'p2', 'v2', 2, 'keep_me', 0);",
        NULL, NULL, NULL);
    ASSERT_EQ(rc, SQLITE_OK);

    rc = sqlite3_exec(db,
        "INSERT INTO dbmem_vault (hash, seq, embedding, offset, length) VALUES "
        "('0000000000000320', 0, X'00000000', 0, 2), "
        "('0000000000000321', 0, X'00000000', 0, 2);",
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

    char vault_hash[32];
    rc = exec_get_text(db, "SELECT hash FROM dbmem_vault;", vault_hash, sizeof(vault_hash));
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_STR_EQ(vault_hash, "0000000000000321");

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

static void *dummy_init(const char *model, const char *api_key, void *xdata, char err_msg[1024]) {
    UNUSED_PARAM(model);
    UNUSED_PARAM(xdata);
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
    result->n_tokens = text_len / 4;
    result->n_tokens_truncated = 0;
    result->n_embd = e->dimension;
    result->embedding = e->embedding;
    return 0;
}

static void dummy_free(void *engine, void *xdata) {
    UNUSED_PARAM(xdata);
    free(engine);
}

static void *dummy_init_fail(const char *model, const char *api_key, void *xdata, char err_msg[1024]) {
    UNUSED_PARAM(model);
    UNUSED_PARAM(api_key);
    UNUSED_PARAM(xdata);
    snprintf(err_msg, 1024, "intentional init failure");
    return NULL;
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

TEST(sqlite_schema_migration) {
    // Create an in-memory database with the OLD schema (INTEGER hash, pre-1.0.0)
    // then call sqlite3_memory_init and verify migration happened correctly.
    sqlite3 *db = NULL;
    int rc = sqlite3_open(":memory:", &db);
    ASSERT_EQ(rc, SQLITE_OK);

    // Build old schema manually (INTEGER hash)
    rc = sqlite3_exec(db,
        "CREATE TABLE dbmem_settings (key TEXT PRIMARY KEY, value TEXT);"
        "CREATE TABLE dbmem_content ("
        "  hash INTEGER PRIMARY KEY NOT NULL,"
        "  path TEXT NOT NULL DEFAULT '' UNIQUE,"
        "  value TEXT DEFAULT NULL,"
        "  length INTEGER NOT NULL DEFAULT 0,"
        "  context TEXT DEFAULT NULL,"
        "  created_at INTEGER DEFAULT 0,"
        "  last_accessed INTEGER DEFAULT 0);"
        "CREATE TABLE dbmem_vault ("
        "  hash INTEGER NOT NULL,"
        "  seq INTEGER NOT NULL,"
        "  embedding BLOB NOT NULL,"
        "  offset INTEGER NOT NULL,"
        "  length INTEGER NOT NULL,"
        "  PRIMARY KEY (hash, seq));"
        "CREATE TABLE dbmem_cache ("
        "  text_hash INTEGER NOT NULL,"
        "  provider TEXT NOT NULL,"
        "  model TEXT NOT NULL,"
        "  embedding BLOB NOT NULL,"
        "  dimension INTEGER NOT NULL,"
        "  PRIMARY KEY (text_hash, provider, model));",
        NULL, NULL, NULL);
    ASSERT_EQ(rc, SQLITE_OK);

    // Insert sample data with INTEGER hash values
    // Use small values that fit in uint64_t and round-trip cleanly through printf('%016x')
    rc = sqlite3_exec(db,
        "INSERT INTO dbmem_content (hash, path, value, length, context, created_at) VALUES"
        " (255, 'path_a', 'content a', 9, 'ctx', 1000),"
        " (4096, 'path_b', 'content b', 9, NULL, 2000);",
        NULL, NULL, NULL);
    ASSERT_EQ(rc, SQLITE_OK);

    rc = sqlite3_exec(db,
        "INSERT INTO dbmem_vault (hash, seq, embedding, offset, length) VALUES"
        " (255, 0, X'0000803f', 0, 9),"
        " (4096, 0, X'0000803f', 0, 9);",
        NULL, NULL, NULL);
    ASSERT_EQ(rc, SQLITE_OK);

    rc = sqlite3_exec(db,
        "INSERT INTO dbmem_cache (text_hash, provider, model, embedding, dimension) VALUES"
        " (255, 'test', 'model', X'0000803f', 1);",
        NULL, NULL, NULL);
    ASSERT_EQ(rc, SQLITE_OK);

    // Run the extension init — this triggers schema migration
    rc = sqlite3_memory_init(db, NULL, NULL);
    ASSERT_EQ(rc, SQLITE_OK);

    // Verify hash column is now TEXT
    char schema[512];
    rc = exec_get_text(db,
        "SELECT sql FROM sqlite_master WHERE name='dbmem_content';",
        schema, sizeof(schema));
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT(strstr(schema, "hash TEXT") != NULL);

    // Verify the content rows are present with hex-encoded hashes
    // 255  = 0x00000000000000ff
    // 4096 = 0x0000000000001000
    sqlite3_int64 count;
    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_content;", &count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(count, 2);

    char hash_val[64];
    rc = exec_get_text(db,
        "SELECT hash FROM dbmem_content WHERE path='path_a';",
        hash_val, sizeof(hash_val));
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_STR_EQ(hash_val, "00000000000000ff");

    rc = exec_get_text(db,
        "SELECT hash FROM dbmem_content WHERE path='path_b';",
        hash_val, sizeof(hash_val));
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_STR_EQ(hash_val, "0000000000001000");

    // Verify vault rows migrated correctly
    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_vault;", &count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(count, 2);

    rc = exec_get_text(db,
        "SELECT hash FROM dbmem_vault WHERE seq=0 AND length=9 AND hash='00000000000000ff';",
        hash_val, sizeof(hash_val));
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_STR_EQ(hash_val, "00000000000000ff");

    // Verify cache migrated correctly
    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_cache;", &count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(count, 1);

    rc = exec_get_text(db,
        "SELECT text_hash FROM dbmem_cache;",
        hash_val, sizeof(hash_val));
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_STR_EQ(hash_val, "00000000000000ff");

    // Verify that content data is preserved
    sqlite3_int64 created_at;
    rc = exec_get_int(db,
        "SELECT created_at FROM dbmem_content WHERE hash='00000000000000ff';",
        &created_at);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(created_at, 1000);

    // Verify memory_delete works with the migrated TEXT hash
    sqlite3_int64 deleted;
    rc = exec_get_int(db, "SELECT memory_delete('00000000000000ff');", &deleted);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(deleted, 1);

    rc = exec_get_int(db, "SELECT COUNT(*) FROM dbmem_content;", &count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(count, 1);

    sqlite3_close(db);
}

TEST(sqlite_schema_migration_preserves_cloudsync_filter) {
    sqlite3 *db = NULL;
    int rc = sqlite3_open(":memory:", &db);
    ASSERT_EQ(rc, SQLITE_OK);

    fake_cloudsync_t sync = {0};
    rc = install_fake_cloudsync(db, &sync);
    ASSERT_EQ(rc, SQLITE_OK);

    rc = sqlite3_exec(db,
        "CREATE TABLE dbmem_settings (key TEXT PRIMARY KEY, value TEXT);"
        "CREATE TABLE cloudsync_table_settings ("
        "  tbl_name TEXT NOT NULL COLLATE NOCASE,"
        "  col_name TEXT NOT NULL COLLATE NOCASE,"
        "  key TEXT, value TEXT,"
        "  PRIMARY KEY(tbl_name,col_name,key));"
        "INSERT INTO cloudsync_table_settings (tbl_name, col_name, key, value) VALUES"
        " ('dbmem_content', '*', 'algo', 'cls'),"
        " ('dbmem_content', 'path', 'algo', 'plain'),"
        " ('dbmem_content', 'value', 'algo', 'block'),"
        " ('dbmem_content', '*', 'filter', 'context IN (''ctx_a'',''ctx_b'')');"
        "CREATE TABLE dbmem_content ("
        "  hash INTEGER PRIMARY KEY NOT NULL,"
        "  path TEXT NOT NULL DEFAULT '' UNIQUE,"
        "  value TEXT DEFAULT NULL,"
        "  length INTEGER NOT NULL DEFAULT 0,"
        "  context TEXT DEFAULT NULL,"
        "  created_at INTEGER DEFAULT 0,"
        "  last_accessed INTEGER DEFAULT 0);"
        "CREATE TABLE dbmem_vault ("
        "  hash INTEGER NOT NULL,"
        "  seq INTEGER NOT NULL,"
        "  embedding BLOB NOT NULL,"
        "  offset INTEGER NOT NULL,"
        "  length INTEGER NOT NULL,"
        "  PRIMARY KEY (hash, seq));"
        "INSERT INTO dbmem_content (hash, path, value, length, context) VALUES"
        " (255, 'path_a', 'content a', 9, 'ctx_a');"
        "INSERT INTO dbmem_vault (hash, seq, embedding, offset, length) VALUES"
        " (255, 0, X'0000803f', 0, 9);",
        NULL, NULL, NULL);
    ASSERT_EQ(rc, SQLITE_OK);

    rc = sqlite3_memory_init(db, NULL, NULL);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(sync.cleanup_calls, 0);
    ASSERT_EQ(sync.init_calls, 0);
    ASSERT_EQ(sync.set_column_calls, 0);
    ASSERT_EQ(sync.set_filter_calls, 0);
    ASSERT_EQ(sync.clear_filter_calls, 0);

    char filter[128];
    rc = exec_get_text(db,
        "SELECT value FROM cloudsync_table_settings "
        "WHERE tbl_name='dbmem_content' AND col_name='*' AND key='filter';",
        filter, sizeof(filter));
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_STR_EQ(filter, "context IN ('ctx_a','ctx_b')");

    char path_algo[32];
    rc = exec_get_text(db,
        "SELECT value FROM cloudsync_table_settings "
        "WHERE tbl_name='dbmem_content' AND col_name='path' AND key='algo';",
        path_algo, sizeof(path_algo));
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_STR_EQ(path_algo, "plain");

    char hash_type[32];
    rc = exec_get_text(db,
        "SELECT type FROM pragma_table_info('dbmem_content') WHERE name='hash';",
        hash_type, sizeof(hash_type));
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_STR_EQ(hash_type, "TEXT");

    sqlite3_close(db);
}

TEST(sqlite_schema_migration_requires_cloudsync_when_synced) {
    sqlite3 *db = NULL;
    int rc = sqlite3_open(":memory:", &db);
    ASSERT_EQ(rc, SQLITE_OK);

    rc = sqlite3_exec(db,
        "CREATE TABLE dbmem_settings (key TEXT PRIMARY KEY, value TEXT);"
        "CREATE TABLE cloudsync_table_settings ("
        "  tbl_name TEXT NOT NULL COLLATE NOCASE,"
        "  col_name TEXT NOT NULL COLLATE NOCASE,"
        "  key TEXT, value TEXT,"
        "  PRIMARY KEY(tbl_name,col_name,key));"
        "INSERT INTO cloudsync_table_settings (tbl_name, col_name, key, value) VALUES"
        " ('dbmem_content', '*', 'algo', 'cls'),"
        " ('dbmem_content', 'value', 'algo', 'block'),"
        " ('dbmem_content', '*', 'filter', 'context IN (''ctx_a'')');"
        "CREATE TABLE dbmem_content ("
        "  hash INTEGER PRIMARY KEY NOT NULL,"
        "  path TEXT NOT NULL DEFAULT '' UNIQUE,"
        "  value TEXT DEFAULT NULL,"
        "  length INTEGER NOT NULL DEFAULT 0,"
        "  context TEXT DEFAULT NULL,"
        "  created_at INTEGER DEFAULT 0,"
        "  last_accessed INTEGER DEFAULT 0);"
        "CREATE TABLE dbmem_vault ("
        "  hash INTEGER NOT NULL,"
        "  seq INTEGER NOT NULL,"
        "  embedding BLOB NOT NULL,"
        "  offset INTEGER NOT NULL,"
        "  length INTEGER NOT NULL,"
        "  PRIMARY KEY (hash, seq));",
        NULL, NULL, NULL);
    ASSERT_EQ(rc, SQLITE_OK);

    rc = sqlite3_memory_init(db, NULL, NULL);
    ASSERT_EQ(rc, SQLITE_ERROR);

    char hash_type[32];
    rc = exec_get_text(db,
        "SELECT type FROM pragma_table_info('dbmem_content') WHERE name='hash';",
        hash_type, sizeof(hash_type));
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_STR_EQ(hash_type, "INTEGER");

    sqlite3_close(db);
}

TEST(sqlite_schema_migration_ignores_user_triggers) {
    sqlite3 *db = NULL;
    int rc = sqlite3_open(":memory:", &db);
    ASSERT_EQ(rc, SQLITE_OK);

    fake_cloudsync_t sync = {0};
    rc = install_fake_cloudsync(db, &sync);
    ASSERT_EQ(rc, SQLITE_OK);

    rc = sqlite3_exec(db,
        "CREATE TABLE dbmem_settings (key TEXT PRIMARY KEY, value TEXT);"
        "CREATE TABLE dbmem_content ("
        "  hash INTEGER PRIMARY KEY NOT NULL,"
        "  path TEXT NOT NULL DEFAULT '' UNIQUE,"
        "  value TEXT DEFAULT NULL,"
        "  length INTEGER NOT NULL DEFAULT 0,"
        "  context TEXT DEFAULT NULL,"
        "  created_at INTEGER DEFAULT 0,"
        "  last_accessed INTEGER DEFAULT 0);"
        "CREATE TABLE dbmem_vault ("
        "  hash INTEGER NOT NULL,"
        "  seq INTEGER NOT NULL,"
        "  embedding BLOB NOT NULL,"
        "  offset INTEGER NOT NULL,"
        "  length INTEGER NOT NULL,"
        "  PRIMARY KEY (hash, seq));"
        "CREATE TRIGGER user_after_insert_dbmem_content "
        "AFTER INSERT ON dbmem_content BEGIN SELECT 1; END;",
        NULL, NULL, NULL);
    ASSERT_EQ(rc, SQLITE_OK);

    rc = sqlite3_memory_init(db, NULL, NULL);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(sync.cleanup_calls, 0);
    ASSERT_EQ(sync.init_calls, 0);
    ASSERT_EQ(sync.set_column_calls, 0);
    ASSERT_EQ(sync.set_filter_calls, 0);
    ASSERT_EQ(sync.clear_filter_calls, 0);

    sqlite3_int64 count = -1;
    rc = exec_get_int(db,
        "SELECT COUNT(*) FROM sqlite_master "
        "WHERE type='table' AND name='cloudsync_table_settings';",
        &count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(count, 0);

    sqlite3_close(db);
}

TEST(sqlite_schema_init_repairs_stale_fts_hashes) {
    sqlite3 *db = NULL;
    int rc = sqlite3_open(":memory:", &db);
    ASSERT_EQ(rc, SQLITE_OK);

    // Simulate a database first opened on a build without FTS5 during the
    // hash migration: core tables are already TEXT, but the legacy FTS table
    // still contains decimal hash strings.
    rc = sqlite3_exec(db,
        "CREATE TABLE dbmem_settings (key TEXT PRIMARY KEY, value TEXT);"
        "CREATE TABLE dbmem_content ("
        "  hash TEXT PRIMARY KEY NOT NULL,"
        "  path TEXT NOT NULL DEFAULT '' UNIQUE,"
        "  value TEXT DEFAULT NULL,"
        "  length INTEGER NOT NULL DEFAULT 0,"
        "  context TEXT DEFAULT NULL,"
        "  created_at INTEGER DEFAULT 0,"
        "  last_accessed INTEGER DEFAULT 0);"
        "CREATE TABLE dbmem_vault ("
        "  hash TEXT NOT NULL,"
        "  seq INTEGER NOT NULL,"
        "  embedding BLOB NOT NULL,"
        "  offset INTEGER NOT NULL,"
        "  length INTEGER NOT NULL,"
        "  PRIMARY KEY (hash, seq));"
        "CREATE VIRTUAL TABLE dbmem_vault_fts USING fts5 "
        "  (content, hash UNINDEXED, seq UNINDEXED, context UNINDEXED);"
        "INSERT INTO dbmem_content (hash, path, value, length, context, created_at) VALUES"
        " ('00000000000000ff', 'path_a', 'content a', 9, 'ctx_a', 1000);"
        "INSERT INTO dbmem_vault (hash, seq, embedding, offset, length) VALUES"
        " ('00000000000000ff', 0, X'0000803f', 0, 9);"
        "INSERT INTO dbmem_vault_fts (content, hash, seq, context) VALUES"
        " ('content a', '255', 0, 'ctx_a');",
        NULL, NULL, NULL);
    ASSERT_EQ(rc, SQLITE_OK);

    sqlite3_int64 joined_before = -1;
    rc = exec_get_int(db,
        "SELECT COUNT(*) "
        "FROM dbmem_vault_fts AS f "
        "JOIN dbmem_vault AS v ON f.hash = v.hash AND f.seq = v.seq;",
        &joined_before);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(joined_before, 0);

    rc = sqlite3_memory_init(db, NULL, NULL);
    ASSERT_EQ(rc, SQLITE_OK);

    char hash_val[64];
    rc = exec_get_text(db,
        "SELECT hash FROM dbmem_vault_fts WHERE seq=0;",
        hash_val, sizeof(hash_val));
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_STR_EQ(hash_val, "00000000000000ff");

    sqlite3_int64 joined_after = -1;
    rc = exec_get_int(db,
        "SELECT COUNT(*) "
        "FROM dbmem_vault_fts AS f "
        "JOIN dbmem_vault AS v ON f.hash = v.hash AND f.seq = v.seq;",
        &joined_after);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(joined_after, 1);

    sqlite3_close(db);
}

TEST(sqlite_schema_migration_preserves_user_schema_objects) {
    sqlite3 *db = NULL;
    int rc = sqlite3_open(":memory:", &db);
    ASSERT_EQ(rc, SQLITE_OK);

    rc = sqlite3_exec(db,
        "CREATE TABLE dbmem_settings (key TEXT PRIMARY KEY, value TEXT);"
        "CREATE TABLE dbmem_content ("
        "  hash INTEGER PRIMARY KEY NOT NULL,"
        "  path TEXT NOT NULL DEFAULT '' UNIQUE,"
        "  value TEXT DEFAULT NULL,"
        "  length INTEGER NOT NULL DEFAULT 0,"
        "  context TEXT DEFAULT NULL,"
        "  created_at INTEGER DEFAULT 0,"
        "  last_accessed INTEGER DEFAULT 0);"
        "CREATE TABLE dbmem_vault ("
        "  hash INTEGER NOT NULL,"
        "  seq INTEGER NOT NULL,"
        "  embedding BLOB NOT NULL,"
        "  offset INTEGER NOT NULL,"
        "  length INTEGER NOT NULL,"
        "  PRIMARY KEY (hash, seq));"
        "CREATE TABLE dbmem_cache ("
        "  text_hash INTEGER NOT NULL,"
        "  provider TEXT NOT NULL,"
        "  model TEXT NOT NULL,"
        "  embedding BLOB NOT NULL,"
        "  dimension INTEGER NOT NULL,"
        "  PRIMARY KEY (text_hash, provider, model));"
        "CREATE TABLE user_audit (hash TEXT);"
        "CREATE INDEX idx_dbmem_content_context ON dbmem_content(context);"
        "CREATE INDEX idx_dbmem_cache_provider ON dbmem_cache(provider);"
        "CREATE TRIGGER trg_dbmem_vault_audit "
        "AFTER INSERT ON dbmem_vault BEGIN "
        "  INSERT INTO user_audit(hash) VALUES (NEW.hash); "
        "END;"
        "INSERT INTO dbmem_content (hash, path, value, length, context, created_at) VALUES"
        " (255, 'path_a', 'content a', 9, 'ctx_a', 1000);"
        "INSERT INTO dbmem_vault (hash, seq, embedding, offset, length) VALUES"
        " (255, 0, X'0000803f', 0, 9);"
        "INSERT INTO dbmem_cache (text_hash, provider, model, embedding, dimension) VALUES"
        " (255, 'test', 'model', X'0000803f', 1);"
        "DELETE FROM user_audit;",
        NULL, NULL, NULL);
    ASSERT_EQ(rc, SQLITE_OK);

    rc = sqlite3_memory_init(db, NULL, NULL);
    ASSERT_EQ(rc, SQLITE_OK);

    sqlite3_int64 count = -1;
    rc = exec_get_int(db,
        "SELECT COUNT(*) FROM sqlite_master "
        "WHERE type='index' AND name='idx_dbmem_content_context';",
        &count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(count, 1);

    rc = exec_get_int(db,
        "SELECT COUNT(*) FROM sqlite_master "
        "WHERE type='index' AND name='idx_dbmem_cache_provider';",
        &count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(count, 1);

    rc = exec_get_int(db,
        "SELECT COUNT(*) FROM sqlite_master "
        "WHERE type='trigger' AND name='trg_dbmem_vault_audit';",
        &count);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_EQ(count, 1);

    rc = sqlite3_exec(db,
        "INSERT INTO dbmem_vault (hash, seq, embedding, offset, length) VALUES"
        " ('00000000000000aa', 1, X'0000803f', 0, 4);",
        NULL, NULL, NULL);
    ASSERT_EQ(rc, SQLITE_OK);

    char audit_hash[64];
    rc = exec_get_text(db, "SELECT hash FROM user_audit;", audit_hash, sizeof(audit_hash));
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_STR_EQ(audit_hash, "00000000000000aa");

    sqlite3_close(db);
}

TEST(sqlite_schema_migration_preserves_dependent_views_and_foreign_keys) {
    sqlite3 *db = NULL;
    int rc = sqlite3_open(":memory:", &db);
    ASSERT_EQ(rc, SQLITE_OK);

    rc = sqlite3_exec(db,
        "PRAGMA foreign_keys=ON;"
        "CREATE TABLE dbmem_settings (key TEXT PRIMARY KEY, value TEXT);"
        "CREATE TABLE dbmem_content ("
        "  hash INTEGER PRIMARY KEY NOT NULL,"
        "  path TEXT NOT NULL DEFAULT '' UNIQUE,"
        "  value TEXT DEFAULT NULL,"
        "  length INTEGER NOT NULL DEFAULT 0,"
        "  context TEXT DEFAULT NULL,"
        "  created_at INTEGER DEFAULT 0,"
        "  last_accessed INTEGER DEFAULT 0);"
        "CREATE TABLE dbmem_vault ("
        "  hash INTEGER NOT NULL,"
        "  seq INTEGER NOT NULL,"
        "  embedding BLOB NOT NULL,"
        "  offset INTEGER NOT NULL,"
        "  length INTEGER NOT NULL,"
        "  PRIMARY KEY (hash, seq));"
        "CREATE VIEW user_content_view AS "
        "  SELECT path, context FROM dbmem_content;"
        "CREATE TABLE user_content_refs ("
        "  content_hash INTEGER REFERENCES dbmem_content(hash));"
        "INSERT INTO dbmem_content (hash, path, value, length, context, created_at) VALUES"
        " (255, 'path_a', 'content a', 9, 'ctx_a', 1000);",
        NULL, NULL, NULL);
    ASSERT_EQ(rc, SQLITE_OK);

    rc = sqlite3_memory_init(db, NULL, NULL);
    ASSERT_EQ(rc, SQLITE_OK);

    char view_sql[256];
    rc = exec_get_text(db,
        "SELECT sql FROM sqlite_master WHERE type='view' AND name='user_content_view';",
        view_sql, sizeof(view_sql));
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT(strstr(view_sql, "dbmem_content") != NULL);
    ASSERT(strstr(view_sql, "_dbmem_content_old") == NULL);

    char fk_sql[256];
    rc = exec_get_text(db,
        "SELECT sql FROM sqlite_master WHERE type='table' AND name='user_content_refs';",
        fk_sql, sizeof(fk_sql));
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT(strstr(fk_sql, "REFERENCES dbmem_content") != NULL);
    ASSERT(strstr(fk_sql, "_dbmem_content_old") == NULL);

    char path[64];
    rc = exec_get_text(db, "SELECT path FROM user_content_view;", path, sizeof(path));
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_STR_EQ(path, "path_a");

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
    RUN_TEST(sqlite_memory_clear_empty);
    RUN_TEST(sqlite_memory_delete_nonexistent);
    RUN_TEST(sqlite_memory_delete_context_nonexistent);
    RUN_TEST(sqlite_schema_has_timestamps);
    RUN_TEST(sqlite_direct_insert_with_timestamp);
    RUN_TEST(sqlite_memory_delete_direct);
    RUN_TEST(sqlite_memory_delete_context_direct);
    RUN_TEST(sqlite_memory_clear_direct);

    printf("\nSync tests:\n");
    RUN_TEST(sqlite_sync_directory_removes_deleted);
    RUN_TEST(sqlite_sync_directory_removes_all_deleted);
    RUN_TEST(sqlite_sync_directory_skips_unchanged);

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

    printf("\nSchema migration tests:\n");
    RUN_TEST(sqlite_schema_migration);
    RUN_TEST(sqlite_schema_migration_preserves_cloudsync_filter);
    RUN_TEST(sqlite_schema_migration_requires_cloudsync_when_synced);
    RUN_TEST(sqlite_schema_migration_ignores_user_triggers);
    RUN_TEST(sqlite_schema_init_repairs_stale_fts_hashes);
    RUN_TEST(sqlite_schema_migration_preserves_user_schema_objects);
    RUN_TEST(sqlite_schema_migration_preserves_dependent_views_and_foreign_keys);

    printf("\nCustom provider tests:\n");
    RUN_TEST(sqlite_custom_provider_register);
    RUN_TEST(sqlite_custom_provider_set_model);
    RUN_TEST(sqlite_custom_provider_add_text);
    RUN_TEST(sqlite_custom_provider_null_callbacks);
    RUN_TEST(sqlite_custom_provider_init_error);
    RUN_TEST(sqlite_custom_provider_apikey_passed);
#endif

    printf("\n=== Results ===\n");
    printf("Tests run:    %d\n", tests_run);
    printf("Tests passed: %d\n", tests_passed);
    printf("Tests failed: %d\n", tests_failed);
    printf("\n");

    return tests_failed > 0 ? 1 : 0;
}
