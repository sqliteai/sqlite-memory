//
//  dbmem-http.h
//  sqlitememory
//
//  Created by Marco Bambini on 17/03/26.
//

#ifndef __DBMEM_HTTP__
#define __DBMEM_HTTP__

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Synchronous HTTP POST using NSURLSession.
// Returns 0 on success, -1 on error.
// On success: *out_data is malloc'd response body (caller frees), *out_size is its length, *out_http_code is the status.
// On error: err_msg is filled with a description.
int dbmem_http_post(const char *url, const char *api_key, const char *body,
                    void **out_data, size_t *out_size, long *out_http_code,
                    char *err_msg, size_t err_msg_size);

#ifdef __cplusplus
}
#endif

#endif
