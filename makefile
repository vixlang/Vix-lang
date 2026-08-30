LLVM_CFLAGS := $(shell llvm-config --cflags)
LLVM_CXXFLAGS := $(shell llvm-config --cxxflags)
LLVM_LDFLAGS := $(shell llvm-config --ldflags --libs all)
LLD_LIBS := -llldELF -llldCommon -lz -lzstd
GC_LIBS := $(shell pkg-config --libs bdw-gc 2>/dev/null || echo -lgc)
GC_WRAP_LDFLAGS := -Wl,--wrap=malloc -Wl,--wrap=realloc -Wl,--wrap=free

GCC ?= clang 
CXX ?= clang++
CLANG ?= clang

# Keep a compiler regression inside one process instead of letting it starve
# the desktop or invoke the global OOM killer.
VIXC_RUN ?= nice -n 10 prlimit --as=1610612736 --stack=536870912

SRC_DIR := src
BUILD_DIR := build
RUNTIME_DIR := runtime

TARGET := $(BUILD_DIR)/vixc
SEED_OBJ := $(BUILD_DIR)/vixc-seed.o
SEED_GC_TARGET := $(BUILD_DIR)/vixc-seed-gc
BOOTSTRAP_OBJ := $(BUILD_DIR)/vixc-bootstrap.o
BOOTSTRAP_TARGET := $(BUILD_DIR)/vixc-bootstrap
HELPER_OBJ := $(BUILD_DIR)/helper.o
RUNTIME_OBJ := $(RUNTIME_DIR)/runtime.o
VIXC_OBJ := $(BUILD_DIR)/vixc.o
SELF_STAGE_OBJ := $(BUILD_DIR)/vixc-self.o
SELF_STAGE_TARGET := $(BUILD_DIR)/vixc-self
SELF_LIR_STAGE_OBJ := $(BUILD_DIR)/vixc-self-lir.o
SELF_LIR_STAGE_TARGET := $(BUILD_DIR)/vixc-self-lir
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
	clone_keeps_owner_ok clone_nested_call_ok clone_nested_condition_ok \
	copy_and_borrow_ok extern_pointer_borrows_ok if_expression_branch_move_ok \
	lifetime_return_ok lifetime_syntax mut_typed_pointer_ref_runtime \
	shared_borrows_ok string_index_i8 terminated_loop_state_ok \
	typed_mir_debug typed_pointer_copy_ok
OWNERSHIP_REJECT := \
	assign_while_mut_borrow assign_while_shared_borrow duplicate_mut_borrow \
	lifetime_mismatch move_while_mut_borrow move_while_shared_borrow \
	multiple_return_borrow_sources mut_borrow_immutable owned_parameter_consumes \
	return_local_ref returned_borrow_blocks_assign shared_plus_mut_borrow \
	shared_reference_is_readonly string_use_after_move use_after_array_move \
	use_after_field_move use_after_move

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
	$(TARGET) --ownership-check tests/lifetime_return_ok.vix
	$(TARGET) --ownership-check tests/lifetime_generic.vix
	@set -e; for name in lifetime_mismatch lifetime_local_escape lifetime_mutation_while_borrowed; do \
		if $(TARGET) --ownership-check tests/$$name.vix >/dev/null 2>&1; then \
			echo "expected lifetime rejection: $$name"; exit 1; \
		fi; \
	done
	python3 tests/diagnostics.py $(TARGET)

diagnostic-test: all
	python3 tests/diagnostics.py $(TARGET)

pytest: all
	python3 -m pytest ../tests/vixc0_tests/*.py

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

.PHONY: all self-stage self-lir-stage clean test pytest ownership-test diagnostic-test
