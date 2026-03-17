//
//  test_nsurl.c
//  Test NSURLSession HTTP backend against vectors.space API
//

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "dbmem-http.h"

#define API_KEY "sk_EGIa3gxdMmUphX8at-yIxnfT07hHK2BmMm9Xeqch5tY"
#define API_URL "https://api.vectors.space/v1/embeddings"

int main(void) {
    printf("=== NSURLSession HTTP Backend Test ===\n\n");

    const char *body =
        "{"
        "\"provider\": \"llama\","
        "\"model\": \"embeddinggemma-300m\","
        "\"input\": \"Hello world\","
        "\"strategy\": {\"type\": \"truncate\"}"
        "}";

    void *response_data = NULL;
    size_t response_size = 0;
    long http_code = 0;
    char err_msg[1024] = {0};

    printf("Sending POST to %s...\n", API_URL);

    int rc = dbmem_http_post(API_URL, API_KEY, body,
                             &response_data, &response_size, &http_code,
                             err_msg, sizeof(err_msg));

    if (rc != 0) {
        printf("FAILED: HTTP request error: %s\n", err_msg);
        return 1;
    }

    printf("HTTP status: %ld\n", http_code);
    printf("Response size: %zu bytes\n", response_size);

    if (http_code != 200) {
        printf("FAILED: Expected HTTP 200, got %ld\n", http_code);
        printf("Response: %.*s\n", (int)(response_size < 500 ? response_size : 500), (char *)response_data);
        free(response_data);
        return 1;
    }

    // Check that response contains expected fields
    char *resp = (char *)response_data;
    if (strstr(resp, "embedding") == NULL) {
        printf("FAILED: Response missing 'embedding' field\n");
        printf("Response: %.*s\n", (int)(response_size < 500 ? response_size : 500), resp);
        free(response_data);
        return 1;
    }

    if (strstr(resp, "output_dimension") == NULL) {
        printf("FAILED: Response missing 'output_dimension' field\n");
        free(response_data);
        return 1;
    }

    printf("Response contains embedding data: OK\n");

    // Print first 100 chars of response for visual verification
    printf("Response preview: %.100s...\n", resp);

    free(response_data);

    printf("\n=== ALL TESTS PASSED ===\n");
    return 0;
}
