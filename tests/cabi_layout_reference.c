#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

typedef struct { int32_t left; int32_t right; } CPair;
typedef struct { uint8_t tag; uint32_t value; } CBytes;
typedef struct { uint8_t tag; void *value; uint16_t code; } CPtr;
typedef struct { int32_t values[4]; } CArray;

int main(void) {
  printf("%zu %zu %zu\n", sizeof(CPair), _Alignof(CPair), offsetof(CPair, right));
  printf("%zu %zu %zu\n", sizeof(CBytes), _Alignof(CBytes), offsetof(CBytes, value));
  printf("%zu %zu %zu\n", sizeof(CPtr), _Alignof(CPtr), offsetof(CPtr, code));
  printf("%zu %zu\n", sizeof(CArray), _Alignof(CArray));
  return 0;
}
