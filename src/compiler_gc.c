#include <stddef.h>
#include <stdlib.h>

/*
 * Compiler-only allocation bridge.
 *
 * The self-hosted frontend creates many short-lived AST and type-inference
 * objects.  Generated Vix code intentionally does not free those objects, so
 * a full bootstrap otherwise retains several gigabytes until process exit.
 * Link compiler executables with --wrap to route their allocations through
 * Boehm GC without changing the runtime used by compiled user programs.
 */
extern void *GC_malloc(size_t size);
extern void *GC_realloc(void *ptr, size_t size);
extern void GC_free(void *ptr);
extern void *GC_base(void *ptr);

#if defined(_WIN32) || defined(WIN32) || defined(__APPLE__)
/* COFF/LLD and Apple's ld do not provide GNU linker's --wrap aliases. */
#define __real_realloc realloc
#define __real_free free
#else
extern void *__real_realloc(void *ptr, size_t size);
extern void __real_free(void *ptr);
#endif

void *__wrap_malloc(size_t size) {
  /*
   * The immutable seed emits a 32-bit store for byte-index assignment.  Its
   * source-line copier can therefore touch three bytes past a string buffer.
   * Current sources avoid that path via vix_source_line_copy, but the seed
   * compiler itself still needs a compiler-only compatibility tail.
   */
  if (size <= (size_t)-1 - 3)
    size += 3;
  return GC_malloc(size);
}

void *__wrap_realloc(void *ptr, size_t size) {
  if (ptr != NULL && GC_base(ptr) == NULL)
    return __real_realloc(ptr, size);
  /* Keep the immutable seed's three-byte compatibility tail after growth as
   * well as initial allocation.  Preserve realloc(ptr, 0) semantics. */
  if (size != 0 && size <= (size_t)-1 - 3)
    size += 3;
  return GC_realloc(ptr, size);
}

void __wrap_free(void *ptr) {
  if (ptr == NULL)
    return;
  if (GC_base(ptr) != NULL)
    GC_free(ptr);
  else
    __real_free(ptr);
}
