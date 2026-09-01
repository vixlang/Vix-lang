LLVM_CFLAGS := $(shell llvm-config --cflags)
LLVM_CXXFLAGS := $(shell llvm-config --cxxflags)
LLVM_LDFLAGS := $(shell llvm-config --ldflags --libs all)
LLD_LIBS := -llldELF -llldCommon -lz -lzstd
GC_LIBS := $(shell pkg-config --libs bdw-gc 2>/dev/null || echo -lgc)
HOST_UNAME := $(shell uname -s)
ifeq ($(OS),Windows_NT)
GC_WRAP_LDFLAGS :=
else ifeq ($(HOST_UNAME),Darwin)
GC_WRAP_LDFLAGS :=
else
GC_WRAP_LDFLAGS := -Wl,--wrap=malloc -Wl,--wrap=realloc -Wl,--wrap=free
endif

GCC ?= clang 
CXX ?= clang++
CLANG ?= clang
PYTHON ?= $(if $(wildcard .venv/bin/python),.venv/bin/python,python3)
PYTEST ?= $(PYTHON) -m pytest
PYTEST_DIR ?= ../tests/vixc0_tests

# Limits are opt-in byte counts.  The launcher preserves child failures and
# reports unsupported limit mechanisms instead of silently running unbounded.
VIXC_AS_LIMIT ?= 0
VIXC_STACK_LIMIT ?= 0
VIXC_RUN ?= VIXC_AS_LIMIT=$(VIXC_AS_LIMIT) VIXC_STACK_LIMIT=$(VIXC_STACK_LIMIT) ./scripts/vixc-run

SRC_DIR := src
BUILD_DIR := build
RUNTIME_DIR := runtime

ifeq ($(OS),Windows_NT)
EXE_EXT := .exe
else
EXE_EXT :=
endif

TARGET := $(BUILD_DIR)/vixc$(EXE_EXT)
SEED_OBJ := $(BUILD_DIR)/vixc-seed.o
SEED_GC_TARGET := $(BUILD_DIR)/vixc-seed-gc$(EXE_EXT)
BOOTSTRAP_OBJ := $(BUILD_DIR)/vixc-bootstrap.o
BOOTSTRAP_TARGET := $(BUILD_DIR)/vixc-bootstrap$(EXE_EXT)
HELPER_OBJ := $(BUILD_DIR)/helper.o
RUNTIME_OBJ := $(RUNTIME_DIR)/runtime.o
VIXC_OBJ := $(BUILD_DIR)/vixc.o
SELF_STAGE_OBJ := $(BUILD_DIR)/vixc-self.o
SELF_STAGE_TARGET := $(BUILD_DIR)/vixc-self$(EXE_EXT)
SELF_LIR_STAGE_OBJ := $(BUILD_DIR)/vixc-self-lir.o
SELF_LIR_STAGE_TARGET := $(BUILD_DIR)/vixc-self-lir$(EXE_EXT)
API_OBJ := $(BUILD_DIR)/api.o
LLC_OBJ := $(BUILD_DIR)/Llc.o
LINKER_OBJ := $(BUILD_DIR)/Linker.o
PASSES_OBJ := $(BUILD_DIR)/Passes.o
COMPILER_GC_OBJ := $(BUILD_DIR)/compiler_gc.o
LLVM_API_OBJS := $(API_OBJ) $(LLC_OBJ) $(LINKER_OBJ) $(PASSES_OBJ)
COMPILER_SUPPORT_OBJS := $(HELPER_OBJ) $(RUNTIME_OBJ) $(LLVM_API_OBJS) $(COMPILER_GC_OBJ)
VIXC ?= $(SEED_GC_TARGET)

VIX_SOURCES := $(shell find $(SRC_DIR) -type f -name '*.vix')

OWNERSHIP_OK := \
	clone_owner clone_call clone_cond \
	copy_borrow ext_ptr_borrow if_branch_move \
	life_return life_syntax mut_ptr_ref \
	prim_copy \
	shr_borrow str_idx8 loop_terminate \
	mir_debug typed_ptr_copy
OWNERSHIP_REJECT := \
	asn_mut_borrow asn_shr_borrow duplicate_mut_borrow \
	life_bad mv_mut_borrow mv_shr_borrow \
	multi_ret mut_borrow_imm owned_param \
	ret_local ret_borrow_asn shr_mut_borrow \
	shared_ref_ro str_use_move array_use_move \
	field_use_move use_after_move

all: $(TARGET)
	rm -f vixc


$(BUILD_DIR) $(RUNTIME_DIR):
	mkdir -p $@

$(HELPER_OBJ): $(SRC_DIR)/helper.c | $(BUILD_DIR)
	$(GCC) -c $< -o $@ $(LLVM_CFLAGS) -Wno-deprecated-declarations

$(API_OBJ): lib/api.c | $(BUILD_DIR)
	$(GCC) -c $< -o $@

$(LLC_OBJ): lib/llvm/Llc.cpp | $(BUILD_DIR)
	$(CXX) -c $< -o $@ $(LLVM_CXXFLAGS) -Wno-deprecated-declarations

$(LINKER_OBJ): lib/llvm/Linker.cpp | $(BUILD_DIR)
	$(CXX) -c $< -o $@ $(LLVM_CXXFLAGS) -Wno-deprecated-declarations

$(PASSES_OBJ): lib/llvm/Passes.cpp | $(BUILD_DIR)
	$(CXX) -c $< -o $@ $(LLVM_CXXFLAGS) -Wno-deprecated-declarations

$(RUNTIME_OBJ): $(SRC_DIR)/runtime.c | $(RUNTIME_DIR)
	$(GCC) -c $< -o $@

$(COMPILER_GC_OBJ): $(SRC_DIR)/compiler_gc.c | $(BUILD_DIR)
	$(GCC) -c $< -o $@

$(SEED_OBJ): seed/vixc.ll | $(BUILD_DIR)
	$(CLANG) -c $< -o $@

$(SEED_GC_TARGET): $(SEED_OBJ) $(COMPILER_SUPPORT_OBJS) | $(BUILD_DIR)
	$(CXX) -fuse-ld=lld -o $@ $^ $(LLVM_LDFLAGS) $(GC_WRAP_LDFLAGS) $(GC_LIBS) $(LLD_LIBS)

$(BOOTSTRAP_OBJ): $(VIX_SOURCES) $(VIXC) | $(BUILD_DIR)
	$(VIXC_RUN) $(VIXC) $(SRC_DIR)/main.vix -obj -o $@

$(BOOTSTRAP_TARGET): $(BOOTSTRAP_OBJ) $(COMPILER_SUPPORT_OBJS) | $(BUILD_DIR)
	$(CXX) -fuse-ld=lld -o $@ $^ $(LLVM_LDFLAGS) $(GC_WRAP_LDFLAGS) $(GC_LIBS) $(LLD_LIBS)

$(VIXC_OBJ): $(VIX_SOURCES) $(BOOTSTRAP_TARGET) | $(BUILD_DIR)
	$(VIXC_RUN) $(BOOTSTRAP_TARGET) $(SRC_DIR)/main.vix -obj -o $@

$(TARGET): $(VIXC_OBJ) $(COMPILER_SUPPORT_OBJS) | $(BUILD_DIR)
	$(CXX) -fuse-ld=lld -o $@ $^ $(LLVM_LDFLAGS) $(GC_WRAP_LDFLAGS) $(GC_LIBS) $(LLD_LIBS)

$(SELF_STAGE_OBJ): $(VIX_SOURCES) $(TARGET) | $(BUILD_DIR)
	$(VIXC_RUN) $(TARGET) --backend=self $(SRC_DIR)/main.vix -obj -o $@

$(SELF_STAGE_TARGET): $(SELF_STAGE_OBJ) $(COMPILER_SUPPORT_OBJS) | $(BUILD_DIR)
	$(CXX) -fuse-ld=lld -o $@ $^ $(LLVM_LDFLAGS) $(GC_WRAP_LDFLAGS) $(GC_LIBS) $(LLD_LIBS)

self-stage: $(SELF_STAGE_TARGET)

$(SELF_LIR_STAGE_OBJ): $(VIX_SOURCES) $(TARGET) | $(BUILD_DIR)
	$(VIXC_RUN) $(TARGET) --backend=self-lir $(SRC_DIR)/main.vix -obj -o $@

$(SELF_LIR_STAGE_TARGET): $(SELF_LIR_STAGE_OBJ) $(COMPILER_SUPPORT_OBJS) | $(BUILD_DIR)
	$(CXX) -fuse-ld=lld -o $@ $^ $(LLVM_LDFLAGS) $(GC_WRAP_LDFLAGS) $(GC_LIBS) $(LLD_LIBS)

self-lir-stage: $(SELF_LIR_STAGE_TARGET)

clean:
	rm -f $(TARGET) $(SEED_GC_TARGET) $(BOOTSTRAP_TARGET) $(SELF_STAGE_TARGET) $(SELF_LIR_STAGE_TARGET) $(SEED_OBJ) $(HELPER_OBJ) $(RUNTIME_OBJ) $(COMPILER_GC_OBJ) $(BOOTSTRAP_OBJ) $(VIXC_OBJ) $(SELF_STAGE_OBJ) $(SELF_LIR_STAGE_OBJ) $(LLVM_API_OBJS) test test.ll

test: all
	$(TARGET) tests/files/test20.vix -o test
	./test >/dev/null
	$(TARGET) --check tests/cabi_struct_parse.vix
	$(TARGET) --check tests/cabi_struct_layout.vix
	$(TARGET) --check tests/cabi_nested_struct.vix
	@set -e; for name in cabi_invalid_fields cabi_packed_unsupported; do \
		if $(TARGET) --check tests/$$name.vix >/dev/null 2>&1; then \
			echo "expected C ABI rejection: $$name"; exit 1; \
		fi; \
	done
	$(TARGET) --ownership-check tests/ownership/life_return.vix
	$(TARGET) --ownership-check tests/lifetime_generic.vix
	@set -e; for path in tests/ownership/life_bad.vix tests/lifetime_local_escape.vix tests/life_mut_borrow.vix; do \
		if $(TARGET) --ownership-check $$path >/dev/null 2>&1; then \
			echo "expected lifetime rejection: $$path"; exit 1; \
		fi; \
	done
	python3 tests/diagnostics.py $(TARGET)
	python3 tests/entry_alloca.py --compiler $(TARGET)
	$(PYTHON) tests/complex_tests.py --compiler $(TARGET)

diagnostic-test: all
	python3 tests/diagnostics.py $(TARGET)

entry-alloca-test: all
	python3 tests/entry_alloca.py --compiler $(TARGET)

complex-test: all
	$(PYTHON) tests/complex_tests.py --compiler $(TARGET)

pytest: all
	@if test -d "$(PYTEST_DIR)"; then \
		$(PYTEST) "$(PYTEST_DIR)"; \
	else \
		echo "pytest suite not found at $(PYTEST_DIR); running repository regression tests"; \
		$(PYTHON) tests/diagnostics.py $(TARGET); \
		$(PYTHON) tests/run.py --compiler $(TARGET); \
	fi

ownership-test: all
	@set -e; for name in $(OWNERSHIP_OK); do \
		$(TARGET) --ownership-check tests/ownership/$$name.vix -o $(BUILD_DIR)/ownership_$$name; \
	done
	@set -e; for name in $(OWNERSHIP_REJECT); do \
		if $(TARGET) --ownership-check tests/ownership/$$name.vix -o $(BUILD_DIR)/ownership_$$name >/dev/null 2>&1; then \
			echo "expected ownership rejection: $$name"; exit 1; \
		fi; \
	done
	@echo "ownership tests passed: $(words $(OWNERSHIP_OK)) accepted, $(words $(OWNERSHIP_REJECT)) rejected"

.PHONY: all self-stage self-lir-stage clean test pytest ownership-test diagnostic-test entry-alloca-test complex-test
