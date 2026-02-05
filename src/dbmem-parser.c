//
//  dbmem-parser.c
//  sqlitememory_test
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
 * Each chunk provides:
 *   • offset / length  — byte range in the original Markdown buffer
 *   • text / text_len  — heap-allocated plain text (Markdown stripped)
 *   • tokens           — estimated token count of the plain text
 *
 * Token estimation: 1 token ≈ CHARS_PER_TOKEN characters (default 4).
 *
 * Depends on: md4c.h  (from https://github.com/mity/md4c)
 */

#include "dbmem-parser.h"
#include "dbmem-utils.h"
#include "md4c.h"

#include <stdio.h>
#include <string.h>

#define RAW_PUSH(r) do {                                               \
    if (raw_cnt >= raw_cap) {                                          \
        raw_cap = raw_cap ? raw_cap * 2 : 64;                          \
        raws = (raw_t *)dbmem_realloc(raws, raw_cap * sizeof *raws);   \
    }                                                                  \
    raws[raw_cnt++] = (r);                                             \
} while(0)

/* A single chunk produced by semantic chunking */
typedef struct {
    size_t  offset;         // Start byte in the original Markdown
    size_t  length;         // Byte count in the original Markdown
    char   *text;           // Heap-allocated plain text (no markup)
    size_t  text_len;       // strlen(text)
    size_t  tokens;         // Estimated token count of text
} md_chunk_t;

/* Result returned by md_semantic_chunk() */
typedef struct {
    md_chunk_t *chunks;     // Heap-allocated array of chunks
    size_t      count;      // Number of valid chunks
} md_chunk_result_t;

/* md4c parse context  */
typedef struct {
    size_t start;
    size_t end;
    int    is_heading;
    int    heading_level;
} seg_t;

typedef struct {
    const char *src;
    size_t      src_len;

    seg_t      *segs;
    size_t      seg_count;
    size_t      seg_cap;

    int         depth;
    int         cur_is_heading;
    int         cur_level;
    size_t      cur_text_min;
    size_t      cur_text_max;
    int         cur_has_text;
} pctx_t;

// Dynamic chunk array (used while building the result)
typedef struct {
    md_chunk_t *items;
    size_t      count;
    size_t      cap;
} chunk_vec_t;

// MARK: -

static void cv_init (chunk_vec_t *v) {
    v->items = NULL;
    v->count = v->cap = 0;
}

static int cv_push (chunk_vec_t *v, md_chunk_t c) {
    if (v->count >= v->cap) {
        v->cap = v->cap ? v->cap * 2 : 16;
        md_chunk_t *tmp = (md_chunk_t *)dbmem_realloc(v->items, v->cap * sizeof *tmp);
        if (!tmp) return -1;
        v->items = tmp;
    }
    v->items[v->count++] = c;
    return 0;
}

static void cv_free_contents (chunk_vec_t *v) {
    for (size_t i = 0; i < v->count; i++) {
        dbmem_free(v->items[i].text);
    }
    dbmem_free(v->items);
    v->items = NULL;
    v->count = v->cap = 0;
}


// MARK: -

static size_t chars_to_tokens(size_t chars, size_t chars_per_token) {
    return (chars + chars_per_token - 1) / chars_per_token;
}

static size_t tokens_to_chars(size_t tokens, size_t chars_per_token) {
    return tokens * chars_per_token;
}

static void seg_push(pctx_t *c, seg_t s) {
    if (c->seg_count >= c->seg_cap) {
        c->seg_cap = c->seg_cap ? c->seg_cap * 2 : 64;
        c->segs = (seg_t *)dbmem_realloc(c->segs, c->seg_cap * sizeof *c->segs);
    }
    c->segs[c->seg_count++] = s;
}

static size_t line_start_of(const char *buf, size_t pos) {
    while (pos > 0 && buf[pos - 1] != '\n') --pos;
    return pos;
}

static size_t line_end_of(const char *buf, size_t len, size_t pos) {
    while (pos < len && buf[pos] != '\n') ++pos;
    if (pos < len) ++pos;
    return pos;
}

/* Return 1 if the line is a table separator row. */
static int is_table_separator (const char *line, size_t len) {
    int has_dash = 0;
    for (size_t i = 0; i < len; i++) {
        char c = line[i];
        if (c == '-') has_dash = 1;
        else if (c != '|' && c != ':' && c != ' ' && c != '\t')
            return 0;
    }
    return has_dash;
}

/* Return the split point (byte offset) within `text` of length `len`
   such that text[0..split) fits in `max_chars` characters.  Prefers
   splitting at newlines for clean chunk boundaries.                    */
static size_t find_split (const char *text, size_t len, size_t max_chars) {
    if (len <= max_chars) return len;

    /* Walk backwards from max_chars looking for a newline. */
    size_t pos = max_chars;
    while (pos > 0 && text[pos - 1] != '\n')
        --pos;

    /* If we found a newline, use it (the newline is included). */
    if (pos > 0)
        return pos;

    /* No newline at all — hard-cut at budget. */
    return max_chars;
}

static void print_boxed(const char *text, size_t len) {
    const char *p   = text;
    const char *end = text + len;
    printf("  ┌───────────────────────────────────────────────\n");
    while (p < end) {
        const char *nl2 = (const char *)memchr(p, '\n', (size_t)(end - p));
        size_t ll = nl2 ? (size_t)(nl2 - p) : (size_t)(end - p);
        printf("  │ %.*s\n", (int)ll, p);
        p += ll + (nl2 ? 1 : 0);
    }
    printf("  └───────────────────────────────────────────────\n");
}

// MARK: -

static char *strip_markdown (const char *src, size_t len, size_t *out_len) {
    char *buf = (char *)dbmem_alloc(len + 1);
    if (!buf) return NULL;

    size_t wp = 0;
    const char *p   = src;
    const char *end = src + len;

    int in_fenced_code = 0;
    char fence_char    = 0;
    int  fence_width   = 0;

    while (p < end) {
        const char *nl = p;
        while (nl < end && *nl != '\n') ++nl;
        const char *line_end = nl;
        size_t line_len = (size_t)(line_end - p);

        /* --- fenced code ------------------------------------------- */
        if (!in_fenced_code) {
            const char *fp = p; int sp = 0;
            while (fp < line_end && *fp == ' ' && sp < 3) { ++fp; ++sp; }
            if (fp < line_end && (*fp == '`' || *fp == '~')) {
                char fc = *fp; int fw = 0;
                while (fp < line_end && *fp == fc) { ++fp; ++fw; }
                if (fw >= 3) {
                    in_fenced_code = 1;
                    fence_char = fc;  fence_width = fw;
                    p = (nl < end) ? nl + 1 : nl;
                    continue;
                }
            }
        } else {
            const char *fp = p; int sp = 0;
            while (fp < line_end && *fp == ' ' && sp < 3) { ++fp; ++sp; }
            if (fp < line_end && *fp == fence_char) {
                int fw = 0;
                while (fp < line_end && *fp == fence_char) { ++fp; ++fw; }
                int only_sp = 1;
                while (fp < line_end) {
                    if (*fp != ' ' && *fp != '\t') { only_sp = 0; break; }
                    ++fp;
                }
                if (fw >= fence_width && only_sp) {
                    in_fenced_code = 0;
                    p = (nl < end) ? nl + 1 : nl;
                    continue;
                }
            }
            memcpy(buf + wp, p, line_len);
            wp += line_len;
            buf[wp++] = '\n';
            p = (nl < end) ? nl + 1 : nl;
            continue;
        }

        /* --- blank lines ------------------------------------------- */
        {
            const char *tp = p;
            while (tp < line_end && (*tp == ' ' || *tp == '\t')) ++tp;
            if (tp == line_end) {
                buf[wp++] = '\n';
                p = (nl < end) ? nl + 1 : nl;
                continue;
            }
        }

        /* --- thematic breaks --------------------------------------- */
        {
            const char *tp = p; int sp2 = 0;
            while (tp < line_end && *tp == ' ' && sp2 < 3) { ++tp; ++sp2; }
            if (tp < line_end && (*tp == '-' || *tp == '*' || *tp == '_')) {
                char tc = *tp; int cnt = 0; int only = 1;
                const char *tp2 = tp;
                while (tp2 < line_end) {
                    if (*tp2 == tc) ++cnt;
                    else if (*tp2 != ' ' && *tp2 != '\t') { only = 0; break; }
                    ++tp2;
                }
                if (cnt >= 3 && only) {
                    p = (nl < end) ? nl + 1 : nl;
                    continue;
                }
            }
        }

        /* --- table separator rows ---------------------------------- */
        if (is_table_separator(p, line_len)) {
            p = (nl < end) ? nl + 1 : nl;
            continue;
        }

        /* --- ATX headings ------------------------------------------ */
        const char *lp = p;
        {
            int sp2 = 0;
            while (lp < line_end && *lp == ' ' && sp2 < 3) { ++lp; ++sp2; }
            if (lp < line_end && *lp == '#') {
                int lev = 0;
                while (lp < line_end && *lp == '#' && lev < 7)
                    { ++lp; ++lev; }
                if (lev >= 1 && lev <= 6 &&
                    (lp >= line_end || *lp == ' ' || *lp == '\t')) {
                    while (lp < line_end && (*lp == ' ' || *lp == '\t'))
                        ++lp;
                    const char *re = line_end;
                    while (re > lp && (re[-1] == ' ' || re[-1] == '\t'))
                        --re;
                    while (re > lp && re[-1] == '#') --re;
                    while (re > lp && (re[-1] == ' ' || re[-1] == '\t'))
                        --re;
                    line_end = re;
                    goto emit_inline;
                }
                lp = p;
            }
        }

        /* --- list markers ------------------------------------------ */
        lp = p;
        {
            int sp2 = 0;
            while (lp < line_end && *lp == ' ' && sp2 < 3) { ++lp; ++sp2; }
            if (lp < line_end &&
                (*lp == '-' || *lp == '*' || *lp == '+')) {
                const char *after = lp + 1;
                if (after < line_end && (*after == ' ' || *after == '\t')) {
                    lp = after;
                    while (lp < line_end && (*lp == ' ' || *lp == '\t'))
                        ++lp;
                    goto emit_inline;
                }
                lp = p;
            }
            lp = p; int sp3 = 0;
            while (lp < line_end && *lp == ' ' && sp3 < 3) { ++lp; ++sp3; }
            if (lp < line_end && *lp >= '0' && *lp <= '9') {
                const char *dp = lp;
                while (dp < line_end && *dp >= '0' && *dp <= '9') ++dp;
                if (dp < line_end && (*dp == '.' || *dp == ')')) {
                    ++dp;
                    if (dp < line_end && (*dp == ' ' || *dp == '\t')) {
                        lp = dp;
                        while (lp < line_end && (*lp == ' ' || *lp == '\t'))
                            ++lp;
                        goto emit_inline;
                    }
                }
                lp = p;
            }
        }
        lp = p;

emit_inline:
        {
            const char *ip = lp;
            while (ip < line_end) {
                if (*ip == '<') {
                    const char *cl = ip + 1;
                    while (cl < line_end && *cl != '>') ++cl;
                    if (cl < line_end) { ip = cl + 1; continue; }
                }
                if (*ip == '!' && ip + 1 < line_end && ip[1] == '[') {
                    const char *as = ip + 2, *ae = as;
                    int d = 1;
                    while (ae < line_end && d > 0) {
                        if (*ae == '[') ++d;
                        else if (*ae == ']') --d;
                        if (d > 0) ++ae;
                    }
                    if (ae < line_end && *ae == ']' &&
                        ae + 1 < line_end && ae[1] == '(') {
                        memcpy(buf + wp, as, (size_t)(ae - as));
                        wp += (size_t)(ae - as);
                        const char *ue = ae + 2;
                        while (ue < line_end && *ue != ')') ++ue;
                        ip = (ue < line_end) ? ue + 1 : ue;
                        continue;
                    }
                }
                if (*ip == '[') {
                    const char *ts = ip + 1, *te = ts;
                    int d = 1;
                    while (te < line_end && d > 0) {
                        if (*te == '[') ++d;
                        else if (*te == ']') --d;
                        if (d > 0) ++te;
                    }
                    if (te < line_end && *te == ']' &&
                        te + 1 < line_end && te[1] == '(') {
                        memcpy(buf + wp, ts, (size_t)(te - ts));
                        wp += (size_t)(te - ts);
                        const char *ue = te + 2;
                        while (ue < line_end && *ue != ')') ++ue;
                        ip = (ue < line_end) ? ue + 1 : ue;
                        continue;
                    }
                }
                if (*ip == '`') {
                    int bt = 0;
                    const char *cs = ip;
                    while (cs < line_end && *cs == '`') { ++cs; ++bt; }
                    const char *ce = cs;
                    while (ce + bt <= line_end) {
                        if (*ce == '`') {
                            int cb = 0; const char *t = ce;
                            while (t < line_end && *t == '`') { ++t; ++cb; }
                            if (cb == bt) {
                                memcpy(buf + wp, cs, (size_t)(ce - cs));
                                wp += (size_t)(ce - cs);
                                ip = t;
                                goto next_char;
                            }
                            ce = t;
                        } else { ++ce; }
                    }
                }
                if (*ip == '*' || *ip == '_') {
                    char mc = *ip; const char *ms = ip;
                    while (ip < line_end && *ip == mc) ++ip;
                    int n = (int)(ip - ms);
                    if (n > 3) { memcpy(buf + wp, ms, (size_t)n); wp += (size_t)n; }
                    continue;
                }
                if (*ip == '~' && ip + 1 < line_end && ip[1] == '~') {
                    ip += 2; continue;
                }
                if (*ip == '|') { ++ip; continue; }

                buf[wp++] = *ip++;
next_char:;
            }
        }

        while (wp > 0 && buf[wp - 1] == ' ') --wp;
        buf[wp++] = '\n';
        p = (nl < end) ? nl + 1 : nl;
    }

    while (wp > 0 && (buf[wp - 1] == '\n' || buf[wp - 1] == ' '))
        --wp;

    buf[wp] = '\0';
    if (out_len) *out_len = wp;
    return buf;
}

// MARK: -

static int cb_enter_block (MD_BLOCKTYPE type, void *detail, void *ud) {
    pctx_t *c = (pctx_t *)ud;
    if (type == MD_BLOCK_DOC) return 0;
    c->depth++;
    
    if (c->depth == 1) {
        c->cur_has_text = 0;
        c->cur_is_heading = (type == MD_BLOCK_H);
        c->cur_level = 0;
        
        if (type == MD_BLOCK_H) {
            MD_BLOCK_H_DETAIL *h = (MD_BLOCK_H_DETAIL *)detail;
            c->cur_level = (int)h->level;
        }
    }
    return 0;
}

static int cb_leave_block (MD_BLOCKTYPE type, void *detail, void *ud) {
    pctx_t *c = (pctx_t *)ud;
    (void)detail;
    if (type == MD_BLOCK_DOC) return 0;
    if (c->depth == 1 && c->cur_has_text) {
        seg_t s;
        s.start         = line_start_of(c->src, c->cur_text_min);
        s.end           = line_end_of(c->src, c->src_len, c->cur_text_max);
        s.is_heading    = c->cur_is_heading;
        s.heading_level = c->cur_level;
        seg_push(c, s);
    }
    c->depth--;
    return 0;
}

static int cb_enter_span (MD_SPANTYPE t, void *d, void *u) {
    return 0;
}

static int cb_leave_span (MD_SPANTYPE t, void *d, void *u) {
    return 0;
}

static int cb_text (MD_TEXTTYPE type, const MD_CHAR *text, MD_SIZE size, void *ud) {
    pctx_t *c = (pctx_t *)ud;
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
        if (e   > c->cur_text_max) c->cur_text_max = e;
    }
    return 0;
}

// MARK: -

static int md_semantic_chunk (const char *buffer, size_t buffer_size, dbmem_parse_settings *settings, md_chunk_result_t *result) {
    if (!buffer || !result) return -1;
    memset(result, 0, sizeof(*result));
    if (buffer_size == 0) return 0;

    size_t max_tokens = settings->max_tokens;
    size_t overlay_tokens = settings->overlay_tokens;
    size_t chars_per_token = settings->chars_per_token;
    if (chars_per_token == 0) chars_per_token = 4;
    const size_t max_chars = max_tokens ? tokens_to_chars(max_tokens, chars_per_token) : 0;
    const size_t overlay_chars = overlay_tokens ? tokens_to_chars(overlay_tokens, chars_per_token) : 0;

    // 1. Parse

    pctx_t ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.src     = buffer;
    ctx.src_len = buffer_size;

    MD_PARSER parser;
    memset(&parser, 0, sizeof parser);
    parser.abi_version = 0;
    parser.flags       = MD_FLAG_TABLES | MD_FLAG_STRIKETHROUGH | MD_FLAG_TASKLISTS;
    parser.enter_block = cb_enter_block;
    parser.leave_block = cb_leave_block;
    parser.enter_span  = cb_enter_span;
    parser.leave_span  = cb_leave_span;
    parser.text        = cb_text;

    if (md_parse(buffer, (MD_SIZE)buffer_size, &parser, &ctx) != 0) {
        dbmem_free(ctx.segs);
        return -1;
    }

    // 2. Group segments into sections

    size_t n_sec = 0;
    size_t *sec_start = NULL;
    size_t *sec_end   = NULL;

    if (ctx.seg_count == 0) {
        /* No blocks found — the whole buffer is one section. */
        n_sec     = 1;
        sec_start = (size_t *)dbmem_alloc(sizeof *sec_start);
        sec_end = (size_t *)dbmem_alloc(sizeof *sec_end);
        if (!sec_start || !sec_end) {
            dbmem_free(sec_start);
            dbmem_free(sec_end);
            dbmem_free(ctx.segs);
            return -1;
        }
        sec_start[0] = 0;
        sec_end[0]   = buffer_size;
    } else {
        sec_start = (size_t *)dbmem_alloc(ctx.seg_count * sizeof *sec_start);
        sec_end   = (size_t *)dbmem_alloc(ctx.seg_count * sizeof *sec_end);
        if (!sec_start || !sec_end) {
            dbmem_free(sec_start);
            dbmem_free(sec_end);
            dbmem_free(ctx.segs);
            return -1;
        }

        for (size_t i = 0; i < ctx.seg_count; i++) {
            seg_t *s = &ctx.segs[i];
            if (s->is_heading || n_sec == 0) {
                sec_start[n_sec] = s->start;
                sec_end[n_sec]   = s->end;
                n_sec++;
            } else {
                if (s->end > sec_end[n_sec - 1])
                    sec_end[n_sec - 1] = s->end;
            }
        }

        /* Sections must span the full buffer with no gaps. */
        sec_start[0] = 0;
        sec_end[n_sec - 1] = buffer_size;
        for (size_t i = 1; i < n_sec; i++) {
            if (sec_end[i - 1] < sec_start[i])
                sec_end[i - 1] = sec_start[i];
            if (sec_start[i] < sec_end[i - 1])
                sec_start[i] = sec_end[i - 1];
        }
    }

    dbmem_free(ctx.segs);
    ctx.segs = NULL;

    // 3. Strip each section to plain text

    char  **sec_text     = (char **)dbmem_zeroalloc(n_sec * sizeof(char *));
    size_t *sec_text_len = (size_t *)dbmem_zeroalloc(n_sec * sizeof(size_t));
    if (!sec_text || !sec_text_len) {
        dbmem_free(sec_text);
        dbmem_free(sec_text_len);
        dbmem_free(sec_start);
        dbmem_free(sec_end);
        return -1;
    }
    for (size_t i = 0; i < n_sec; i++) {
        sec_text[i] = strip_markdown(buffer + sec_start[i], sec_end[i] - sec_start[i], &sec_text_len[i]);
    }

    // 4. Split sections that exceed max_tokens
    //    Build a flat list of "raw chunks" (no overlay yet)

    typedef struct { size_t sec_idx; size_t text_off; size_t text_len; } raw_t;

    raw_t  *raws     = NULL;
    size_t  raw_cnt  = 0;
    size_t  raw_cap  = 0;

    for (size_t i = 0; i < n_sec; i++) {
        size_t tlen = sec_text_len[i];

        if (max_chars == 0 || tlen <= max_chars) {
            /* Fits in one chunk. */
            raw_t r = { i, 0, tlen };
            RAW_PUSH(r);
        } else {
            /* Must split this section into multiple sub-chunks. */
            size_t pos = 0;
            while (pos < tlen) {
                size_t remaining = tlen - pos;
                size_t split = find_split(sec_text[i] + pos, remaining, max_chars);
                raw_t r = { i, pos, split };
                RAW_PUSH(r);
                pos += split;
            }
        }
    }
    #undef RAW_PUSH

    // 5. Build final chunks with overlay

    chunk_vec_t cv;
    cv_init(&cv);

    for (size_t i = 0; i < raw_cnt; i++) {
        raw_t  *r   = &raws[i];
        char   *src_text = sec_text[r->sec_idx] + r->text_off;
        size_t  src_len  = r->text_len;

        /* ── Compute source offset/length in original Markdown ────── */
        /* We map the text sub-range proportionally back to the
           section's byte range in the source buffer.                 */
        size_t sec_raw_len = sec_end[r->sec_idx] - sec_start[r->sec_idx];
        size_t full_txt    = sec_text_len[r->sec_idx];

        size_t src_off, src_sz;
        if (full_txt == 0) {
            src_off = sec_start[r->sec_idx];
            src_sz  = sec_raw_len;
        } else {
            src_off = sec_start[r->sec_idx]
                    + (r->text_off * sec_raw_len / full_txt);
            size_t src_off_end = sec_start[r->sec_idx]
                    + ((r->text_off + r->text_len) * sec_raw_len / full_txt);
            if (src_off_end > sec_end[r->sec_idx])
                src_off_end = sec_end[r->sec_idx];
            src_sz = src_off_end - src_off;
        }

        /* ── Build the plain-text payload (with overlay prefix) ───── */
        char  *chunk_text;
        size_t chunk_text_len;

        if (i == 0 || overlay_chars == 0) {
            /* No overlay — just copy this sub-chunk's text. */
            chunk_text = (char *)dbmem_alloc(src_len + 1);
            if (!chunk_text) { cv_free_contents(&cv); goto fail; }
            memcpy(chunk_text, src_text, src_len);
            chunk_text[src_len] = '\0';
            chunk_text_len = src_len;
        } else {
            /* Grab the tail of the *previous chunk's text* as overlay.
               This works correctly whether the previous chunk came from
               the same section or a different one.                     */
            md_chunk_t *prev    = &cv.items[cv.count - 1];
            size_t prev_len     = prev->text_len;
            size_t ov_want      = overlay_chars;
            if (ov_want > prev_len)
                ov_want = prev_len;

            /* Snap to a line boundary inside the previous text. */
            size_t ov_start = prev_len - ov_want;
            while (ov_start < prev_len && prev->text[ov_start] != '\n')
                ++ov_start;
            if (ov_start < prev_len && prev->text[ov_start] == '\n')
                ++ov_start;

            size_t ov_len = prev_len - ov_start;
            size_t total  = ov_len + (ov_len > 0 ? 1 : 0) + src_len;

            chunk_text = (char *)dbmem_alloc(total + 1);
            if (!chunk_text) { cv_free_contents(&cv); goto fail; }

            size_t wp = 0;
            if (ov_len > 0) {
                memcpy(chunk_text, prev->text + ov_start, ov_len);
                wp += ov_len;
                chunk_text[wp++] = '\n';
            }
            memcpy(chunk_text + wp, src_text, src_len);
            wp += src_len;
            chunk_text[wp] = '\0';
            chunk_text_len = wp;

            /* Extend the source offset backwards for the overlay. */
            if (ov_len > 0 && src_off > 0) {
                /* Proportionally map overlay chars to source bytes. */
                size_t ov_src_bytes = ov_len;
                if (full_txt > 0)
                    ov_src_bytes = ov_len * sec_raw_len / full_txt;
                if (ov_src_bytes > src_off)
                    ov_src_bytes = src_off;
                src_off -= ov_src_bytes;
                src_sz  += ov_src_bytes;
            }
        }

        md_chunk_t ch;
        ch.offset   = src_off;
        ch.length   = src_sz;
        ch.text     = chunk_text;
        ch.text_len = (i == 0 || overlay_chars == 0) ? src_len : chunk_text_len;
        ch.tokens   = chars_to_tokens(ch.text_len, chars_per_token);

        if (cv_push(&cv, ch) != 0) {
            dbmem_free(chunk_text);
            cv_free_contents(&cv);
            goto fail;
        }
    }

    // 6. Transfer to output

    result->chunks = cv.items;
    result->count  = cv.count;
    /* cv.items ownership transferred — don't free via cv. */

    dbmem_free(raws);
    for (size_t i = 0; i < n_sec; i++) {
        dbmem_free(sec_text[i]);
    }
    dbmem_free(sec_text);
    dbmem_free(sec_text_len);
    dbmem_free(sec_start);
    dbmem_free(sec_end);
    
    return 0;

fail:
    dbmem_free(raws);
    for (size_t i = 0; i < n_sec; i++) {
        dbmem_free(sec_text[i]);
    }
    dbmem_free(sec_text);
    dbmem_free(sec_text_len);
    dbmem_free(sec_start);
    dbmem_free(sec_end);
    return -1;
}

static void md_chunk_result_free(md_chunk_result_t *r) {
    if (!r) return;
    for (size_t i = 0; i < r->count; i++) {
        dbmem_free(r->chunks[i].text);
    }
    dbmem_free(r->chunks);
    r->chunks = NULL;
    r->count  = 0;
}

// MARK: -

int dbmem_parse (const char *md, size_t md_len, dbmem_parse_settings *settings) {
    md_chunk_result_t res = {0};
    
    if (md_semantic_chunk(md, md_len, settings, &res) != 0) {
        printf("chunking failed\n");
        return -1;
    }
    
    printf("-> %zu chunks produced\n\n", res.count);

    for (size_t i = 0; i < res.count; i++) {
        md_chunk_t *ch = &res.chunks[i];
        printf("CHUNK %zu   offset=%-4zu  length=%-4zu  text_len=%-4zu  tokens=~%zu\n", i, ch->offset, ch->length, ch->text_len, ch->tokens);
        printf("  [plain text]\n");
        print_boxed(ch->text, ch->text_len);
        printf("\n");
    }

    md_chunk_result_free(&res);
    return 0;
}

