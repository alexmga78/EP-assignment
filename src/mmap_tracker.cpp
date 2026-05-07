#include "mmap_tracker.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <sys/mman.h>

namespace memtracer {

std::string make_display_name(const std::string& filename) {
    if (filename.empty())                return "anon";
    if (filename == "[heap]")            return "heap";
    if (filename == "[stack]")           return "stack";
    if (filename == "[vdso]")            return "vdso";
    if (filename == "[vvar]")            return "vvar";
    if (filename == "[vsyscall]")        return "vsyscall";
    if (filename.rfind("[stack:", 0) == 0) return "stack";  // [stack:tid]
    if (filename.front() == '[')         return filename.substr(1, filename.size() - 2);

    // Path → basename.
    auto slash = filename.find_last_of('/');
    if (slash == std::string::npos) return filename;
    return filename.substr(slash + 1);
}

bool parse_proc_maps_line(const std::string& line, MappedObject& out) {
    // Format: start-end perms offset dev inode  pathname
    // Example: 7f3a40000000-7f3a40021000 r-xp 00000000 08:01 1234567  /usr/lib/x86_64-linux-gnu/libc.so.6
    uint64_t start = 0, end = 0, off = 0;
    char perms[8] = {0};
    unsigned int dev_maj = 0, dev_min = 0;
    unsigned long inode = 0;
    int n = 0;
    int matched = std::sscanf(line.c_str(), "%lx-%lx %7s %lx %x:%x %lu %n",
                              &start, &end, perms, &off, &dev_maj, &dev_min, &inode, &n);
    if (matched < 7) return false;

    std::string fname;
    if (n > 0 && (size_t)n < line.size()) {
        // skip leading whitespace
        size_t i = (size_t)n;
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
        if (i < line.size()) fname = line.substr(i);
        // strip trailing newline / spaces
        while (!fname.empty() && (fname.back() == '\n' || fname.back() == '\r' ||
                                  fname.back() == ' '  || fname.back() == '\t')) {
            fname.pop_back();
        }
    }

    out.start    = start;
    out.end      = end;
    out.prot     = 0;
    if (perms[0] == 'r') out.prot |= PROT_READ;
    if (perms[1] == 'w') out.prot |= PROT_WRITE;
    if (perms[2] == 'x') out.prot |= PROT_EXEC;
    out.filename = std::move(fname);
    out.display  = make_display_name(out.filename);
    return true;
}

void MemoryMap::load_from_proc(pid_t pid) {
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/%d/maps", (int)pid);
    std::ifstream in(path);
    if (!in) {
        // The child may have already exited or maps may not be readable. Not fatal —
        // perf MMAP2 events will still populate as new mappings appear.
        std::fprintf(stderr, "memtracer: warn: could not open %s\n", path);
        return;
    }
    std::string line;
    while (std::getline(in, line)) {
        MappedObject m;
        if (parse_proc_maps_line(line, m)) {
            insert_or_replace(std::move(m));
        }
    }
}

void MemoryMap::handle_mmap2(const ParsedMmap2& m) {
    MappedObject obj;
    obj.start    = m.addr;
    obj.end      = m.addr + m.len;
    obj.prot     = m.prot;
    obj.filename.assign(m.filename.data(), m.filename.size());
    obj.display  = make_display_name(obj.filename);
    insert_or_replace(std::move(obj));
}

const MappedObject* MemoryMap::lookup(uint64_t addr) const {
    if (regions_.empty()) return nullptr;
    // Find the first region whose start > addr; the candidate is the one before.
    auto it = std::upper_bound(regions_.begin(), regions_.end(), addr,
        [](uint64_t a, const MappedObject& r) { return a < r.start; });
    if (it == regions_.begin()) return nullptr;
    --it;
    if (addr >= it->start && addr < it->end) return &*it;
    return nullptr;
}

void MemoryMap::for_each(std::function<void(const MappedObject&)> f) const {
    for (const auto& r : regions_) f(r);
}

void MemoryMap::insert_or_replace(MappedObject obj) {
    if (obj.start >= obj.end) return;

    // Strategy: drop / shrink any existing region whose interval intersects
    // [obj.start, obj.end), then insert in sorted position.
    std::vector<MappedObject> kept;
    kept.reserve(regions_.size() + 1);

    for (auto& r : regions_) {
        if (r.end <= obj.start || r.start >= obj.end) {
            // Disjoint — keep as-is.
            kept.push_back(std::move(r));
            continue;
        }
        // Overlap — split into the parts that are not covered by obj.
        if (r.start < obj.start) {
            MappedObject left = r;
            left.end = obj.start;
            kept.push_back(std::move(left));
        }
        if (r.end > obj.end) {
            MappedObject right = r;
            right.start = obj.end;
            kept.push_back(std::move(right));
        }
        // The intersection of r with obj is dropped (obj wins).
    }
    kept.push_back(std::move(obj));

    std::sort(kept.begin(), kept.end(),
              [](const MappedObject& a, const MappedObject& b) { return a.start < b.start; });
    regions_ = std::move(kept);
}

}  // namespace memtracer
