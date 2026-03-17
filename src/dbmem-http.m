//
//  dbmem-http.m
//  sqlitememory
//
//  Created by Marco Bambini on 17/03/26.
//

#import <Foundation/Foundation.h>
#include "dbmem-http.h"
#include <string.h>
#include <stdlib.h>

int dbmem_http_post(const char *url, const char *api_key, const char *body,
                    void **out_data, size_t *out_size, long *out_http_code,
                    char *err_msg, size_t err_msg_size) {
    @autoreleasepool {
        NSURL *nsurl = [NSURL URLWithString:[NSString stringWithUTF8String:url]];
        if (!nsurl) {
            snprintf(err_msg, err_msg_size, "Invalid URL: %s", url);
            return -1;
        }

        NSMutableURLRequest *request = [NSMutableURLRequest requestWithURL:nsurl];
        request.HTTPMethod = @"POST";
        request.timeoutInterval = 30;

        NSString *auth = [NSString stringWithFormat:@"Bearer %s", api_key];
        [request setValue:auth forHTTPHeaderField:@"Authorization"];
        [request setValue:@"application/json" forHTTPHeaderField:@"Content-Type"];

        request.HTTPBody = [NSData dataWithBytes:body length:strlen(body)];

        dispatch_semaphore_t sem = dispatch_semaphore_create(0);
        __block NSData *responseData = nil;
        __block NSHTTPURLResponse *httpResponse = nil;
        __block NSError *requestError = nil;

        NSURLSessionDataTask *task = [[NSURLSession sharedSession]
            dataTaskWithRequest:request
            completionHandler:^(NSData *data, NSURLResponse *response, NSError *error) {
                responseData = data;
                httpResponse = (NSHTTPURLResponse *)response;
                requestError = error;
                dispatch_semaphore_signal(sem);
            }];
        [task resume];
        dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);

        if (requestError) {
            snprintf(err_msg, err_msg_size, "%s", requestError.localizedDescription.UTF8String);
            return -1;
        }

        *out_http_code = httpResponse.statusCode;
        *out_size = responseData.length;
        *out_data = malloc(responseData.length + 1);
        if (!*out_data) {
            snprintf(err_msg, err_msg_size, "Failed to allocate response buffer");
            return -1;
        }
        memcpy(*out_data, responseData.bytes, responseData.length);
        ((char *)*out_data)[responseData.length] = '\0';

        return 0;
    }
}
