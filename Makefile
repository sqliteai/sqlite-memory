# sqlite-memory Makefile
# Builds the SQLite memory extension with optional llama.cpp for local embeddings

# Configuration options (set via command line: make OMIT_LOCAL_ENGINE=1)
OMIT_LOCAL_ENGINE  ?= 0
OMIT_REMOTE_ENGINE ?= 0
OMIT_IO            ?= 0
LLAMA ?=

PLATFORM ?= $(shell uname -s | tr '[:upper:]' '[:lower:]')
ARCH     ?= $(shell uname -m)

ifeq ($(PLATFORM),darwin)
    PLATFORM := macos
endif
ifneq (,$(findstring mingw,$(PLATFORM)))
    PLATFORM := windows
endif
ifneq (,$(findstring msys,$(PLATFORM)))
    PLATFORM := windows
endif

SRC_DIR     := src
BUILD_DIR   := build
DIST_DIR    := dist
LLAMA_DIR   := modules/llama.cpp
LLAMA_BUILD := $(LLAMA_DIR)/build
TEST_DIR    := test

VERSION := $(shell grep 'SQLITE_DBMEMORY_VERSION' $(SRC_DIR)/sqlite-memory.h | head -1 | sed 's/.*"\(.*\)".*/\1/')

CPUS := $(shell sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)
MAKEFLAGS += -j$(CPUS)

CC          ?= clang
CXX         ?= clang++
CFLAGS      := -Wall -Wextra -O2 -fPIC
CXXFLAGS    := -Wall -Wextra -O2 -fPIC -std=c++17
DEFINES     :=

INCLUDES    := -I$(SRC_DIR)

C_SOURCES   := $(SRC_DIR)/sqlite-memory.c \
               $(SRC_DIR)/dbmem-utils.c \
               $(SRC_DIR)/dbmem-parser.c \
               $(SRC_DIR)/dbmem-search.c \
               $(SRC_DIR)/md4c.c

OUTPUT_NAME := memory

ifeq ($(PLATFORM),macos)
    EXT := dylib
    FRAMEWORKS := -framework Security
    LDFLAGS := -dynamiclib $(FRAMEWORKS)
    INCLUDES += -I/opt/homebrew/include -I/usr/local/include
    TEST_LDFLAGS := -L/opt/homebrew/lib -L/usr/local/lib -lsqlite3

    ifeq ($(ARCH),x86_64)
        CFLAGS += -arch x86_64
        LDFLAGS += -arch x86_64
    else ifeq ($(ARCH),arm64)
        CFLAGS += -arch arm64
        LDFLAGS += -arch arm64
    else
        CFLAGS += -arch arm64 -arch x86_64
        LDFLAGS += -arch arm64 -arch x86_64
    endif

else ifeq ($(PLATFORM),linux)
    EXT := so
    CC := gcc
    CXX := g++
    LDFLAGS := -shared -lpthread -lm -ldl
    TEST_LDFLAGS := -lsqlite3 -lpthread -lm -ldl

else ifeq ($(PLATFORM),windows)
    EXT := dll
    CC := gcc
    CXX := g++
    LDFLAGS := -shared -static-libgcc -lbcrypt
    OUTPUT_NAME := memory
    TEST_LDFLAGS := -lsqlite3 -lbcrypt

else ifeq ($(PLATFORM),android)
    EXT := so

    NDK_HOST := $(shell uname -s | tr '[:upper:]' '[:lower:]')-x86_64
    ifeq ($(NDK_HOST),darwin-x86_64)
        NDK_HOST := darwin-x86_64
    endif
    TOOLCHAIN := $(ANDROID_NDK)/toolchains/llvm/prebuilt/$(NDK_HOST)

    ANDROID_ARCH := $(ARCH)
    ifeq ($(ARCH),arm64-v8a)
        ANDROID_ARCH := aarch64
        CC := $(TOOLCHAIN)/bin/aarch64-linux-android26-clang
        CXX := $(TOOLCHAIN)/bin/aarch64-linux-android26-clang++
    else ifeq ($(ARCH),armeabi-v7a)
        CC := $(TOOLCHAIN)/bin/armv7a-linux-androideabi26-clang
        CXX := $(TOOLCHAIN)/bin/armv7a-linux-androideabi26-clang++
    else ifeq ($(ARCH),x86_64)
        CC := $(TOOLCHAIN)/bin/x86_64-linux-android26-clang
        CXX := $(TOOLCHAIN)/bin/x86_64-linux-android26-clang++
    endif

    LDFLAGS := -shared -static-libstdc++ -llog -Wl,-z,max-page-size=16384
    TEST_LDFLAGS := -ldl -llog -lm

else ifeq ($(PLATFORM),ios)
    EXT := dylib

    SDK := $(shell xcrun --sdk iphoneos --show-sdk-path)
    CC := $(shell xcrun --sdk iphoneos -f clang)
    CXX := $(shell xcrun --sdk iphoneos -f clang++)
    CFLAGS += -isysroot $(SDK) -arch arm64 -miphoneos-version-min=14.0
    LDFLAGS := -dynamiclib -isysroot $(SDK) -arch arm64 -miphoneos-version-min=14.0 -framework Security

else ifeq ($(PLATFORM),ios-sim)
    EXT := dylib

    SDK := $(shell xcrun --sdk iphonesimulator --show-sdk-path)
    CC := $(shell xcrun --sdk iphonesimulator -f clang)
    CXX := $(shell xcrun --sdk iphonesimulator -f clang++)
    CFLAGS += -isysroot $(SDK) -arch arm64 -arch x86_64 -miphonesimulator-version-min=14.0
    LDFLAGS := -dynamiclib -isysroot $(SDK) -arch arm64 -arch x86_64 -miphonesimulator-version-min=14.0 -framework Security
endif

LLAMA_OPTIONS := $(LLAMA) \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIBS=OFF \
    -DLLAMA_CURL=OFF \
    -DLLAMA_HTTPLIB=OFF \
    -DLLAMA_BUILD_EXAMPLES=OFF \
    -DLLAMA_BUILD_TESTS=OFF \
    -DLLAMA_BUILD_TOOLS=OFF \
    -DLLAMA_BUILD_SERVER=OFF \
    -DGGML_RPC=OFF

ifeq ($(OMIT_LOCAL_ENGINE),0)
    INCLUDES += -I$(LLAMA_DIR)/include -I$(LLAMA_DIR)/ggml/include
    C_SOURCES += $(SRC_DIR)/dbmem-lembed.c

    LLAMA_LIBS := $(LLAMA_BUILD)/src/libllama.a \
                  $(LLAMA_BUILD)/ggml/src/libggml.a \
                  $(LLAMA_BUILD)/ggml/src/libggml-base.a \
                  $(LLAMA_BUILD)/ggml/src/libggml-cpu.a \
                  $(LLAMA_BUILD)/common/libcommon.a

    ifeq ($(PLATFORM),macos)
        LLAMA_OPTIONS += -DGGML_NATIVE=OFF -DGGML_OPENMP=OFF -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0
        LLAMA_LIBS += $(LLAMA_BUILD)/ggml/src/ggml-metal/libggml-metal.a
        LLAMA_LIBS += $(LLAMA_BUILD)/ggml/src/ggml-blas/libggml-blas.a
        ifeq ($(ARCH),x86_64)
            LLAMA_OPTIONS += -DCMAKE_OSX_ARCHITECTURES="x86_64"
        else ifeq ($(ARCH),arm64)
            LLAMA_OPTIONS += -DCMAKE_OSX_ARCHITECTURES="arm64"
        else
            LLAMA_OPTIONS += '-DCMAKE_OSX_ARCHITECTURES=x86_64;arm64'
        endif
        LDFLAGS := -dynamiclib -framework Metal -framework Foundation -framework Accelerate -framework Security
        ifeq ($(ARCH),x86_64)
            LDFLAGS += -arch x86_64
        else ifeq ($(ARCH),arm64)
            LDFLAGS += -arch arm64
        else
            LDFLAGS += -arch arm64 -arch x86_64
        endif
    else ifeq ($(PLATFORM),linux)
        LLAMA_OPTIONS += -DGGML_NATIVE=OFF -DGGML_OPENMP=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON
    else ifeq ($(PLATFORM),windows)
        # Target Windows 7+ (0x0601)
        LLAMA_OPTIONS += -DGGML_NATIVE=OFF -DGGML_OPENMP=OFF -DCMAKE_CXX_FLAGS="-D_WIN32_WINNT=0x0601"
        LDFLAGS := -shared -lbcrypt -static-libgcc -Wl,--push-state,-Bstatic,-lstdc++,-lwinpthread,--pop-state
        # Windows: Ninja puts libs in different locations, use cmake --install
        GGML_PREFIX := $(BUILD_DIR)/ggml
        LLAMA_LIBS := $(GGML_PREFIX)/lib/libllama.a \
                      $(GGML_PREFIX)/lib/ggml.a \
                      $(GGML_PREFIX)/lib/ggml-base.a \
                      $(GGML_PREFIX)/lib/ggml-cpu.a \
                      $(LLAMA_BUILD)/common/libcommon.a
    else ifeq ($(PLATFORM),android)
        ANDROID_OPTIONS := -DCMAKE_TOOLCHAIN_FILE=$(ANDROID_NDK)/build/cmake/android.toolchain.cmake \
            -DANDROID_ABI=$(ARCH) \
            -DANDROID_PLATFORM=android-26 \
            -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
            -DGGML_NATIVE=OFF \
            -DGGML_OPENMP=OFF \
            -DGGML_LLAMAFILE=OFF
        ifeq ($(ARCH),arm64-v8a)
            ANDROID_OPTIONS += -DGGML_CPU_ARM_ARCH=armv8.2-a+dotprod
        endif
        LLAMA_OPTIONS += $(ANDROID_OPTIONS)
    else ifeq ($(PLATFORM),ios)
        LLAMA_OPTIONS += -DGGML_NATIVE=OFF -DGGML_OPENMP=OFF -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0
        LLAMA_LIBS += $(LLAMA_BUILD)/ggml/src/ggml-metal/libggml-metal.a
        LLAMA_LIBS += $(LLAMA_BUILD)/ggml/src/ggml-blas/libggml-blas.a
        LDFLAGS := -dynamiclib -isysroot $(SDK) -arch arm64 -miphoneos-version-min=14.0 \
            -framework Metal -framework Foundation -framework Accelerate -framework CoreFoundation -framework Security \
            -ldl -lpthread -lm -headerpad_max_install_names
    else ifeq ($(PLATFORM),ios-sim)
        LLAMA_OPTIONS += -DGGML_NATIVE=OFF -DGGML_OPENMP=OFF -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphonesimulator -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0 '-DCMAKE_OSX_ARCHITECTURES=x86_64;arm64'
        LLAMA_LIBS += $(LLAMA_BUILD)/ggml/src/ggml-metal/libggml-metal.a
        LLAMA_LIBS += $(LLAMA_BUILD)/ggml/src/ggml-blas/libggml-blas.a
        LDFLAGS := -dynamiclib -isysroot $(SDK) -arch arm64 -arch x86_64 -miphonesimulator-version-min=14.0 \
            -framework Metal -framework Foundation -framework Accelerate -framework CoreFoundation -framework Security \
            -ldl -lpthread -lm -headerpad_max_install_names
    endif

    ifneq (,$(findstring GGML_VULKAN=ON,$(LLAMA)))
        LLAMA_LIBS += $(LLAMA_BUILD)/ggml/src/ggml-vulkan/libggml-vulkan.a
        ifeq ($(PLATFORM),windows)
            ifdef VULKAN_SDK
                LDFLAGS += -L$(VULKAN_SDK)/lib -lvulkan-1
            else
                LDFLAGS += -lvulkan-1
            endif
        else
            ifdef VULKAN_SDK
                LDFLAGS += -L$(VULKAN_SDK)/lib -lvulkan
            else
                LDFLAGS += -lvulkan
            endif
        endif
    endif
    ifneq (,$(findstring GGML_OPENCL=ON,$(LLAMA)))
        LLAMA_LIBS += $(LLAMA_BUILD)/ggml/src/ggml-opencl/libggml-opencl.a
        LDFLAGS += -lOpenCL
    endif

    LINKER := $(CXX)
    BUILD_DEPS := llama
else
    DEFINES += -DDBMEM_OMIT_LOCAL_ENGINE
    LLAMA_LIBS :=
    LINKER := $(CC)
    BUILD_DEPS :=
endif

ifeq ($(OMIT_REMOTE_ENGINE),0)
    C_SOURCES += $(SRC_DIR)/dbmem-rembed.c
else
    DEFINES += -DDBMEM_OMIT_REMOTE_ENGINE
endif

ifeq ($(OMIT_IO),1)
    DEFINES += -DDBMEM_OMIT_IO
endif

C_OBJECTS := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(C_SOURCES))

TARGET := $(DIST_DIR)/$(OUTPUT_NAME).$(EXT)

.PHONY: all
all: extension

.PHONY: extension
extension: $(BUILD_DEPS) $(TARGET)

.PHONY: llama
ifeq ($(PLATFORM),windows)
# Windows: Ninja puts libs in different locations, use cmake --install for consistent paths
llama: $(GGML_PREFIX)/lib/libllama.a

$(GGML_PREFIX)/lib/libllama.a:
	@echo "Building llama.cpp with options: $(LLAMA_OPTIONS)"
	@mkdir -p $(LLAMA_BUILD) $(GGML_PREFIX)
	cmake -B $(LLAMA_BUILD) $(LLAMA_OPTIONS) $(LLAMA_DIR)
	cmake --build $(LLAMA_BUILD) --config Release -j$(CPUS)
	cmake --install $(LLAMA_BUILD) --prefix $(GGML_PREFIX)
	@echo "llama.cpp build complete"

$(GGML_PREFIX)/lib/ggml.a $(GGML_PREFIX)/lib/ggml-base.a $(GGML_PREFIX)/lib/ggml-cpu.a: $(GGML_PREFIX)/lib/libllama.a
	@:
$(LLAMA_BUILD)/common/libcommon.a: $(GGML_PREFIX)/lib/libllama.a
	@:
else
llama: $(LLAMA_BUILD)/src/libllama.a

$(LLAMA_BUILD)/src/libllama.a:
	@echo "Building llama.cpp with options: $(LLAMA_OPTIONS)"
	@mkdir -p $(LLAMA_BUILD)
	cmake -B $(LLAMA_BUILD) $(LLAMA_OPTIONS) $(LLAMA_DIR)
	cmake --build $(LLAMA_BUILD) --config Release -j$(CPUS)
	@echo "llama.cpp build complete"

$(LLAMA_BUILD)/ggml/src/libggml.a $(LLAMA_BUILD)/ggml/src/libggml-base.a $(LLAMA_BUILD)/ggml/src/libggml-cpu.a $(LLAMA_BUILD)/common/libcommon.a $(LLAMA_BUILD)/ggml/src/ggml-metal/libggml-metal.a $(LLAMA_BUILD)/ggml/src/ggml-blas/libggml-blas.a $(LLAMA_BUILD)/ggml/src/ggml-vulkan/libggml-vulkan.a $(LLAMA_BUILD)/ggml/src/ggml-opencl/libggml-opencl.a: $(LLAMA_BUILD)/src/libllama.a
	@:
endif

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

$(DIST_DIR):
	@mkdir -p $(DIST_DIR)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	@echo "Compiling $<..."
	@$(CC) $(CFLAGS) $(DEFINES) $(INCLUDES) -c $< -o $@

$(TARGET): $(C_OBJECTS) $(LLAMA_LIBS) | $(DIST_DIR)
	@echo "Linking $(TARGET)..."
	@$(LINKER) $(C_OBJECTS) $(LLAMA_LIBS) $(LDFLAGS) -o $(TARGET)
	@echo "Build complete: $(TARGET)"

.PHONY: test
test: $(BUILD_DEPS) $(TARGET) $(BUILD_DIR)/unittest
	@echo "Running unit tests..."
ifeq ($(PLATFORM),windows)
	@mkdir -p $(BUILD_DIR)/test_tmp
endif
	@$(BUILD_DIR)/unittest
	@echo ""
	@echo "Testing extension loading..."
	@sqlite3 :memory: ".load $(TARGET)" "SELECT 'memory_version: ' || memory_version();"
	@echo "Extension loading test passed!"

# SQLITE_CORE needed to use direct SQLite calls instead of extension API
TEST_DEFINES := -DSQLITE_CORE

TEST_LINK_EXTRAS :=
ifeq ($(OMIT_LOCAL_ENGINE),0)
    ifeq ($(PLATFORM),macos)
        TEST_LINK_EXTRAS := -framework Metal -framework Foundation -framework Accelerate -lobjc
    else ifeq ($(PLATFORM),android)
        TEST_LINK_EXTRAS := -static-libstdc++
    endif
endif

# Android: compile SQLite amalgamation into unittest (set SQLITE_AMALGAM=path/to/sqlite3.c)
SQLITE_AMALGAM ?=
TEST_SQLITE_OBJ :=
ifneq ($(SQLITE_AMALGAM),)
    TEST_SQLITE_OBJ := $(BUILD_DIR)/test-sqlite3.o
endif

$(BUILD_DIR)/unittest.o: $(TEST_DIR)/unittest.c | $(BUILD_DIR)
	@echo "Compiling unittest.c..."
	@$(CC) $(CFLAGS) $(TEST_DEFINES) $(DEFINES) $(INCLUDES) -c $< -o $@

$(BUILD_DIR)/test-%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	@echo "Compiling $< (for test)..."
	@$(CC) $(CFLAGS) $(TEST_DEFINES) $(DEFINES) $(INCLUDES) -c $< -o $@

$(BUILD_DIR)/test-sqlite3.o: $(SQLITE_AMALGAM) | $(BUILD_DIR)
	@echo "Compiling sqlite3.c (amalgamation)..."
	@$(CC) $(CFLAGS) -DSQLITE_ENABLE_FTS5 -c $< -o $@

TEST_C_OBJECTS := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/test-%.o,$(C_SOURCES))

$(BUILD_DIR)/unittest: $(BUILD_DIR)/unittest.o $(TEST_C_OBJECTS) $(TEST_SQLITE_OBJ) $(LLAMA_LIBS) | $(BUILD_DIR)
	@echo "Linking unittest..."
	@$(LINKER) $(BUILD_DIR)/unittest.o $(TEST_C_OBJECTS) $(TEST_SQLITE_OBJ) $(LLAMA_LIBS) \
		$(TEST_LDFLAGS) $(FRAMEWORKS) $(TEST_LINK_EXTRAS) \
		-o $@

.PHONY: remote
remote:
	@$(MAKE) OMIT_LOCAL_ENGINE=1 extension

.PHONY: local
local:
	@$(MAKE) OMIT_REMOTE_ENGINE=1 extension

.PHONY: wasm
wasm:
	@$(MAKE) OMIT_LOCAL_ENGINE=1 OMIT_IO=1 extension

.PHONY: version
version:
	@echo $(VERSION)

.PHONY: clean
clean:
	@echo "Cleaning build directory..."
	@rm -rf $(BUILD_DIR)
	@rm -rf $(DIST_DIR)

.PHONY: distclean
distclean: clean
	@echo "Cleaning llama.cpp build..."
	@rm -rf $(LLAMA_BUILD)

define PLIST
<?xml version=\"1.0\" encoding=\"UTF-8\"?>\
<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\
<plist version=\"1.0\">\
<dict>\
<key>CFBundleDevelopmentRegion</key>\
<string>en</string>\
<key>CFBundleExecutable</key>\
<string>memory</string>\
<key>CFBundleIdentifier</key>\
<string>ai.sqlite.memory</string>\
<key>CFBundleInfoDictionaryVersion</key>\
<string>6.0</string>\
<key>CFBundlePackageType</key>\
<string>FMWK</string>\
<key>CFBundleSignature</key>\
<string>????</string>\
<key>CFBundleVersion</key>\
<string>$(shell make version)</string>\
<key>CFBundleShortVersionString</key>\
<string>$(shell make version)</string>\
<key>MinimumOSVersion</key>\
<string>14.0</string>\
</dict>\
</plist>
endef

define MODULEMAP
framework module memory {\
  umbrella header \"sqlite-memory.h\"\
  export *\
}
endef

LIB_PREFIXES = ios ios-sim macos
FMWK_NAMES = ios-arm64 ios-arm64_x86_64-simulator macos-arm64_x86_64
XCFRAMEWORK_LLAMA = LLAMA="-DGGML_NATIVE=OFF -DGGML_METAL=ON -DGGML_ACCELERATE=ON -DGGML_BLAS=ON -DGGML_BLAS_VENDOR=Apple"

define create_xcframework
	@$(foreach i,1 2 3,\
		prefix=$(word $(i),$(LIB_PREFIXES)); \
		fmwk=$(word $(i),$(FMWK_NAMES)); \
		mkdir -p $(DIST_DIR)/$$fmwk/memory.framework/Headers; \
		mkdir -p $(DIST_DIR)/$$fmwk/memory.framework/Modules; \
		cp src/sqlite-memory.h $(DIST_DIR)/$$fmwk/memory.framework/Headers; \
		printf "$(PLIST)" > $(DIST_DIR)/$$fmwk/memory.framework/Info.plist; \
		printf "$(MODULEMAP)" > $(DIST_DIR)/$$fmwk/memory.framework/Modules/module.modulemap; \
		mv $(DIST_DIR)/$${prefix}$(1).dylib $(DIST_DIR)/$$fmwk/memory.framework/memory; \
		install_name_tool -id "@rpath/memory.framework/memory" $(DIST_DIR)/$$fmwk/memory.framework/memory; \
	)
	xcodebuild -create-xcframework $(foreach fmwk,$(FMWK_NAMES),-framework $(DIST_DIR)/$(fmwk)/memory.framework) -output $(DIST_DIR)/$(2).xcframework
	rm -rf $(foreach fmwk,$(FMWK_NAMES),$(DIST_DIR)/$(fmwk))
endef

.PHONY: xcframework
xcframework:
	MAKEFLAGS= $(MAKE) distclean && MAKEFLAGS= $(MAKE) PLATFORM=ios OMIT_LOCAL_ENGINE=1 && \
		mv $(DIST_DIR)/memory.dylib $(DIST_DIR)/ios_remote.dylib && \
		rm -rf $(BUILD_DIR) && MAKEFLAGS= $(MAKE) PLATFORM=ios-sim OMIT_LOCAL_ENGINE=1 && \
		mv $(DIST_DIR)/memory.dylib $(DIST_DIR)/ios-sim_remote.dylib && \
		rm -rf $(BUILD_DIR) && MAKEFLAGS= $(MAKE) PLATFORM=macos OMIT_LOCAL_ENGINE=1 && \
		mv $(DIST_DIR)/memory.dylib $(DIST_DIR)/macos_remote.dylib
	$(call create_xcframework,_remote,memory-remote)
	rm -rf $(BUILD_DIR) $(LLAMA_BUILD) && MAKEFLAGS= $(MAKE) PLATFORM=ios OMIT_REMOTE_ENGINE=1 $(XCFRAMEWORK_LLAMA) && \
		mv $(DIST_DIR)/memory.dylib $(DIST_DIR)/ios_local.dylib && \
		rm -rf $(BUILD_DIR) $(LLAMA_BUILD) && MAKEFLAGS= $(MAKE) PLATFORM=ios-sim OMIT_REMOTE_ENGINE=1 $(XCFRAMEWORK_LLAMA) && \
		mv $(DIST_DIR)/memory.dylib $(DIST_DIR)/ios-sim_local.dylib && \
		rm -rf $(BUILD_DIR) $(LLAMA_BUILD) && MAKEFLAGS= $(MAKE) PLATFORM=macos OMIT_REMOTE_ENGINE=1 $(XCFRAMEWORK_LLAMA) && \
		mv $(DIST_DIR)/memory.dylib $(DIST_DIR)/macos_local.dylib
	$(call create_xcframework,_local,memory-local)
	rm -rf $(BUILD_DIR) $(LLAMA_BUILD) && MAKEFLAGS= $(MAKE) PLATFORM=ios $(XCFRAMEWORK_LLAMA) && \
		mv $(DIST_DIR)/memory.dylib $(DIST_DIR)/ios_full.dylib && \
		rm -rf $(BUILD_DIR) $(LLAMA_BUILD) && MAKEFLAGS= $(MAKE) PLATFORM=ios-sim $(XCFRAMEWORK_LLAMA) && \
		mv $(DIST_DIR)/memory.dylib $(DIST_DIR)/ios-sim_full.dylib && \
		rm -rf $(BUILD_DIR) $(LLAMA_BUILD) && MAKEFLAGS= $(MAKE) PLATFORM=macos $(XCFRAMEWORK_LLAMA) && \
		mv $(DIST_DIR)/memory.dylib $(DIST_DIR)/macos_full.dylib
	$(call create_xcframework,_full,memory-full)

AAR_ARM = packages/android/src/main/jniLibs/arm64-v8a/
AAR_X86 = packages/android/src/main/jniLibs/x86_64/

.PHONY: aar
aar:
	mkdir -p $(AAR_ARM) $(AAR_X86)
	$(MAKE) distclean && $(MAKE) PLATFORM=android ARCH=arm64-v8a OMIT_LOCAL_ENGINE=1
	mv $(DIST_DIR)/memory.so $(AAR_ARM)/memory_remote.so
	$(MAKE) clean && $(MAKE) PLATFORM=android ARCH=x86_64 OMIT_LOCAL_ENGINE=1
	mv $(DIST_DIR)/memory.so $(AAR_X86)/memory_remote.so
	$(MAKE) distclean && $(MAKE) PLATFORM=android ARCH=arm64-v8a OMIT_REMOTE_ENGINE=1
	mv $(DIST_DIR)/memory.so $(AAR_ARM)/memory_local.so
	$(MAKE) distclean && $(MAKE) PLATFORM=android ARCH=x86_64 OMIT_REMOTE_ENGINE=1
	mv $(DIST_DIR)/memory.so $(AAR_X86)/memory_local.so
	$(MAKE) distclean && $(MAKE) PLATFORM=android ARCH=arm64-v8a
	mv $(DIST_DIR)/memory.so $(AAR_ARM)/memory_full.so
	$(MAKE) distclean && $(MAKE) PLATFORM=android ARCH=x86_64
	mv $(DIST_DIR)/memory.so $(AAR_X86)/memory_full.so
	cd packages/android && ./gradlew clean assembleRelease
	cp packages/android/build/outputs/aar/android-release.aar $(DIST_DIR)/memory.aar

.PHONY: install
install: $(TARGET)
	@echo "Installing to /usr/local/lib..."
	@install -d /usr/local/lib
	@install -m 755 $(TARGET) /usr/local/lib/
	@echo "Installed: /usr/local/lib/$(OUTPUT_NAME).$(EXT)"

.PHONY: help
help:
	@echo "sqlite-memory build system"
	@echo ""
	@echo "Targets:"
	@echo "  all/extension - Build the extension (default)"
	@echo "  local         - Build with local engine only (no remote)"
	@echo "  remote        - Build with remote engine only (no llama.cpp)"
	@echo "  wasm          - Build for WASM (no local engine, no file I/O)"
	@echo "  llama         - Build llama.cpp only"
	@echo "  xcframework   - Build 3 Apple XCFrameworks (remote, local, full)"
	@echo "  aar           - Build Android AAR with all 3 variants"
	@echo "  test          - Build and run unit tests"
	@echo "  clean         - Remove build artifacts"
	@echo "  distclean     - Remove all build artifacts including llama.cpp"
	@echo "  install       - Install extension to /usr/local/lib"
	@echo "  version       - Print version number"
	@echo "  help          - Show this help"
	@echo ""
	@echo "Platform options (set via command line):"
	@echo "  PLATFORM=macos|linux|windows|android|ios|ios-sim"
	@echo "  ARCH=x86_64|arm64|arm64-v8a|armeabi-v7a"
	@echo ""
	@echo "Build options:"
	@echo "  OMIT_LOCAL_ENGINE=1   - Build without llama.cpp (local embeddings)"
	@echo "  OMIT_REMOTE_ENGINE=1  - Build without remote embedding support"
	@echo "  OMIT_IO=1             - Build without file/directory functions"
	@echo ""
	@echo "Examples:"
	@echo "  make                                    # Full build (local + remote)"
	@echo "  make remote                             # Remote engine only"
	@echo "  make PLATFORM=android ARCH=arm64-v8a   # Android arm64"
	@echo "  make PLATFORM=ios                       # iOS device"
	@echo "  make test                               # Build and run tests"
	@echo ""
	@echo "Current configuration:"
	@echo "  VERSION=$(VERSION)"
	@echo "  PLATFORM=$(PLATFORM)"
	@echo "  ARCH=$(ARCH)"
	@echo "  CC=$(CC)"
	@echo "  CXX=$(CXX)"
	@echo "  OMIT_LOCAL_ENGINE=$(OMIT_LOCAL_ENGINE)"
	@echo "  OMIT_REMOTE_ENGINE=$(OMIT_REMOTE_ENGINE)"
	@echo "  OMIT_IO=$(OMIT_IO)"

.PHONY: debug
debug: CFLAGS += -g -O0 -DENABLE_DBMEM_DEBUG=1
debug: CXXFLAGS += -g -O0
debug: clean extension

.PHONY: vars
vars:
	@echo "VERSION           = $(VERSION)"
	@echo "PLATFORM          = $(PLATFORM)"
	@echo "ARCH              = $(ARCH)"
	@echo "SRC_DIR           = $(SRC_DIR)"
	@echo "BUILD_DIR         = $(BUILD_DIR)"
	@echo "DIST_DIR          = $(DIST_DIR)"
	@echo "LLAMA_DIR         = $(LLAMA_DIR)"
	@echo "C_SOURCES         = $(C_SOURCES)"
	@echo "C_OBJECTS         = $(C_OBJECTS)"
	@echo "LLAMA_LIBS        = $(LLAMA_LIBS)"
	@echo "TARGET            = $(TARGET)"
	@echo "DEFINES           = $(DEFINES)"
	@echo "INCLUDES          = $(INCLUDES)"
	@echo "LINKER            = $(LINKER)"
	@echo "OMIT_LOCAL_ENGINE = $(OMIT_LOCAL_ENGINE)"
	@echo "OMIT_REMOTE_ENGINE= $(OMIT_REMOTE_ENGINE)"
	@echo "OMIT_IO           = $(OMIT_IO)"
