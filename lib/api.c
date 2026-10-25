#define _GNU_SOURCE
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32) || defined(WIN32)
#include <io.h>
#define access _access
#define strdup _strdup
#ifndef R_OK
#define R_OK 4
#endif
#ifndef X_OK
#define X_OK 0
#endif
#else
#include <unistd.h>
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

extern int vix_llc_compile_ir_to_object(const char *input_path,
                                        const char *output_path,
                                        const char *target_triple);
extern int vix_llc_compile_ir_to_asm(const char *input_path,
                                     const char *output_path,
                                     const char *target_triple);
extern const char *vix_llc_last_error(void);
extern int vix_lld_link_elf(const char *args_text);
#if defined(_WIN32) || defined(WIN32)
extern int vix_lld_link_coff(const char *args_text);
#endif
extern const char *vix_lld_last_error(void);
extern int vix_passes_optimize_ir(const char *input_path,
                                  const char *output_path,
                                  const char *target_triple, int opt_level);
extern const char *vix_passes_last_error(void);

static const char *readable_file(const char *path) {
  if (path && access(path, R_OK) == 0)
    return path;
  return NULL;
}

static char *dup_readable(const char *path) {
  if (!readable_file(path))
    return NULL;
  return strdup(path);
}

static char *find_first_file(const char **paths) {
  for (int i = 0; paths[i] != NULL; i++) {
    char *found = dup_readable(paths[i]);
    if (found)
      return found;
  }
  return NULL;
}

static char *find_in_gcc_dir(const char *name) {
  const char *bases[] = {"/usr/lib/gcc", "/usr/lib64/gcc", NULL};
  const char *triples[] = {"x86_64-pc-linux-gnu", "x86_64-linux-gnu", NULL};
  const int majors[] = {20, 19, 18, 17, 16, 15, 14, 13, 12, 11, 10, 9, 0};

  for (int b = 0; bases[b] != NULL; b++) {
    for (int t = 0; triples[t] != NULL; t++) {
      for (int m = 0; majors[m] != 0; m++) {
        char candidate[PATH_MAX];
        snprintf(candidate, sizeof(candidate), "%s/%s/%d/%s", bases[b],
                 triples[t], majors[m], name);
        char *found = dup_readable(candidate);
        if (found)
          return found;
      }
    }
  }
  return NULL;
}

/* The linker command line is assembled here and handed to the in-process lld
 * drivers as one string.  It used to live in a fixed 64KB buffer whose overflow
 * silently dropped arguments, so a program with many link inputs got a corrupt
 * link command.  Grow instead, and remember whether anything was dropped so an
 * out-of-memory situation is at least visible. */
typedef struct {
  char *buf;
  size_t len;
  size_t cap;
  int dropped;
} ArgBuf;

static void argbuf_append(ArgBuf *buf, const char *arg) {
  if (!arg || arg[0] == '\0')
    return;
  size_t alen = strlen(arg);
  size_t needed = buf->len + (buf->len > 0 ? 1 : 0) + alen + 1;
  if (needed > buf->cap) {
    size_t cap = buf->cap ? buf->cap : 1024;
    while (cap < needed) {
      if (cap > SIZE_MAX / 2) {
        buf->dropped = 1;
        return;
      }
      cap *= 2;
    }
    char *grown = (char *)realloc(buf->buf, cap);
    if (grown == NULL) {
      buf->dropped = 1;
      return;
    }
    buf->buf = grown;
    buf->cap = cap;
  }
  if (buf->len > 0)
    buf->buf[buf->len++] = ' ';
  memcpy(buf->buf + buf->len, arg, alen + 1);
  buf->len += alen;
}

static void append_opt_path(ArgBuf *buf, const char *opt, const char *path) {
  if (!path || path[0] == '\0')
    return;
  argbuf_append(buf, opt);
  argbuf_append(buf, path);
}

static void append_default_search_dirs(ArgBuf *buf) {
  const char *dirs[] = {"/usr/lib", "/usr/lib64", "/lib", "/lib64",
                        "/usr/lib/x86_64-linux-gnu",
                        "/lib/x86_64-linux-gnu", NULL};
  for (int i = 0; dirs[i] != NULL; i++) {
    if (access(dirs[i], R_OK) == 0) {
      char arg[PATH_MAX + 3];
      snprintf(arg, sizeof(arg), "-L%s", dirs[i]);
      argbuf_append(buf, arg);
    }
  }
}

int vix_api_llc_compile_object(const char *input_path, const char *output_path,
                               const char *target_triple) {
  return vix_llc_compile_ir_to_object(input_path, output_path, target_triple);
}

int vix_api_llc_compile_asm(const char *input_path, const char *output_path,
                            const char *target_triple) {
  return vix_llc_compile_ir_to_asm(input_path, output_path, target_triple);
}

const char *vix_api_llc_error(void) { return vix_llc_last_error(); }

int vix_api_optimize_ir(const char *input_path, const char *output_path,
                        const char *target_triple, int opt_level) {
  return vix_passes_optimize_ir(input_path, output_path, target_triple,
                                opt_level);
}

const char *vix_api_passes_error(void) { return vix_passes_last_error(); }

int vix_api_source_has_no_std(const char *src) {
  if (!src)
    return 0;
  return strstr(src, "#[no_std]") != NULL;
}

int vix_api_link_executable(const char *object_path, const char *runtime_object,
                            const char *output_path, const char *link_args,
                            int no_std) {
  ArgBuf args;
  args.buf = NULL;
  args.len = 0;
  args.cap = 0;
  args.dropped = 0;

  if (!object_path || !output_path || object_path[0] == '\0' ||
      output_path[0] == '\0') {
    return 1;
  }

#if defined(_WIN32) || defined(WIN32)
  /* PE/COFF: drive lld-link in-process.  Library search relies on the %LIB%
   * environment variable (as with link.exe) so the MSVC CRT and Windows SDK
   * import libraries can be found. */
  char out_arg[PATH_MAX + 16];
  snprintf(out_arg, sizeof(out_arg), "-OUT:%s", output_path);
  argbuf_append(&args, "-NOLOGO");
  argbuf_append(&args, "-MACHINE:X64");
  argbuf_append(&args, "-STACK:16777216");
  argbuf_append(&args, out_arg);

  argbuf_append(&args, object_path);
  if (!no_std && runtime_object && runtime_object[0] != '\0')
    argbuf_append(&args, runtime_object);

  if (link_args && link_args[0] != '\0')
    argbuf_append(&args, link_args);

  if (!no_std) {
    argbuf_append(&args, "msvcrt.lib");
    argbuf_append(&args, "ucrt.lib");
    argbuf_append(&args, "vcruntime.lib");
    argbuf_append(&args, "kernel32.lib");
  } else {
    argbuf_append(&args, "-ENTRY:main");
    argbuf_append(&args, "-SUBSYSTEM:CONSOLE");
  }

  if (args.dropped)
    fprintf(stderr, "vixc: out of memory while assembling the link command\n");
  if (args.buf == NULL)
    args.buf = strdup("");
  int status = vix_lld_link_coff(args.buf);
  free(args.buf);
  return status;
#else
  argbuf_append(&args, "-o");
  argbuf_append(&args, output_path);

  if (!no_std) {
    const char *crt_paths[] = {"/usr/lib/Scrt1.o",
                               "/usr/lib/x86_64-linux-gnu/Scrt1.o",
                               "/usr/lib/crt1.o",
                               "/usr/lib/x86_64-linux-gnu/crt1.o", NULL};
    const char *crti_paths[] = {"/usr/lib/crti.o",
                                "/usr/lib/x86_64-linux-gnu/crti.o", NULL};
    char *crt = find_first_file(crt_paths);
    char *crti = find_first_file(crti_paths);
    char *crtbegin = find_in_gcc_dir("crtbeginS.o");
    argbuf_append(&args, crt);
    argbuf_append(&args, crti);
    argbuf_append(&args, crtbegin);
    free(crt);
    free(crti);
    free(crtbegin);
  }

  argbuf_append(&args, object_path);
  if (!no_std && runtime_object && runtime_object[0] != '\0')
    argbuf_append(&args, runtime_object);

  if (link_args && link_args[0] != '\0')
    argbuf_append(&args, link_args);

  if (!no_std) {
    append_default_search_dirs(&args);
    const char *interp_paths[] = {"/usr/lib/ld-linux-x86-64.so.2",
                                  "/lib64/ld-linux-x86-64.so.2",
                                  "/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2",
                                  NULL};
    const char *crtn_paths[] = {"/usr/lib/crtn.o",
                                "/usr/lib/x86_64-linux-gnu/crtn.o", NULL};
    char *interp = find_first_file(interp_paths);
    char *crtend = find_in_gcc_dir("crtendS.o");
    char *crtn = find_first_file(crtn_paths);
    append_opt_path(&args, "-dynamic-linker", interp);
    argbuf_append(&args, "-lm");
    argbuf_append(&args, "-lc");
    argbuf_append(&args, crtend);
    argbuf_append(&args, crtn);
    free(interp);
    free(crtend);
    free(crtn);
  }

  if (args.dropped)
    fprintf(stderr, "vixc: out of memory while assembling the link command\n");
  if (args.buf == NULL)
    args.buf = strdup("");
  int status = vix_lld_link_elf(args.buf);
  free(args.buf);
  return status;
#endif
}

int vix_api_link_executable_std(const char *object_path,
                                const char *runtime_object,
                                const char *output_path,
                                const char *link_args) {
  return vix_api_link_executable(object_path, runtime_object, output_path,
                                 link_args, 0);
}

int vix_api_link_executable_nostd(const char *object_path,
                                  const char *output_path,
                                  const char *link_args) {
  return vix_api_link_executable(object_path, "", output_path, link_args, 1);
}

int vix_api_link_object(const char *object_path, const char *output_path,
                        const char *link_args, int no_std) {
  return vix_api_link_executable(object_path, "", output_path, link_args,
                                 no_std);
}

const char *vix_api_link_error(void) { return vix_lld_last_error(); }
