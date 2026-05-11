#include "ipc.h"
#include "stats.h"

#include <algorithm>
#include <cstring>

// Names emitted here come from /proc/<pid>/maps and from PERF_RECORD_MMAP2
// filenames, both of which are filesystem paths plus a few bracketed
// pseudo-paths ("[heap]"). Only the characters that can occur in such
// names need escaping; a fully general JSON escaper is unnecessary.
static std::string json_escape(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 4);
    for (char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:   out += c;      break;
        }
    }
    return out;
}

Ipc::Ipc(FILE* pipe_file, bool is_pipe) : file_(pipe_file), is_pipe_(is_pipe) {}

Ipc::~Ipc()
{
    if (file_) {
        fflush(file_);
        if (is_pipe_) pclose(file_);
        else          fclose(file_);
        file_ = nullptr;
    }
}

std::string Ipc::region_name(const Region* r)
{
    if (!r) return "UNKNOWN";
    if (!r->name.empty()) return r->name;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "ANON@0x%lx", r->start);
    return buf;
}

void Ipc::send_sample(uint64_t ip, uint64_t addr, bool is_write, uint64_t ts_ns,
                      const Region* ip_region, const Region* addr_region)
{
    if (!file_) return;

    // The bucket index sent on the wire must match the one computed in
    // Stats::record() (src/stats.cpp), otherwise the live plotter heatmap
    // and the final summary table disagree. Any change here needs the
    // mirror change there.
    int bucket = 0;
    if (addr_region) {
        uint64_t span = addr_region->end - addr_region->start;
        if (span > 0 && addr >= addr_region->start) {
            uint64_t offset = addr - addr_region->start;
            bucket = static_cast<int>(
                std::min<uint64_t>(
                    static_cast<uint64_t>(NUM_BUCKETS - 1),
                    offset * NUM_BUCKETS / span));
        }
    }

    const char* type_str = "UNKNOWN";
    if (addr_region) {
        switch (addr_region->type) {
        case RegionType::FILE_TEXT: type_str = "text";  break;
        case RegionType::FILE_DATA: type_str = "data";  break;
        case RegionType::HEAP:      type_str = "heap";  break;
        case RegionType::STACK:     type_str = "stack"; break;
        case RegionType::VDSO:      type_str = "vdso";  break;
        case RegionType::ANON:      type_str = "anon";  break;
        default:                    type_str = "unk";   break;
        }
    }

    std::fprintf(file_,
        "{\"ts_ns\":%lu,\"ip\":\"0x%lx\",\"addr\":\"0x%lx\","
        "\"rw\":\"%s\",\"ip_obj\":\"%s\","
        "\"region\":\"%s\",\"region_type\":\"%s\","
        "\"bucket\":%d}\n",
        ts_ns, ip, addr,
        is_write ? "w" : "r",
        json_escape(region_name(ip_region)).c_str(),
        json_escape(region_name(addr_region)).c_str(),
        type_str,
        bucket);
}

void Ipc::flush()
{
    if (file_) fflush(file_);
}
