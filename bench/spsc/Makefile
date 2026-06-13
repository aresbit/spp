# ============================================================================
# Makefile for spp::Concurrency::Spsc_Ring benchmark.
# Modelled on the modern-c-makefile (gnaro) skill conventions.
#
# Targets:
#   make             -- release build (default), produces bin/spsc_bench
#   make debug=1     -- debug build with -O0 -g, no NDEBUG
#   make run         -- build and run the bench
#   make remote      -- copy + build + run on the supplied test machine
#   make asm         -- emit annotated assembly for spsc_ring inlined push/pop
#   make tsan        -- ThreadSanitizer build (verify no data races)
#   make clean       -- remove build/ and bin/
# ============================================================================

debug ?= 0

NAME       := spsc_bench
PROJECT    := spp-spsc-bench

# Layout (relative to this Makefile)
ROOT       := $(abspath $(CURDIR)/../..)
INCLUDE    := $(ROOT)/include
SRC_DIR    := $(CURDIR)
BUILD_DIR  := build
BIN_DIR    := bin

SRCS       := $(wildcard $(SRC_DIR)/*.cpp)
OBJS       := $(patsubst $(SRC_DIR)/%.cpp,$(BUILD_DIR)/%.o,$(SRCS))
DEPS       := $(OBJS:.o=.d)

# ----------------------------------------------------------------------------
# Toolchain
# ----------------------------------------------------------------------------
CXX        ?= g++
LINKER     := $(CXX)

# ----------------------------------------------------------------------------
# Target uarch
# ----------------------------------------------------------------------------
# arch=native    -> build for whatever CPU we are on (default during dev)
# arch=skylake-avx512 -> the test box (i9-10980XE, Cascade Lake-X). Use this
#                       when building for the deployment machine so that
#                       __builtin_memcpy() of the 152-byte envelope emits
#                       AVX-512 ZMM stores (3x faster than the SSE2 fallback
#                       gcc picks under generic x86-64).
arch ?= native
CXXFLAGS_ARCH := -march=$(arch) -mtune=$(arch)

# Hot-path flags. -O3 + -march=native gives the compiler everything it needs
# to inline try_push / try_pop and unroll the calibration loop.
CXXFLAGS_COMMON := \
	-std=c++17 \
	-Wall -Wextra -Wpedantic \
	-Wno-unused-parameter -Wno-unused-function \
	-fno-rtti -fno-exceptions \
	-fno-strict-aliasing \
	-fno-plt \
	-I$(INCLUDE) -I$(SRC_DIR) \
	-MMD -MP

ifeq ($(debug),1)
CXXFLAGS  := $(CXXFLAGS_COMMON) -O0 -g3 -DSPP_SPSC_DEBUG
else
# -O3 inlines try_push/try_pop.
# -march=native picks up AVX2/AVX512 stores for the memcpy of the 152-byte
#   envelope (on the test box: Cascade Lake, so AVX-512).
# -fno-plt cuts PLT indirection on shared-library bound symbols.
# -fno-semantic-interposition lets gcc devirtualise across translation units.
# -fomit-frame-pointer frees up RBP.
# -flto turns the two TUs into a single optimisation unit, which lets the
#   benchmark inline the queue calls.
CXXFLAGS  := $(CXXFLAGS_COMMON) \
	-O3 -DNDEBUG \
	$(CXXFLAGS_ARCH) \
	-fomit-frame-pointer \
	-fno-semantic-interposition \
	-flto
endif

LDFLAGS    := -lpthread
ifneq ($(debug),1)
LDFLAGS    += -flto -fuse-linker-plugin
endif

# ----------------------------------------------------------------------------
# Pretty-print
# ----------------------------------------------------------------------------
V ?= 0
ifeq ($(V),1)
Q :=
else
Q := @
endif

# ----------------------------------------------------------------------------
# Default
# ----------------------------------------------------------------------------
.PHONY: all
all: $(BIN_DIR)/$(NAME)

# ----------------------------------------------------------------------------
# Link
# ----------------------------------------------------------------------------
$(BIN_DIR)/$(NAME): $(OBJS) | $(BIN_DIR)
	@printf "  LD      %s\n" "$@"
	$(Q)$(LINKER) $(CXXFLAGS) -o $@ $(OBJS) $(LDFLAGS)

# ----------------------------------------------------------------------------
# Compile
# ----------------------------------------------------------------------------
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp | $(BUILD_DIR)
	@printf "  CXX     %s\n" "$<"
	$(Q)$(CXX) $(CXXFLAGS) -c $< -o $@

# Auto-include header dep files (generated via -MMD -MP).
-include $(DEPS)

# ----------------------------------------------------------------------------
# Dirs
# ----------------------------------------------------------------------------
$(BUILD_DIR) $(BIN_DIR):
	$(Q)mkdir -p $@

# ----------------------------------------------------------------------------
# Phony targets
# ----------------------------------------------------------------------------
.PHONY: run
run: $(BIN_DIR)/$(NAME)
	@echo "==> running $(BIN_DIR)/$(NAME)"
	@$(BIN_DIR)/$(NAME)

# Dump the post-inlining x86 of try_push / try_pop so we can eyeball the
# generated code (catch regressions where the compiler stops inlining).
.PHONY: asm
asm: $(BUILD_DIR)/main.s
$(BUILD_DIR)/main.s: $(SRC_DIR)/main.cpp | $(BUILD_DIR)
	$(Q)$(CXX) $(CXXFLAGS) -S -fverbose-asm -o $@ $<
	@grep -n -E 'try_push|try_pop|push_blocking|pop_blocking|rdtsc' $@ | head -40

.PHONY: tsan
tsan: CXXFLAGS := $(CXXFLAGS_COMMON) -O1 -g -fsanitize=thread
tsan: LDFLAGS  := -lpthread -fsanitize=thread
tsan: clean $(BIN_DIR)/$(NAME)

.PHONY: clean
clean:
	$(Q)rm -rf $(BUILD_DIR) $(BIN_DIR)

# ----------------------------------------------------------------------------
# Remote helpers — invoked from the host. SCP source to the test box, build
# remotely via devtoolset-9, run pinned to the isolated CPU set.
# ----------------------------------------------------------------------------
REMOTE_HOST := 120.55.166.26
REMOTE_USER := yyang
REMOTE_PORT := 40012
REMOTE_DIR  := ~/spsc

.PHONY: remote-copy remote-build remote-run remote

remote-copy:
	@echo "==> rsync spp + bench to $(REMOTE_USER)@$(REMOTE_HOST):$(REMOTE_DIR)"
	@python3 $(ROOT)/../spsc/scripts/remote.py run "mkdir -p $(REMOTE_DIR)"
	@python3 $(ROOT)/../spsc/scripts/remote.py put $(ROOT)/include $(REMOTE_DIR)/
	@python3 $(ROOT)/../spsc/scripts/remote.py put $(CURDIR) $(REMOTE_DIR)/bench-spsc

remote-build:
	@echo "==> building on remote (arch=skylake-avx512, devtoolset-9)"
	@python3 $(ROOT)/../spsc/scripts/remote.py run \
		"source /opt/rh/devtoolset-9/enable && cd $(REMOTE_DIR)/bench-spsc && make clean && make arch=skylake-avx512 -j4"

# Isolated CPU set on the test box is 9-17. Pin the producer (core 9) and
# consumer (core 11) to two separate, isolated cores; taskset locks the
# entire process mask so any libc-spawned helper threads inherit it.
remote-run:
	@echo "==> running on remote (pinned to isolated CPUs 9-17 via taskset)"
	@python3 $(ROOT)/../spsc/scripts/remote.py run \
		"cd $(REMOTE_DIR)/bench-spsc && taskset -c 9-17 ./bin/$(NAME)"

remote: remote-copy remote-build remote-run

# ----------------------------------------------------------------------------
.PHONY: help
help:
	@echo "Targets:"
	@echo "  make            - release build (default)"
	@echo "  make debug=1    - debug build"
	@echo "  make run        - build and run locally"
	@echo "  make asm        - emit annotated x86 assembly"
	@echo "  make tsan       - build with ThreadSanitizer"
	@echo "  make clean      - clean build artifacts"
	@echo "  make remote     - copy, build, run on test machine"
