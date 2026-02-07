//
//  dbmem-parser.h
//  sqlitememory
//
//  Created by Marco Bambini on 05/02/26.
//

#ifndef __DBMEM_PARSER__
#define __DBMEM_PARSER__

#include <stddef.h>
#include <stdbool.h>

typedef struct {
    int         (*callback) (const char *text, size_t len, size_t offset, size_t length, void *xdata, size_t index);
    void        *xdata;
    
    size_t      max_tokens;             // maximum number of token per chunk
    size_t      overlay_tokens;         // token overlay for each chunk
    size_t      chars_per_token;        // estimated number of characters per token
    bool        skip_semantic;          // if true, do not semantically parse MD file
    bool        skip_html;              // if true, remove html tags
} dbmem_parse_settings;

int dbmem_parse (const char *md, size_t md_len, dbmem_parse_settings *settings);

#endif
