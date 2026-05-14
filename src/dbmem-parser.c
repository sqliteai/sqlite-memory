//
//  dbmem-parser.c
//  sqlitememory
//
//  Created by Marco Bambini on 05/02/26.
//

/*
 * Semantic Markdown chunking with md4c (token-based)
 *
 * Splits a Markdown buffer into semantic chunks along heading boundaries,
 * enforcing a maximum token budget per chunk and applying a configurable
 * token overlap between consecutive chunks.
 *
 * Token estimation: 1 token ≈ chars_per_token characters (default 4).
 *
 * Depends on: md4c.h (from https://github.com/mity/md4c)
 */

#include "dbmem-parser.h"
#include "dbmem-utils.h"
#include "md4c.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

// A section represents a heading + its content (or content without heading)
typedef struct {
    size_t  start;              // Byte offset in source buffer
    size_t  end;                // Byte end in source buffer
    int     is_heading;         // True if this section starts with a heading block
    char   *text;               // Stripped plain text (allocated)
    size_t  text_len;           // Length of stripped text
} section_t;

// md4c parse context - tracks state during markdown parsing
typedef struct {
    // Source buffer
    const char *src;            // Original markdown buffer being parsed
    size_t      src_len;        // Length of source buffer in bytes

    // Section accumulation
    section_t  *sections;       // Dynamic array of parsed sections
    size_t      sec_count;      // Number of sections in array
    size_t      sec_cap;        // Allocated capacity of sections array

    // Block nesting state (for md4c callbacks)
    int         depth;          // Current block nesting depth (0 = document level)

    // Current block tracking (reset on each top-level block)
    int         cur_is_heading; // True if current block is a heading (MD_BLOCK_H)
    size_t      cur_text_min;   // Minimum byte offset of text seen in current block
    size_t      cur_text_max;   // Maximum byte offset of text seen in current block
    int         cur_has_text;   // True if any text has been seen in current block
} parse_ctx_t;

typedef struct {
    size_t sec_idx;             // Which section this chunk comes from
    size_t text_off;            // Offset within section's stripped text
    size_t text_len;            // Length within section's stripped text
} raw_chunk_t;

// Context for markdown stripping (passed to helper functions)
typedef struct {
    char       *buf;            // Output buffer
    size_t      wp;             // Write position in buffer
    const char *line_end;       // End of current line
    bool        skip_html;      // Whether to skip HTML tags
    bool        mdx_mode;       // Whether to strip MDX-specific syntax
    int         in_html_tag;    // Currently inside multi-line HTML tag
    int         in_fenced_code; // Currently inside fenced code block
    char        fence_char;     // Fence character (` or ~)
    int         fence_width;    // Number of fence characters
    int         in_mdx_expr;    // Currently inside a multi-line MDX expression
    int         mdx_expr_depth; // Nested brace depth for MDX expressions
    char        mdx_expr_quote; // Active quote inside MDX expression
    int         mdx_expr_block_comment;
    int         in_mdx_esm;     // Currently skipping a multi-line ESM statement
    int         mdx_esm_depth;  // Nested delimiter depth for ESM statements
    char        mdx_esm_quote;  // Active quote inside ESM statement
    int         mdx_esm_block_comment;
} strip_ctx_t;

// MARK: - Helpers -

static inline size_t tokens_to_chars (size_t tokens, size_t chars_per_token) {
    return tokens * chars_per_token;
}

static size_t line_start_of (const char *buf, size_t pos) {
    while (pos > 0 && buf[pos - 1] != '\n') --pos;
    return pos;
}

static size_t line_end_of (const char *buf, size_t len, size_t pos) {
    while (pos < len && buf[pos] != '\n') ++pos;
    if (pos < len) ++pos;
    return pos;
}

// Return 1 if the line is a table separator row (|---|---|)
static int is_table_separator (const char *line, size_t len) {
    int has_dash = 0;
    
    for (size_t i = 0; i < len; i++) {
        char c = line[i];
        if (c == '-') has_dash = 1;
        else if (c != '|' && c != ':' && c != ' ' && c != '\t') return 0;
    }
    
    return has_dash;
}

// Find split point within text that fits in max_chars, preferring newline boundaries
static size_t find_split (const char *text, size_t len, size_t max_chars) {
    if (len <= max_chars) return len;
    
    size_t pos = max_chars;
    while (pos > 0 && text[pos - 1] != '\n') --pos;
    
    return pos > 0 ? pos : max_chars;
}

// Push a section to dynamic array
static int section_push (parse_ctx_t *ctx, size_t start, size_t end, int is_heading) {
    if (ctx->sec_count >= ctx->sec_cap) {
        size_t new_cap = ctx->sec_cap ? ctx->sec_cap * 2 : 16;
        section_t *tmp = (section_t *)dbmemory_realloc(ctx->sections, new_cap * sizeof(section_t));
        if (!tmp) return -1;
        ctx->sections = tmp;
        ctx->sec_cap = new_cap;
    }
    
    section_t *s = &ctx->sections[ctx->sec_count++];
    s->start = start;
    s->end = end;
    s->is_heading = is_heading;
    s->text = NULL;
    s->text_len = 0;
    
    return 0;
}

// MARK: - Markdown Stripping -

// Skip up to 3 leading spaces, return new position
static const char *skip_leading_spaces (const char *p, const char *end, int max) {
    int count = 0;
    
    while (p < end && *p == ' ' && count < max) { ++p; ++count; }
    return p;
}

// Find matching bracket, handling nesting. Returns position of closing bracket or end.
static const char *find_bracket_end (const char *start, const char *end) {
    int depth = 1;
    const char *p = start;
    
    while (p < end && depth > 0) {
        if (*p == '[') ++depth;
        else if (*p == ']') --depth;
        if (depth > 0) ++p;
    }
    
    return p;
}

// Skip content until closing paren or bracket
static const char *skip_until (const char *p, const char *end, char c) {
    while (p < end && *p != c) ++p;
    return (p < end) ? p + 1 : p;
}

static const char *skip_spaces_tabs (const char *p, const char *end) {
    while (p < end && (*p == ' ' || *p == '\t')) ++p;
    return p;
}

static int is_ident_continue (char c) {
    return (isalnum((unsigned char)c) || c == '_' || c == '$');
}

static int starts_keyword (const char *p, const char *end, const char *kw) {
    size_t n = strlen(kw);
    if ((size_t)(end - p) < n) return 0;
    if (strncmp(p, kw, n) != 0) return 0;
    return (p + n >= end || !is_ident_continue(p[n]));
}

static const char *skip_identifier (const char *p, const char *end) {
    if (p >= end || !is_ident_continue(*p)) return NULL;
    while (p < end && is_ident_continue(*p)) ++p;
    return p;
}

static const char *mdx_top_level_start (const char *p, const char *end) {
    int spaces = 0;
    while (p < end && *p == ' ') { ++p; ++spaces; }
    if (spaces > 3 || (p < end && *p == '\t')) return NULL;
    return p;
}

static int mdx_has_from_keyword (const char *p, const char *end) {
    char quote = 0;
    int in_block_comment = 0;

    while (p < end) {
        char c = *p;

        if (quote) {
            if (c == '\\' && p + 1 < end) {
                p += 2;
                continue;
            }
            if (c == quote) quote = 0;
            ++p;
            continue;
        }

        if (in_block_comment) {
            if (c == '*' && p + 1 < end && p[1] == '/') {
                in_block_comment = 0;
                p += 2;
                continue;
            }
            ++p;
            continue;
        }

        if (c == '/' && p + 1 < end) {
            if (p[1] == '/') break;
            if (p[1] == '*') {
                in_block_comment = 1;
                p += 2;
                continue;
            }
        }

        if (c == '\'' || c == '"' || c == '`') {
            quote = c;
            ++p;
            continue;
        }

        if (starts_keyword(p, end, "from")) return 1;
        ++p;
    }

    return 0;
}

static int mdx_line_starts_esm (const char *p, const char *end) {
    p = mdx_top_level_start(p, end);
    if (!p) return 0;

    if (starts_keyword(p, end, "import")) {
        p += strlen("import");
        if (p >= end || (*p != ' ' && *p != '\t')) return 0;
        p = skip_spaces_tabs(p, end);
        if (p < end && (*p == '\'' || *p == '"')) return 1;
        if (p >= end) return 0;
        if (*p == '{' || *p == '*') return mdx_has_from_keyword(p, end);

        const char *ident_end = skip_identifier(p, end);
        if (!ident_end) return 0;

        if ((size_t)(ident_end - p) == 4 && strncmp(p, "type", 4) == 0) {
            p = skip_spaces_tabs(ident_end, end);
            if (p < end && (*p == '{' || *p == '*')) return mdx_has_from_keyword(p, end);
            ident_end = skip_identifier(p, end);
            if (!ident_end) return 0;
        }

        p = skip_spaces_tabs(ident_end, end);
        if (starts_keyword(p, end, "from")) return 1;
        if (p < end && *p == ',') return mdx_has_from_keyword(p + 1, end);
        return 0;
    }

    if (starts_keyword(p, end, "export")) {
        p += strlen("export");
        if (p >= end || (*p != ' ' && *p != '\t')) return 0;
        p = skip_spaces_tabs(p, end);
        if (p >= end) return 0;
        if (*p == '{' || *p == '*') return 1;
        return starts_keyword(p, end, "const") ||
               starts_keyword(p, end, "let") ||
               starts_keyword(p, end, "var") ||
               starts_keyword(p, end, "function") ||
               starts_keyword(p, end, "class") ||
               starts_keyword(p, end, "default") ||
               starts_keyword(p, end, "async") ||
               starts_keyword(p, end, "type") ||
               starts_keyword(p, end, "interface") ||
               starts_keyword(p, end, "enum");
    }

    return 0;
}

static int mdx_update_esm_state (strip_ctx_t *ctx, const char *p, const char *end) {
    int saw_semicolon = 0;

    while (p < end) {
        char c = *p;

        if (ctx->mdx_esm_quote) {
            if (c == '\\' && p + 1 < end) {
                p += 2;
                continue;
            }
            if (c == ctx->mdx_esm_quote) ctx->mdx_esm_quote = 0;
            ++p;
            continue;
        }

        if (ctx->mdx_esm_block_comment) {
            if (c == '*' && p + 1 < end && p[1] == '/') {
                ctx->mdx_esm_block_comment = 0;
                p += 2;
                continue;
            }
            ++p;
            continue;
        }

        if (c == '/' && p + 1 < end) {
            if (p[1] == '/') break;
            if (p[1] == '*') {
                ctx->mdx_esm_block_comment = 1;
                p += 2;
                continue;
            }
        }

        if (c == '\'' || c == '"' || c == '`') {
            ctx->mdx_esm_quote = c;
            ++p;
            continue;
        }

        if (c == '(' || c == '[' || c == '{') {
            ctx->mdx_esm_depth++;
        } else if (c == ')' || c == ']' || c == '}') {
            if (ctx->mdx_esm_depth > 0) ctx->mdx_esm_depth--;
        } else if (c == ';' && ctx->mdx_esm_depth == 0) {
            saw_semicolon = 1;
        }

        ++p;
    }

    if (ctx->mdx_esm_quote || ctx->mdx_esm_block_comment || ctx->mdx_esm_depth > 0) return 0;
    return saw_semicolon || ctx->mdx_esm_depth == 0;
}

static const char *mdx_skip_expression (strip_ctx_t *ctx, const char *p, const char *end) {
    if (!ctx->in_mdx_expr) {
        ctx->in_mdx_expr = 1;
        ctx->mdx_expr_depth = 1;
        ctx->mdx_expr_quote = 0;
        ctx->mdx_expr_block_comment = 0;
        ++p;
    }

    while (p < end) {
        char c = *p;

        if (ctx->mdx_expr_quote) {
            if (c == '\\' && p + 1 < end) {
                p += 2;
                continue;
            }
            if (c == ctx->mdx_expr_quote) ctx->mdx_expr_quote = 0;
            ++p;
            continue;
        }

        if (ctx->mdx_expr_block_comment) {
            if (c == '*' && p + 1 < end && p[1] == '/') {
                ctx->mdx_expr_block_comment = 0;
                p += 2;
                continue;
            }
            ++p;
            continue;
        }

        if (c == '/' && p + 1 < end) {
            if (p[1] == '/') break;
            if (p[1] == '*') {
                ctx->mdx_expr_block_comment = 1;
                p += 2;
                continue;
            }
        }

        if (c == '\'' || c == '"' || c == '`') {
            ctx->mdx_expr_quote = c;
            ++p;
            continue;
        }

        if (c == '{') {
            ctx->mdx_expr_depth++;
        } else if (c == '}') {
            ctx->mdx_expr_depth--;
            ++p;
            if (ctx->mdx_expr_depth <= 0) {
                ctx->in_mdx_expr = 0;
                ctx->mdx_expr_depth = 0;
                return p;
            }
            continue;
        }

        ++p;
    }

    return p;
}

// Check if line starts a fenced code block. Returns fence width (0 if not a fence).
static int check_fence_start (const char *p, const char *end, char *out_char) {
    p = skip_leading_spaces(p, end, 3);
    if (p >= end || (*p != '`' && *p != '~')) return 0;
    char fc = *p;
    int fw = 0;
    
    while (p < end && *p == fc) { ++p; ++fw; }
    if (fw >= 3) {
        *out_char = fc;
        return fw;
    }
    
    return 0;
}

// Check if line closes a fenced code block
static int check_fence_end (const char *p, const char *end, char fence_char, int fence_width) {
    p = skip_leading_spaces(p, end, 3);
    if (p >= end || *p != fence_char) return 0;
    int fw = 0;
    while (p < end && *p == fence_char) { ++p; ++fw; }
    // Rest must be only whitespace
    while (p < end) {
        if (*p != ' ' && *p != '\t') return 0;
        ++p;
    }
    return fw >= fence_width;
}

// Check if line is a thematic break (---, ***, ___)
static int is_thematic_break (const char *p, const char *end) {
    p = skip_leading_spaces(p, end, 3);
    if (p >= end) return 0;
    if (*p != '-' && *p != '*' && *p != '_') return 0;
    char tc = *p;
    int cnt = 0;
    while (p < end) {
        if (*p == tc) ++cnt;
        else if (*p != ' ' && *p != '\t') return 0;
        ++p;
    }
    return cnt >= 3;
}

// Skip blockquote markers (> > >), return position after markers
static const char *skip_blockquote_markers (const char *p, const char *end) {
    p = skip_leading_spaces(p, end, 3);
    while (p < end && *p == '>') {
        ++p;
        if (p < end && *p == ' ') ++p;
    }
    return p;
}

// Try to parse ATX heading, returns content start and adjusts line_end for trailing #
static const char *try_parse_atx_heading (const char *p, const char *end, const char **new_end) {
    p = skip_leading_spaces(p, end, 3);
    if (p >= end || *p != '#') return NULL;
    int level = 0;
    
    while (p < end && *p == '#' && level < 7) { ++p; ++level; }
    if (level < 1 || level > 6) return NULL;
    if (p < end && *p != ' ' && *p != '\t') return NULL;
    
    // Skip spaces after #
    while (p < end && (*p == ' ' || *p == '\t')) ++p;
    
    // Trim trailing # and spaces
    const char *re = end;
    while (re > p && (re[-1] == ' ' || re[-1] == '\t')) --re;
    while (re > p && re[-1] == '#') --re;
    while (re > p && (re[-1] == ' ' || re[-1] == '\t')) --re;
    *new_end = re;
    
    return p;
}

// Try to parse unordered list marker (-, *, +), returns content start
static const char *try_parse_unordered_list (const char *p, const char *end) {
    p = skip_leading_spaces(p, end, 3);
    if (p >= end) return NULL;
    if (*p != '-' && *p != '*' && *p != '+') return NULL;
    const char *after = p + 1;
    if (after >= end || (*after != ' ' && *after != '\t')) return NULL;
    while (after < end && (*after == ' ' || *after == '\t')) ++after;
    return after;
}

// Try to parse ordered list marker (1. or 1)), returns content start
static const char *try_parse_ordered_list (const char *p, const char *end) {
    p = skip_leading_spaces(p, end, 3);
    
    if (p >= end || *p < '0' || *p > '9') return NULL;
    while (p < end && *p >= '0' && *p <= '9') ++p;
    if (p >= end || (*p != '.' && *p != ')')) return NULL;
    ++p;
    
    if (p >= end || (*p != ' ' && *p != '\t')) return NULL;
    while (p < end && (*p == ' ' || *p == '\t')) ++p;
    return p;
}

// Process a link or image: [text](url) or [text][ref] or [text]
// Writes text content to buffer, returns position after the construct
static const char *process_bracket_construct (const char *p, const char *end, char *buf, size_t *wp, int is_image) {
    const char *text_start = p + (is_image ? 2 : 1);  // Skip ![ or [
    const char *text_end = find_bracket_end(text_start, end);

    if (text_end >= end || *text_end != ']') return NULL;

    // Copy the text content
    size_t text_len = (size_t)(text_end - text_start);
    memcpy(buf + *wp, text_start, text_len);
    *wp += text_len;

    // Check what follows the ]
    if (text_end + 1 < end) {
        if (text_end[1] == '(') {
            // Inline: [text](url) - skip to closing )
            return skip_until(text_end + 2, end, ')');
        }
        if (text_end[1] == '[') {
            // Reference: [text][ref] - skip to closing ]
            return skip_until(text_end + 2, end, ']');
        }
    }
    // Shortcut reference: [text]
    return text_end + 1;
}

// Process inline code span, returns position after code span
static const char *process_inline_code (const char *p, const char *end, char *buf, size_t *wp) {
    int backticks = 0;
    const char *cs = p;
    while (cs < end && *cs == '`') { ++cs; ++backticks; }

    // Find matching closing backticks
    const char *ce = cs;
    while (ce + backticks <= end) {
        if (*ce == '`') {
            int cb = 0;
            const char *t = ce;
            while (t < end && *t == '`') { ++t; ++cb; }
            if (cb == backticks) {
                // Found matching close - copy content
                memcpy(buf + *wp, cs, (size_t)(ce - cs));
                *wp += (size_t)(ce - cs);
                return t;
            }
            ce = t;
        } else {
            ++ce;
        }
    }
    return NULL;  // No matching close found
}

// Process inline content (links, images, code, emphasis, etc.)
static void process_inline (strip_ctx_t *ctx, const char *start, const char *end) {
    const char *p = start;

    while (p < end) {
        // Continue skipping a multi-line MDX expression.
        if (ctx->mdx_mode && ctx->in_mdx_expr) {
            p = mdx_skip_expression(ctx, p, end);
            continue;
        }

        // Escaped opening braces are literal text in MDX.
        if (ctx->mdx_mode && *p == '\\' && p + 1 < end && p[1] == '{') {
            ctx->buf[ctx->wp++] = '{';
            p += 2;
            continue;
        }

        // MDX expression: remove the JavaScript expression but keep nearby text.
        if (ctx->mdx_mode && *p == '{') {
            p = mdx_skip_expression(ctx, p, end);
            continue;
        }

        // HTML tags
        if (ctx->skip_html && *p == '<') {
            const char *gt = p + 1;
            while (gt < end && *gt != '>') ++gt;
            if (gt < end) {
                p = gt + 1;
                continue;
            }
            ctx->in_html_tag = 1;
            return;
        }

        // Images ![alt](url)
        if (*p == '!' && p + 1 < end && p[1] == '[') {
            const char *next = process_bracket_construct(p, end, ctx->buf, &ctx->wp, 1);
            if (next) { p = next; continue; }
        }

        // Links [text](url)
        if (*p == '[') {
            const char *next = process_bracket_construct(p, end, ctx->buf, &ctx->wp, 0);
            if (next) { p = next; continue; }
        }

        // Inline code
        if (*p == '`') {
            const char *next = process_inline_code(p, end, ctx->buf, &ctx->wp);
            if (next) { p = next; continue; }
        }

        // Bold/italic markers (* or _)
        if (*p == '*' || *p == '_') {
            char mc = *p;
            const char *ms = p;
            while (p < end && *p == mc) ++p;
            int n = (int)(p - ms);
            // Only preserve if more than 3 (unusual case)
            if (n > 3) {
                memcpy(ctx->buf + ctx->wp, ms, (size_t)n);
                ctx->wp += (size_t)n;
            }
            continue;
        }

        // Strikethrough ~~
        if (*p == '~' && p + 1 < end && p[1] == '~') {
            p += 2;
            continue;
        }

        // Table pipes
        if (*p == '|') {
            ++p;
            continue;
        }

        // Regular character
        ctx->buf[ctx->wp++] = *p++;
    }
}

// Main markdown stripping function
static char *strip_markdown (const char *src, size_t len, size_t *out_len, bool skip_html, bool mdx_mode) {
    char *buf = (char *)dbmemory_alloc(len + 1);
    if (!buf) return NULL;

    strip_ctx_t ctx = {
        .buf = buf,
        .wp = 0,
        .skip_html = skip_html,
        .mdx_mode = mdx_mode,
        .in_html_tag = 0,
        .in_fenced_code = 0,
        .fence_char = 0,
        .fence_width = 0,
        .in_mdx_expr = 0,
        .mdx_expr_depth = 0,
        .mdx_expr_quote = 0,
        .mdx_expr_block_comment = 0,
        .in_mdx_esm = 0,
        .mdx_esm_depth = 0,
        .mdx_esm_quote = 0,
        .mdx_esm_block_comment = 0
    };

    const char *p = src;
    const char *end = src + len;

    while (p < end) {
        // Find line boundaries
        const char *nl = p;
        while (nl < end && *nl != '\n') ++nl;
        const char *line_end = nl;
        size_t wp_line_start = ctx.wp;

        // Handle multi-line HTML tag continuation
        if (ctx.skip_html && ctx.in_html_tag) {
            const char *gt = p;
            while (gt < line_end && *gt != '>') ++gt;
            if (gt < line_end) {
                ctx.in_html_tag = 0;
                p = gt + 1;
                if (p >= nl) { p = (nl < end) ? nl + 1 : nl; continue; }
            } else {
                p = (nl < end) ? nl + 1 : nl;
                continue;
            }
        }

        // Handle fenced code blocks
        if (!ctx.in_fenced_code) {
            char fc;
            int fw = check_fence_start(p, line_end, &fc);
            if (fw > 0) {
                ctx.in_fenced_code = 1;
                ctx.fence_char = fc;
                ctx.fence_width = fw;
                p = (nl < end) ? nl + 1 : nl;
                continue;
            }
        } else {
            if (check_fence_end(p, line_end, ctx.fence_char, ctx.fence_width)) {
                ctx.in_fenced_code = 0;
                p = (nl < end) ? nl + 1 : nl;
                continue;
            }
            // Inside code block - copy verbatim
            size_t line_len = (size_t)(line_end - p);
            memcpy(buf + ctx.wp, p, line_len);
            ctx.wp += line_len;
            buf[ctx.wp++] = '\n';
            p = (nl < end) ? nl + 1 : nl;
            continue;
        }

        // MDX top-level ESM (import/export) is scaffolding, not searchable prose.
        if (ctx.mdx_mode) {
            if (ctx.in_mdx_esm) {
                if (mdx_update_esm_state(&ctx, p, line_end)) ctx.in_mdx_esm = 0;
                p = (nl < end) ? nl + 1 : nl;
                continue;
            }

            if (mdx_line_starts_esm(p, line_end)) {
                ctx.in_mdx_esm = !mdx_update_esm_state(&ctx, p, line_end);
                p = (nl < end) ? nl + 1 : nl;
                continue;
            }
        }

        // Blank lines
        const char *tp = p;
        while (tp < line_end && (*tp == ' ' || *tp == '\t')) ++tp;
        if (tp == line_end) {
            buf[ctx.wp++] = '\n';
            p = (nl < end) ? nl + 1 : nl;
            continue;
        }

        // Thematic breaks
        if (is_thematic_break(p, line_end)) {
            p = (nl < end) ? nl + 1 : nl;
            continue;
        }

        // Table separator rows
        if (is_table_separator(p, (size_t)(line_end - p))) {
            p = (nl < end) ? nl + 1 : nl;
            continue;
        }

        // Skip blockquote markers
        const char *content_start = skip_blockquote_markers(p, line_end);
        const char *content_end = line_end;

        // Try ATX heading
        const char *heading_content = try_parse_atx_heading(content_start, line_end, &content_end);
        if (heading_content) {
            content_start = heading_content;
        } else {
            // Try list markers
            const char *list_content = try_parse_unordered_list(content_start, line_end);
            if (!list_content) {
                list_content = try_parse_ordered_list(content_start, line_end);
            }
            if (list_content) {
                content_start = list_content;
            }
        }

        // Process inline content
        ctx.line_end = content_end;
        process_inline(&ctx, content_start, content_end);

        // Trim trailing spaces and add newline
        while (ctx.wp > 0 && buf[ctx.wp - 1] == ' ') --ctx.wp;
        if (!ctx.in_html_tag || ctx.wp > wp_line_start) {
            buf[ctx.wp++] = '\n';
        }
        p = (nl < end) ? nl + 1 : nl;
    }

    // Trim trailing whitespace
    while (ctx.wp > 0 && (buf[ctx.wp - 1] == '\n' || buf[ctx.wp - 1] == ' ')) --ctx.wp;
    buf[ctx.wp] = '\0';

    if (out_len) *out_len = ctx.wp;
    return buf;
}

// MARK: - md4c Callbacks -

static int cb_enter_block (MD_BLOCKTYPE type, void *detail, void *ud) {
    UNUSED_PARAM(detail);
    parse_ctx_t *c = (parse_ctx_t *)ud;
    if (type == MD_BLOCK_DOC) return 0;
    c->depth++;
    if (c->depth == 1) {
        c->cur_has_text = 0;
        c->cur_is_heading = (type == MD_BLOCK_H);
    }
    return 0;
}

static int cb_leave_block (MD_BLOCKTYPE type, void *detail, void *ud) {
    parse_ctx_t *c = (parse_ctx_t *)ud;
    (void)detail;
    if (type == MD_BLOCK_DOC) return 0;
    if (c->depth == 1 && c->cur_has_text) {
        size_t start = line_start_of(c->src, c->cur_text_min);
        size_t end = line_end_of(c->src, c->src_len, c->cur_text_max);
        if (section_push(c, start, end, c->cur_is_heading) != 0) return -1;
    }
    c->depth--;
    return 0;
}

static int cb_enter_span (MD_SPANTYPE t, void *d, void *u) {
    UNUSED_PARAM(t);
    UNUSED_PARAM(d);
    UNUSED_PARAM(u);
    return 0;
}
static int cb_leave_span (MD_SPANTYPE t, void *d, void *u) {
    UNUSED_PARAM(t);
    UNUSED_PARAM(d);
    UNUSED_PARAM(u);
    return 0;
}

static int cb_text (MD_TEXTTYPE type, const MD_CHAR *text, MD_SIZE size, void *ud) {
    parse_ctx_t *c = (parse_ctx_t *)ud;
    (void)type;
    if (c->depth < 1 || size == 0) return 0;
    size_t off = (size_t)(text - c->src);
    if (off > c->src_len) return 0;
    size_t e = off + (size_t)size;
    if (!c->cur_has_text) {
        c->cur_text_min = off;
        c->cur_text_max = e;
        c->cur_has_text = 1;
    } else {
        if (off < c->cur_text_min) c->cur_text_min = off;
        if (e > c->cur_text_max) c->cur_text_max = e;
    }
    return 0;
}

// MARK: - Section Parsing -

// Parse buffer into sections using md4c, or treat as single section if skip_semantic
static int parse_sections (const char *buffer, size_t buffer_size, bool skip_semantic, parse_ctx_t *ctx) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->src = buffer;
    ctx->src_len = buffer_size;

    if (skip_semantic) {
        // Raw mode: entire buffer is one section
        return section_push(ctx, 0, buffer_size, 0);
    }

    // Parse with md4c
    MD_PARSER parser = {0};
    parser.flags = MD_FLAG_TABLES | MD_FLAG_STRIKETHROUGH | MD_FLAG_TASKLISTS;
    parser.enter_block = cb_enter_block;
    parser.leave_block = cb_leave_block;
    parser.enter_span = cb_enter_span;
    parser.leave_span = cb_leave_span;
    parser.text = cb_text;

    if (md_parse(buffer, (MD_SIZE)buffer_size, &parser, ctx) != 0) {
        dbmemory_free(ctx->sections);
        ctx->sections = NULL;
        return -1;
    }

    // If no sections found, treat entire buffer as one section
    if (ctx->sec_count == 0) {
        return section_push(ctx, 0, buffer_size, 0);
    }

    // Merge non-heading sections into previous heading section
    size_t write_idx = 0;
    for (size_t i = 0; i < ctx->sec_count; i++) {
        section_t *s = &ctx->sections[i];
        // First section or heading starts new section
        if (write_idx == 0 || s->is_heading) {
            ctx->sections[write_idx++] = *s;
        } else {
            // Extend previous section to include this one
            ctx->sections[write_idx - 1].end = s->end;
        }
    }
    ctx->sec_count = write_idx;

    // Ensure sections span full buffer with no gaps
    ctx->sections[0].start = 0;
    ctx->sections[ctx->sec_count - 1].end = buffer_size;
    for (size_t i = 1; i < ctx->sec_count; i++) {
        if (ctx->sections[i - 1].end < ctx->sections[i].start)
            ctx->sections[i - 1].end = ctx->sections[i].start;
        if (ctx->sections[i].start < ctx->sections[i - 1].end)
            ctx->sections[i].start = ctx->sections[i - 1].end;
    }

    return 0;
}

// Strip markdown from all sections
static int strip_sections (parse_ctx_t *ctx, const char *buffer, bool skip_html, bool mdx_mode) {
    for (size_t i = 0; i < ctx->sec_count; i++) {
        section_t *s = &ctx->sections[i];
        s->text = strip_markdown(buffer + s->start, s->end - s->start, &s->text_len, skip_html, mdx_mode);
        if (!s->text) {
            // Free previously allocated texts and set to NULL to avoid double-free
            for (size_t j = 0; j < i; j++) {
                dbmemory_free(ctx->sections[j].text);
                ctx->sections[j].text = NULL;
            }
            return -1;
        }
    }
    return 0;
}

// Free all section resources
static void free_sections (parse_ctx_t *ctx) {
    if (ctx->sections) {
        for (size_t i = 0; i < ctx->sec_count; i++) {
            dbmemory_free(ctx->sections[i].text);
        }
        dbmemory_free(ctx->sections);
        ctx->sections = NULL;
    }
    ctx->sec_count = ctx->sec_cap = 0;
}

// MARK: - Chunk Building -

// Build raw chunks by splitting sections that exceed max_chars
static int build_raw_chunks (parse_ctx_t *ctx, size_t max_chars, raw_chunk_t **out_chunks, size_t *out_count) {
    size_t cap = 16, count = 0;
    raw_chunk_t *chunks = (raw_chunk_t *)dbmemory_alloc(cap * sizeof(raw_chunk_t));
    if (!chunks) return -1;

    for (size_t i = 0; i < ctx->sec_count; i++) {
        size_t tlen = ctx->sections[i].text_len;

        if (max_chars == 0 || tlen <= max_chars) {
            // Fits in one chunk
            if (count >= cap) {
                cap *= 2;
                raw_chunk_t *tmp = (raw_chunk_t *)dbmemory_realloc(chunks, cap * sizeof(raw_chunk_t));
                if (!tmp) { dbmemory_free(chunks); return -1; }
                chunks = tmp;
            }
            chunks[count++] = (raw_chunk_t){ i, 0, tlen };
        } else {
            // Split into multiple chunks
            size_t pos = 0;
            while (pos < tlen) {
                size_t remaining = tlen - pos;
                size_t split = find_split(ctx->sections[i].text + pos, remaining, max_chars);
                if (count >= cap) {
                    cap *= 2;
                    raw_chunk_t *tmp = (raw_chunk_t *)dbmemory_realloc(chunks, cap * sizeof(raw_chunk_t));
                    if (!tmp) { dbmemory_free(chunks); return -1; }
                    chunks = tmp;
                }
                chunks[count++] = (raw_chunk_t){ i, pos, split };
                pos += split;
            }
        }
    }

    *out_chunks = chunks;
    *out_count = count;
    return 0;
}

// Build final chunk with optional overlay from previous chunk
static char *build_chunk_text (parse_ctx_t *ctx, raw_chunk_t *raw, char *prev_text, size_t prev_len, size_t overlay_chars, size_t *out_len) {
    char *src_text = ctx->sections[raw->sec_idx].text + raw->text_off;
    size_t src_len = raw->text_len;

    if (prev_text == NULL || overlay_chars == 0) {
        // No overlay
        char *text = (char *)dbmemory_alloc(src_len + 1);
        if (!text) return NULL;
        memcpy(text, src_text, src_len);
        text[src_len] = '\0';
        *out_len = src_len;
        return text;
    }

    // Calculate overlay from previous chunk
    size_t ov_want = overlay_chars;
    if (ov_want > prev_len) ov_want = prev_len;

    // Snap to line boundary
    size_t ov_start = prev_len - ov_want;
    while (ov_start < prev_len && prev_text[ov_start] != '\n') ++ov_start;
    if (ov_start < prev_len && prev_text[ov_start] == '\n') ++ov_start;

    size_t ov_len = prev_len - ov_start;
    size_t total = ov_len + (ov_len > 0 ? 1 : 0) + src_len;

    char *text = (char *)dbmemory_alloc(total + 1);
    if (!text) return NULL;

    size_t wp = 0;
    if (ov_len > 0) {
        memcpy(text, prev_text + ov_start, ov_len);
        wp += ov_len;
        text[wp++] = '\n';
    }
    memcpy(text + wp, src_text, src_len);
    wp += src_len;
    text[wp] = '\0';
    *out_len = wp;
    return text;
}

// MARK: - Public API -

int dbmem_parse (const char *md, size_t md_len, dbmem_parse_settings *settings) {
    if (!md || !settings) return -1;
    if (md_len == 0) return 0;

    size_t chars_per_token = settings->chars_per_token ? settings->chars_per_token : 4;
    size_t max_chars = settings->max_tokens ? tokens_to_chars(settings->max_tokens, chars_per_token) : 0;
    size_t overlay_chars = settings->overlay_tokens ? tokens_to_chars(settings->overlay_tokens, chars_per_token) : 0;

    // 1. Parse into sections
    parse_ctx_t ctx;
    if (parse_sections(md, md_len, settings->skip_semantic, &ctx) != 0) {
        return -1;
    }

    // 2. Strip markdown from sections
    if (strip_sections(&ctx, md, settings->skip_html, settings->mdx_mode) != 0) {
        free_sections(&ctx);
        return -1;
    }

    // 3. Build raw chunks (splitting large sections)
    raw_chunk_t *raw_chunks = NULL;
    size_t raw_count = 0;
    if (build_raw_chunks(&ctx, max_chars, &raw_chunks, &raw_count) != 0) {
        free_sections(&ctx);
        return -1;
    }

    // 4. Build final chunks with overlay and invoke callback
    char *prev_text = NULL;
    size_t prev_len = 0;
    int rc = 0;

    for (size_t i = 0; i < raw_count && rc == 0; i++) {
        raw_chunk_t *raw = &raw_chunks[i];
        section_t *sec = &ctx.sections[raw->sec_idx];

        // Build chunk text with overlay
        size_t chunk_len;
        char *chunk_text = build_chunk_text(&ctx, raw, prev_text, prev_len, overlay_chars, &chunk_len);
        if (!chunk_text) {
            dbmemory_free(prev_text);
            dbmemory_free(raw_chunks);
            free_sections(&ctx);
            return -1;
        }

        // Calculate source offset/length (proportional mapping)
        size_t sec_raw_len = sec->end - sec->start;
        size_t full_txt = sec->text_len;
        size_t src_off, src_len;

        if (full_txt == 0) {
            src_off = sec->start;
            src_len = sec_raw_len;
        } else {
            src_off = sec->start + (raw->text_off * sec_raw_len / full_txt);
            size_t src_end = sec->start + ((raw->text_off + raw->text_len) * sec_raw_len / full_txt);
            if (src_end > sec->end) src_end = sec->end;
            src_len = src_end - src_off;
        }

        // Invoke callback (skip whitespace-only chunks)
        bool has_text = false;
        for (size_t k = 0; k < chunk_len; k++) {
            if (!isspace((unsigned char)chunk_text[k])) { has_text = true; break; }
        }
        if (has_text && settings->callback) {
            rc = settings->callback(chunk_text, chunk_len, src_off, src_len, settings->xdata, i);
            if (rc != 0) break;
        }

        // Keep this chunk's text for next iteration's overlay
        dbmemory_free(prev_text);
        prev_text = chunk_text;
        prev_len = chunk_len;
    }

    // Cleanup
    dbmemory_free(prev_text);
    dbmemory_free(raw_chunks);
    free_sections(&ctx);

    return rc;
}
