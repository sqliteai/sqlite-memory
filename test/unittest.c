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
#else
#include <unistd.h>
#define mkdir_p(path) mkdir(path, 0755)
#define rmdir_p(path) rmdir(path)
#endif

#include "dbmem-utils.h"
#include "dbmem-parser.h"

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
    return 1;  // Continue scanning
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
    const char *test_dir = "/tmp/dbmem_test_empty";
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
    const char *test_dir = "/tmp/dbmem_test_single";
    const char *test_file = "/tmp/dbmem_test_single/file.txt";

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
    const char *test_dir = "/tmp/dbmem_test_multi";

    // Clean up first
    remove("/tmp/dbmem_test_multi/a.txt");
    remove("/tmp/dbmem_test_multi/b.txt");
    remove("/tmp/dbmem_test_multi/c.md");
    rmdir_p(test_dir);

    mkdir_p(test_dir);
    create_test_file("/tmp/dbmem_test_multi/a.txt", "a");
    create_test_file("/tmp/dbmem_test_multi/b.txt", "b");
    create_test_file("/tmp/dbmem_test_multi/c.md", "c");

    scan_ctx_t ctx = {0};
    int rc = dbmem_dir_scan(test_dir, scan_callback, &ctx);

    ASSERT_EQ(rc, 0);
    ASSERT_EQ(ctx.count, 3);
    ASSERT(path_contains(&ctx, "a.txt"));
    ASSERT(path_contains(&ctx, "b.txt"));
    ASSERT(path_contains(&ctx, "c.md"));

    free_scan_ctx(&ctx);
    remove("/tmp/dbmem_test_multi/a.txt");
    remove("/tmp/dbmem_test_multi/b.txt");
    remove("/tmp/dbmem_test_multi/c.md");
    rmdir_p(test_dir);
}

TEST(dbmem_dir_scan_recursive) {
    // Create nested directory structure
    const char *base = "/tmp/dbmem_test_recursive";
    const char *sub1 = "/tmp/dbmem_test_recursive/sub1";
    const char *sub2 = "/tmp/dbmem_test_recursive/sub2";
    const char *subsub = "/tmp/dbmem_test_recursive/sub1/subsub";

    // Clean up first
    remove("/tmp/dbmem_test_recursive/root.txt");
    remove("/tmp/dbmem_test_recursive/sub1/file1.txt");
    remove("/tmp/dbmem_test_recursive/sub1/subsub/deep.txt");
    remove("/tmp/dbmem_test_recursive/sub2/file2.txt");
    rmdir_p(subsub);
    rmdir_p(sub1);
    rmdir_p(sub2);
    rmdir_p(base);

    // Create structure
    mkdir_p(base);
    mkdir_p(sub1);
    mkdir_p(sub2);
    mkdir_p(subsub);

    create_test_file("/tmp/dbmem_test_recursive/root.txt", "root");
    create_test_file("/tmp/dbmem_test_recursive/sub1/file1.txt", "file1");
    create_test_file("/tmp/dbmem_test_recursive/sub2/file2.txt", "file2");
    create_test_file("/tmp/dbmem_test_recursive/sub1/subsub/deep.txt", "deep");

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
    remove("/tmp/dbmem_test_recursive/root.txt");
    remove("/tmp/dbmem_test_recursive/sub1/file1.txt");
    remove("/tmp/dbmem_test_recursive/sub1/subsub/deep.txt");
    remove("/tmp/dbmem_test_recursive/sub2/file2.txt");
    rmdir_p(subsub);
    rmdir_p(sub1);
    rmdir_p(sub2);
    rmdir_p(base);
}

TEST(dbmem_dir_scan_skips_hidden) {
    // Create directory with hidden files (dot files)
    const char *test_dir = "/tmp/dbmem_test_hidden";

    remove("/tmp/dbmem_test_hidden/visible.txt");
    remove("/tmp/dbmem_test_hidden/.hidden");
    rmdir_p(test_dir);

    mkdir_p(test_dir);
    create_test_file("/tmp/dbmem_test_hidden/visible.txt", "visible");
    create_test_file("/tmp/dbmem_test_hidden/.hidden", "hidden");

    scan_ctx_t ctx = {0};
    int rc = dbmem_dir_scan(test_dir, scan_callback, &ctx);

    ASSERT_EQ(rc, 0);
    ASSERT_EQ(ctx.count, 1);  // Only visible file
    ASSERT(path_contains(&ctx, "visible.txt"));
    ASSERT(!path_contains(&ctx, ".hidden"));

    free_scan_ctx(&ctx);
    remove("/tmp/dbmem_test_hidden/visible.txt");
    remove("/tmp/dbmem_test_hidden/.hidden");
    rmdir_p(test_dir);
}

TEST(dbmem_dir_scan_skips_hidden_dirs) {
    // Create directory with hidden subdirectory
    const char *test_dir = "/tmp/dbmem_test_hidden_dir";
    const char *hidden_dir = "/tmp/dbmem_test_hidden_dir/.hidden_dir";

    remove("/tmp/dbmem_test_hidden_dir/visible.txt");
    remove("/tmp/dbmem_test_hidden_dir/.hidden_dir/secret.txt");
    rmdir_p(hidden_dir);
    rmdir_p(test_dir);

    mkdir_p(test_dir);
    mkdir_p(hidden_dir);
    create_test_file("/tmp/dbmem_test_hidden_dir/visible.txt", "visible");
    create_test_file("/tmp/dbmem_test_hidden_dir/.hidden_dir/secret.txt", "secret");

    scan_ctx_t ctx = {0};
    int rc = dbmem_dir_scan(test_dir, scan_callback, &ctx);

    ASSERT_EQ(rc, 0);
    ASSERT_EQ(ctx.count, 1);  // Only visible file, hidden dir not scanned
    ASSERT(path_contains(&ctx, "visible.txt"));
    ASSERT(!path_contains(&ctx, "secret.txt"));

    free_scan_ctx(&ctx);
    remove("/tmp/dbmem_test_hidden_dir/visible.txt");
    remove("/tmp/dbmem_test_hidden_dir/.hidden_dir/secret.txt");
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
    int rc = dbmem_dir_scan("/tmp", NULL, NULL);
    ASSERT_EQ(rc, -1);
}

TEST(dbmem_dir_scan_nonexistent) {
    scan_ctx_t ctx = {0};
    int rc = dbmem_dir_scan("/tmp/nonexistent_dir_12345", scan_callback, &ctx);
    ASSERT_EQ(rc, -1);
    free_scan_ctx(&ctx);
}

static int abort_scan_callback(const char *path, void *data) {
    UNUSED_PARAM(path);
    int *count = (int *)data;
    (*count)++;
    if (*count >= 2) return 0;  // Abort after 2 files
    return 1;
}

TEST(dbmem_dir_scan_callback_abort) {
    // Create directory with multiple files
    const char *test_dir = "/tmp/dbmem_test_abort";

    remove("/tmp/dbmem_test_abort/a.txt");
    remove("/tmp/dbmem_test_abort/b.txt");
    remove("/tmp/dbmem_test_abort/c.txt");
    rmdir_p(test_dir);

    mkdir_p(test_dir);
    create_test_file("/tmp/dbmem_test_abort/a.txt", "a");
    create_test_file("/tmp/dbmem_test_abort/b.txt", "b");
    create_test_file("/tmp/dbmem_test_abort/c.txt", "c");

    int count = 0;
    int rc = dbmem_dir_scan(test_dir, abort_scan_callback, &count);

    ASSERT_EQ(rc, -1);  // Should return error when callback aborts
    ASSERT_EQ(count, 2);  // Should have stopped after 2 files

    remove("/tmp/dbmem_test_abort/a.txt");
    remove("/tmp/dbmem_test_abort/b.txt");
    remove("/tmp/dbmem_test_abort/c.txt");
    rmdir_p(test_dir);
}

TEST(dbmem_dir_scan_trailing_slash) {
    // Test with trailing slash in path
    const char *test_dir = "/tmp/dbmem_test_slash";

    remove("/tmp/dbmem_test_slash/file.txt");
    rmdir_p(test_dir);

    mkdir_p(test_dir);
    create_test_file("/tmp/dbmem_test_slash/file.txt", "test");

    scan_ctx_t ctx = {0};
    int rc = dbmem_dir_scan("/tmp/dbmem_test_slash/", scan_callback, &ctx);

    ASSERT_EQ(rc, 0);
    ASSERT_EQ(ctx.count, 1);
    ASSERT(path_contains(&ctx, "file.txt"));

    free_scan_ctx(&ctx);
    remove("/tmp/dbmem_test_slash/file.txt");
    rmdir_p(test_dir);
}

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

    printf("\n=== Results ===\n");
    printf("Tests run:    %d\n", tests_run);
    printf("Tests passed: %d\n", tests_passed);
    printf("Tests failed: %d\n", tests_failed);
    printf("\n");

    return tests_failed > 0 ? 1 : 0;
}
