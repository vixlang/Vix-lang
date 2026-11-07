/*
 * Vix interned type pool.
 *
 * Split out of helper.c so that tools which only need the front end (the
 * language server, for example) can link the pool without dragging in the LLVM
 * C API wrappers that helper.c also carries.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Interned type pool.
 *
 * A type used to be a string: consumers re-parsed the text (see the old
 * ty_from_text) and compared strings to decide whether two types were the
 * same.  Here every distinct type becomes one node, so two occurrences of the
 * same type share an index and identity is an integer comparison.  The node
 * keeps the canonical text as well, because diagnostics, interface files and
 * --ast-json must still print exactly what the source said.
 *
 * kind: 0 named, 1 pointer, 2 array, 3 slice, 4 tuple, 5 function, 6 option,
 *       7 unknown, 8 error
 * ------------------------------------------------------------------------- */

#define MAX_TYPES 32768
#define MAX_TYPE_ARGS 32
#define MAX_TYPE_STRINGS 32768
#define TYPE_STR_SIZE 512
#define TYPE_HASH_SIZE (1 << 16)

typedef struct {
  int kind;
  int sym;        /* resolved type symbol id, -1 until resolution runs */
  int arg_count;
  int args[MAX_TYPE_ARGS];
  int ret;        /* function return, -1 otherwise */
  int array_len;  /* fixed array length, -1 otherwise */
  int is_mut;
  int is_ref;     /* 1 for "&T", 0 for the legacy "ptr:T" spelling */
  int is_ref_to_ptr; /* 1 for the synthetic "refptr:T" lowering marker */
  int lifetime;   /* interned string index of a lifetime name, -1 if none */
  int text;       /* interned string index of the canonical spelling */
  int base;       /* interned string index for a named type, -1 otherwise */
} TypePoolNode;

static TypePoolNode type_nodes[MAX_TYPES];
static int type_node_count = 0;
static char type_strings[MAX_TYPE_STRINGS][TYPE_STR_SIZE];
static int type_string_count = 0;
static int type_string_hash[TYPE_HASH_SIZE];
static int type_by_string[MAX_TYPE_STRINGS];

static void vix_type_pool_init(void) {
  static int initialized = 0;
  if (initialized)
    return;
  initialized = 1;
  for (int i = 0; i < TYPE_HASH_SIZE; i++)
    type_string_hash[i] = -1;
  for (int i = 0; i < MAX_TYPE_STRINGS; i++)
    type_by_string[i] = -1;
}

static unsigned int type_hash_text(const char *s) {
  unsigned int h = 2166136261u;
  while (*s) {
    h ^= (unsigned char)*s++;
    h *= 16777619u;
  }
  return h;
}

static int vix_type_intern_string(const char *s) {
  vix_type_pool_init();
  unsigned int slot = type_hash_text(s) & (TYPE_HASH_SIZE - 1);
  while (type_string_hash[slot] >= 0) {
    if (strcmp(type_strings[type_string_hash[slot]], s) == 0)
      return type_string_hash[slot];
    slot = (slot + 1) & (TYPE_HASH_SIZE - 1);
  }
  if (type_string_count >= MAX_TYPE_STRINGS) {
    fprintf(stderr, "vixc: internal error: type string table overflow\n");
    return -1;
  }
  int idx = type_string_count++;
  strncpy(type_strings[idx], s, TYPE_STR_SIZE - 1);
  type_strings[idx][TYPE_STR_SIZE - 1] = '\0';
  type_string_hash[slot] = idx;
  return idx;
}

int vix_type_intern(const char *text, int kind, const char *base, int *args,
                    int arg_count, int ret, int array_len, int is_mut) {
  vix_type_pool_init();
  int text_idx = vix_type_intern_string(text);
  if (text_idx < 0)
    return -1;
  if (type_by_string[text_idx] >= 0)
    return type_by_string[text_idx];
  if (type_node_count >= MAX_TYPES) {
    fprintf(stderr, "vixc: internal error: type pool overflow\n");
    return -1;
  }
  if (arg_count > MAX_TYPE_ARGS)
    arg_count = MAX_TYPE_ARGS;
  int idx = type_node_count++;
  type_nodes[idx].kind = kind;
  type_nodes[idx].sym = -1;
  type_nodes[idx].arg_count = arg_count;
  for (int i = 0; i < arg_count; i++)
    type_nodes[idx].args[i] = args[i];
  type_nodes[idx].ret = ret;
  type_nodes[idx].array_len = array_len;
  type_nodes[idx].is_mut = is_mut;
  type_nodes[idx].is_ref = 0;
  type_nodes[idx].is_ref_to_ptr = 0;
  type_nodes[idx].lifetime = -1;
  type_nodes[idx].text = text_idx;
  type_nodes[idx].base = (base != NULL && base[0] != '\0') ? vix_type_intern_string(base) : -1;
  type_by_string[text_idx] = idx;
  return idx;
}

int vix_type_unknown(void) {
  int no_args[MAX_TYPE_ARGS];
  return vix_type_intern("unknown", 7, "", no_args, 0, -1, -1, 0);
}

int vix_type_invalid(void) {
  int no_args[MAX_TYPE_ARGS];
  return vix_type_intern("<error>", 8, "", no_args, 0, -1, -1, 0);
}

int vix_type_find(const char *text) {
  int text_idx = vix_type_intern_string(text);
  if (text_idx < 0)
    return -1;
  return type_by_string[text_idx];
}

const char *vix_type_text(int idx) {
  if (idx < 0 || idx >= type_node_count)
    return "unknown";
  int s = type_nodes[idx].text;
  return (s >= 0 && s < type_string_count) ? type_strings[s] : "unknown";
}

const char *vix_type_base(int idx) {
  if (idx < 0 || idx >= type_node_count)
    return "";
  int s = type_nodes[idx].base;
  return (s >= 0 && s < type_string_count) ? type_strings[s] : "";
}

int vix_type_kind(int idx) {
  return (idx >= 0 && idx < type_node_count) ? type_nodes[idx].kind : 7;
}

int vix_type_arg_count(int idx) {
  return (idx >= 0 && idx < type_node_count) ? type_nodes[idx].arg_count : 0;
}

int vix_type_arg(int idx, int i) {
  if (idx < 0 || idx >= type_node_count)
    return -1;
  if (i < 0 || i >= type_nodes[idx].arg_count)
    return -1;
  return type_nodes[idx].args[i];
}

int vix_type_ret(int idx) {
  return (idx >= 0 && idx < type_node_count) ? type_nodes[idx].ret : -1;
}

int vix_type_array_len(int idx) {
  return (idx >= 0 && idx < type_node_count) ? type_nodes[idx].array_len : -1;
}

int vix_type_is_mut(int idx) {
  return (idx >= 0 && idx < type_node_count) ? type_nodes[idx].is_mut : 0;
}

void vix_type_set_ref_info(int idx, int is_ref, const char *lifetime) {
  if (idx < 0 || idx >= type_node_count)
    return;
  type_nodes[idx].is_ref = is_ref;
  type_nodes[idx].lifetime = (lifetime != NULL && lifetime[0] != 0)
                                 ? vix_type_intern_string(lifetime)
                                 : -1;
}

void vix_type_set_ref_to_ptr(int idx, int value) {
  if (idx >= 0 && idx < type_node_count)
    type_nodes[idx].is_ref_to_ptr = value;
}

int vix_type_is_ref_to_ptr(int idx) {
  return (idx >= 0 && idx < type_node_count) ? type_nodes[idx].is_ref_to_ptr : 0;
}

int vix_type_is_ref(int idx) {
  return (idx >= 0 && idx < type_node_count) ? type_nodes[idx].is_ref : 0;
}

const char *vix_type_lifetime(int idx) {
  if (idx < 0 || idx >= type_node_count)
    return "";
  int s = type_nodes[idx].lifetime;
  return (s >= 0 && s < type_string_count) ? type_strings[s] : "";
}

int vix_type_sym(int idx) {
  return (idx >= 0 && idx < type_node_count) ? type_nodes[idx].sym : -1;
}

void vix_type_set_sym(int idx, int sym) {
  if (idx >= 0 && idx < type_node_count)
    type_nodes[idx].sym = sym;
}

int vix_type_count(void) {
  return type_node_count;
}
