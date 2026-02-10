//
//  utils.c
//  sqlitememory
//
//  Created by Marco Bambini on 30/01/26.
//

#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
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
#include <bcrypt.h>
#include <io.h>
#define file_close      _close
#else
#include <unistd.h>
#include <dirent.h>
#if defined(__APPLE__)
#include <Security/Security.h>
#elif !defined(__ANDROID__)
#include <sys/random.h>
#endif
#define file_close      close
#endif

#ifndef MAX_PATH
#define MAX_PATH        2048
#endif

#define DBMEM_UUID_LEN  16

// MARK: - HASH -

static inline uint64_t rotl64 (uint64_t x, int r) {
  // Guard against UB: shifting by 64 is undefined for 64-bit types
  if (r == 0) return x;
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
  if (!data || len == 0) return 0x9e3779b97f4a7c15ULL;  // Return seed for empty/NULL input

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

// MARK: - UUIDv7 -

/*
    UUIDv7 is a 128-bit unique identifier like it's older siblings, such as the widely used UUIDv4.
    But unlike v4, UUIDv7 is time-sortable with 1 ms precision.
    By combining the timestamp and the random parts, UUIDv7 becomes an excellent choice for record identifiers in databases, including distributed ones.
 
    UUIDv7 offers several advantages.
    It includes a 48-bit Unix timestamp with millisecond accuracy and will overflow far in the future (10899 AD).
    It also include 74 random bits which means billions can be created every second without collisions.
    Because of its structure UUIDv7s are globally sortable and can be created in parallel in a distributed system.
 
    https://antonz.org/uuidv7/#c
    https://www.rfc-editor.org/rfc/rfc9562.html#name-uuid-version-7
 */

int dbmem_compute_uuid_v7 (uint8_t value[DBMEM_UUID_LEN]) {
    // fill the buffer with high-quality random data
    #ifdef _WIN32
    if (BCryptGenRandom(NULL, (BYTE*)value, DBMEM_UUID_LEN, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != STATUS_SUCCESS) return -1;
    #elif defined(__APPLE__)
    // Use SecRandomCopyBytes for macOS/iOS
    if (SecRandomCopyBytes(kSecRandomDefault, DBMEM_UUID_LEN, value) != errSecSuccess) return -1;
    #elif defined(__ANDROID__)
    //arc4random_buf doesn't have a return value to check for success
    arc4random_buf(value, DBMEM_UUID_LEN);
    #else
    if (getentropy(value, DBMEM_UUID_LEN) != 0) return -1;
    #endif
    
    // get current timestamp in ms
    struct timespec ts;
    #ifdef __ANDROID__
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return -1;
    #else
    if (timespec_get(&ts, TIME_UTC) == 0) return -1;
    #endif
    
    // add timestamp part to UUID
    uint64_t timestamp = (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    value[0] = (timestamp >> 40) & 0xFF;
    value[1] = (timestamp >> 32) & 0xFF;
    value[2] = (timestamp >> 24) & 0xFF;
    value[3] = (timestamp >> 16) & 0xFF;
    value[4] = (timestamp >> 8) & 0xFF;
    value[5] = timestamp & 0xFF;
    
    // version and variant
    value[6] = (value[6] & 0x0F) | 0x70; // UUID version 7
    value[8] = (value[8] & 0x3F) | 0x80; // RFC 4122 variant
    
    return 0;
}

char *dbmem_uuid_v7_stringify (uint8_t uuid[DBMEM_UUID_LEN], char value[DBMEM_UUID_STR_MAXLEN], bool dash_format) {
    if (dash_format) {
        snprintf(value, DBMEM_UUID_STR_MAXLEN, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
            uuid[0], uuid[1], uuid[2], uuid[3], uuid[4], uuid[5], uuid[6], uuid[7],
            uuid[8], uuid[9], uuid[10], uuid[11], uuid[12], uuid[13], uuid[14], uuid[15]
        );
    } else {
        snprintf(value, DBMEM_UUID_STR_MAXLEN, "%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
            uuid[0], uuid[1], uuid[2], uuid[3], uuid[4], uuid[5], uuid[6], uuid[7],
            uuid[8], uuid[9], uuid[10], uuid[11], uuid[12], uuid[13], uuid[14], uuid[15]
        );
    }
    
    return (char *)value;
}

char *dbmem_uuid_v7 (char value[DBMEM_UUID_STR_MAXLEN]) {
    uint8_t uuid[DBMEM_UUID_LEN];
    
    if (dbmem_compute_uuid_v7(uuid) != 0) return NULL;
    return dbmem_uuid_v7_stringify(uuid, value, false);
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

    memcpy(copy, s, len + 1);  // Include null terminator
    return copy;
}

// MARK: - IO -

static bool dbmem_file_read_all (int fd, char *buf, size_t n) {
    size_t off = 0;
    while (off < n) {
        #ifdef _WIN32
        // _read takes unsigned int, limit chunk size to avoid truncation
        size_t to_read = n - off;
        if (to_read > UINT_MAX) to_read = UINT_MAX;
        int r = _read(fd, buf + off, (unsigned)to_read);
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
    if (!path) {
        if (len) *len = -1;
        return NULL;
    }

    int fd = -1;
    char *buffer = NULL;
    int saved_errno = 0;

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
    // Check for regular file
    if (!(st.st_mode & _S_IFREG)) goto abort_read;
    int64_t isz = st.st_size;
    #else
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < 0) goto abort_read;
    // Check for regular file
    if (!S_ISREG(st.st_mode)) goto abort_read;
    int64_t isz = st.st_size;
    #endif

    size_t sz = (size_t)isz;
    // Guard against huge files that don't fit in size_t
    if ((int64_t)sz != isz) goto abort_read;

    buffer = (char *)dbmem_alloc(sz + 1);
    if (!buffer) goto abort_read;
    buffer[sz] = '\0';

    if (!dbmem_file_read_all(fd, buffer, sz)) goto abort_read;
    if (len) *len = (int64_t)sz;

    file_close(fd);
    return buffer;

abort_read:
    saved_errno = errno;  // Save errno before cleanup calls modify it
    if (buffer) dbmem_free(buffer);
    if (fd >= 0) file_close(fd);
    DEBUG_DBMEM("file_read: failed to read '%s': %s", path ? path : "(null)", strerror(saved_errno));
    (void)saved_errno;  // Suppress unused warning when DEBUG_DBMEM is disabled
    if (len) *len = -1;
    return NULL;
}

bool dbmem_file_exists (const char *path) {
    if (!path) return false;

    #ifdef _WIN32
    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) return true;
    #else
    if (access(path, F_OK) == 0) return true;
    #endif

    return false;
}

bool dbmem_file_has_extension (const char *path, const char *extensions) {
    if (!path || !extensions) return false;

    // Find the last dot in the path (for the extension)
    const char *dot = strrchr(path, '.');
    if (!dot || dot == path) return false;

    // Skip the dot to get the actual extension
    const char *file_ext = dot + 1;
    if (*file_ext == '\0') return false;

    size_t file_ext_len = strlen(file_ext);

    // Parse comma-separated list of extensions
    const char *p = extensions;
    while (*p) {
        // Skip leading whitespace and commas
        while (*p == ',' || *p == ' ') p++;
        if (*p == '\0') break;

        // Find end of current extension (comma or end of string)
        const char *ext_start = p;
        while (*p && *p != ',') p++;

        // Trim trailing whitespace
        const char *ext_end = p;
        while (ext_end > ext_start && ext_end[-1] == ' ') ext_end--;

        size_t ext_len = (size_t)(ext_end - ext_start);
        if (ext_len == 0) continue;

        // Compare extensions (case-insensitive)
        if (ext_len == file_ext_len) {
            bool match = true;
            for (size_t i = 0; i < ext_len; i++) {
                char c1 = file_ext[i];
                char c2 = ext_start[i];
                // Convert to lowercase for comparison
                if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
                if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
                if (c1 != c2) {
                    match = false;
                    break;
                }
            }
            if (match) return true;
        }
    }

    return false;
}

bool dbmem_dir_exists (const char *dir_path) {
    if (!dir_path) return false;

    #ifdef _WIN32
    DWORD attrs = GetFileAttributesA(dir_path);
    if (attrs == INVALID_FILE_ATTRIBUTES) return false;
    return (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
    #else
    // Unix/Linux/macOS/iOS/Android
    struct stat st;
    if (stat(dir_path, &st) != 0) return false;
    return S_ISDIR(st.st_mode);
    #endif
}

int dbmem_dir_scan (const char *dir_path, dir_scan_callback callback, void *data) {
    if (!dir_path || !callback) return -1;
        
    size_t dir_len = strlen(dir_path);
    // Remove trailing separator for consistent path building
    while (dir_len > 0 && (dir_path[dir_len - 1] == '/' || dir_path[dir_len - 1] == '\\')) {
        dir_len--;
    }
    if (dir_len >= MAX_PATH - 2) return -1;  // Path too long
        
    char full_path[MAX_PATH];
    memcpy(full_path, dir_path, dir_len);

    #ifdef _WIN32
    // Build search pattern: dir_path\*
    char search_path[MAX_PATH];
    memcpy(search_path, dir_path, dir_len);
    search_path[dir_len] = '\\';
    search_path[dir_len + 1] = '*';
    search_path[dir_len + 2] = '\0';

    WIN32_FIND_DATAA findData;
    HANDLE hFind = FindFirstFileA(search_path, &findData);
    if (hFind == INVALID_HANDLE_VALUE) return -1;

    int result = 0;
    do {
        // Skip empty names and hidden/dot files
        if (findData.cFileName[0] == '\0') continue;
        if (findData.cFileName[0] == '.') continue;

        // Build full path
        size_t name_len = strlen(findData.cFileName);
        if (dir_len + 1 + name_len >= MAX_PATH) continue;
        full_path[dir_len] = '\\';
        memcpy(full_path + dir_len + 1, findData.cFileName, name_len + 1);

        if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            // Recursively scan subdirectory
            result = dbmem_dir_scan(full_path, callback, data);
            if (result != 0) break;
        } else {
            // Regular file - invoke callback
            if (callback(full_path, data) != 0) {
                result = -1;
                break;
            }
        }
    } while (FindNextFileA(hFind, &findData) != 0);

    FindClose(hFind);
    return result;

    #else
    // Unix/Linux/macOS/iOS/Android
    DIR *dir = opendir(dir_path);
    if (!dir) return -1;

    int result = 0;
    struct dirent *d;
    while ((d = readdir(dir)) != NULL) {
        // Skip empty names and hidden/dot files
        if (d->d_name[0] == '\0') continue;
        if (d->d_name[0] == '.') continue;

        // Build full path
        size_t name_len = strlen(d->d_name);
        if (dir_len + 1 + name_len >= MAX_PATH) continue;
        full_path[dir_len] = '/';
        memcpy(full_path + dir_len + 1, d->d_name, name_len + 1);

        // Check entry type
        int is_dir = 0;
        int is_file = 0;

        #if defined(_DIRENT_HAVE_D_TYPE) || defined(__APPLE__) || defined(__linux__) || defined(__ANDROID__)
        if (d->d_type == DT_DIR) {
            is_dir = 1;
        } else if (d->d_type == DT_REG) {
            is_file = 1;
        } else if (d->d_type == DT_UNKNOWN) {
            // d_type not available, use stat
            struct stat st;
            if (stat(full_path, &st) == 0) {
                is_dir = S_ISDIR(st.st_mode);
                is_file = S_ISREG(st.st_mode);
            }
        }
        // Skip other types (symlinks, devices, etc.)
        #else
        // No d_type support, always use stat
        struct stat st;
        if (stat(full_path, &st) == 0) {
            is_dir = S_ISDIR(st.st_mode);
            is_file = S_ISREG(st.st_mode);
        }
        #endif

        if (is_dir) {
            // Recursively scan subdirectory
            result = dbmem_dir_scan(full_path, callback, data);
            if (result != 0) break;
        } else if (is_file) {
            // Regular file - invoke callback
            if (callback(full_path, data) != 0) {
                result = -1;
                break;
            }
        }
    }

    closedir(dir);
    return result;
    #endif
}
