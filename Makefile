CXX      := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -O2 -g
LDFLAGS  :=

SRC_DIR  := src
BLD_DIR  := build
BIN      := my_tracer

SRCS := $(wildcard $(SRC_DIR)/*.cpp)
OBJS := $(patsubst $(SRC_DIR)/%.cpp, $(BLD_DIR)/%.o, $(SRCS))

BENCH_SRCS := $(wildcard benchmarks/*.cpp)
BENCH_BINS := $(patsubst benchmarks/%.cpp, benchmarks/%, $(BENCH_SRCS))

.PHONY: all clean benchmarks

all: $(BIN)

$(BIN): $(OBJS)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

$(BLD_DIR)/%.o: $(SRC_DIR)/%.cpp | $(BLD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BLD_DIR):
	mkdir -p $(BLD_DIR)

benchmarks: $(BENCH_BINS)

benchmarks/%: benchmarks/%.cpp
	$(CXX) $(CXXFLAGS) $< -o $@

clean:
	rm -rf $(BLD_DIR) $(BIN) $(BENCH_BINS)
