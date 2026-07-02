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
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include "../include/compat.h"
#include "../include/compiler.h"

static const char* current_filename = "unknown";
static int current_line = 1;
static int current_column = 1;
static int error_count = 0;
static int warning_count = 0;
static char last_error_message[1024] = {0};
static char* source_content = NULL;

static void vreport_error(ErrorLevel level, ErrorType error_type, const char* format, va_list args);
static void emit_diagnostic(ErrorLevel level, ErrorType error_type, const char* message, int length, const char* suggestion_override);
static char* get_line_content(int line_number);
static void show_source_context(ErrorLevel level, ErrorType error_type, int line_number, int column, int length);
static void print_highlighted_line(const char* line, int column, int length, const char* color);
static void print_caret_line(const char* line, int column, int length, const char* color);
static void push_location(const char* filename, int line, int column, const char** old_filename, int* old_line, int* old_column);
static void pop_location(const char* old_filename, int old_line, int old_column);
void report_undefined_function_with_location_and_column(const char* function_name, const char* filename, int line, int column);
#define ANSI_RESET "\033[0m"
#define ANSI_BOLD "\033[1m"
#define ANSI_DIM "\033[2m"
#define ANSI_RED "\033[31m"
#define ANSI_YELLOW "\033[33m"
#define ANSI_BLUE "\033[34m"
#define ANSI_MAGENTA "\033[35m"
#define ANSI_CYAN "\033[36m"
#define ANSI_WHITE "\033[37m"

static int supports_color(void);
static const char* colorize(const char* code);
static const char* level_label(ErrorLevel level);
static const char* level_color(ErrorLevel level);
static const char* error_type_to_string(ErrorType error_type);
static const char* error_type_color(ErrorType error_type);
static const char* get_suggestion(ErrorType error_type);
static int line_number_width(int line_number);
static void sanitize_message(const char* message, char* buffer, size_t buffer_size);
static void print_suggestion_block(const char* suggestion);
static void print_diagnostic_header(ErrorLevel level, ErrorType error_type, const char* message);
static void print_internal_header(const char* message);

void set_location(const char* filename, int line) {
    current_filename = filename ? filename : "unknown";
    current_line = line;
    current_column = 1;
}

void set_location_with_column(const char* filename, int line, int column) {
    current_filename = filename ? filename : "unknown";
    current_line = line;
    current_column = column > 0 ? column : 1;
}

void load_source_file(const char* filename) {
    if (!filename) {
        fprintf(stderr, "error: cannot open file\n");
        return;
    }

    FILE* file = fopen(filename, "r");
    if (!file) return;

    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);

    if (source_content) {
        free(source_content);
        source_content = NULL;
    }

    source_content = (char*)malloc(file_size + 1);
    if (source_content) {
        fread(source_content, 1, file_size, file);
        source_content[file_size] = '\0';
    }

    fclose(file);
}

static void push_location(const char* filename, int line, int column, const char** old_filename, int* old_line, int* old_column) {
    *old_filename = current_filename;
    *old_line = current_line;
    *old_column = current_column;
    current_filename = filename ? filename : "unknown";
    current_line = line;
    current_column = column > 0 ? column : 1;
}

static void pop_location(const char* old_filename, int old_line, int old_column) {
    current_filename = old_filename;
    current_line = old_line;
    current_column = old_column;
}

static char* get_line_content(int line_number) {
    if (!source_content || line_number <= 0) return NULL;

    char* start = source_content;
    int current = 1;
    while (current < line_number && *start) {
        if (*start == '\n') {
            current++;
        }
        start++;
    }

    if (current != line_number) {
        return NULL;
    }

    char* end = start;
    while (*end && *end != '\n') {
        end++;
    }

    int line_len = (int)(end - start);
    char* line_content = (char*)malloc((size_t)line_len + 1);
    if (!line_content) {
        return NULL;
    }

    memcpy(line_content, start, (size_t)line_len);
    line_content[line_len] = '\0';
    if (line_len > 0 && line_content[line_len - 1] == '\r') {
        line_content[line_len - 1] = '\0';
    }

    return line_content;
}

static void print_highlighted_line(const char* line, int column, int length, const char* color) {
    int col = column > 0 ? column : 1;
    int len = length > 0 ? length : 1;
    int line_len = (int)strlen(line);
    if (col > line_len + 1) col = line_len + 1;

    int start = col - 1;
    int end = start + len;
    if (end > line_len) end = line_len;

    if (start > 0) {
        fwrite(line, 1, (size_t)start, stderr);
    }
    if (start < line_len) {
        fprintf(stderr, "%s%s", colorize(color), colorize(ANSI_BOLD));
        fwrite(line + start, 1, (size_t)(end - start), stderr);
        fprintf(stderr, "%s", colorize(ANSI_RESET));
    }
    if (end < line_len) {
        fwrite(line + end, 1, (size_t)(line_len - end), stderr);
    }
}

static void print_caret_line(const char* line, int column, int length, const char* color) {
    int col = column > 0 ? column : 1;
    int len = length > 0 ? length : 1;
    int line_len = (int)strlen(line);
    if (col > line_len + 1) col = line_len + 1;

    int display_col = 0;
    for (int i = 0; i < col - 1 && line[i] != '\0'; i++) {
        if (line[i] == '\t') {
            display_col = ((display_col / 8) + 1) * 8;
        } else {
            display_col++;
        }
    }

    for (int i = 0; i < display_col; i++) {
        fputc(' ', stderr);
    }

    fprintf(stderr, "%s%s", colorize(color), colorize(ANSI_BOLD));
    for (int i = 0; i < len; i++) {
        fputc('^', stderr);
    }
    fprintf(stderr, "%s", colorize(ANSI_RESET));
}

static void show_source_context(ErrorLevel level, ErrorType error_type, int line_number, int column, int length) {
    if (!source_content || line_number <= 0) return;

    char* prev_line = get_line_content(line_number - 1);
    char* line_content = get_line_content(line_number);
    char* next_line = get_line_content(line_number + 1);

    if (!line_content) {
        free(prev_line);
        free(next_line);
        return;
    }

    int width = line_number_width(line_number + 1);
    const char* reset = colorize(ANSI_RESET);
    const char* gutter = colorize(ANSI_DIM);
    const char* highlight = (level == ERROR_LEVEL_WARNING) ? ANSI_YELLOW : error_type_color(error_type);

    fprintf(stderr, "%s%*s |%s\n", gutter, width, "", reset);
    if (prev_line) {
        fprintf(stderr, "%s%*d | %s%s\n", gutter, width, line_number - 1, prev_line, reset);
    }

    fprintf(stderr, "%s%*d |%s ", colorize(ANSI_BOLD), width, line_number, reset);
    print_highlighted_line(line_content, column, length, highlight);
    fprintf(stderr, "\n");

    fprintf(stderr, "%s%*s |%s ", gutter, width, "", reset);
    print_caret_line(line_content, column, length, highlight);
    fprintf(stderr, "\n");

    if (next_line) {
        fprintf(stderr, "%s%*d | %s%s\n", gutter, width, line_number + 1, next_line, reset);
    }
    fprintf(stderr, "%s%*s |%s\n", gutter, width, "", reset);

    free(prev_line);
    free(line_content);
    free(next_line);
}

static int supports_color(void) {
    static int cached = -1;
    if (cached != -1) return cached;

    const char* force = getenv("FORCE_COLOR");
    if (force && *force) {
        cached = 1;
        return cached;
    }

    const char* no_color = getenv("NO_COLOR");
    if (no_color && *no_color) {
        cached = 0;
        return cached;
    }

    const char* term = getenv("TERM");
    if (!term || strcmp(term, "dumb") == 0) {
        cached = 0;
        return cached;
    }

    cached = vix_is_stderr_tty() ? 1 : 0;
    return cached;
}

static const char* colorize(const char* code) {
    return supports_color() ? code : "";
}

static const char* level_label(ErrorLevel level) {
    switch (level) {
        case ERROR_LEVEL_WARNING:
            return "warning";
        case ERROR_LEVEL_ERROR:
            return "error";
        case ERROR_LEVEL_FATAL:
            return "fatal";
        default:
            return "error";
    }
}

static const char* level_color(ErrorLevel level) {
    switch (level) {
        case ERROR_LEVEL_WARNING:
            return ANSI_YELLOW;
        case ERROR_LEVEL_ERROR:
        case ERROR_LEVEL_FATAL:
            return ANSI_RED;
        default:
            return ANSI_WHITE;
    }
}

static const char* error_type_to_string(ErrorType error_type) {
    switch (error_type) {
        case ERROR_SYNTAX:
            return "SyntaxError";
        case ERROR_LEXICAL:
            return "LexicalError";
        case ERROR_TYPE:
            return "TypeError";
        case ERROR_UNDEFINED:
        case ERROR_UNDEFINED_FUNC:
            return "NameError";
        case ERROR_REDEFINITION:
            return "RedefinitionError";
        case ERROR_SEMANTIC:
            return "SemanticError";
        case ERROR_RUNTIME:
            return "RuntimeError";
        case ERROR_WARNING:
            return "Warning";
        case ERROR_ARRAY_OUT_OF_BOUNDS:
            return "BoundsError";
        default:
            return "Error";
    }
}

static const char* error_type_color(ErrorType error_type) {
    switch (error_type) {
        case ERROR_SYNTAX:
        case ERROR_LEXICAL:
            return ANSI_MAGENTA;
        case ERROR_TYPE:
        case ERROR_SEMANTIC:
            return ANSI_BLUE;
        case ERROR_UNDEFINED:
        case ERROR_UNDEFINED_FUNC:
        case ERROR_REDEFINITION:
            return ANSI_CYAN;
        case ERROR_RUNTIME:
        case ERROR_ARRAY_OUT_OF_BOUNDS:
            return ANSI_RED;
        case ERROR_WARNING:
            return ANSI_YELLOW;
        default:
            return ANSI_WHITE;
    }
}

static const char* get_suggestion(ErrorType error_type) {
    switch (error_type) {
        case ERROR_UNDEFINED:
            return "Declare the identifier before use or check for typos";
        case ERROR_UNDEFINED_FUNC:
            return "Ensure the function is declared and linked, or check its name";
        case ERROR_REDEFINITION:
            return "Remove or rename the previous declaration to avoid conflicts";
        case ERROR_TYPE:
            return "Ensure both sides of the expression have compatible types (add a cast if needed)";
        case ERROR_SYNTAX:
            return "Check syntax near the indicated location (missing semicolon, parenthesis, or unexpected token)";
        case ERROR_LEXICAL:
            return "Remove unsupported characters or fix the token spelling";
        case ERROR_RUNTIME:
            return "Verify runtime conditions (null/overflow) or add guards";
        case ERROR_ARRAY_OUT_OF_BOUNDS:
            return "Check index range before accessing the array";
        default:
            return NULL;
    }
}

static int line_number_width(int line_number) {
    int width = 1;
    int n = line_number > 0 ? line_number : 1;
    while (n >= 10) {
        n /= 10;
        width++;
    }
    return width;
}

static void sanitize_message(const char* message, char* buffer, size_t buffer_size) {
    if (!buffer_size) return;
    if (!message) {
        buffer[0] = '\0';
        return;
    }

    size_t out = 0;
    for (size_t i = 0; message[i] != '\0' && out + 1 < buffer_size; i++) {
        char c = message[i];
        if (c == '\n' || c == '\r' || c == '\t') {
            c = ' ';
        }
        buffer[out++] = c;
    }
    buffer[out] = '\0';
}

static void print_suggestion_block(const char* suggestion) {
    (void)suggestion;
    return;
    if (!suggestion || !*suggestion) return;
    fprintf(stderr, "%s   = %s%shelp%s: %s\n",
            colorize(ANSI_DIM),
            colorize(ANSI_BOLD),
            colorize(ANSI_CYAN),
            colorize(ANSI_RESET),
            suggestion);
}

static void print_diagnostic_note_or_help(const char* line) {
    if (!line || !*line) return;
    while (*line == ' ' || *line == '\t') line++;

    const char* label = NULL;
    const char* text = line;
    if (strncmp(line, "note:", 5) == 0) {
        label = "note";
        text = line + 5;
    } else if (strncmp(line, "help:", 5) == 0) {
        return;
    }
    while (*text == ' ' || *text == '\t') text++;

    if (label) {
        fprintf(stderr, "%s   = %s%s%s%s: %s\n",
                colorize(ANSI_DIM),
                colorize(ANSI_BOLD),
                strcmp(label, "help") == 0 ? colorize(ANSI_CYAN) : colorize(ANSI_BLUE),
                label,
                colorize(ANSI_RESET),
                text);
    } else {
        fprintf(stderr, "%s   =%s %s\n", colorize(ANSI_DIM), colorize(ANSI_RESET), line);
    }
}

static void print_message_notes(const char* message) {
    if (!message) return;
    const char* line = strchr(message, '\n');
    while (line) {
        line++;
        const char* end = strchr(line, '\n');
        size_t len = end ? (size_t)(end - line) : strlen(line);
        char buffer[1024];
        if (len >= sizeof(buffer)) len = sizeof(buffer) - 1;
        memcpy(buffer, line, len);
        buffer[len] = '\0';
        print_diagnostic_note_or_help(buffer);
        line = end;
    }
}

static void print_diagnostic_header(ErrorLevel level, ErrorType error_type, const char* message) {
    char msg_buf[1024];
    size_t title_len = 0;
    if (message) {
        const char* newline = strchr(message, '\n');
        title_len = newline ? (size_t)(newline - message) : strlen(message);
    }
    if (title_len >= sizeof(msg_buf)) title_len = sizeof(msg_buf) - 1;
    if (message && title_len > 0) {
        memcpy(msg_buf, message, title_len);
        msg_buf[title_len] = '\0';
    } else {
        msg_buf[0] = '\0';
    }
    sanitize_message(msg_buf, msg_buf, sizeof(msg_buf));

    const char* reset = colorize(ANSI_RESET);
    const char* level_text = level_label(level);
    const char* level_col = level_color(level);
    const char* type_text = error_type_to_string(error_type);

    fprintf(stderr, "%s%s%s%s", colorize(level_col), colorize(ANSI_BOLD), level_text, reset);
    if (type_text && error_type != ERROR_WARNING) {
        fprintf(stderr, " %s[%s%s%s]%s",
                colorize(ANSI_DIM),
                colorize(error_type_color(error_type)),
                type_text,
                colorize(ANSI_DIM),
                reset);
    }
    fprintf(stderr, ": %s\n", msg_buf[0] ? msg_buf : "");
    fprintf(stderr, "  %s-->%s %s:%d:%d\n", colorize(ANSI_BLUE), reset, current_filename, current_line, current_column);
}

static void print_internal_header(const char* message) {
    char msg_buf[1024];
    sanitize_message(message, msg_buf, sizeof(msg_buf));

    fprintf(stderr, "%s%sinternal error%s: %s\n",
            colorize(ANSI_RED),
            colorize(ANSI_BOLD),
            colorize(ANSI_RESET),
            msg_buf[0] ? msg_buf : "");
    fprintf(stderr, "%s-->%s %s:%d:%d\n",
            colorize(ANSI_BLUE),
            colorize(ANSI_RESET),
            current_filename,
            current_line,
            current_column);
}

int get_error_count() {
    return error_count;
}

int get_warning_count() {
    return warning_count;
}

const char* get_last_error_message() {
    return last_error_message;
}

static void emit_diagnostic(ErrorLevel level, ErrorType error_type, const char* message, int length, const char* suggestion_override) {
    if (level != ERROR_LEVEL_WARNING) {
        if (message) {
            strncpy(last_error_message, message, sizeof(last_error_message) - 1);
            last_error_message[sizeof(last_error_message) - 1] = '\0';
        } else {
            last_error_message[0] = '\0';
        }
    }

    if (level == ERROR_LEVEL_WARNING) {
        warning_count++;
    } else {
        error_count++;
    }

    print_diagnostic_header(level, error_type, message);

    if (source_content && current_line > 0) {
        show_source_context(level, error_type, current_line, current_column, length);
    }

    print_message_notes(message);

    if (suggestion_override && *suggestion_override) {
        print_suggestion_block(suggestion_override);
    } else if (!message || strstr(message, "help:") == NULL) {
        print_suggestion_block(get_suggestion(error_type));
    }
}

static void vreport_error(ErrorLevel level, ErrorType error_type, const char* format, va_list args) {
    char message[1024];
    vsnprintf(message, sizeof(message), format, args);
    emit_diagnostic(level, error_type, message, 1, NULL);
}

void report_simple_error_with_length(ErrorLevel level, ErrorType error_type, const char* msg, int length) {
    emit_diagnostic(level, error_type, msg, length, NULL);
}

void report_simple_error(ErrorLevel level, ErrorType error_type, const char* msg) {
    report_simple_error_with_length(level, error_type, msg, 1);
}

void report_warning(const char* format, ...) {
    va_list args;
    va_start(args, format);
    vreport_error(ERROR_LEVEL_WARNING, ERROR_WARNING, format, args);
    va_end(args);
}

void report_warning_with_location_and_snippet(const char* msg, const char* filename, int line, const char* snippet) {
    if (!msg || !filename || line <= 0) return;

    const char* old_filename;
    int old_line;
    int old_column;
    int column = 1;
    int length = 1;

    if (snippet && *snippet) {
        length = (int)strlen(snippet);
        char* line_content = get_line_content(line);
        if (line_content) {
            char* found = strstr(line_content, snippet);
            if (found) {
                column = (int)(found - line_content) + 1;
            }
            free(line_content);
        }
    }

    push_location(filename, line, column, &old_filename, &old_line, &old_column);
    report_simple_error_with_length(ERROR_LEVEL_WARNING, ERROR_WARNING, msg, length);
    pop_location(old_filename, old_line, old_column);
}

void report_unused_variable_warning(const char* variable_name, const char* filename, int line) {
    report_unused_variable_warning_with_location(variable_name, filename, line, 1);
}

void report_unused_variable_warning_with_location(const char* variable_name, const char* filename, int line, int column) {
    if (variable_name && filename && line > 0) {
        const char* old_filename;
        int old_line;
        int old_column;
        char buffer[256];
        int adjusted_column = column;

        char* line_content = get_line_content(line);
        if (line_content) {
            char* found = strstr(line_content, variable_name);
            if (found) {
                adjusted_column = (int)(found - line_content) + 1;
            }
            free(line_content);
        }

        push_location(filename, line, adjusted_column, &old_filename, &old_line, &old_column);
        snprintf(buffer, sizeof(buffer), "unused variable '%s'", variable_name);
        report_simple_error_with_length(ERROR_LEVEL_WARNING, ERROR_WARNING, buffer, (int)strlen(variable_name));
        pop_location(old_filename, old_line, old_column);
    }
}

void report_error(const char* format, ...) {
    va_list args;
    va_start(args, format);
    vreport_error(ERROR_LEVEL_ERROR, ERROR_SEMANTIC, format, args);
    va_end(args);
}

void report_fatal_error(const char* format, ...) {
    va_list args;
    va_start(args, format);
    vreport_error(ERROR_LEVEL_FATAL, ERROR_SEMANTIC, format, args);
    va_end(args);
    exit(EXIT_FAILURE);
}

void internal_error(const char* format, ...) {
    va_list args;
    char message[1024];

    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    if (source_content && current_line > 0) {
        print_internal_header(message);
        show_source_context(ERROR_LEVEL_FATAL, ERROR_RUNTIME, current_line, current_column, 1);
    } else {
        print_internal_header(message);
    }

    exit(EXIT_FAILURE);
}

void report_lexical_error_with_location(const char* message, const char* filename, int line) {
    const char* old_filename;
    int old_line;
    int old_column;
    push_location(filename, line, 1, &old_filename, &old_line, &old_column);
    report_simple_error(ERROR_LEVEL_ERROR, ERROR_LEXICAL, message);
    pop_location(old_filename, old_line, old_column);
}

void report_syntax_error_with_location(const char* token, const char* filename, int line) {
    report_syntax_error_with_location_column(token, filename, line, 1);
}

void report_syntax_error_with_location_column(const char* token, const char* filename, int line, int column) {
    const char* old_filename;
    int old_line;
    int old_column;
    char buffer[256];

    push_location(filename, line, column, &old_filename, &old_line, &old_column);
    if (token) {
        snprintf(buffer, sizeof(buffer), "unexpected token '%s'", token);
    } else {
        snprintf(buffer, sizeof(buffer), "unexpected token");
    }
    report_simple_error(ERROR_LEVEL_ERROR, ERROR_SYNTAX, buffer);
    pop_location(old_filename, old_line, old_column);
}

void report_undefined_identifier_with_location(const char* identifier, const char* filename, int line) {
    const char* old_filename;
    int old_line;
    int old_column;
    char buffer[256];

    push_location(filename, line, 1, &old_filename, &old_line, &old_column);
    if (identifier) {
        snprintf(buffer, sizeof(buffer), "undefined identifier: '%s'", identifier);
        report_simple_error_with_length(ERROR_LEVEL_ERROR, ERROR_UNDEFINED, buffer, (int)strlen(identifier));
    } else {
        report_simple_error(ERROR_LEVEL_ERROR, ERROR_UNDEFINED, "undefined identifier");
    }
    pop_location(old_filename, old_line, old_column);
}

void report_undefined_identifier_with_location_and_column(const char* identifier, const char* filename, int line, int column) {
    const char* old_filename;
    int old_line;
    int old_column;
    char buffer[256];

    push_location(filename, line, column, &old_filename, &old_line, &old_column);
    if (identifier) {
        snprintf(buffer, sizeof(buffer), "undefined identifier: '%s'", identifier);
        report_simple_error_with_length(ERROR_LEVEL_ERROR, ERROR_UNDEFINED, buffer, (int)strlen(identifier));
    } else {
        report_simple_error(ERROR_LEVEL_ERROR, ERROR_UNDEFINED, "undefined identifier");
    }
    pop_location(old_filename, old_line, old_column);
}

void report_undefined_function_with_location(const char* function_name, const char* filename, int line) {
    report_undefined_function_with_location_and_column(function_name, filename, line, 1);
}

void report_undefined_function_with_location_and_column(const char* function_name, const char* filename, int line, int column) {
    const char* old_filename;
    int old_line;
    int old_column;
    char buffer[256];

    push_location(filename, line, column, &old_filename, &old_line, &old_column);
    if (function_name) {
        snprintf(buffer, sizeof(buffer), "call undefined function: '%s'", function_name);
        report_simple_error_with_length(ERROR_LEVEL_ERROR, ERROR_UNDEFINED_FUNC, buffer, (int)strlen(function_name));
    } else {
        report_simple_error(ERROR_LEVEL_ERROR, ERROR_UNDEFINED_FUNC, "call to undefined function");
    }
    pop_location(old_filename, old_line, old_column);
}

void report_undefined_variable_with_location(const char* variable_name, const char* filename, int line) {
    const char* old_filename;
    int old_line;
    int old_column;

    push_location(filename, line, 1, &old_filename, &old_line, &old_column);
    if (variable_name) {
        char buffer[256];
        snprintf(buffer, sizeof(buffer), "undefined variable: '%s'", variable_name);
        report_simple_error_with_length(ERROR_LEVEL_ERROR, ERROR_UNDEFINED, buffer, (int)strlen(variable_name));
    } else {
        report_simple_error(ERROR_LEVEL_ERROR, ERROR_UNDEFINED, "undefined variable");
    }
    pop_location(old_filename, old_line, old_column);
}

void report_redefinition_error_with_location(const char* identifier, const char* filename, int line) {
    const char* old_filename;
    int old_line;
    int old_column;
    char buffer[256];

    push_location(filename, line, 1, &old_filename, &old_line, &old_column);
    if (identifier) {
        snprintf(buffer, sizeof(buffer), "redefinition of '%s'", identifier);
        report_simple_error_with_length(ERROR_LEVEL_ERROR, ERROR_REDEFINITION, buffer, (int)strlen(identifier));
    } else {
        report_simple_error(ERROR_LEVEL_ERROR, ERROR_REDEFINITION, "redefinition of identifier");
    }
    pop_location(old_filename, old_line, old_column);
}

void report_type_error_with_location(const char* expected, const char* actual, const char* filename, int line) {
    const char* old_filename;
    int old_line;
    int old_column;
    char buffer[256];

    push_location(filename, line, 1, &old_filename, &old_line, &old_column);
    if (expected && actual) {
        snprintf(buffer, sizeof(buffer), "expected %s but got %s", expected, actual);
        report_simple_error(ERROR_LEVEL_ERROR, ERROR_TYPE, buffer);
    } else {
        report_simple_error(ERROR_LEVEL_ERROR, ERROR_TYPE, "type error");
    }
    pop_location(old_filename, old_line, old_column);
}

void report_semantic_error_with_location(const char* message, const char* filename, int line) {
    const char* old_filename;
    int old_line;
    int old_column;

    push_location(filename, line, 1, &old_filename, &old_line, &old_column);
    report_simple_error(ERROR_LEVEL_ERROR, ERROR_SEMANTIC, message);
    pop_location(old_filename, old_line, old_column);
}

void report_array_out_of_bounds_error_with_location(const char* array_name, int index, int size, const char* filename, int line) {
    const char* old_filename;
    int old_line;
    int old_column;
    char buffer[512];

    push_location(filename, line, 1, &old_filename, &old_line, &old_column);
    if (array_name) {
        snprintf(buffer, sizeof(buffer),
            "array index out of bounds: index %d in array '%s' of size %d",
            index, array_name, size);
    } else {
        snprintf(buffer, sizeof(buffer),
            "array index out of bounds: index %d",
            index);
    }
    report_simple_error(ERROR_LEVEL_ERROR, ERROR_ARRAY_OUT_OF_BOUNDS, buffer);
    pop_location(old_filename, old_line, old_column);
}

void report_runtime_error_with_location(const char* message, const char* filename, int line) {
    const char* old_filename;
    int old_line;
    int old_column;

    push_location(filename, line, 1, &old_filename, &old_line, &old_column);
    report_simple_error(ERROR_LEVEL_ERROR, ERROR_RUNTIME, message);
    pop_location(old_filename, old_line, old_column);
}

void report_syntax_error(const char* token) {
    char buffer[256];
    if (token) {
        snprintf(buffer, sizeof(buffer), "unexpected token '%s'", token);
    } else {
        snprintf(buffer, sizeof(buffer), "unexpected token");
    }
    report_simple_error(ERROR_LEVEL_ERROR, ERROR_SYNTAX, buffer);
}

void report_lexical_error(const char* message) {
    report_simple_error(ERROR_LEVEL_ERROR, ERROR_LEXICAL, message);
}

void report_type_error(const char* expected, const char* actual) {
    if (expected && actual) {
        char buffer[256];
        snprintf(buffer, sizeof(buffer), "expected %s but got %s", expected, actual);
        report_simple_error(ERROR_LEVEL_ERROR, ERROR_TYPE, buffer);
    } else {
        report_simple_error(ERROR_LEVEL_ERROR, ERROR_TYPE, "type error");
    }
}

void report_undefined_identifier(const char* identifier) {
    if (identifier) {
        char buffer[256];
        snprintf(buffer, sizeof(buffer), "undefined identifier: '%s'", identifier);
        report_simple_error_with_length(ERROR_LEVEL_ERROR, ERROR_UNDEFINED, buffer, (int)strlen(identifier));
    } else {
        report_simple_error(ERROR_LEVEL_ERROR, ERROR_UNDEFINED, "undefined identifier");
    }
}

void report_undefined_function(const char* function_name) {
    if (function_name) {
        char buffer[256];
        snprintf(buffer, sizeof(buffer), "call to undefined function: '%s'", function_name);
        report_simple_error_with_length(ERROR_LEVEL_ERROR, ERROR_UNDEFINED_FUNC, buffer, (int)strlen(function_name));
    } else {
        report_simple_error(ERROR_LEVEL_ERROR, ERROR_UNDEFINED_FUNC, "call to undefined function");
    }
}

void report_undefined_variable(const char* variable_name) {
    if (variable_name) {
        char buffer[256];
        snprintf(buffer, sizeof(buffer), "undefined variable: '%s'", variable_name);
        report_simple_error_with_length(ERROR_LEVEL_ERROR, ERROR_UNDEFINED, buffer, (int)strlen(variable_name));
    } else {
        report_simple_error(ERROR_LEVEL_ERROR, ERROR_UNDEFINED, "undefined variable");
    }
}

void report_redefinition_error(const char* identifier) {
    if (identifier) {
        char buffer[256];
        snprintf(buffer, sizeof(buffer), "redefinition of '%s'", identifier);
        report_simple_error_with_length(ERROR_LEVEL_ERROR, ERROR_REDEFINITION, buffer, (int)strlen(identifier));
    } else {
        report_simple_error(ERROR_LEVEL_ERROR, ERROR_REDEFINITION, "redefinition of identifier");
    }
}

void report_mismatched_parentheses() {
    report_simple_error(ERROR_LEVEL_ERROR, ERROR_SYNTAX, "mismatched parentheses");
}

void print_error_summary() {
    if (error_count > 0 || warning_count > 0) {
        fprintf(stderr, "\nFound ");
        if (error_count > 0 && warning_count > 0) {
            fprintf(stderr, "%d error%s and %d warning%s",
                    error_count, error_count == 1 ? "" : "s",
                    warning_count, warning_count == 1 ? "" : "s");
        } else if (error_count > 0) {
            fprintf(stderr, "%d error%s", error_count, error_count == 1 ? "" : "s");
        } else {
            fprintf(stderr, "%d warning%s", warning_count, warning_count == 1 ? "" : "s");
        }
        fprintf(stderr, "\n");
    }
}

void cleanup_error_handler() {
    if (source_content) {
        free(source_content);
        source_content = NULL;
    }
}

void report_struct_field_missing_with_location_and_suggestion(const char* struct_name, const char* field_name, const char* suggestion, const char* filename, int line, int column) {
    const char* old_filename;
    int old_line;
    int old_column;
    char buffer[512];
    char hint[256];
    const char* hint_text = NULL;

    push_location(filename, line, column, &old_filename, &old_line, &old_column);

    if (struct_name && field_name) {
        snprintf(buffer, sizeof(buffer), "struct '%s' has no field: '%s'", struct_name, field_name);
    } else if (field_name) {
        snprintf(buffer, sizeof(buffer), "struct has no field: '%s'", field_name);
    } else {
        snprintf(buffer, sizeof(buffer), "struct has no such field");
    }

    if (suggestion && *suggestion) {
        snprintf(hint, sizeof(hint), "did you mean '%s'?", suggestion);
        hint_text = hint;
    }

    emit_diagnostic(ERROR_LEVEL_ERROR, ERROR_UNDEFINED, buffer,
        field_name ? (int)strlen(field_name) : 1,
        hint_text);

    pop_location(old_filename, old_line, old_column);
}
