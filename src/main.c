/*
vix 语言 0.0.1版本完工
*/
//操,发现之前写的命名太长了，改了一点函数和变量的名字
/*
output_filename -> out_f
input_filename -> in_f
qbe_ir_filename -> qbe_f
llvm_ir_filename -> llvm_f
obj_filename -> obj_f
bytecode_filename -> bc_f
cpp_filename -> cpp_f
enable_debug_log -> dbg
is_vic_file -> is_vic
save_cpp_file -> save_c
keep_cpp_file -> keep_c
generate_llvm_ir -> gen_llvm
generate_object_file -> gen_obj
output_ast_only -> out_ast
bare_metal_mode -> bare
has_explicit_output_mode -> exp_mode
target_triple -> target
effective_target -> eff_t
compile_command -> ccmd
compile_result -> cres
llc_cmd -> lcmd
type_ctx -> t_ctx
global_table -> g_tbl
semantic_errors -> errs
\(^o^)/
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#   include <windows.h>
#   include <direct.h>
#   include <io.h>
#   define access _access
#   define F_O K 0
#   define R_OK 4
#   define sleep(x) Sleep(x*1000)
#   define setenv(name,val,over) SetEnvironmentVariableA(name,val)
#   include <pathcch.h>
#   pragma comment(lib, "pathcch.lib")

    char* realpath(const char* path, char* resolved) {
        static char buffer[MAX_PATH];
        if (!resolved) resolved = buffer;

        wchar_t wpath[MAX_PATH];
        wchar_t wfull[MAX_PATH];
        MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, MAX_PATH);
        if (PathCchCanonicalize(wfull, MAX_PATH, wpath) != S_OK) return NULL;

        WideCharToMultiByte(CP_UTF8, 0, wfull, -1, resolved, MAX_PATH, NULL, NULL);
        return resolved;
    }
#else
#   include <unistd.h>
#endif

#include "../include/ast.h"
#include "../include/parser.h"
#include "../include/compiler.h"
#include "../include/codegen.h"
#include "../include/semantic.h"

extern FILE* yyin;
extern ASTNode* root;
void create_lib_files();
void analyze_ast(TypeInferenceContext* ctx, ASTNode* node);
const char* current_input_filename = NULL;

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <input.vix> [-o output_file]\n", argv[0]);
        fprintf(stderr, "       %s <input.vix> -ll <llvm_ir_file>\n", argv[0]);
        fprintf(stderr, "       %s <input.vix> -obj [obj_file] (output object file via llc)\n", argv[0]);
        fprintf(stderr, "       %s <input.vix> -ast (output AST only)\n", argv[0]);
        fprintf(stderr, "       %s <input.vix> -ll (output LLVM IR only)\n", argv[0]);
        fprintf(stderr, "       %s <input.vix> -llvm (output LLVM IR only)\n", argv[0]);
        fprintf(stderr, "       %s <input.vix> --debug (enable debug logs)\n", argv[0]);
        fprintf(stderr, "       %s <input.vix> --target=<triple> (set codegen/link target)\n", argv[0]);
        return 1;
    }
    
    char* out_f = NULL;
    char* llvm_f = NULL;
    char* obj_f = NULL;
    char* in_f = NULL;
    int is_vic = 0;
    int save_c = 0;
    int keep_c = 0;
    int gen_llvm = 0;
    int gen_obj = 0;
    int out_ast = 0;
    int out_llvm = 0;
    int dbg = 0;
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
            printf("Vix Compiler 0.1.0_rc1_2 (Beta_26.01.01) by:Mincx1203 Copyright(c) 2025-2026\n");
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
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            fprintf(stderr, "usage: %s <input.vix> [-o output_file]\n", argv[0]);
            fprintf(stderr, "       %s <input.vix> -llvm <llvm_ir_file>\n", argv[0]);
            fprintf(stderr, "       %s <input.vix> -ll <llvm_ir_file>\n", argv[0]);
            fprintf(stderr, "       %s <input.vix> -obj [obj_file] (output object file via llc)\n", argv[0]);
            fprintf(stderr, "       %s <input.vix> -ast (output AST only)\n", argv[0]);
            fprintf(stderr, "       %s <input.vix> -ll (output LLVM IR only)\n", argv[0]);
            fprintf(stderr, "       %s <input.vix> -llvm (output LLVM IR only)\n", argv[0]);
            fprintf(stderr, "       %s <input.vix> --debug (enable debug logs)\n", argv[0]);
            fprintf(stderr, "       %s <input.vix> --target=<triple> (set codegen/link target, e.g. x86_64-unknown-none)\n", argv[0]);
            fprintf(stderr, "       %s <input.vix> (LLVM backend is the default backend)\n", argv[0]);
            return 0;
        } else if (argv[i][0] == '-' && strcmp(argv[i], "-") != 0) {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            return 1;
        } else {
            is_vic = strlen(argv[i]) > 4 && strcmp(argv[i] + strlen(argv[i]) - 4, ".vic") == 0;
        }
    }
    setenv("VIX_DEBUG", dbg ? "1" : "0", 1);//通过环境变量控制调试输出
    if (!in_f) {
        in_f = argv[1];
    }
    int exp_mode =
        out_ast ||
        out_llvm ||
        gen_llvm ||
        gen_obj;

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
                if (!keep_c) {
                    remove(llvm_f);
                }
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
                char lcmd[8192];
                if (eff_t) {
                    snprintf(lcmd, sizeof(lcmd),
                             "llc -filetype=obj -relocation-model=%s -mtriple=%s %s -o %s",
                             bare ? "static" : "pic",
                             eff_t, llvm_f, fobj);
                } else {
                    snprintf(lcmd, sizeof(lcmd),
                             "llc -filetype=obj -relocation-model=%s %s -o %s",
                             bare ? "static" : "pic",
                             llvm_f, fobj);
                }

                int lres = system(lcmd);
                if (lres != 0) {
                    fprintf(stderr, "Error: Failed to compile LLVM IR to object file via llc\n");
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
            if (out_f && save_c) {
                const char* ls = "linker.ld";
                if (bare && access(ls, R_OK) != 0 && access("src/linker.ld", R_OK) == 0) {
                    ls = "src/linker.ld";
                }

                size_t ccmd_sz = 8192;
                char *ccmd = malloc(ccmd_sz);
                if (ccmd == NULL) {
                    fprintf(stderr, "Er: Failed to allocate memory for clang command\n");
                    fclose(input_file);
                    return 1;
                }

                if (bare) {
                    const char* f_t = eff_t ? eff_t : "x86_64-unknown-none";
                    snprintf(ccmd, ccmd_sz,
                             "clang -O2 %s -o %s -target %s -ffreestanding -fno-builtin -fno-pic -fno-pie -no-pie -nostdlib -nostartfiles -nodefaultlibs -static -Wl,--build-id=none -Wl,--no-dynamic-linker -Wl,-z,max-page-size=0x1000 -Wl,-e,_start -Wl,-T,%s",
                             llvm_f, out_f, f_t, ls);
                } else if (eff_t) {
#ifdef _WIN32
                    snprintf(ccmd, ccmd_sz,
                             "clang -O2 %s -o %s -target %s -static -lpsapi -lntdll -ladvapi32 -lshell32 -lole32 -luser32",
                             llvm_f, out_f, eff_t);
#else
                    snprintf(ccmd, ccmd_sz,
                             "clang -O2 %s -o %s -target %s $(llvm-config --ldflags --libs all) -lm -lstdc++",
                             llvm_f, out_f, eff_t);
#endif
                } else {
#ifdef _WIN32
                    snprintf(ccmd, ccmd_sz,
                             "clang -O2 %s -o %s -static -lpsapi -lntdll -ladvapi32 -lshell32 -lole32 -luser32",
                             llvm_f, out_f);
#else
                    snprintf(ccmd, ccmd_sz,
                             "clang -O2 %s -o %s $(llvm-config --ldflags --libs all) -lm -lstdc++",
                             llvm_f, out_f);
#endif
                }
                
                int cres = system(ccmd);
                if (cres != 0) {
                    fprintf(stderr, "Error: Failed to compile LLVM IR to executable\n");
                    free(ccmd);
                    fclose(input_file);
                    return 1;
                }
                
                free(ccmd);
                
                if (!keep_c) {
                    remove(llvm_f);
                }
            }
            
            if (root) {
                free_ast(root);
            }
            fclose(input_file);
            if (out_f && out_f != in_f && out_f != argv[1] && out_f != llvm_f) {
            }
            
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
    if (out_f && out_f != in_f && out_f != argv[1]) {
        if (out_f != argv[1] && out_f[0] != '-') {
            ;
        }
    }
    
    return result;
}

void analyze_ast(TypeInferenceContext* ctx, ASTNode* node) {
    if (!node) return;
    
    switch (node->type) {
        case AST_PROGRAM:
            for (int i = 0; i < node->data.program.statement_count; i++) {
                analyze_ast(ctx, node->data.program.statements[i]);
            }
            break;
        case AST_CONST:
            infer_type(ctx, node->data.assign.right);
            if (node->data.assign.left->type == AST_IDENTIFIER) {
                set_variable_type(ctx, node->data.assign.left->data.identifier.name,
                    infer_type(ctx, node->data.assign.right));
            }
            analyze_ast(ctx, node->data.assign.left);
            analyze_ast(ctx, node->data.assign.right);
            break;
        case AST_PRINT:
            analyze_ast(ctx, node->data.print.expr);
            break;
            
        case AST_BINOP:
            analyze_ast(ctx, node->data.binop.left);
            analyze_ast(ctx, node->data.binop.right);
            break;
            
        case AST_UNARYOP:
            analyze_ast(ctx, node->data.unaryop.expr);
            break;
            
        case AST_IDENTIFIER: {
            InferredType type = get_variable_type(ctx, node->data.identifier.name);
            if (type == TYPE_UNKNOWN) {
                /* only report if variable truly not declared in context */
                if (!has_variable(ctx, node->data.identifier.name)) {
                    report_undefined_variable_with_location(
                        node->data.identifier.name,
                        current_input_filename ? current_input_filename : "unknown",
                        node->location.first_line
                    );
                }
            }
            break;
        }
            
        default:
            break;
    }
}