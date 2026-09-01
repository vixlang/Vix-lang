#define _GNU_SOURCE
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32) || defined(WIN32)
#include <io.h>
#define access _access
#define strdup _strdup
#define R_OK 4
#define X_OK 0
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

static void append_arg(char *buf, size_t size, const char *arg) {
  if (!arg || arg[0] == '\0')
    return;
  size_t len = strlen(buf);
  if (len + strlen(arg) + 2 >= size)
    return;
  if (len > 0)
    strncat(buf, " ", size - strlen(buf) - 1);
  strncat(buf, arg, size - strlen(buf) - 1);
}

static void append_opt_path(char *buf, size_t size, const char *opt,
                            const char *path) {
  if (!path || path[0] == '\0')
    return;
  append_arg(buf, size, opt);
  append_arg(buf, size, path);
}

static void append_default_search_dirs(char *buf, size_t size) {
  const char *dirs[] = {"/usr/lib", "/usr/lib64", "/lib", "/lib64",
                        "/usr/lib/x86_64-linux-gnu",
                        "/lib/x86_64-linux-gnu", NULL};
  for (int i = 0; dirs[i] != NULL; i++) {
    if (access(dirs[i], R_OK) == 0) {
      char arg[PATH_MAX + 3];
      snprintf(arg, sizeof(arg), "-L%s", dirs[i]);
      append_arg(buf, size, arg);
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
  char args[65536];
  args[0] = '\0';

  if (!object_path || !output_path || object_path[0] == '\0' ||
      output_path[0] == '\0') {
    return 1;
  }

  append_arg(args, sizeof(args), "-o");
  append_arg(args, sizeof(args), output_path);

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
    append_arg(args, sizeof(args), crt);
    append_arg(args, sizeof(args), crti);
    append_arg(args, sizeof(args), crtbegin);
    free(crt);
    free(crti);
    free(crtbegin);
  }

  append_arg(args, sizeof(args), object_path);
  if (!no_std && runtime_object && runtime_object[0] != '\0')
    append_arg(args, sizeof(args), runtime_object);

  if (link_args && link_args[0] != '\0')
    append_arg(args, sizeof(args), link_args);

  if (!no_std) {
    append_default_search_dirs(args, sizeof(args));
    const char *interp_paths[] = {"/usr/lib/ld-linux-x86-64.so.2",
                                  "/lib64/ld-linux-x86-64.so.2",
                                  "/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2",
                                  NULL};
    const char *crtn_paths[] = {"/usr/lib/crtn.o",
                                "/usr/lib/x86_64-linux-gnu/crtn.o", NULL};
    char *interp = find_first_file(interp_paths);
    char *crtend = find_in_gcc_dir("crtendS.o");
    char *crtn = find_first_file(crtn_paths);
    append_opt_path(args, sizeof(args), "-dynamic-linker", interp);
    append_arg(args, sizeof(args), "-lm");
    append_arg(args, sizeof(args), "-lc");
    append_arg(args, sizeof(args), crtend);
    append_arg(args, sizeof(args), crtn);
    free(interp);
    free(crtend);
    free(crtn);
  }

  return vix_lld_link_elf(args);
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
