//
//  utils.c
//  sqlitememory
//
//  Created by Marco Bambini on 30/01/26.
//

#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdbool.h>
#include <sys/stat.h>

#include "dbmem-utils.h"
#ifndef SQLITE_CORE
#include "sqlite3ext.h"
#else
#include "sqlite3.h"
#endif

#ifndef SQLITE_CORE
SQLITE_EXTENSION_INIT3
#endif

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#define file_close      _close
#else
#include <unistd.h>
#define file_close      close
#endif

// MARK: - HASH -

static inline uint64_t rotl64 (uint64_t x, int r) {
  return (x << r) | (x >> (64 - r));
}

#if defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64)
/* x86: unaligned loads ok */
static inline uint64_t load64_le (const void *p) {
  uint64_t v = *(const uint64_t*)p;
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
  v = ((v & 0x00000000000000FFULL) << 56) |
      ((v & 0x000000000000FF00ULL) << 40) |
      ((v & 0x0000000000FF0000ULL) << 24) |
      ((v & 0x00000000FF000000ULL) <<  8) |
      ((v & 0x000000FF00000000ULL) >>  8) |
      ((v & 0x0000FF0000000000ULL) >> 24) |
      ((v & 0x00FF000000000000ULL) >> 40) |
      ((v & 0xFF00000000000000ULL) >> 56);
#endif
  return v;
}
#else
/* portable */
static inline uint64_t load64_le (const void *p) {
  uint64_t v;
  memcpy(&v, p, sizeof(v));
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
  v = ((v & 0x00000000000000FFULL) << 56) |
      ((v & 0x000000000000FF00ULL) << 40) |
      ((v & 0x0000000000FF0000ULL) << 24) |
      ((v & 0x00000000FF000000ULL) <<  8) |
      ((v & 0x000000FF00000000ULL) >>  8) |
      ((v & 0x0000FF0000000000ULL) >> 24) |
      ((v & 0x00FF000000000000ULL) >> 40) |
      ((v & 0xFF00000000000000ULL) >> 56);
#endif
  return v;
}
#endif

static inline uint64_t mix64 (uint64_t x) {
  x ^= x >> 33;
  x *= 0xff51afd7ed558ccdULL;
  x ^= x >> 33;
  x *= 0xc4ceb9fe1a85ec53ULL;
  x ^= x >> 33;
  return x;
}

uint64_t dbmem_hash_compute (const void *data, size_t len) {
  const uint8_t *p = (const uint8_t*)data;

  uint64_t h = 0x9e3779b97f4a7c15ULL ^ (uint64_t)len;

  while (len >= 16) {
    uint64_t a = load64_le(p);
    uint64_t b = load64_le(p + 8);

    h ^= mix64(a);
    h = rotl64(h, 27) * 0x3c79ac492ba7b653ULL + 0x1c69b3f74ac4ae35ULL;

    h ^= mix64(b);
    h = rotl64(h, 27) * 0x3c79ac492ba7b653ULL + 0x1c69b3f74ac4ae35ULL;

    p += 16;
    len -= 16;
  }

  if (len >= 8) {
    uint64_t a = load64_le(p);
    h ^= mix64(a);
    h = rotl64(h, 27) * 0x3c79ac492ba7b653ULL + 0x1c69b3f74ac4ae35ULL;
    p += 8;
    len -= 8;
  }

  if (len) {
    uint64_t tail = 0;
    for (size_t i = 0; i < len; i++) tail |= (uint64_t)p[i] << (8u * i);
    h ^= mix64(tail);
    h = rotl64(h, 27) * 0x3c79ac492ba7b653ULL + 0x1c69b3f74ac4ae35ULL;
  }

  return mix64(h);
}

// MARK: - MEMORY -
void *dbmem_alloc (uint64_t size) {
    return sqlite3_malloc64((sqlite3_uint64)size);
}

void *dbmem_zeroalloc (uint64_t size) {
    void *ptr = (void *)dbmem_alloc(size);
    if (!ptr) return NULL;
    
    memset(ptr, 0, (size_t)size);
    return ptr;
}

void *dbmem_realloc (void *ptr, uint64_t new_size) {
    return sqlite3_realloc64(ptr, (sqlite3_uint64)new_size);
}

void dbmem_free (void *ptr) {
    sqlite3_free(ptr);
}

uint64_t dbmem_size (void *ptr) {
    return (uint64_t)sqlite3_msize(ptr);
}

char *dbmem_strdup (const char *s) {
    if (!s) return NULL;
    
    size_t len = strlen(s);
    char *copy = (char *)dbmem_alloc((uint64_t)(len + 1));
    if (!copy) return NULL;
    
    memcpy(copy, s, len);
    copy[len] = 0;
    
    return copy;
}

// MARK: - IO -

static bool dbmem_file_read_all (int fd, char *buf, size_t n) {
    size_t off = 0;
    while (off < n) {
        #ifdef _WIN32
        int r = _read(fd, buf + off, (unsigned)(n - off));
        if (r <= 0) return false;
        #else
        ssize_t r = read(fd, buf + off, n - off);
        if (r < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (r == 0) return false; // unexpected EOF
        #endif
        off += (size_t)r;
    }
    return true;
}

char *dbmem_file_read (const char *path, int64_t *len) {
    int fd = -1;
    char *buffer = NULL;

    #ifdef _WIN32
    fd = _open(path, _O_RDONLY | _O_BINARY);
    #else
    fd = open(path, O_RDONLY);
    #endif
    if (fd < 0) goto abort_read;

    // Get size after open to reduce TOCTTOU
    #ifdef _WIN32
    struct _stat64 st;
    if (_fstat64(fd, &st) != 0 || st.st_size < 0) goto abort_read;
    int64_t isz = st.st_size;
    #else
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < 0) goto abort_read;
    int64_t isz = st.st_size;
    #endif

    size_t sz = (size_t)isz;
    // optional: guard against huge files that don't fit in size_t
    if ((int64_t)sz != isz) goto abort_read;

    buffer = (char *)dbmem_alloc(sz + 1);
    if (!buffer) goto abort_read;
    buffer[sz] = '\0';

    if (!dbmem_file_read_all(fd, buffer, sz)) goto abort_read;
    if (len) *len = sz;
    
    file_close(fd);
    return buffer;
    
abort_read:
    DEBUG_DBMEM("file_read: failed to read '%s': %s\n", path, strerror(errno));
    if (len) *len = -1;
    if (buffer) dbmem_free(buffer);
    if (fd >= 0) file_close(fd);
    return NULL;
}

bool dbmem_file_exists (const char *path) {
    #ifdef _WIN32
    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) return true;
    #else
    if (access(path, F_OK) == 0) return true;
    #endif
    
    return false;
}
