#include "stats.hpp"

#include <algorithm>

namespace memtracer {

StatsAggregator::StatsAggregator(uint64_t window_ns) : window_ns_(window_ns) {}

uint8_t StatsAggregator::bucket_of(uint64_t addr, uint64_t start, uint64_t end) {
    if (end <= start || addr < start || addr >= end) return 0;
    const uint64_t span = end - start;
    // Avoid 128-bit math by computing (addr-start)/(span/buckets) when possible.
    // span >= 1 so this is safe.
    const uint64_t off = addr - start;
    const uint64_t idx = (off * (uint64_t)kBucketsPerObject) / span;
    return (uint8_t)std::min<uint64_t>(idx, kBucketsPerObject - 1);
}

ObjectStats& StatsAggregator::touch(std::unordered_map<uint64_t, ObjectStats>& tbl,
                                    const MappedObject& obj) {
    auto it = tbl.find(obj.start);
    if (it == tbl.end()) {
        ObjectStats os;
        os.display = obj.display;
        os.start   = obj.start;
        os.end     = obj.end;
        auto [ins, _] = tbl.emplace(obj.start, std::move(os));
        return ins->second;
    }
    // Refresh end / display in case the region was extended or replaced in
    // a way that kept the same start.
    it->second.end     = obj.end;
    it->second.display = obj.display;
    return it->second;
}

void StatsAggregator::apply(ObjectStats& o, uint8_t bucket, MemAccessOp op, int64_t delta) {
    if (op == MemAccessOp::Load) {
        o.buckets[bucket].reads += (uint64_t)delta;
        o.total_reads            += (uint64_t)delta;
    } else {
        o.buckets[bucket].writes += (uint64_t)delta;
        o.total_writes            += (uint64_t)delta;
    }
}

void StatsAggregator::on_sample(const ParsedSample& s,
                                MemAccessOp         op,
                                const MappedObject* ip_obj,
                                const MappedObject* addr_obj) {
    ++total_samples_;

    uint64_t code_key = 0; uint8_t code_bucket = 0;
    if (ip_obj) {
        ObjectStats& o = touch(code_, *ip_obj);
        code_bucket = bucket_of(s.ip, ip_obj->start, ip_obj->end);
        apply(o, code_bucket, op, +1);
        code_key = ip_obj->start;
    }

    uint64_t data_key = 0; uint8_t data_bucket = 0;
    if (addr_obj && s.addr != 0) {
        ObjectStats& o = touch(data_, *addr_obj);
        data_bucket = bucket_of(s.addr, addr_obj->start, addr_obj->end);
        apply(o, data_bucket, op, +1);
        data_key = addr_obj->start;
    }

    if (window_ns_ > 0) {
        TimedSample t;
        t.ts          = s.time;
        t.op          = (op == MemAccessOp::Load) ? 0 : 1;
        t.code_key    = code_key;
        t.code_bucket = code_bucket;
        t.data_key    = data_key;
        t.data_bucket = data_bucket;
        window_.push_back(t);
    }
}

void StatsAggregator::on_mmap(const MappedObject& obj) {
    // Pre-create entries so the snapshot always lists known regions even if
    // no sample has hit them yet. Useful for the plotter to allocate a slot.
    touch(code_, obj);
    touch(data_, obj);
}

void StatsAggregator::roll_window(uint64_t now_ns) {
    if (window_ns_ == 0) return;
    const uint64_t cutoff = (now_ns > window_ns_) ? (now_ns - window_ns_) : 0;
    while (!window_.empty() && window_.front().ts < cutoff) {
        const TimedSample& t = window_.front();
        const auto op = (t.op == 0) ? MemAccessOp::Load : MemAccessOp::Store;

        if (t.code_key) {
            auto it = code_.find(t.code_key);
            if (it != code_.end()) apply(it->second, t.code_bucket, op, -1);
        }
        if (t.data_key) {
            auto it = data_.find(t.data_key);
            if (it != data_.end()) apply(it->second, t.data_bucket, op, -1);
        }
        window_.pop_front();
    }
}

std::vector<ObjectStats> StatsAggregator::code_snapshot() const {
    std::vector<ObjectStats> out;
    out.reserve(code_.size());
    for (const auto& [_, v] : code_) out.push_back(v);
    std::sort(out.begin(), out.end(),
              [](const ObjectStats& a, const ObjectStats& b) { return a.start < b.start; });
    return out;
}

std::vector<ObjectStats> StatsAggregator::data_snapshot() const {
    std::vector<ObjectStats> out;
    out.reserve(data_.size());
    for (const auto& [_, v] : data_) out.push_back(v);
    std::sort(out.begin(), out.end(),
              [](const ObjectStats& a, const ObjectStats& b) { return a.start < b.start; });
    return out;
}

}  // namespace memtracer
