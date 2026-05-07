#pragma once

#include "records.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <sys/types.h>
#include <vector>

namespace memtracer {

// One mapped region in the target process's address space.
//   filename — original path / pseudo-name as reported by /proc or perf.
//              Examples: "/usr/lib/x86_64-linux-gnu/libc.so.6", "[heap]",
//              "[stack]", "[anon]", "[vdso]", "/path/to/program".
//   display  — short, plot-friendly label derived from filename. The same
//              display string is shared by all regions backed by the same
//              file (e.g. libc's code and data segments both display as
//              "libc.so.6"), so the plotter can group on it if it likes.
struct MappedObject {
    uint64_t    start    = 0;
    uint64_t    end      = 0;     // half-open: [start, end)
    uint32_t    prot     = 0;     // PROT_READ | PROT_WRITE | PROT_EXEC
    std::string filename;
    std::string display;
};

class MemoryMap {
public:
    // Parse /proc/<pid>/maps and seed the table. Call once, immediately after
    // the child execs — perf_event_open does not synthesize MMAP records for
    // mappings created before it was attached.
    void load_from_proc(pid_t pid);

    // Apply a PERF_RECORD_MMAP2 event (incremental update).
    void handle_mmap2(const ParsedMmap2& m);

    // O(log n) interval lookup. Returns nullptr if addr is unmapped.
    const MappedObject* lookup(uint64_t addr) const;

    // Iterate (in address order) over the current mappings.
    void for_each(std::function<void(const MappedObject&)> f) const;

    size_t size() const { return regions_.size(); }

    // Exposed for the JSONL sink: emit a one-time dump of every known region.
    const std::vector<MappedObject>& regions() const { return regions_; }

private:
    // Insert obj at the right sorted position. If the new region overlaps any
    // existing regions, the existing ones are split or evicted as necessary
    // — mmap()/munmap() can repurpose ranges arbitrarily.
    void insert_or_replace(MappedObject obj);

    // Sorted by .start, non-overlapping, half-open intervals.
    std::vector<MappedObject> regions_;
};

// Helpers — exposed for unit tests.

// Given a /proc/<pid>/maps line, populate a MappedObject. Returns false on parse error.
bool parse_proc_maps_line(const std::string& line, MappedObject& out);

// Build a plot-friendly display name from a raw path / pseudo-name.
//   "/usr/lib/.../libc.so.6"  -> "libc.so.6"
//   "[heap]" / "[stack]"      -> "heap"   / "stack"
//   ""        (anonymous)     -> "anon"
//   filename starting with "/memfd:..." -> "memfd:..."
std::string make_display_name(const std::string& filename);

}  // namespace memtracer
