#include "maps.h"

#include <fstream>
#include <sstream>
#include <stdexcept>

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static RegionType classify(const std::string& name, bool is_exec,
                           const std::string& /*perms*/)
{
    if (name == "[heap]")  return RegionType::HEAP;
    if (name == "[stack]") return RegionType::STACK;
    if (name == "[vdso]" || name == "[vvar]") return RegionType::VDSO;
    if (name.empty())      return RegionType::ANON;
    // Kernel uses "//anon" (basename → "anon") for anonymous MAP_PRIVATE pages.
    if (name == "anon")     return RegionType::ANON;

    // A named file: executable permission → text segment, else data segment.
    return is_exec ? RegionType::FILE_TEXT : RegionType::FILE_DATA;
}

// ---------------------------------------------------------------------------
// Maps implementation
// ---------------------------------------------------------------------------

void Maps::seed(pid_t pid)
{
    std::string path = "/proc/" + std::to_string(pid) + "/maps";
    std::ifstream f(path);
    if (!f) {
        throw std::runtime_error("Cannot open " + path);
    }

    std::string line;
    while (std::getline(f, line)) {
        // Format: start-end perms offset dev inode [pathname]
        uint64_t    start, end;
        char        perms_buf[8]{};
        uint64_t    offset;
        unsigned    devmaj, devmin;
        uint64_t    inode;
        char        name_buf[512]{};

        int n = std::sscanf(line.c_str(),
            "%lx-%lx %7s %lx %x:%x %lu %511[^\n]",
            &start, &end, perms_buf, &offset, &devmaj, &devmin, &inode,
            name_buf);

        std::string name;
        if (n >= 8) {
            // Strip leading whitespace from the path field.
            name = name_buf;
            size_t pos = name.find_first_not_of(" \t");
            if (pos != std::string::npos) name = name.substr(pos);
            else name.clear();

            // Keep only the basename for display.
            size_t slash = name.rfind('/');
            if (slash != std::string::npos && name[0] == '/') {
                name = name.substr(slash + 1);
            }
        }

        std::string perms(perms_buf);
        bool is_exec = perms.size() > 2 && perms[2] == 'x';

        Region r;
        r.start = start;
        r.end   = end;
        r.name  = name;
        r.type  = classify(name, is_exec, perms);
        regions_[start] = r;
    }
}

void Maps::insert(uint64_t start, uint64_t len, const std::string& filename,
                  bool is_exec)
{
    // Remove any existing regions that overlap [start, start+len).
    uint64_t end = start + len;
    auto it = regions_.lower_bound(start);
    // Also check the region just before `start` — it might extend into our range.
    if (it != regions_.begin()) {
        auto prev = std::prev(it);
        if (prev->second.end > start) {
            it = prev;
        }
    }
    while (it != regions_.end() && it->second.start < end) {
        it = regions_.erase(it);
    }

    // Build the display name from filename.
    std::string name = filename;
    if (!name.empty()) {
        size_t slash = name.rfind('/');
        if (slash != std::string::npos) name = name.substr(slash + 1);
    }

    Region r;
    r.start = start;
    r.end   = end;
    r.name  = name;
    r.type  = classify(name, is_exec, "");
    regions_[start] = r;
}

const Region* Maps::lookup(uint64_t addr) const
{
    if (regions_.empty()) return nullptr;

    // Find the first region whose start is strictly greater than addr.
    auto it = regions_.upper_bound(addr);
    if (it == regions_.begin()) return nullptr;
    --it;

    const Region& r = it->second;
    if (addr >= r.start && addr < r.end) return &r;
    return nullptr;
}
