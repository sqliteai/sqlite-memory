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
    size_t  max_tokens;
    size_t  overlay_tokens;
    size_t  chars_per_token;
    bool    skip_semantic;
    bool    skip_html;
} dbmem_parse_settings;

int dbmem_parse (const char *md, size_t md_len, dbmem_parse_settings *settings);

#endif
