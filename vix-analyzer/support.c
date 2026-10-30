/*
 * vix-analyzer support object.
 *
 * vixc emits a Mach-O object through its LLVM backend. The analyzer links that
 * object with the shared Vix runtime plus this small shim, which supplies the
 * three frontend helpers the analyzer actually references and bridges stdio for
 * the LSP transport. The full src/helper.c is not used because it also exposes
 * the LLVM C API wrappers, which would drag the whole LLVM library set into a
 * tool that never generates code.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#else
#include <fcntl.h>
#include <io.h>
#endif

int32_t compiler_string_byte(const char *text, int32_t index) {
  if (text == NULL || index < 0)
    return 0;
  return (int32_t)(int8_t)(unsigned char)text[index];
}

void *vix_lexer_peek_char(const char *src, int pos) {
  if (src == NULL || pos < 0 || (size_t)pos >= strlen(src))
    return NULL;
  return (void *)(uintptr_t)((unsigned char)src[pos] + 1u);
}

void *vix_lexer_next_non_space_char(const char *src, int pos) {
  if (src == NULL || pos < 0)
    return NULL;
  size_t len = strlen(src);
  size_t i = (size_t)pos;
  while (i < len) {
    if (src[i] == ' ' || src[i] == '\t' || src[i] == '\r' || src[i] == '\n') {
      i++;
    } else if (src[i] == '/' && i + 1 < len && src[i + 1] == '/') {
      while (i < len && src[i] != '\n')
        i++;
    } else if (src[i] == '/' && i + 1 < len && src[i + 1] == '*') {
      i += 2;
      while (i + 1 < len && (src[i] != '*' || src[i + 1] != '/'))
        i++;
      if (i + 1 < len)
        i += 2;
    } else {
      return (void *)(uintptr_t)((unsigned char)src[i] + 1u);
    }
  }
  return NULL;
}

#ifdef _WIN32
/* Windows opens stdin/stdout in text mode: CRLF translation and a Ctrl+Z EOF
 * marker would corrupt the byte-counted LSP framing.  Both streams are put
 * into binary mode once, before the first framed byte moves. */
static void vix_analyzer_binary_stdio(void) {
  static int configured = 0;
  if (configured)
    return;
  _setmode(0, _O_BINARY);
  _setmode(1, _O_BINARY);
  configured = 1;
}
#endif

/* Returns the next stdin byte, or -1 at end of input. */
int32_t vix_analyzer_read_byte(void) {
  unsigned char byte = 0;
#ifdef _WIN32
  vix_analyzer_binary_stdio();
  int count = _read(0, &byte, 1);
#else
  ssize_t count = read(0, &byte, 1);
#endif
  if (count <= 0)
    return -1;
  return (int32_t)byte;
}

/* Writes exactly length bytes to stdout and flushes, so LSP frames arrive. */
void vix_analyzer_write(const char *text, int32_t length) {
  if (text == NULL || length <= 0)
    return;
#ifdef _WIN32
  vix_analyzer_binary_stdio();
#endif
  fwrite(text, 1, (size_t)length, stdout);
  fflush(stdout);
}

/* Diagnostics mode state. The analyzer links src/diag.vix, which reads these.
 * They mirror the defaults used by the compiler's own helper object. */
static int vix_diag_format_mode = 0;
static int vix_diag_color_mode = 0;
static int vix_diag_test_mode = 0;

void vix_diag_configure(int format, int color, int test_mode) {
  vix_diag_format_mode = format;
  vix_diag_color_mode = color;
  vix_diag_test_mode = test_mode;
}

int vix_diag_format(void) { return vix_diag_format_mode; }

int vix_diag_test(void) { return vix_diag_test_mode; }

int vix_diag_color_enabled(void) {
  if (vix_diag_test_mode || vix_diag_color_mode == 2)
    return 0;
  if (vix_diag_color_mode == 1)
    return 1;
#ifdef _WIN32
  return _isatty(_STDERR_FILENO) ? 1 : 0;
#else
  return isatty(STDERR_FILENO) ? 1 : 0;
#endif
}

int vix_diag_strlen(const char *s) {
  if (!s)
    return 0;
  return (int)strlen(s);
}

/* Debug tracing for the analyzer. Diagnostics must never go to stdout, which
 * carries the LSP framing. */
void vix_analyzer_debug(const char *text) {
  if (text == NULL)
    return;
  fputs(text, stderr);
  fflush(stderr);
}
