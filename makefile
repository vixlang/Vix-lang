LLVM_CFLAGS := $(shell llvm-config --cflags)
LLVM_LDFLAGS := $(shell llvm-config --ldflags --libs)

GCC ?= gcc
CLANG ?= clang
VIXC ?= ./seed/vixc

SRC_DIR := src
BUILD_DIR := build
RUNTIME_DIR := runtime

TARGET := $(BUILD_DIR)/vixc
HELPER_OBJ := $(BUILD_DIR)/helper.o
RUNTIME_OBJ := $(RUNTIME_DIR)/runtime.o
VIXC_OBJ := $(BUILD_DIR)/vixc.o

VIX_SOURCES := $(shell find $(SRC_DIR) -type f -name '*.vix')

all: $(TARGET)
	rm -f vixc


$(BUILD_DIR) $(RUNTIME_DIR):
	mkdir -p $@

$(HELPER_OBJ): $(SRC_DIR)/helper.c | $(BUILD_DIR)
	$(GCC) -c $< -o $@ $(LLVM_CFLAGS) -Wno-deprecated-declarations

$(RUNTIME_OBJ): $(SRC_DIR)/runtime.c | $(RUNTIME_DIR)
	$(GCC) -c $< -o $@

$(VIXC):
	@echo "missing $(VIXC); run ./seed.sh first" >&2
	@exit 1

$(VIXC_OBJ): $(VIX_SOURCES) $(VIXC) | $(BUILD_DIR)
	$(VIXC) $(SRC_DIR)/main.vix -obj -o $@

$(TARGET): $(VIXC_OBJ) $(HELPER_OBJ) $(RUNTIME_OBJ) | $(BUILD_DIR)
	$(CLANG) -o $@ $^ $(LLVM_LDFLAGS)

clean:
	rm -f $(TARGET) $(HELPER_OBJ) $(RUNTIME_OBJ) $(VIXC_OBJ) test test.ll

test: all
	$(TARGET) ../tests/vixc0/let_and_print.vix -o test
	./test >/dev/null

pytest: all
	python3 -m pytest ../tests/vixc0_tests/*.py

.PHONY: all clean test pytest
