#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace memtracer {

enum class SinkKind { Text, Jsonl };

struct Options {
    // Sample one event every `period` retired memory ops (per event type).
    // Lower = more samples = more overhead. 10k is a sane default for micro-benchmarks.
    uint64_t sample_period = 10000;

    // How often to emit a histogram snapshot to the sink (milliseconds).
    uint32_t snapshot_ms = 250;

    // Rolling window length in milliseconds; 0 = lifetime stats.
    // The bonus task is implemented when window > 0.
    uint32_t window_ms = 0;

    // log2 of the perf ring buffer's data area size, in pages.
    // 7 -> 128 data pages. Must be a power of two; the kernel mmap layout
    // is (1 + 2^N) * pagesize bytes.
    uint32_t mmap_pages_log2 = 7;

    // If true, ask perf to also report mmap()s for non-executable mappings
    // (so we get [heap] grow events, mmap'd file data segments, etc.).
    bool mmap_data = true;

    // Output sink to instantiate.
    SinkKind sink = SinkKind::Jsonl;

    // The child invocation (everything after the "--" on argv).
    std::vector<std::string> child_argv;
};

}  // namespace memtracer
