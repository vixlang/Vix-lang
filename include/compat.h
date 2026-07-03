#ifndef VIX_COMPAT_H
#define VIX_COMPAT_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef STRINGIFY
#define STRINGIFY_(x) #x
#define STRINGIFY(x) STRINGIFY_(x)
#endif

#define VIX_VERSION_MAJOR 0
#define VIX_VERSION_MINOR 2
#define VIX_VERSION_PATCH 0
#define VIX_VERSION_STRING "Vix v" STRINGIFY(VIX_VERSION_MAJOR) "." STRINGIFY(VIX_VERSION_MINOR) "." STRINGIFY(VIX_VERSION_PATCH)

#ifdef _WIN32
#include <io.h>
#include <direct.h>
#ifndef access
#define access _access
#endif
#ifndef F_OK
#define F_OK 0
#endif
#ifndef R_OK
#define R_OK 4
#endif
#ifndef strdup
#define strdup _strdup
#endif
#else
#include <unistd.h>
#endif

static inline int vix_file_exists(const char* path) {
    FILE* f = fopen(path, "r");
    if (f) { fclose(f); return 1; }
    return 0;
}

static inline int vix_file_readable(const char* path) {
    FILE* f = fopen(path, "rb");
    if (f) { fclose(f); return 1; }
    return 0;
}

static inline char* vix_realpath(const char* path, char* resolved, size_t resolved_size) {
    if (!path || !resolved || resolved_size == 0) return NULL;
#ifdef _WIN32
    if (_fullpath(resolved, path, (unsigned int)resolved_size) != NULL) return resolved;
#else
    {
        char* r = realpath(path, NULL);
        if (r != NULL) {
            strncpy(resolved, r, resolved_size - 1);
            resolved[resolved_size - 1] = '\0';
            free(r);
            return resolved;
        }
    }
#endif
    strncpy(resolved, path, resolved_size - 1);
    resolved[resolved_size - 1] = '\0';
    return resolved;
}

static inline int vix_is_stderr_tty(void) {
#ifdef _WIN32
    return _isatty(_fileno(stderr));
#else
    return isatty(fileno(stderr));
#endif
}

static inline int vix_setenv(const char* name, const char* value, int overwrite) {
    if (!name || !value) return -1;
#ifdef _WIN32
    (void)overwrite;
    size_t len = strlen(name) + strlen(value) + 2;
    char* buf = (char*)malloc(len);
    if (!buf) return -1;
    snprintf(buf, len, "%s=%s", name, value);
    int ret = _putenv(buf);
    free(buf);
    return ret;
#else
    return setenv(name, value, overwrite);
#endif
}

#endif
