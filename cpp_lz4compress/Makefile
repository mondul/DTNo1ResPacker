EMXX ?= em++
LZ4_LIB_DIR := lz4/lib

LZ4_SRCS  := $(LZ4_LIB_DIR)/lz4.c $(LZ4_LIB_DIR)/lz4hc.c
PROJ      := lz4compress
SRC       := src/$(PROJ).cpp
OUT_DIR   := ../src/lib

# Common flags
INCLUDES  := -I$(LZ4_LIB_DIR)
STD       := -std=c++17

# Emscripten module flags (common)
EMFLAGS_COMMON := \
  -s MODULARIZE=1 \
  -s EXPORT_ES6=1 \
  -s EXPORT_NAME=lz4_init \
  -s ENVIRONMENT=web \
  -s ALLOW_MEMORY_GROWTH=1 \
  -s FILESYSTEM=0 \
  --bind

# Debug: easier debugging, source maps, runtime checks
CXXFLAGS_DEBUG := -O1 $(STD) $(INCLUDES)
EMFLAGS_DEBUG  := \
  $(EMFLAGS_COMMON) \
  -gsource-map \
  -s ASSERTIONS=1 \
  --closure 0

# Release: smaller/faster wasm
CXXFLAGS_RELEASE := -O3 -flto $(STD) $(INCLUDES)
EMFLAGS_RELEASE  := \
  $(EMFLAGS_COMMON) \
  -s ASSERTIONS=0

# Default target
.PHONY: all debug release clean
all: debug

debug:
	@mkdir -p $(OUT_DIR)
	$(EMXX) $(CXXFLAGS_DEBUG) $(SRC) $(LZ4_SRCS) -o $(OUT_DIR)/$(PROJ).js $(EMFLAGS_DEBUG)

release:
	@mkdir -p $(OUT_DIR)
	$(EMXX) $(CXXFLAGS_RELEASE) $(SRC) $(LZ4_SRCS) -o $(OUT_DIR)/$(PROJ).js $(EMFLAGS_RELEASE)

clean:
	rm -f $(OUT_DIR)/$(PROJ).*
