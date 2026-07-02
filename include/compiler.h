#ifndef COMPILER_H
#define COMPILER_H
#include "typeinfer.h"
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif
typedef enum {
    ERROR_LEVEL_WARNING,
    ERROR_LEVEL_ERROR,
    ERROR_LEVEL_FATAL
} ErrorLevel;
typedef enum {
    ERROR_SYNTAX,
    ERROR_LEXICAL,
    ERROR_TYPE,
    ERROR_UNDEFINED,
    ERROR_UNDEFINED_FUNC,
    ERROR_REDEFINITION,
    ERROR_SEMANTIC,
    ERROR_RUNTIME,
    ERROR_WARNING,
    ERROR_ARRAY_OUT_OF_BOUNDS
} ErrorType;
void set_location(const char* filename, int line);
void set_location_with_column(const char* filename, int line, int column);
void load_source_file(const char* filename);
void report_lexical_error_with_location(const char* message, const char* filename, int line);
void report_syntax_error_with_location(const char* token, const char* filename, int line);
void report_syntax_error_with_location_column(const char* token, const char* filename, int line, int column);
void report_undefined_identifier_with_location(const char* identifier, const char* filename, int line);
void report_undefined_function_with_location(const char* function_name, const char* filename, int line);
void report_undefined_variable_with_location(const char* variable_name, const char* filename, int line);
void report_redefinition_error_with_location(const char* identifier, const char* filename, int line);
void report_runtime_error_with_location(const char* message, const char* filename, int line);
void report_type_error_with_location(const char* expected, const char* actual, const char* filename, int line);
void report_semantic_error_with_location(const char* message, const char* filename, int line);
void report_array_out_of_bounds_error_with_location(const char* array_name, int index, int size, const char* filename, int line);
void report_warning(const char* format, ...);
void report_error(const char* format, ...);
void report_fatal_error(const char* format, ...);
void internal_error(const char* format, ...);
void report_syntax_error(const char* token);
void report_lexical_error(const char* message);
void report_type_error(const char* expected, const char* actual);
void report_undefined_identifier(const char* identifier);
void report_undefined_function(const char* function_name);
void report_undefined_variable(const char* variable_name);
void report_redefinition_error(const char* identifier);
void report_mismatched_parentheses();
int get_error_count();
int get_warning_count();
const char* get_last_error_message();
void print_error_summary();
void cleanup_error_handler();
void report_simple_error(ErrorLevel level, ErrorType error_type, const char* msg);
void report_simple_error_with_length(ErrorLevel level, ErrorType error_type, const char* msg, int length);
void report_warning_with_location_and_snippet(const char* msg, const char* filename, int line, const char* snippet);
void report_unused_variable_warning(const char* variable_name, const char* filename, int line);
void report_unused_variable_warning_with_location(const char* variable_name, const char* filename, int line, int column);
void report_struct_field_missing_with_location_and_suggestion(const char* struct_name, const char* field_name, const char* suggestion, const char* filename, int line, int column);
#ifdef __cplusplus
}
#endif
#endif /*COMPILER_H*/


