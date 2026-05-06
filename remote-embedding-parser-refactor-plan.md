# Remote Embedding Parser Refactor Plan

This note captures the preferred implementation plan for later work. The goal is
to make vectors.space response parsing directly testable without turning the
parser into public API or giving production logic a test-only name.

## Goal

Refactor `src/dbmem-rembed.c` so the JSON response parsing currently embedded in
`dbmem_remote_compute_embedding()` lives in a reusable internal function:

```c
int dbmem_remote_parse_embedding_response(...);
```

The function should be production logic, used by `dbmem_remote_compute_embedding()`
and also callable from `test/unittest.c` via a manual forward declaration.

Do not expose it in `sqlite-memory.h` or another public header.

## Preferred Shape

Use a normal internal production name, not a test-only name:

```c
int dbmem_remote_parse_embedding_response(
    const char *json,
    size_t json_len,
    float **embedding,
    size_t *embedding_capacity,
    jsmntok_t **tokens,
    int *tokens_capacity,
    embedding_result_t *result,
    char *err_msg,
    size_t err_msg_len
);
```

This keeps ownership explicit while avoiding exposure of `dbmem_remote_engine_t`
or a new parser-state struct in test code.

## Production Usage

`dbmem_remote_compute_embedding()` keeps responsibility for:

- request construction
- HTTP transport
- HTTP status handling
- context error propagation
- aggregate remote-engine stats

After receiving a successful HTTP 200 response, it calls:

```c
char err_msg[DBMEM_ERRBUF_SIZE] = {0};
int rc = dbmem_remote_parse_embedding_response(
    engine->data,
    engine->data_size,
    &engine->embedding,
    &engine->embedding_capacity,
    &engine->tokens,
    &engine->tokens_capacity,
    result,
    err_msg,
    sizeof(err_msg)
);

if (rc != 0) {
    dbmem_context_set_error(engine->context, err_msg);
    return -1;
}

engine->total_tokens_processed += result->n_tokens;
engine->total_embeddings_generated++;
return 0;
```

## Parser Responsibility

`dbmem_remote_parse_embedding_response()` should own:

- parsing JSON with `jsmn`
- allocating/growing the token buffer
- locating top-level `output_dimension`
- locating `data[0].embedding`
- allocating/growing the embedding buffer
- parsing embedding floats
- reading `data[0].truncated`
- reading token metadata from `usage`
- filling `embedding_result_t`

Token count priority should remain:

1. `usage.exact_prompt_tokens`
2. `usage.estimated_prompt_tokens`
3. `usage.prompt_tokens`
4. `0` if none are present

## Unit Test Usage

`test/unittest.c` can manually forward-declare the function under the relevant
test guards:

```c
#if defined(TEST_SQLITE_EXTENSION) && !defined(DBMEM_OMIT_REMOTE_ENGINE)
int dbmem_remote_parse_embedding_response(
    const char *json,
    size_t json_len,
    float **embedding,
    size_t *embedding_capacity,
    jsmntok_t **tokens,
    int *tokens_capacity,
    embedding_result_t *result,
    char *err_msg,
    size_t err_msg_len
);
#endif
```

Tests create local buffers:

```c
float *embedding = NULL;
size_t embedding_capacity = 0;
jsmntok_t *tokens = NULL;
int tokens_capacity = 0;
embedding_result_t result = {0};
char err_msg[1024] = {0};
```

Then call the parser with static JSON fixtures and free the buffers afterward:

```c
dbmemory_free(embedding);
dbmemory_free(tokens);
```

## Fixture Tests To Add Later

Recommended deterministic cases:

- exact token count is preferred over estimated and prompt token counts
- estimated token count is used when exact token count is absent
- prompt token count is used when exact and estimated token counts are absent
- missing usage object leaves `result.n_tokens == 0`
- `data[0].truncated: false` maps to `result.truncated == false`
- `data[0].truncated: true` maps to `result.truncated == true`
- embedding float array is parsed correctly
- output dimension is parsed correctly
- missing `data`
- missing `embedding`
- empty embedding array
- invalid top-level response shape

Also decide whether the parser should reject mismatches between
`output_dimension` and the embedding array length. Failing fast is likely safer,
because a dimension mismatch can break later vector initialization/search.

## Why This Plan

This approach avoids:

- live network dependence for parser correctness tests
- exposing parser internals as public API
- duplicating parser behavior in test-only code
- coupling tests to `dbmem_remote_engine_t`
- adding a new internal header before it is needed

The e2e test discussion can proceed separately, especially around whether token
metadata should become persisted product state or remain parser-only metadata.
