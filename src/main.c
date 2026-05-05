/*
vix 语言 0.0.1版本完工
*/

/*this vixc is for Linux and Unix-like system Not for Windows cause a lot of lib not compatible(Fuck you windows
 BUT azhz's fork fix these "BUGS"
 you can clone win-build-support(Thank you azhz
 Enjoy it!
*/


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include "../include/vix_wincompat.h"
#else
#include <unistd.h>
#endif
#include "../include/ast.h"
#include "../include/parser.h"
#include "../include/compiler.h"
#include "../include/codegen.h"
#include "../include/semantic.h"
#include "../include/typeck.h"
#include "compiler/Llc/Llc.h"

extern FILE* yyin;
extern ASTNode* root;
const char* current_input_filename = NULL;

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <input.vix> [-o output_file]\n", argv[0]);
        fprintf(stderr, "       %s <input.vix> -ll <llvm_ir_file>\n", argv[0]);
        fprintf(stderr, "       %s <input.vix> -obj [obj_file] (output object file via llc)\n", argv[0]);
        fprintf(stderr, "       %s <input.vix> -S [asm_file] (output assembly via llc)\n", argv[0]);
        fprintf(stderr, "       %s <input.vix> -ast (output AST only)\n", argv[0]);
        fprintf(stderr, "       %s <input.vix> -ll (output LLVM IR only)\n", argv[0]);
        fprintf(stderr, "       %s <input.vix> -llvm (output LLVM IR only)\n", argv[0]);
        fprintf(stderr, "       %s <input.vix> --debug (enable debug logs)\n", argv[0]);
        fprintf(stderr, "       %s <input.vix> -opt=lN (optimization level N=0..3)\n", argv[0]);
        fprintf(stderr, "       %s <input.vix> --target=<triple> (set codegen/link target)\n", argv[0]);
        return 1;
    }
    
    char* out_f = NULL;
    char* llvm_f = NULL;
    char* obj_f = NULL;
    char* asm_f = NULL;
    char* in_f = NULL;
    int is_vic = 0;
    int save_c = 0;
    int gen_llvm = 0;
    int gen_obj = 0;
    int gen_asm = 0;
    int out_ast = 0;
    int out_llvm = 0;
    int dbg = 0;
    int opt_level = 0;
    char* target = NULL;
    int no_std = 0;
    int no_main = 0;
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
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0 || strcmp(argv[i] , "-ver") == 0){
            printf("Vix Compiler 0.1.0_rc.1 (Beta_26.05.01) Copyright(c) 2025-2026\n");
            return 0;
        }
        else if (strcmp(argv[i], "-llvm") == 0) {
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
        }else if (strcmp(argv[i], "-ast") == 0) {
            out_ast = 1;
        } else if (strcmp(argv[i], "--debug") == 0) {
            dbg = 1;
        } else if (strncmp(argv[i], "-opt=l", 6) == 0) {
            int lvl = argv[i][6] - '0';
            if (lvl < 0 || lvl > 3 || argv[i][7] != '\0') {
                fprintf(stderr, "Error: -opt=lN requires N in 0..3 (got %s)\n", argv[i]);
                return 1;
            }
            opt_level = lvl;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            fprintf(stderr, "usage: %s <input.vix> [-o output_file]\n", argv[0]);
            fprintf(stderr, "       %s <input.vix> -llvm <llvm_ir_file>\n", argv[0]);
            fprintf(stderr, "       %s <input.vix> -ll <llvm_ir_file>\n", argv[0]);
            fprintf(stderr, "       %s <input.vix> -obj [obj_file] (output object file via llc)\n", argv[0]);
            fprintf(stderr, "       %s <input.vix> -ast (output AST only)\n", argv[0]);
            fprintf(stderr, "       %s <input.vix> -ll (output LLVM IR only)\n", argv[0]);
            fprintf(stderr, "       %s <input.vix> -llvm (output LLVM IR only)\n", argv[0]);
            fprintf(stderr, "       %s <input.vix> --debug (enable debug logs)\n", argv[0]);
            fprintf(stderr, "       %s <input.vix> -opt=lN (optimization level N=0..3)\n", argv[0]);
            fprintf(stderr, "       %s <input.vix> --target=<triple> (set codegen/link target, e.g. x86_64-unknown-none)\n", argv[0]);
            fprintf(stderr, "       %s <input.vix> (LLVM backend is the default backend)\n", argv[0]);
            fprintf(stderr, "       %s <input.vix> -S (output assembly only)\n", argv[0]);
            return 0;
        } else if (argv[i][0] == '-' && strcmp(argv[i], "-") != 0) {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            return 1;
        } else {
            is_vic = strlen(argv[i]) > 4 && strcmp(argv[i] + strlen(argv[i]) - 4, ".vic") == 0;
        }
    }
#ifdef _WIN32
    char env_buf[32];
    snprintf(env_buf, sizeof(env_buf), "VIX_DEBUG=%s", dbg ? "1" : "0");
    _putenv(env_buf);
#else
    setenv("VIX_DEBUG", dbg ? "1" : "0", 1);
#endif
    vix_set_opt_level(opt_level);
    if (!in_f) {
        in_f = argv[1];
    }
    int exp_mode =
        out_ast ||
        out_llvm ||
        gen_llvm ||
        gen_obj ||
        gen_asm;

    if (!exp_mode &&
        !out_f &&
        save_c == 0) {
        char* dot = strrchr(in_f, '.');
        if (dot) {
            size_t len = dot - in_f;
            char* def_out = malloc(len + 1);
            if (def_out) {
                strncpy(def_out, in_f, len);
                def_out[len] = '\0';
                out_f = def_out;
                save_c = 1;
            }
        } else {
            out_f = in_f;
            save_c = 1;
        }
    }
    
    FILE* input_file = fopen(in_f, "r");
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

    const char* eff_t = target;
    if (!eff_t && (no_std || no_main)) {
        eff_t = "x86_64-unknown-none";
    }
    llvm_set_target_triple(eff_t);

    int bare = 0;
    if (eff_t && (strstr(eff_t, "unknown-none") != NULL || strstr(eff_t, "unknow-noe") != NULL)) {
        bare = 1;
    }
    if (no_std || no_main) {
        bare = 1;
    }
    
    if (save_c) {
        gen_llvm = 1;
        if (!llvm_f) {
            char* dot = strrchr(out_f, '.');
            if (dot) {
                size_t len = dot - out_f;
                char* llvm_name = malloc(len + 4);
                if (llvm_name) {
                    strncpy(llvm_name, out_f, len);
                    llvm_name[len] = '\0';
                    strcat(llvm_name, ".ll");
                    llvm_f = llvm_name;
                }
            } else {
                char* llvm_name = malloc(strlen(out_f) + 4);
                if (llvm_name) {
                    strcpy(llvm_name, out_f);
                    strcat(llvm_name, ".ll");
                    llvm_f = llvm_name;
                }
            }
        }
    }

    if (is_vic && gen_llvm) {
        char llvm_filename[256];
        if (strstr(llvm_f, ".ll") == NULL) {
            snprintf(llvm_filename, sizeof(llvm_filename), "%s.ll", llvm_f);
        } else {
            strcpy(llvm_filename, llvm_f);
        }
        
        FILE* llvm_file = fopen(llvm_filename, "w");
        if (!llvm_file) {
            fprintf(stderr, "Er: Cannot open LLVM IR file %s for writing\n", llvm_filename);
            fclose(input_file);
            return 1;
        }
        
        llvm_emit_from_ast(root, llvm_file);
        fclose(llvm_file);
        fclose(input_file);
        return 0;
    }
    
    current_input_filename = in_f;
    load_source_file(in_f);
    set_location_with_column(in_f, 1, 1);
    yyin = input_file;
    
    int result = yyparse();
    if (result == 0 && root) {
        inline_imports(root);
    }
    
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
        SymbolTable* g_tbl = create_symbol_table(NULL);
        int uvars = check_unused_variables(root, g_tbl);
        destroy_symbol_table(g_tbl);
        if (uvars > 0) {
            fprintf(stderr, "\033[33mFound %d unused variable(s)\033[0m\n", uvars);
        }
        if (get_error_count() > 0) {
            fprintf(stderr, "Compilation failed with %d error(s)\n", get_error_count());
            if (root) {
                free_ast(root);
            }
            cleanup_error_handler();
            fclose(input_file);
            return 1;
        }
        
        if (gen_llvm) {
            char llvm_filename[2048];
            if (!llvm_f) {
                char* dot = strrchr(in_f, '.');
                if (dot) {
                    size_t len = dot - in_f;
                    snprintf(llvm_filename, sizeof(llvm_filename), "%.*s.ll", (int)len, in_f);
                } else {
                    snprintf(llvm_filename, sizeof(llvm_filename), "%s.ll", in_f);
                }
                llvm_f = llvm_filename;
            } else {
                if (strstr(llvm_f, ".ll") == NULL) {
                    snprintf(llvm_filename, sizeof(llvm_filename), "%s.ll", llvm_f);
                    llvm_f = llvm_filename;
                }
            }
            
            FILE* llvm_file = fopen(llvm_f, "w");
            if (!llvm_file) {
                fprintf(stderr, "Error: Cannot open LLVM IR file %s for writing\n", llvm_f);
                fclose(input_file);
                return 1;
            }
            
            llvm_emit_from_ast(root, llvm_file);
            fclose(llvm_file);

            if (get_error_count() > 0) {
                fprintf(stderr, "Compilation failed with %d error(s)\n", get_error_count());
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
                const char* fobj = obj_f;
                if (!fobj) {
                    char* dot = strrchr(in_f, '.');
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
                const char* llc_err = NULL;
                if (!llc_compile_to_object(llvm_f, fobj, eff_t ? eff_t : "", bare, opt_level, &llc_err)) {
                    fprintf(stderr, "Error: Failed to compile LLVM IR to object file via Llc");
                    if (llc_err && llc_err[0] != '\0') {
                        fprintf(stderr, ": %s", llc_err);
                    }
                    fprintf(stderr, "\n");
                    fclose(input_file);
                    return 1;
                }

                if (!save_c) {
                    if (root) {
                        free_ast(root);
                    }
                    fclose(input_file);
                    return 0;
                }
            }
            if (gen_asm) {
                char oname[2048];
                const char* fasm = asm_f;
                if (!fasm) {
                    char* dot = strrchr(in_f, '.');
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

                const char* llc_err = NULL;
                if (!llc_compile_to_assembly(llvm_f, fasm, eff_t ? eff_t : "", bare, opt_level, &llc_err)) {
                    fprintf(stderr, "Error: Failed to compile LLVM IR to assembly via Llc");
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
                return 0;
            }
            if (out_f && save_c) {
                const char* ls = "linker.ld";
                if (bare && access(ls, R_OK) != 0 && access("src/linker.ld", R_OK) == 0) {
                    ls = "src/linker.ld";
                }

                char obj_file[2048];
                {
                    char* dot = strrchr(llvm_f, '.');
                    if (dot) {
                        size_t len = dot - llvm_f;
                        snprintf(obj_file, sizeof(obj_file), "%.*s.o", (int)len, llvm_f);
                    } else {
                        snprintf(obj_file, sizeof(obj_file), "%s.o", llvm_f);
                    }
                }

                const char* llc_err = NULL;
                if (!llc_compile_to_object(llvm_f, obj_file, eff_t ? eff_t : "", bare, opt_level, &llc_err)) {
                    fprintf(stderr, "Error: Failed to compile LLVM IR to object file via Llc");
                    if (llc_err && llc_err[0] != '\0') {
                        fprintf(stderr, ": %s", llc_err);
                    }
                    fprintf(stderr, "\n");
                    remove(llvm_f);
                    fclose(input_file);
                    return 1;
                }

                size_t ccmd_sz = 8192;
                char *ccmd = malloc(ccmd_sz);
                if (ccmd == NULL) {
                    fprintf(stderr, "Er: Failed to allocate memory for clang command\n");
                    remove(llvm_f);
                    remove(obj_file);
                    fclose(input_file);
                    return 1;
                }

                if (bare) {
                    const char* f_t = eff_t ? eff_t : "x86_64-unknown-none";
                    snprintf(ccmd, ccmd_sz,
                             "clang %s -o %s -target %s -ffreestanding -fno-builtin -fno-pic -fno-pie -no-pie -nostdlib -nostartfiles -nodefaultlibs -static -Wl,--build-id=none -Wl,--no-dynamic-linker -Wl,-z,max-page-size=0x1000 -Wl,-e,_start -Wl,-T,%s",
                             obj_file, out_f, f_t, ls);
                } else if (eff_t) {
                    snprintf(ccmd, ccmd_sz,
                             "clang %s -o %s -target %s -lm -lstdc++",
                             obj_file, out_f, eff_t);
                } else {
                    snprintf(ccmd, ccmd_sz, "clang %s -o %s -lm -lstdc++", obj_file, out_f);
                }
                
                int cres = system(ccmd);
                if (cres != 0) {
                    fprintf(stderr, "Error: Failed to link object file to executable\n");
                    free(ccmd);
                    remove(llvm_f);
                    remove(obj_file);
                    fclose(input_file);
                    return 1;
                }
                
                free(ccmd);
                
                remove(llvm_f);
                remove(obj_file);
            }
            
            if (root) {
                free_ast(root);
            }
            fclose(input_file);
            return 0;
        }

        if (out_ast) {
            printf("===========================AST=======================\n");
            print_ast(root, 0);
            printf("===================================================\n");
            if (root) free_ast(root);
            fclose(input_file);
            return 0;
        }

        if (out_llvm) {
            printf("=========================LLVM IR===================\n");
            llvm_emit_from_ast(root, stdout);
            printf("===================================================\n");
            if (root) free_ast(root);
            fclose(input_file);
            return 0;
        }
    } else {
        if (get_error_count() == 0) {
            const char* fname = current_input_filename ? current_input_filename : "unknown";
            report_syntax_error_with_location("parsing failed due to syntax errors", fname, 1);
        }
    }
    
    if (root) {
        free_ast(root);
    }
    cleanup_error_handler();
    fclose(input_file);
    return result;
}
