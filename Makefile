## EP Memory Access Tracer
## Build:    make            (release-ish, -O2 -g)
## Debug:    make debug      (-O0 -g3 -DDEBUG_LOG=1)
## Bench:    make bench
## Clean:    make clean

CXX      ?= g++
CC       ?= gcc

CXXSTD   := -std=c++17
WARN     := -Wall -Wextra -Wpedantic -Wno-unused-parameter
INCLUDES := -Isrc

ifeq ($(MAKECMDGOALS),debug)
  OPT := -O0 -g3 -fno-omit-frame-pointer -DDEBUG_LOG=1
else
  OPT := -O2 -g
endif

CXXFLAGS := $(CXXSTD) $(WARN) $(OPT) $(INCLUDES)
LDFLAGS  := -pthread

SRC_DIR   := src
BIN_DIR   := bin
BENCH_DIR := benchmarks

SOURCES := \
  $(SRC_DIR)/main.cpp \
  $(SRC_DIR)/tracer.cpp \
  $(SRC_DIR)/child_process.cpp \
  $(SRC_DIR)/perf_event.cpp \
  $(SRC_DIR)/ring_buffer.cpp \
  $(SRC_DIR)/records.cpp \
  $(SRC_DIR)/mmap_tracker.cpp \
  $(SRC_DIR)/stats.cpp \
  $(SRC_DIR)/output_text.cpp \
  $(SRC_DIR)/output_jsonl.cpp \
  $(SRC_DIR)/signal_handler.cpp

OBJECTS := $(SOURCES:.cpp=.o)
TARGET  := $(BIN_DIR)/memtracer

BENCH_SRCS := $(wildcard $(BENCH_DIR)/*.c)
BENCH_BINS := $(BENCH_SRCS:.c=.bin)

.PHONY: all debug bench clean

all: $(TARGET)

debug: $(TARGET)

$(TARGET): $(OBJECTS) | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

bench: $(BENCH_BINS)

$(BENCH_DIR)/%.bin: $(BENCH_DIR)/%.c
	$(CC) -O2 -g -Wall $< -o $@

clean:
	rm -f $(OBJECTS) $(TARGET) $(BENCH_BINS)
	rmdir $(BIN_DIR) 2>/dev/null || true
