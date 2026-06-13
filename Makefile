debug ?= 1
NAME := spp

CXX := clang++
AR ?= ar
FORMATTER ?= clang-format

BUILD_DIR := build
OBJ_DIR := $(BUILD_DIR)/obj
LIB_DIR := $(BUILD_DIR)/lib
BIN_DIR := $(BUILD_DIR)/bin

INCLUDES := -Iinclude
APP_INCLUDES :=
STD_FLAGS := -std=c++20
WARN_FLAGS := -Wall -Wextra -Wpedantic
SAFETY_FLAGS := -fno-exceptions -fno-rtti
COMMON_FLAGS := $(STD_FLAGS) $(WARN_FLAGS) $(SAFETY_FLAGS) $(INCLUDES)

ifeq ($(debug),1)
OPT_FLAGS := -O0 -g
else
OPT_FLAGS := -O3 -DNDEBUG
endif

CXXFLAGS := $(COMMON_FLAGS) $(OPT_FLAGS)
LDFLAGS :=

SRCS := src/core/unify.cpp

ifeq ($(OS),Windows_NT)
  SRCS += src/platform/w32/unify.cpp
  PLATFORM_LIBS := -lws2_32 -lSynchronization
else
  SRCS += src/platform/pos/unify.cpp
  PLATFORM_LIBS := -lpthread
endif

# Opt-in TLS adapter (Linux-only, statically/dynamically linked against
# mbedTLS 3.x). Tests under tests/ext/ require this opt-in to compile; they
# stay out of the default `make test` so the core build remains
# dependency-free. Toggle via `SPP_TLS=1 make test`.
SPP_TLS ?= 0
ifeq ($(SPP_TLS),1)
  TLS_LIBS := -lmbedtls -lmbedx509 -lmbedcrypto
else
  TLS_LIBS :=
endif

OBJS := $(addprefix $(OBJ_DIR)/,$(SRCS:.cpp=.o))
LIB := $(LIB_DIR)/libspp.a
SMOKE_SRC := examples/smoke.cpp
SMOKE_BIN := $(BIN_DIR)/spp_smoke
ifeq ($(SPP_TLS),1)
  TEST_SRCS := $(shell find tests -type f -name '*.cpp' | sort)
else
  TEST_SRCS := $(shell find tests -type f -name '*.cpp' -not -path 'tests/ext/*' | sort)
endif
TEST_BINS := $(patsubst tests/%.cpp,$(BIN_DIR)/tests/%,$(TEST_SRCS))
TEST_RELS := $(patsubst tests/%.cpp,%,$(TEST_SRCS))

# App layer (Binance integration). Uses Memory_Stream-backed tests so the
# core suite stays runnable without TLS; the example CLI under app/examples/
# requires SPP_TLS=1 because it pulls in the Tls_Mbedtls_Stream-backed
# Binance::Client.
APP_INC := -Iapp/include
APP_TEST_SRCS := $(shell find app/tests -type f -name '*.cpp' 2>/dev/null | sort)
APP_TEST_BINS := $(patsubst app/tests/%.cpp,$(BIN_DIR)/app/tests/%,$(APP_TEST_SRCS))
APP_TEST_RELS := $(patsubst app/tests/%.cpp,%,$(APP_TEST_SRCS))

# Unity build dependency tracking:
# Rebuild unity translation units when any included source/header changes.
CORE_UNITY_DEPS := $(shell find src include -type f \( -name '*.cpp' -o -name '*.h' \) \
	-not -path 'src/platform/*' | sort)
POS_UNITY_DEPS := $(shell find src/platform/pos include -type f \( -name '*.cpp' -o -name '*.h' \) | sort)
W32_UNITY_DEPS := $(shell find src/platform/w32 include -type f \( -name '*.cpp' -o -name '*.h' \) | sort)

.PHONY: all lib smoke test app-test ticker-dump bench-check bench-check-async bench-check-containers bench-check-io format clean print-config

all: lib

lib: $(LIB)

smoke: $(SMOKE_BIN)
	$(SMOKE_BIN)

test: $(TEST_BINS) $(APP_TEST_BINS)
	@set -e; \
	root="$(CURDIR)"; \
	for rel in $(TEST_RELS); do \
		src="tests/$$rel.cpp"; \
		bin="$(BIN_DIR)/tests/$$rel"; \
		dir="$$(dirname "$$src")"; \
		echo "[test] $$rel"; \
		(cd "$$dir" && "$$root/$$bin"); \
	done; \
	for rel in $(APP_TEST_RELS); do \
		src="app/tests/$$rel.cpp"; \
		bin="$(BIN_DIR)/app/tests/$$rel"; \
		dir="$$(dirname "$$src")"; \
		echo "[app-test] $$rel"; \
		(cd "$$dir" && "$$root/$$bin"); \
	done

app-test: $(APP_TEST_BINS)
	@set -e; \
	root="$(CURDIR)"; \
	for rel in $(APP_TEST_RELS); do \
		src="app/tests/$$rel.cpp"; \
		bin="$(BIN_DIR)/app/tests/$$rel"; \
		dir="$$(dirname "$$src")"; \
		echo "[app-test] $$rel"; \
		(cd "$$dir" && "$$root/$$bin"); \
	done

ticker-dump: $(BIN_DIR)/app/examples/ticker_dump
	@echo "Binary at: $<"
	@echo "Run with: BINANCE_HOST=testnet.binance.vision $< BTCUSDT"

bench-check:
	./tools/bench_check.sh

bench-check-async:
	./tools/bench_check.sh --cases async_pool

bench-check-containers:
	./tools/bench_check.sh --cases concurrency_map_vec

bench-check-io:
	./tools/bench_check.sh --cases io_files,io_lock,io_net

$(LIB): $(OBJS)
	@mkdir -p $(LIB_DIR)
	$(AR) rcs $@ $^

$(OBJ_DIR)/%.o: %.cpp
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OBJ_DIR)/src/core/unify.o: $(CORE_UNITY_DEPS)
$(OBJ_DIR)/src/platform/pos/unify.o: $(POS_UNITY_DEPS)
$(OBJ_DIR)/src/platform/w32/unify.o: $(W32_UNITY_DEPS)

$(SMOKE_BIN): $(SMOKE_SRC) $(LIB)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(APP_INCLUDES) $< $(LIB) $(PLATFORM_LIBS) $(LDFLAGS) -o $@

$(BIN_DIR)/tests/%: tests/%.cpp $(LIB)
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(APP_INCLUDES) -Itests $< $(LIB) $(PLATFORM_LIBS) $(TLS_LIBS) $(LDFLAGS) -o $@

$(BIN_DIR)/app/tests/%: app/tests/%.cpp $(LIB)
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(APP_INC) $(APP_INCLUDES) -Itests $< $(LIB) $(PLATFORM_LIBS) $(TLS_LIBS) $(LDFLAGS) -o $@

$(BIN_DIR)/app/examples/%: app/examples/%.cpp $(LIB)
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(APP_INC) $(APP_INCLUDES) $< $(LIB) $(PLATFORM_LIBS) $(TLS_LIBS) $(LDFLAGS) -o $@

format:
	@files="$$(find . -type f \\( -name '*.h' -o -name '*.cpp' \\) -not -path './build/*')"; \
	if [ -n "$$files" ]; then \
		$(FORMATTER) -i $$files; \
	fi

print-config:
	@echo "CXX=$(CXX)"
	@echo "CXXFLAGS=$(CXXFLAGS)"
	@echo "debug=$(debug)"
	@echo "SRCS=$(SRCS)"

clean:
	rm -rf $(BUILD_DIR)
