LLVM_CFLAGS := $(shell llvm-config --cflags)
LLVM_CXXFLAGS := $(shell llvm-config --cxxflags)
LLVM_LDFLAGS := $(shell llvm-config --ldflags --libs all)
LLD_LIBS := -llldELF -llldCommon

GCC ?= clang 
CXX ?= clang++
CLANG ?= clang
VIXC ?= ./seed/vixc

SRC_DIR := src
BUILD_DIR := build
RUNTIME_DIR := runtime

TARGET := $(BUILD_DIR)/vixc
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
LLVM_API_OBJS := $(API_OBJ) $(LLC_OBJ) $(LINKER_OBJ) $(PASSES_OBJ)
COMPILER_SUPPORT_OBJS := $(HELPER_OBJ) $(RUNTIME_OBJ) $(LLVM_API_OBJS)

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

$(VIXC):
	@echo "missing $(VIXC); run ./seed.sh first" >&2
	@exit 1

$(BOOTSTRAP_OBJ): $(VIX_SOURCES) $(VIXC) | $(BUILD_DIR)
	ulimit -s 65536 && $(VIXC) $(SRC_DIR)/main.vix -obj -o $@

$(BOOTSTRAP_TARGET): $(BOOTSTRAP_OBJ) $(COMPILER_SUPPORT_OBJS) | $(BUILD_DIR)
	$(CXX) -fuse-ld=lld -o $@ $^ $(LLVM_LDFLAGS) $(LLD_LIBS)

$(VIXC_OBJ): $(VIX_SOURCES) $(BOOTSTRAP_TARGET) | $(BUILD_DIR)
	ulimit -s 65536 && $(BOOTSTRAP_TARGET) $(SRC_DIR)/main.vix -obj -o $@

$(TARGET): $(VIXC_OBJ) $(COMPILER_SUPPORT_OBJS) | $(BUILD_DIR)
	$(CXX) -fuse-ld=lld -o $@ $^ $(LLVM_LDFLAGS) $(LLD_LIBS)

$(SELF_STAGE_OBJ): $(VIX_SOURCES) $(TARGET) | $(BUILD_DIR)
	ulimit -s 65536 && $(TARGET) --backend=self $(SRC_DIR)/main.vix -obj -o $@

$(SELF_STAGE_TARGET): $(SELF_STAGE_OBJ) $(COMPILER_SUPPORT_OBJS) | $(BUILD_DIR)
	$(CXX) -fuse-ld=lld -o $@ $^ $(LLVM_LDFLAGS) $(LLD_LIBS)

self-stage: $(SELF_STAGE_TARGET)

$(SELF_LIR_STAGE_OBJ): $(VIX_SOURCES) $(TARGET) | $(BUILD_DIR)
	ulimit -s unlimited && $(TARGET) --backend=self-lir $(SRC_DIR)/main.vix -obj -o $@

$(SELF_LIR_STAGE_TARGET): $(SELF_LIR_STAGE_OBJ) $(COMPILER_SUPPORT_OBJS) | $(BUILD_DIR)
	$(CXX) -fuse-ld=lld -o $@ $^ $(LLVM_LDFLAGS) $(LLD_LIBS)

self-lir-stage: $(SELF_LIR_STAGE_TARGET)

clean:
	rm -f $(TARGET) $(BOOTSTRAP_TARGET) $(SELF_STAGE_TARGET) $(SELF_LIR_STAGE_TARGET) $(HELPER_OBJ) $(RUNTIME_OBJ) $(BOOTSTRAP_OBJ) $(VIXC_OBJ) $(SELF_STAGE_OBJ) $(SELF_LIR_STAGE_OBJ) $(LLVM_API_OBJS) test test.ll

test: all
	$(TARGET) ../tests/vixc0/let_and_print.vix -o test
	./test >/dev/null

pytest: all
	python3 -m pytest ../tests/vixc0_tests/*.py

.PHONY: all self-stage self-lir-stage clean test pytest
