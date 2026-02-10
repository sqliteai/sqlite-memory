# sqlite-memory Makefile
# Builds the SQLite memory extension with optional llama.cpp for local embeddings

# Configuration options (set via command line: make OMIT_LOCAL_ENGINE=1)
OMIT_LOCAL_ENGINE  ?= 0
OMIT_REMOTE_ENGINE ?= 0
OMIT_IO            ?= 0

# Directories
SRC_DIR     := src
BUILD_DIR   := build
LLAMA_DIR   := modules/llama.cpp
LLAMA_BUILD := $(LLAMA_DIR)/build
TEST_DIR    := test

# Compiler settings
CC          := clang
CXX         := clang++
CFLAGS      := -Wall -Wextra -O2 -fPIC
CXXFLAGS    := -Wall -Wextra -O2 -fPIC -std=c++17
DEFINES     := -DSQLITE_CORE

# Include paths (always include src)
# Note: Homebrew SQLite is needed for SQLITE_ENABLE_LOAD_EXTENSION support
INCLUDES    := -I$(SRC_DIR) -I/opt/homebrew/include

# Base source files (always compiled)
C_SOURCES   := $(SRC_DIR)/sqlite-memory.c \
               $(SRC_DIR)/dbmem-utils.c \
               $(SRC_DIR)/dbmem-parser.c \
               $(SRC_DIR)/dbmem-search.c \
               $(SRC_DIR)/md4c.c

# Platform-specific settings
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
    # macOS
    EXT := dylib
    FRAMEWORKS := -framework Security
    LDFLAGS := -dynamiclib $(FRAMEWORKS)
else ifeq ($(UNAME_S),Linux)
    # Linux
    EXT := so
    LDFLAGS := -shared -lpthread -lm -ldl
else
    # Windows (MinGW)
    EXT := dll
    LDFLAGS := -shared
endif

# Conditional: Local embedding engine (llama.cpp)
ifeq ($(OMIT_LOCAL_ENGINE),0)
    # Include llama.cpp
    INCLUDES += -I$(LLAMA_DIR)/include -I$(LLAMA_DIR)/ggml/include
    C_SOURCES += $(SRC_DIR)/dbmem-lembed.c

    # llama.cpp static libraries
    LLAMA_LIBS := $(LLAMA_BUILD)/src/libllama.a \
                  $(LLAMA_BUILD)/ggml/src/libggml.a \
                  $(LLAMA_BUILD)/ggml/src/libggml-base.a \
                  $(LLAMA_BUILD)/ggml/src/libggml-cpu.a \
                  $(LLAMA_BUILD)/common/libcommon.a

    # Platform-specific llama.cpp libs
    ifeq ($(UNAME_S),Darwin)
        FRAMEWORKS += -framework Metal -framework Foundation -framework Accelerate
        LDFLAGS := -dynamiclib $(FRAMEWORKS)
        LLAMA_LIBS += $(LLAMA_BUILD)/ggml/src/ggml-metal/libggml-metal.a \
                      $(LLAMA_BUILD)/ggml/src/ggml-blas/libggml-blas.a
    else ifeq ($(UNAME_S),Linux)
        # Add CUDA libs if available
        ifneq ($(wildcard $(LLAMA_BUILD)/ggml/src/ggml-cuda/libggml-cuda.a),)
            LLAMA_LIBS += $(LLAMA_BUILD)/ggml/src/ggml-cuda/libggml-cuda.a
            LDFLAGS += -lcuda -lcublas -lcudart
        endif
    endif

    # Use C++ linker when linking with llama.cpp
    LINKER := $(CXX)
    BUILD_DEPS := llama
else
    # Omit local engine
    DEFINES += -DDBMEM_OMIT_LOCAL_ENGINE
    LLAMA_LIBS :=
    LINKER := $(CC)
    BUILD_DEPS :=
endif

# Conditional: Remote embedding engine
ifeq ($(OMIT_REMOTE_ENGINE),0)
    C_SOURCES += $(SRC_DIR)/dbmem-rembed.c
else
    DEFINES += -DDBMEM_OMIT_REMOTE_ENGINE
endif

# Conditional: File I/O
ifeq ($(OMIT_IO),1)
    DEFINES += -DDBMEM_OMIT_IO
endif

# Object files
C_OBJECTS := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(C_SOURCES))

# Output
TARGET := $(BUILD_DIR)/memory.$(EXT)

# Default target
.PHONY: all
all: $(BUILD_DEPS) $(TARGET)

# Build llama.cpp (only if not omitted)
.PHONY: llama
llama: $(LLAMA_BUILD)/src/libllama.a

$(LLAMA_BUILD)/src/libllama.a:
	@echo "Building llama.cpp..."
	@mkdir -p $(LLAMA_BUILD)
	@cd $(LLAMA_BUILD) && cmake .. \
		-DCMAKE_BUILD_TYPE=Release \
		-DGGML_METAL=ON \
		-DGGML_BLAS=ON \
		-DLLAMA_BUILD_TESTS=OFF \
		-DLLAMA_BUILD_EXAMPLES=OFF \
		-DLLAMA_BUILD_SERVER=OFF \
		-DBUILD_SHARED_LIBS=OFF
	@cmake --build $(LLAMA_BUILD) --config Release -j$(shell sysctl -n hw.ncpu 2>/dev/null || nproc)
	@echo "llama.cpp build complete"

# Create build directory
$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

# Compile C sources to object files
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	@echo "Compiling $<..."
	@$(CC) $(CFLAGS) $(DEFINES) $(INCLUDES) -c $< -o $@

# Link the extension
$(TARGET): $(C_OBJECTS) $(LLAMA_LIBS)
	@echo "Linking $(TARGET)..."
	@$(LINKER) $(C_OBJECTS) $(LLAMA_LIBS) $(LDFLAGS) -o $(TARGET)
	@echo "Build complete: $(TARGET)"

# Build and run tests
.PHONY: test
test: $(BUILD_DEPS) $(BUILD_DIR)/unittest
	@echo "Running tests..."
	@$(BUILD_DIR)/unittest

$(BUILD_DIR)/unittest.o: $(TEST_DIR)/unittest.c | $(BUILD_DIR)
	@echo "Compiling unittest.c..."
	@$(CC) $(CFLAGS) $(DEFINES) $(INCLUDES) -c $< -o $@

$(BUILD_DIR)/unittest: $(BUILD_DIR)/unittest.o $(C_OBJECTS) $(LLAMA_LIBS) | $(BUILD_DIR)
	@echo "Linking unittest..."
	@$(LINKER) $(BUILD_DIR)/unittest.o $(C_OBJECTS) $(LLAMA_LIBS) \
		-L/opt/homebrew/lib -lsqlite3 $(FRAMEWORKS) \
		-o $@

# Build without local engine (remote-only builds)
.PHONY: remote
remote:
	@$(MAKE) OMIT_LOCAL_ENGINE=1 all

# Build without remote engine (local-only builds)
.PHONY: local
local:
	@$(MAKE) OMIT_REMOTE_ENGINE=1 all

# Build without file I/O (for WASM)
.PHONY: wasm
wasm:
	@$(MAKE) OMIT_LOCAL_ENGINE=1 OMIT_IO=1 all

# Clean build artifacts
.PHONY: clean
clean:
	@echo "Cleaning build directory..."
	@rm -rf $(BUILD_DIR)

# Clean everything including llama.cpp build
.PHONY: distclean
distclean: clean
	@echo "Cleaning llama.cpp build..."
	@rm -rf $(LLAMA_BUILD)

# Install to system (macOS/Linux)
.PHONY: install
install: $(TARGET)
	@echo "Installing to /usr/local/lib..."
	@install -d /usr/local/lib
	@install -m 755 $(TARGET) /usr/local/lib/
	@echo "Installed: /usr/local/lib/memory.$(EXT)"

# Show help
.PHONY: help
help:
	@echo "sqlite-memory build system"
	@echo ""
	@echo "Targets:"
	@echo "  all        - Build with both local and remote engines (default)"
	@echo "  local      - Build with local engine only (no remote)"
	@echo "  remote     - Build with remote engine only (no llama.cpp)"
	@echo "  wasm       - Build for WASM (no local engine, no file I/O)"
	@echo "  llama      - Build llama.cpp only"
	@echo "  test       - Build and run unit tests"
	@echo "  clean      - Remove build artifacts"
	@echo "  distclean  - Remove all build artifacts including llama.cpp"
	@echo "  install    - Install extension to /usr/local/lib"
	@echo "  help       - Show this help"
	@echo ""
	@echo "Options (set via command line):"
	@echo "  OMIT_LOCAL_ENGINE=1   - Build without llama.cpp (local embeddings)"
	@echo "  OMIT_REMOTE_ENGINE=1  - Build without remote embedding support"
	@echo "  OMIT_IO=1             - Build without file/directory functions"
	@echo ""
	@echo "Examples:"
	@echo "  make                       # Full build (local + remote)"
	@echo "  make local                 # Local engine only"
	@echo "  make remote                # Remote engine only (no llama.cpp)"
	@echo "  make OMIT_LOCAL_ENGINE=1   # Same as 'make remote'"
	@echo "  make test                  # Build and run tests"
	@echo ""
	@echo "Current configuration:"
	@echo "  CC=$(CC)"
	@echo "  CXX=$(CXX)"
	@echo "  Platform=$(UNAME_S)"
	@echo "  OMIT_LOCAL_ENGINE=$(OMIT_LOCAL_ENGINE)"
	@echo "  OMIT_REMOTE_ENGINE=$(OMIT_REMOTE_ENGINE)"
	@echo "  OMIT_IO=$(OMIT_IO)"

# Debug build
.PHONY: debug
debug: CFLAGS += -g -O0 -DENABLE_DBMEM_DEBUG=1
debug: CXXFLAGS += -g -O0
debug: clean all

# Print variables (for debugging the Makefile)
.PHONY: vars
vars:
	@echo "SRC_DIR           = $(SRC_DIR)"
	@echo "BUILD_DIR         = $(BUILD_DIR)"
	@echo "LLAMA_DIR         = $(LLAMA_DIR)"
	@echo "C_SOURCES         = $(C_SOURCES)"
	@echo "C_OBJECTS         = $(C_OBJECTS)"
	@echo "LLAMA_LIBS        = $(LLAMA_LIBS)"
	@echo "TARGET            = $(TARGET)"
	@echo "DEFINES           = $(DEFINES)"
	@echo "INCLUDES          = $(INCLUDES)"
	@echo "LINKER             = $(LINKER)"
	@echo "OMIT_LOCAL_ENGINE  = $(OMIT_LOCAL_ENGINE)"
	@echo "OMIT_REMOTE_ENGINE = $(OMIT_REMOTE_ENGINE)"
	@echo "OMIT_IO            = $(OMIT_IO)"
