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

OBJS := $(addprefix $(OBJ_DIR)/,$(SRCS:.cpp=.o))
LIB := $(LIB_DIR)/libspp.a
SMOKE_SRC := examples/smoke.cpp
SMOKE_BIN := $(BIN_DIR)/spp_smoke
TEST_SRCS := $(wildcard tests/*.cpp)
TEST_BINS := $(patsubst tests/%.cpp,$(BIN_DIR)/test_%,$(TEST_SRCS))
TEST_NAMES := $(patsubst tests/%.cpp,%,$(TEST_SRCS))

.PHONY: all lib smoke test format clean print-config

all: lib

lib: $(LIB)

smoke: $(SMOKE_BIN)
	$(SMOKE_BIN)

test: $(TEST_BINS)
	@set -e; \
	for name in $(TEST_NAMES); do \
		echo "[test] $$name"; \
		(cd tests && ../$(BIN_DIR)/test_$$name); \
	done

$(LIB): $(OBJS)
	@mkdir -p $(LIB_DIR)
	$(AR) rcs $@ $^

$(OBJ_DIR)/%.o: %.cpp
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(SMOKE_BIN): $(SMOKE_SRC) $(LIB)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(APP_INCLUDES) $< $(LIB) $(PLATFORM_LIBS) $(LDFLAGS) -o $@

$(BIN_DIR)/test_%: tests/%.cpp $(LIB)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(APP_INCLUDES) -Itests $< $(LIB) $(PLATFORM_LIBS) $(LDFLAGS) -o $@

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
