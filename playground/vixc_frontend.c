#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#else
#define EM_ASM(...)
#endif
#include "../src/libvixc_frontend.h"
#include "parser.h"
#include "semantic.h"
#include "typeck.h"
#include "ownership.h"
#include "compiler.h"
#include <stdlib.h>
#include <string.h>

extern ASTNode *root;
extern int yyparse(void);
extern void load_source_file(const char *name);
extern void inline_imports(ASTNode *root);

struct yy_buffer_state;
typedef struct yy_buffer_state *YY_BUFFER_STATE;
YY_BUFFER_STATE yy_scan_string(const char *str);
void yy_delete_buffer(YY_BUFFER_STATE buffer);

static char error_buf[4096];

static void set_error(const char *msg) {
    strncpy(error_buf, msg, sizeof(error_buf) - 1);
    error_buf[sizeof(error_buf) - 1] = '\0';
}

CompileResult vixc_compile_string(const char *source) {
    EM_ASM({ console.log('[vixc_frontend] compile_string, len:', $0); }, source ? strlen(source) : 0);

    CompileResult result = {NULL, 0};
    error_buf[0] = '\0';

    load_source_file("<input>");

    YY_BUFFER_STATE buf = yy_scan_string(source);
    if (!buf) {
        EM_ASM({ console.log('[vixc_frontend] yy_scan_string failed'); });
        result.error_count = 1;
        set_error("failed to create scanner buffer");
        return result;
    }

    EM_ASM({ console.log('[vixc_frontend] calling yyparse...'); });
    int parse_result = yyparse();
    EM_ASM({ console.log('[vixc_frontend] yyparse done, result:', $0, 'root:', $1); }, parse_result, !!root);

    yy_delete_buffer(buf);

    if (parse_result != 0 || !root) {
        result.error_count = 1;
        set_error(get_last_error_message());
        return result;
    }

    EM_ASM({ console.log('[vixc_frontend] inline_imports...'); });
    inline_imports(root);

    EM_ASM({ console.log('[vixc_frontend] check_undefined_symbols...'); });
    if (check_undefined_symbols(root) > 0) {
        result.error_count = 1;
        set_error(get_last_error_message());
        free_ast(root);
        root = NULL;
        return result;
    }

    EM_ASM({ console.log('[vixc_frontend] typecheck...'); });
    if (typecheck_program(root) != 0) {
        result.error_count = 1;
        set_error(get_last_error_message());
        free_ast(root);
        root = NULL;
        return result;
    }

    EM_ASM({ console.log('[vixc_frontend] ownership_check...'); });
    if (ownership_check_program(root) != 0) {
        result.error_count = 1;
        set_error(get_last_error_message());
        free_ast(root);
        root = NULL;
        return result;
    }

    result.root = root;
    EM_ASM({ console.log('[vixc_frontend] compile success'); });
    return result;
}

void vixc_free_result(CompileResult *result) {
    if (result && result->root) {
        free_ast(result->root);
        result->root = NULL;
    }
}

const char *vixc_get_last_error(void) {
    return error_buf;
}
