LLVM_CFLAGS := $(shell llvm-config --cflags)
LLVM_CXXFLAGS := $(shell llvm-config --cxxflags)
LLVM_LDFLAGS := $(shell llvm-config --ldflags --libs all)
LLD_LIBS := -llldELF -llldCommon
GC_LIBS := $(shell pkg-config --libs bdw-gc 2>/dev/null || echo -lgc)
GC_WRAP_LDFLAGS := -Wl,--wrap=malloc -Wl,--wrap=realloc -Wl,--wrap=free

GCC ?= clang 
CXX ?= clang++
CLANG ?= clang

# Keep a compiler regression inside one process instead of letting it starve
# the desktop or invoke the global OOM killer.
VIXC_RUN ?= nice -n 10 prlimit --as=1610612736 --stack=67108864

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
	$(TARGET) ../tests/vixc0/let_and_print.vix -o test
	./test >/dev/null

pytest: all
	python3 -m pytest ../tests/vixc0_tests/*.py

.PHONY: all self-stage self-lir-stage clean test pytest
