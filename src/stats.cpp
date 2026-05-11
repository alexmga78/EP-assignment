#include "stats.h"

#include <algorithm>

static std::string region_key(const Region* r)
{
    if (!r) return "UNKNOWN";
    if (r->name.empty()) return "ANON@" + std::to_string(r->start);
    return r->name + "@" + std::to_string(r->start);
}

RegionStats& Stats::get_or_create_region(const Region& r)
{
    std::string key = region_key(&r);
    auto it = region_stats_.find(key);
    if (it != region_stats_.end()) return it->second;

    RegionStats rs{};
    rs.start = r.start;
    rs.end   = r.end;
    rs.name  = r.name.empty() ? ("ANON@" + std::to_string(r.start)) : r.name;
    rs.type  = r.type;
    region_stats_[key] = rs;
    return region_stats_[key];
}

void Stats::ensure_region(const Region& r)
{
    get_or_create_region(r);
}

void Stats::record(uint64_t /*ip*/, uint64_t addr, bool is_write,
                   uint64_t /*ts_ns*/,
                   const Region* ip_region, const Region* addr_region)
{
    std::string code_key = ip_region
        ? (ip_region->name.empty()
               ? ("ANON@" + std::to_string(ip_region->start))
               : ip_region->name)
        : "UNKNOWN";

    auto& cs = code_stats_[code_key];
    if (is_write) ++cs.writes;
    else          ++cs.reads;

    if (addr_region) {
        RegionStats& rs = get_or_create_region(*addr_region);

        uint64_t span = rs.end - rs.start;
        int bucket = 0;
        if (span > 0 && addr >= rs.start) {
            // The sample's address can land past the recorded region end
            // when a later mmap shrank the region between the sample being
            // taken and being consumed. Clamp instead of dropping - bucket
            // counts are approximate anyway.
            uint64_t offset = addr - rs.start;
            bucket = static_cast<int>(
                std::min<uint64_t>(
                    static_cast<uint64_t>(NUM_BUCKETS - 1),
                    offset * NUM_BUCKETS / span));
        }

        if (is_write) ++rs.buckets[bucket].writes;
        else          ++rs.buckets[bucket].reads;
    }
}
