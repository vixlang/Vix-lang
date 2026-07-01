/*
 * Copyright (c) 2026 Vix Language Authors. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "../include/ast.h"
#include "../include/codegen.h"
#include "../include/compat.h"
#include "../include/compiler.h"
#include "../include/ownership.h"
#include "../include/parser.h"
#include "../include/semantic.h"
#include "../include/typeck.h"
#include "compiler/Linker/Linker.h"
#include "compiler/Llc/Llc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <time.h>

static int vix_clock_gettime(int unused, struct timespec *ts) {
    (void)unused;
    LARGE_INTEGER freq, counter;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&counter);
    ts->tv_sec = (long)(counter.QuadPart / freq.QuadPart);
    ts->tv_nsec = (long)((counter.QuadPart % freq.QuadPart) * 1000000000 / freq.QuadPart);
    return 0;
}
#define CLOCK_MONOTONIC 1
#define clock_gettime vix_clock_gettime
#else
#include <libgen.h>
#include <limits.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#endif

extern FILE *yyin;
extern ASTNode *root;
static void print_timing_table(struct timespec t_start, struct timespec t_file,
                               struct timespec t_parse, struct timespec t_sema,
                               struct timespec t_codegen);
const char *current_input_filename = NULL;
static const char *find_bundled_libc(void) {
  static char libc_path[4096];
  char exe_dir[4096];

#ifdef _WIN32
  DWORD len = GetModuleFileNameA(NULL, exe_dir, sizeof(exe_dir));
  if (len == 0 || len >= sizeof(exe_dir))
    return NULL;
  char *last_sep = strrchr(exe_dir, '\\');
  if (!last_sep)
    last_sep = strrchr(exe_dir, '/');
  if (last_sep)
    *last_sep = '\0';
#else
  ssize_t len = readlink("/proc/self/exe", exe_dir, sizeof(exe_dir) - 1);
  if (len <= 0)
    return NULL;
  exe_dir[len] = '\0';
  char *last_sep = strrchr(exe_dir, '/');
  if (last_sep)
    *last_sep = '\0';
#endif
  static const char *const suffixes[] = {
#ifdef _WIN32
      "\\..\\libc", "\\libc",
#else
      "/../libc", "/libc",
#endif
      NULL};

  for (const char *const *s = suffixes; *s; ++s) {
    snprintf(libc_path, sizeof(libc_path), "%s%s", exe_dir, *s);
#ifdef _WIN32
    DWORD attr = GetFileAttributesA(libc_path);
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY))
      return libc_path;
#else
    /* Check if directory exists by trying to stat it */
    struct stat st;
    if (stat(libc_path, &st) == 0 && S_ISDIR(st.st_mode))
      return libc_path;
#endif
  }

  return NULL;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "OVERVIEW: Vix Compiler\n\n");
    fprintf(stderr, "USAGE: %s [options] <input.vix>\n", argv[0]);
    fprintf(stderr, "       %s file1.o file2.o ... -o <output>\n\n", argv[0]);
    fprintf(stderr, "OPTIONS:\n");
    fprintf(stderr, "  -o <file>              Write output to <file>\n");
    fprintf(stderr, "  -S [file]              Emit assembly to <file> "
                    "(default: <input>.s)\n");
    fprintf(stderr, "  -obj [file]            Emit object file to <file> "
                    "(default: <input>.o)\n");
    fprintf(stderr, "  -ll [file]             Emit LLVM IR to <file> (default: "
                    "<input>.ll)\n");
    fprintf(stderr, "  -llvm                  Print LLVM IR to stdout\n");
    fprintf(stderr, "  -ast                   Print AST to stdout\n");
    fprintf(stderr,
            "  -opt=lN                Set optimization level (N = 0..3)\n");
    fprintf(stderr,
            "  --target=<triple>      Set codegen/link target triple\n");
    fprintf(stderr,
            "  -static                Static linking (default: dynamic)\n");
    fprintf(stderr, "  -L <path>              Add library search path\n");
    fprintf(stderr, "  --check                Syntax & type check only\n");
    fprintf(stderr, "  --time                 Show phase timing breakdown\n");
    fprintf(stderr, "  --debug                Enable debug output\n");
    fprintf(stderr,
            "  -v, --version          Display compiler version information\n");
    fprintf(stderr, "  -h, --help             Display this help message\n");
    return 1;
  }

  char *out_f = NULL;
  char *llvm_f = NULL;
  char *obj_f = NULL;
  char *asm_f = NULL;
  char *in_f = NULL;
  int is_vic = 0;
  int save_c = 0;
  int gen_llvm = 0;
  int gen_obj = 0;
  int gen_asm = 0;
  int out_ast = 0;
  int out_llvm = 0;
  int dbg = 0;
  int opt_level = 0;
  char *target = NULL;
  int no_std = 0;
  int no_main = 0;
  int check_only = 0;
  int show_time = 0;
#define MAX_OBJ_FILES 256
  char *obj_files[MAX_OBJ_FILES];
  int obj_file_count = 0;
  int link_mode = 0;
  int static_link = 0;
#define MAX_LIB_PATHS 64
  char *lib_paths[MAX_LIB_PATHS];
  int lib_path_count = 0;
#define MAX_EXTRA_LIBS 64
  char *extra_libs[MAX_EXTRA_LIBS];
  int extra_lib_count = 0;
  for (int i = 1; i < argc; i++) {
    if (strncmp(argv[i], "--target=", 9) == 0) {
      target = argv[i] + 9;
    } else if (strcmp(argv[i], "--target") == 0) {
      if (i + 1 < argc) {
        target = argv[i + 1];
        i++;
      } else {
        fprintf(stderr, "Er: --target option requires a target triple\n");
        return 1;
      }
    } else if (strcmp(argv[i], "-o") == 0) {
      if (i + 1 < argc) {
        out_f = argv[i + 1];
        save_c = 1;
        i++;
      } else {
        fprintf(stderr, "Er: -o option requires a filename\n");
        return 1;
      }
    } else if (strcmp(argv[i], "-v") == 0 ||
               strcmp(argv[i], "--version") == 0 ||
               strcmp(argv[i], "-ver") == 0) {
      printf("Vix Compiler 0.4.2 Copyright(c) 2025-2026 LLVM : 22.1.2(8)\n");
      return 0;
    } else if (strcmp(argv[i], "-llvm") == 0) {
      out_llvm = 1;
    } else if (strcmp(argv[i], "-obj") == 0) {
      gen_obj = 1;
      gen_llvm = 1;
      if (i + 1 < argc && argv[i + 1][0] != '-') {
        obj_f = argv[i + 1];
        i++;
      }
    } else if (strcmp(argv[i], "-S") == 0 || strcmp(argv[i], "-s") == 0) {
      gen_asm = 1;
      gen_llvm = 1;
      if (i + 1 < argc && argv[i + 1][0] != '-') {
        asm_f = argv[i + 1];
        i++;
      }
    } else if (strcmp(argv[i], "-ll") == 0) {
      if (i + 1 < argc && argv[i + 1][0] != '-') {
        llvm_f = argv[i + 1];
        gen_llvm = 1;
        i++;
      } else {
        out_llvm = 1;
      }
    } else if (strcmp(argv[i], "-ast") == 0 || strcmp(argv[i], "--ast") == 0) {
      out_ast = 1;
    } else if (strcmp(argv[i], "--debug") == 0) {
      dbg = 1;
    } else if (strcmp(argv[i], "--check") == 0) {
      check_only = 1;
    } else if (strcmp(argv[i], "--time") == 0) {
      show_time = 1;
    } else if (strncmp(argv[i], "-opt=l", 6) == 0) {
      int lvl = argv[i][6] - '0';
      if (lvl < 0 || lvl > 3 || argv[i][7] != '\0') {
        fprintf(stderr, "Error: -opt=lN requires N in 0..3 (got %s)\n",
                argv[i]);
        return 1;
      }
      opt_level = lvl;
    } else if (strcmp(argv[i], "-static") == 0) {
      static_link = 1;
    } else if (strcmp(argv[i], "-L") == 0) {
      if (i + 1 < argc) {
        if (lib_path_count < MAX_LIB_PATHS) {
          lib_paths[lib_path_count++] = argv[i + 1];
        }
        i++;
      } else {
        fprintf(stderr, "Error: -L option requires a path\n");
        return 1;
      }
    } else if (strcmp(argv[i], "-l") == 0) {
      if (i + 1 < argc) {
        if (extra_lib_count < MAX_EXTRA_LIBS) {
          extra_libs[extra_lib_count++] = argv[i + 1];
        }
        i++;
      } else {
        fprintf(stderr, "Error: -l option requires a library name\n");
        return 1;
      }
    } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
      fprintf(stderr, "OVERVIEW: Vix Compiler LLVM Version:22.1.2(8)\n\n");
      fprintf(stderr, "OPTIONS:\n");
      fprintf(stderr, "USAGE: %s [options] <input.vix>\n", argv[0]);
      fprintf(stderr, "       %s file1.o file2.o ... -o <output>\n\n", argv[0]);
      fprintf(stderr, "  -o <file>              Write output to <file>\n");
      fprintf(stderr, "  -S [file]              Emit assembly to <file> "
                      "(default: <input>.s)\n");
      fprintf(stderr, "  -obj [file]            Emit object file to <file> "
                      "(default: <input>.o)\n");
      fprintf(stderr, "  -ll [file]             Emit LLVM IR to <file> "
                      "(default: <input>.ll)\n");
      fprintf(stderr, "  -llvm                  Print LLVM IR to stdout\n");
      fprintf(stderr, "  -ast                   Print AST to stdout\n");
      fprintf(stderr,
              "  -opt=lN                Set optimization level (N = 0..3)\n");
      fprintf(stderr,
              "  --target=<triple>      Set codegen/link target triple\n");
      fprintf(stderr,
              "  -static                Static linking (default: dynamic)\n");
      fprintf(stderr, "  -L <path>              Add library search path\n");
      fprintf(stderr, "  -l <lib>               Link with library\n");
      fprintf(stderr, "  --check                Syntax & type check only\n");
      fprintf(stderr, "  --time                 Show phase timing breakdown\n");
      fprintf(stderr, "  --debug                Enable debug output\n");
      fprintf(
          stderr,
          "  -v, --version          Display compiler version information\n");
      fprintf(stderr, "  -h, --help             Display this help message\n");
      return 0;
    } else if (argv[i][0] == '-' && strcmp(argv[i], "-") != 0) {
      fprintf(stderr, "Unknown option: %s\n", argv[i]);
      return 1;
    } else {
      size_t len = strlen(argv[i]);
      if (len > 2 && strcmp(argv[i] + len - 2, ".o") == 0) {
        if (obj_file_count < MAX_OBJ_FILES) {
          obj_files[obj_file_count++] = argv[i];
          link_mode = 1;
        } else {
          fprintf(stderr, "Error: too many object files (max %d)\n",
                  MAX_OBJ_FILES);
          return 1;
        }
      } else {
        in_f = argv[i];
        is_vic = len > 4 && strcmp(argv[i] + len - 4, ".vic") == 0;
      }
    }
  }
  vix_setenv("VIX_DEBUG", dbg ? "1" : "0", 1); // 通过环境变量控制调试输出
  vix_set_opt_level(opt_level);

  if (link_mode && obj_file_count > 0) {
    if (!out_f) {
      fprintf(stderr,
              "Error: -o <output> required when linking object files\n");
      return 1;
    }

    const char *eff_t = target;
    int bare = 0;
    if (eff_t && (strstr(eff_t, "unknown-none") != NULL ||
                  strstr(eff_t, "unknow-noe") != NULL)) {
      bare = 1;
    }

    const char *ls = NULL;
    if (bare) {
      ls = "linker.ld";
      if (!vix_file_readable(ls) && vix_file_readable("src/linker.ld")) {
        ls = "src/linker.ld";
      }
    }

    const char *link_err = NULL;
    VixLinkOptions link_opts = {
        .target_triple = eff_t,
        .bare_mode = bare,
        .linker_script = ls,
        .static_link = bare || static_link,
        .entry_point = bare ? "_start" : NULL,
        .libc_dir = find_bundled_libc(),
        .lib_paths = lib_path_count > 0 ? (const char **)lib_paths : NULL,
        .lib_path_count = lib_path_count,
        .extra_libs = extra_lib_count > 0 ? (const char **)extra_libs : NULL,
        .extra_lib_count = extra_lib_count,
    };

    if (!vix_link_multi((const char **)obj_files, obj_file_count, out_f,
                        &link_opts, &link_err)) {
      fprintf(stderr, "Error: Failed to link object files");
      if (link_err && link_err[0] != '\0') {
        fprintf(stderr, ":\n%s", link_err);
      }
      fprintf(stderr, "\n");
      return 1;
    }
    return 0;
  }

  if (!in_f) {
    in_f = argv[1];
  }
  int exp_mode = out_ast || out_llvm || gen_llvm || gen_obj || gen_asm;

  if (!exp_mode && !out_f && save_c == 0) {
    char *dot = strrchr(in_f, '.');
    if (dot) {
      size_t len = dot - in_f;
#ifdef _WIN32
      char *def_out = malloc(len + 5);
#else
      char *def_out = malloc(len + 1);
#endif
      if (def_out) {
        strncpy(def_out, in_f, len);
        def_out[len] = '\0';
#ifdef _WIN32
        strcat(def_out, ".exe");
#endif
        out_f = def_out;
        save_c = 1;
      }
    } else {
#ifdef _WIN32
      char *def_out = malloc(strlen(in_f) + 5);
      if (def_out) {
        strcpy(def_out, in_f);
        strcat(def_out, ".exe");
        out_f = def_out;
        save_c = 1;
      } else {
        out_f = in_f;
        save_c = 1;
      }
#else
      out_f = in_f;
      save_c = 1;
#endif
    }
  }

  FILE *input_file = fopen(in_f, "r");
  if (!input_file) {
    perror("Failed to open file");
    if (out_f && out_f != in_f && out_f != argv[1]) {
      free(out_f);
    }
    return 1;
  }

  {
    char buf[1024];
    while (fgets(buf, sizeof(buf), input_file) != NULL) {
      if (strstr(buf, "#[no_std]") != NULL) {
        no_std = 1;
      }
      if (strstr(buf, "#[no_main]") != NULL) {
        no_main = 1;
      }
    }
    rewind(input_file);
  }

  struct timespec t_start, t_file_ts, t_parse_ts, t_sema_ts, t_codegen_ts;
  if (show_time)
    clock_gettime(CLOCK_MONOTONIC, &t_start);

  const char *eff_t = target;
  if (!eff_t && (no_std || no_main)) {
    eff_t = "x86_64-unknown-none";
  }
#ifndef VIXC_FRONTEND_ONLY
  llvm_set_target_triple(eff_t);
#endif

  int bare = 0;
  if (eff_t && (strstr(eff_t, "unknown-none") != NULL ||
                strstr(eff_t, "unknow-noe") != NULL)) {
    bare = 1;
  }
  if (no_std || no_main) {
    bare = 1;
  }

  if (save_c) {
    gen_llvm = 1;
    if (!llvm_f) {
      char *dot = strrchr(out_f, '.');
      if (dot) {
        size_t len = dot - out_f;
        char *llvm_name = malloc(len + 4);
        if (llvm_name) {
          strncpy(llvm_name, out_f, len);
          llvm_name[len] = '\0';
          strcat(llvm_name, ".ll");
          llvm_f = llvm_name;
        }
      } else {
        char *llvm_name = malloc(strlen(out_f) + 4);
        if (llvm_name) {
          strcpy(llvm_name, out_f);
          strcat(llvm_name, ".ll");
          llvm_f = llvm_name;
        }
      }
    }
  }

  (void)is_vic;

  current_input_filename = in_f;
  load_source_file(in_f);
  set_location_with_column(in_f, 1, 1);
  yyin = input_file;

  if (show_time)
    clock_gettime(CLOCK_MONOTONIC, &t_file_ts);

  int result = yyparse();
  if (result == 0 && root) {
    inline_imports(root);
  }

  if (show_time)
    clock_gettime(CLOCK_MONOTONIC, &t_parse_ts);

  if (result == 0) {
    int errs = check_undefined_symbols(root);
    if (errs > 0) {
      fprintf(stderr, "Error: Found %d semantic error(s)\n", errs);
      if (root) {
        free_ast(root);
      }
      cleanup_error_handler();
      fclose(input_file);
      return 1;
    }

    if (typecheck_program(root) != 0) {
      fprintf(stderr, "Compilation failed with type errors\n");
      if (root) {
        free_ast(root);
      }
      cleanup_error_handler();
      fclose(input_file);
      return 1;
    }
    if (ownership_check_program(root) != 0) {
      fprintf(stderr, "Compilation failed with ownership errors\n");
      if (root) {
        free_ast(root);
      }
      cleanup_error_handler();
      fclose(input_file);
      return 1;
    }
    SymbolTable *g_tbl = create_symbol_table(NULL);
    int uvars = check_unused_variables(root, g_tbl);
    destroy_symbol_table(g_tbl);
    (void)uvars;
    if (get_error_count() > 0) {
      fprintf(stderr, "Compilation failed with %d error(s)\n",
              get_error_count());
      if (root) {
        free_ast(root);
      }
      cleanup_error_handler();
      fclose(input_file);
      return 1;
    }

    if (show_time)
      clock_gettime(CLOCK_MONOTONIC, &t_sema_ts);
    if (show_time)
      t_codegen_ts = t_sema_ts; /* default = sema time if no codegen */

    if (check_only) {
      print_error_summary();
      if (show_time)
        print_timing_table(t_start, t_file_ts, t_parse_ts, t_sema_ts,
                           t_codegen_ts);
      if (root)
        free_ast(root);
      cleanup_error_handler();
      fclose(input_file);
      return get_error_count() > 0 ? 1 : 0;
    }

#ifndef VIXC_FRONTEND_ONLY
    if (gen_llvm) {
      char llvm_filename[2048];
      if (!llvm_f) {
        char *dot = strrchr(in_f, '.');
        if (dot) {
          size_t len = dot - in_f;
          snprintf(llvm_filename, sizeof(llvm_filename), "%.*s.ll", (int)len,
                   in_f);
        } else {
          snprintf(llvm_filename, sizeof(llvm_filename), "%s.ll", in_f);
        }
        llvm_f = llvm_filename;
      } else {
        if (strstr(llvm_f, ".ll") == NULL) {
          int written =
              snprintf(llvm_filename, sizeof(llvm_filename), "%s.ll", llvm_f);
          if (written < 0 || (size_t)written >= sizeof(llvm_filename)) {
            fprintf(stderr, "Error: LLVM IR output path is too long\n");
            if (root) {
              free_ast(root);
            }
            cleanup_error_handler();
            fclose(input_file);
            return 1;
          }
          llvm_f = llvm_filename;
        }
      }

      FILE *llvm_file = fopen(llvm_f, "w");
      if (!llvm_file) {
        fprintf(stderr, "Error: Cannot open LLVM IR file %s for writing\n",
                llvm_f);
        fclose(input_file);
        return 1;
      }

      llvm_emit_from_ast(root, llvm_file);
      fclose(llvm_file);

      if (show_time)
        clock_gettime(CLOCK_MONOTONIC, &t_codegen_ts);

      if (get_error_count() > 0) {
        fprintf(stderr, "Compilation failed with %d error(s)\n",
                get_error_count());
        remove(llvm_f);
        if (root) {
          free_ast(root);
        }
        cleanup_error_handler();
        fclose(input_file);
        return 1;
      }

      if (gen_obj) {
        char oname[2048];
        const char *fobj = obj_f;
        if (!fobj) {
          char *dot = strrchr(in_f, '.');
          if (dot) {
            size_t len = dot - in_f;
            snprintf(oname, sizeof(oname), "%.*s.o", (int)len, in_f);
          } else {
            snprintf(oname, sizeof(oname), "%s.o", in_f);
          }
          fobj = oname;
        } else if (strstr(fobj, ".o") == NULL) {
          snprintf(oname, sizeof(oname), "%s.o", fobj);
          fobj = oname;
        }
        const char *llc_err = NULL;
        if (!llc_compile_to_object(llvm_f, fobj, eff_t ? eff_t : "", bare,
                                   opt_level, &llc_err)) {
          fprintf(stderr,
                  "Error: Failed to compile LLVM IR to object file via Llc");
          if (llc_err && llc_err[0] != '\0') {
            fprintf(stderr, ": %s", llc_err);
          }
          fprintf(stderr, "\n");
          fclose(input_file);
          return 1;
        }

        if (!save_c) {
          remove(llvm_f);
          if (root) {
            free_ast(root);
          }
          fclose(input_file);
          if (show_time)
            print_timing_table(t_start, t_file_ts, t_parse_ts, t_sema_ts,
                               t_codegen_ts);
          print_error_summary();
          return 0;
        }
      }
      if (gen_asm) {
        char oname[2048];
        const char *fasm = asm_f;
        if (!fasm) {
          char *dot = strrchr(in_f, '.');
          if (dot) {
            size_t len = dot - in_f;
            snprintf(oname, sizeof(oname), "%.*s.s", (int)len, in_f);
          } else {
            snprintf(oname, sizeof(oname), "%s.s", in_f);
          }
          fasm = oname;
        } else if (strstr(fasm, ".s") == NULL) {
          snprintf(oname, sizeof(oname), "%s.s", fasm);
          fasm = oname;
        }

        const char *llc_err = NULL;
        if (!llc_compile_to_assembly(llvm_f, fasm, eff_t ? eff_t : "", bare,
                                     opt_level, &llc_err)) {
          fprintf(stderr,
                  "Error: Failed to compile LLVM IR to assembly via Llc");
          if (llc_err && llc_err[0] != '\0') {
            fprintf(stderr, ": %s", llc_err);
          }
          fprintf(stderr, "\n");
          fclose(input_file);
          return 1;
        }

        remove(llvm_f);

        if (root) {
          free_ast(root);
        }
        fclose(input_file);
        if (show_time)
          print_timing_table(t_start, t_file_ts, t_parse_ts, t_sema_ts,
                             t_codegen_ts);
        print_error_summary();
        return 0;
      }
      if (out_f && save_c) {
        const char *ls = NULL;
        if (bare) {
          ls = "linker.ld";
          if (!vix_file_readable(ls) && vix_file_readable("src/linker.ld")) {
            ls = "src/linker.ld";
          }
        }

        char obj_file[2048];
        {
          char *dot = strrchr(llvm_f, '.');
          if (dot) {
            size_t len = dot - llvm_f;
            snprintf(obj_file, sizeof(obj_file), "%.*s.o", (int)len, llvm_f);
          } else {
            int written = snprintf(obj_file, sizeof(obj_file), "%s.o", llvm_f);
            if (written < 0 || (size_t)written >= sizeof(obj_file)) {
              fprintf(stderr, "Error: object output path is too long\n");
              remove(llvm_f);
              fclose(input_file);
              return 1;
            }
          }
        }

        const char *llc_err = NULL;
        if (!llc_compile_to_object(llvm_f, obj_file, eff_t ? eff_t : "", bare,
                                   opt_level, &llc_err)) {
          fprintf(stderr,
                  "Error: Failed to compile LLVM IR to object file via Llc");
          if (llc_err && llc_err[0] != '\0') {
            fprintf(stderr, ": %s", llc_err);
          }
          fprintf(stderr, "\n");
          remove(llvm_f);
          fclose(input_file);
          return 1;
        }

        const char *link_err = NULL;
        VixLinkOptions link_opts = {
            .target_triple = eff_t,
            .bare_mode = bare,
            .linker_script = ls,
            .static_link = bare || static_link,
            .entry_point = bare ? "_start" : NULL,
            .libc_dir = find_bundled_libc(),
            .lib_paths = lib_path_count > 0 ? (const char **)lib_paths : NULL,
            .lib_path_count = lib_path_count,
            .extra_libs = extra_lib_count > 0 ? (const char **)extra_libs : NULL,
            .extra_lib_count = extra_lib_count,
        };
        if (!vix_link(obj_file, out_f, &link_opts, &link_err)) {
          fprintf(stderr, "Error: Failed to link object file to executable");
          if (link_err && link_err[0] != '\0') {
            fprintf(stderr, ":\n%s", link_err);
          }
          fprintf(stderr, "\n");
          remove(llvm_f);
          remove(obj_file);
          fclose(input_file);
          return 1;
        }

        remove(llvm_f);
        remove(obj_file);
      }

      if (root) {
        free_ast(root);
      }
      fclose(input_file);
      if (show_time)
        print_timing_table(t_start, t_file_ts, t_parse_ts, t_sema_ts,
                           t_codegen_ts);
      print_error_summary();
      return 0;
    }

    if (out_ast) {
      printf("===========================AST=======================\n");
      print_ast(root, 0);
      printf("===================================================\n");
      if (root)
        free_ast(root);
      fclose(input_file);
      if (show_time)
        print_timing_table(t_start, t_file_ts, t_parse_ts, t_sema_ts,
                           t_codegen_ts);
      print_error_summary();
      return 0;
    }

    if (out_llvm) {
      printf("=========================LLVM IR===================\n");
      llvm_emit_from_ast(root, stdout);
      printf("===================================================\n");
      if (root)
        free_ast(root);
      fclose(input_file);
      if (show_time)
        print_timing_table(t_start, t_file_ts, t_parse_ts, t_sema_ts,
                           t_codegen_ts);
      print_error_summary();
      return 0;
    }
#else
    if (root) {
      free_ast(root);
    }
    fclose(input_file);
    if (show_time)
      print_timing_table(t_start, t_file_ts, t_parse_ts, t_sema_ts,
                         t_codegen_ts);
    print_error_summary();
    return 0;
#endif
  } else {
    if (get_error_count() == 0) {
      const char *fname =
          current_input_filename ? current_input_filename : "unknown";
      report_syntax_error_with_location("parsing failed due to syntax errors",
                                        fname, 1);
    }
  }

  if (root) {
    free_ast(root);
  }
  cleanup_error_handler();
  fclose(input_file);
  return result;
}

static void print_timing_table(struct timespec t_start, struct timespec t_file,
                               struct timespec t_parse, struct timespec t_sema,
                               struct timespec t_codegen) {
  double ms_file = (t_file.tv_sec - t_start.tv_sec) * 1000.0 +
                   (t_file.tv_nsec - t_start.tv_nsec) / 1e6;
  double ms_parse = (t_parse.tv_sec - t_file.tv_sec) * 1000.0 +
                    (t_parse.tv_nsec - t_file.tv_nsec) / 1e6;
  double ms_sema = (t_sema.tv_sec - t_parse.tv_sec) * 1000.0 +
                   (t_sema.tv_nsec - t_parse.tv_nsec) / 1e6;
  double ms_codegen = (t_codegen.tv_sec - t_sema.tv_sec) * 1000.0 +
                      (t_codegen.tv_nsec - t_sema.tv_nsec) / 1e6;
  double ms_total = ms_file + ms_parse + ms_sema + ms_codegen;

  fprintf(stderr,
          "\033[36m── Phase Timings ──────────────────────────────\033[0m\n");
  fprintf(stderr, "  File I/O    %9.2f ms  %5.1f%%\n", ms_file,
          ms_file / ms_total * 100);
  fprintf(stderr, "  Parse       %9.2f ms  %5.1f%%\n", ms_parse,
          ms_parse / ms_total * 100);
  fprintf(stderr, "  Semantic    %9.2f ms  %5.1f%%\n", ms_sema,
          ms_sema / ms_total * 100);
  fprintf(stderr, "  Codegen     %9.2f ms  %5.1f%%\n", ms_codegen,
          ms_codegen / ms_total * 100);
  fprintf(stderr,
          "\033[36m  ────────────────────────────────────────────\033[0m\n");
  fprintf(stderr, "  Total       %9.2f ms\n", ms_total);
}
