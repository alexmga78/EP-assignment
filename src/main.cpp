// memtracer entry point.
//
// Usage:
//   memtracer [--sink=text|jsonl] [--period=N] [--snapshot-ms=N]
//             [--window=N] [--mmap-pages=N] [--no-mmap-data]
//             -- CHILD_CMD [CHILD ARGS...]
//
// Everything after the literal "--" is the program-under-test invocation
// passed verbatim to execvp() in the child process.

#include "options.hpp"
#include "tracer.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

using namespace memtracer;

namespace {

void print_usage(const char* prog) {
    std::fprintf(stderr,
        "Usage: %s [tracer-flags] -- CHILD_CMD [CHILD_ARGS...]\n"
        "\n"
        "Tracer flags:\n"
        "  --sink={text,jsonl}    output sink (default: jsonl)\n"
        "  --period=N             sample every N retired mem ops (default: 10000)\n"
        "  --snapshot-ms=N        snapshot cadence in ms (default: 250)\n"
        "  --window=N             rolling window in ms; 0 = lifetime (default: 0)\n"
        "  --mmap-pages=N         perf ring data pages, must be power of 2 (default: 128)\n"
        "  --no-mmap-data         do not request mmaps for non-executable regions\n"
        "  --help                 this message\n",
        prog);
}

bool parse_uint(std::string_view s, uint64_t& out) {
    if (s.empty()) return false;
    out = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        out = out * 10 + (uint64_t)(c - '0');
    }
    return true;
}

bool starts_with(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

// Returns true on success; false on parse error (with message printed).
bool parse_args(int argc, char** argv, Options& opts) {
    int i = 1;
    bool seen_dashdash = false;

    for (; i < argc; ++i) {
        std::string_view a = argv[i];
        if (a == "--") { seen_dashdash = true; ++i; break; }
        if (a == "--help" || a == "-h") { print_usage(argv[0]); std::exit(0); }

        if (a == "--no-mmap-data") {
            opts.mmap_data = false;
        } else if (starts_with(a, "--sink=")) {
            std::string_view v = a.substr(std::string_view("--sink=").size());
            if (v == "text") opts.sink = SinkKind::Text;
            else if (v == "jsonl") opts.sink = SinkKind::Jsonl;
            else { std::fprintf(stderr, "unknown sink: %.*s\n", (int)v.size(), v.data()); return false; }
        } else if (starts_with(a, "--period=")) {
            uint64_t v; if (!parse_uint(a.substr(9), v) || v == 0) {
                std::fprintf(stderr, "bad --period\n"); return false;
            }
            opts.sample_period = v;
        } else if (starts_with(a, "--snapshot-ms=")) {
            uint64_t v; if (!parse_uint(a.substr(14), v)) {
                std::fprintf(stderr, "bad --snapshot-ms\n"); return false;
            }
            opts.snapshot_ms = (uint32_t)v;
        } else if (starts_with(a, "--window=")) {
            uint64_t v; if (!parse_uint(a.substr(9), v)) {
                std::fprintf(stderr, "bad --window\n"); return false;
            }
            opts.window_ms = (uint32_t)v;
        } else if (starts_with(a, "--mmap-pages=")) {
            uint64_t v; if (!parse_uint(a.substr(13), v) || v == 0 || (v & (v - 1)) != 0) {
                std::fprintf(stderr, "--mmap-pages must be a power of two\n"); return false;
            }
            // store log2
            uint32_t lg = 0;
            while ((1u << lg) < v) ++lg;
            opts.mmap_pages_log2 = lg;
        } else {
            std::fprintf(stderr, "unknown flag: %.*s (use --help)\n", (int)a.size(), a.data());
            return false;
        }
    }

    if (!seen_dashdash) {
        std::fprintf(stderr, "missing '--' before child command\n");
        return false;
    }

    for (; i < argc; ++i) opts.child_argv.emplace_back(argv[i]);

    if (opts.child_argv.empty()) {
        std::fprintf(stderr, "no child command given after '--'\n");
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    Options opts;
    if (!parse_args(argc, argv, opts)) {
        print_usage(argv[0]);
        return 2;
    }

    try {
        Tracer tracer(std::move(opts));
        return tracer.run();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "memtracer: fatal: %s\n", e.what());
        return 1;
    }
}
