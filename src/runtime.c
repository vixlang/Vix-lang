#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
  VIX_ARRAY_HEADER_BYTES = 8,
  VIX_ARRAY_INITIAL_CAPACITY = 8,
};

static int *vix_array_header(void *arr) {
  return (int *)((char *)arr - VIX_ARRAY_HEADER_BYTES);
}

int vix_array_len(void *arr) {
  if (arr == NULL)
    return 0;
  int *header = vix_array_header(arr);
  return header[0];
}

static int vix_array_capacity(void *arr) {
  if (arr == NULL)
    return 0;
  int *header = vix_array_header(arr);
  return header[1];
}

static int vix_array_grown_capacity(int old_capacity, int needed) {
  int capacity = old_capacity;
  if (capacity < VIX_ARRAY_INITIAL_CAPACITY)
    capacity = VIX_ARRAY_INITIAL_CAPACITY;
  while (capacity < needed) {
    int next = capacity * 2;
    if (next <= capacity)
      return needed;
    capacity = next;
  }
  return capacity;
}

static void *vix_array_reserve_for_push(void *arr, size_t elem_size,
                                        int old_len, int new_len) {
  int old_capacity = vix_array_capacity(arr);
  if (old_capacity >= new_len)
    return arr;

  int new_capacity = vix_array_grown_capacity(old_capacity, new_len);
  void *base = (arr == NULL) ? NULL
                             : (void *)((char *)arr - VIX_ARRAY_HEADER_BYTES);
  size_t data_bytes = (size_t)new_capacity * elem_size;
  size_t total_bytes = VIX_ARRAY_HEADER_BYTES + data_bytes;
  void *new_block = realloc(base, total_bytes);
  if (new_block == NULL)
    return NULL;

  int *header = (int *)new_block;
  header[0] = old_len;
  header[1] = new_capacity;
  return (void *)((char *)new_block + VIX_ARRAY_HEADER_BYTES);
}

static void *vix_array_push_raw(void *arr, const void *val, size_t elem_size) {
  if (elem_size == 0)
    return arr;

  int old_len = vix_array_len(arr);
  int new_len = old_len + 1;
  void *reserved = vix_array_reserve_for_push(arr, elem_size, old_len, new_len);
  if (reserved == NULL)
    return NULL;

  char *data = (char *)reserved;
  memcpy(data + ((size_t)old_len * elem_size), val, elem_size);
  vix_array_header(reserved)[0] = new_len;
  return reserved;
}

void *vix_array_push_i32(void *arr, int val) {
  return vix_array_push_raw(arr, &val, sizeof(int));
}

void *vix_array_push_ptr(void *arr, void *val) {
  return vix_array_push_raw(arr, &val, sizeof(void *));
}

void *vix_array_push_bytes(void *arr, void *val, size_t elem_size) {
  return vix_array_push_raw(arr, val, elem_size);
}

void *vix_array_slice_bytes(void *arr, int start, int end, size_t elem_size) {
  if (arr == NULL || elem_size == 0)
    return NULL;
  int length = vix_array_len(arr);
  if (start < 0) start = 0;
  if (end < start) end = start;
  if (start > length) start = length;
  if (end > length) end = length;
  size_t count = (size_t)(end - start);
  if (count == 0 || count > (SIZE_MAX - VIX_ARRAY_HEADER_BYTES) / elem_size)
    return NULL;
  size_t total_bytes = VIX_ARRAY_HEADER_BYTES + count * elem_size;
  int *header = (int *)malloc(total_bytes);
  if (header == NULL) return NULL;
  header[0] = (int)count;
  header[1] = (int)count;
  memcpy((char *)header + VIX_ARRAY_HEADER_BYTES,
         (char *)arr + VIX_ARRAY_HEADER_BYTES + (size_t)start * elem_size,
         count * elem_size);
  return (char *)header + VIX_ARRAY_HEADER_BYTES;
}

int vix_safe_sdiv_i32(int left, int right) {
  if (right == 0 || (left == INT32_MIN && right == -1)) return 0;
  return left / right;
}

int vix_safe_srem_i32(int left, int right) {
  if (right == 0 || (left == INT32_MIN && right == -1)) return 0;
  return left % right;
}

void *vix_string_concat(const char *a, const char *b) {
  if (a == NULL)
    a = "";
  if (b == NULL)
    b = "";
  size_t len_a = strlen(a);
  size_t len_b = strlen(b);
  size_t total = len_a + len_b + 1;
  char *result = (char *)malloc(total);
  if (result == NULL)
    return NULL;
  memcpy(result, a, len_a);
  memcpy(result + len_a, b, len_b);
  result[len_a + len_b] = '\0';
  return result;
}

char *vix_string_slice(const char *s, int start, int end) {
  if (s == NULL)
    s = "";
  size_t length = strlen(s);
  if (start < 0)
    start = 0;
  if (end < start)
    end = start;
  if ((size_t)start > length)
    start = (int)length;
  if ((size_t)end > length)
    end = (int)length;
  size_t count = (size_t)(end - start);
  char *result = (char *)malloc(count + 1);
  if (result == NULL)
    return NULL;
  memcpy(result, s + start, count);
  result[count] = '\0';
  return result;
}
