#include "output.hpp"

#include <cinttypes>
#include <cstdio>

namespace memtracer {

namespace {

class TextSink final : public OutputSink {
public:
    void on_sample(const ParsedSample& s,
                   MemAccessOp         op,
                   const MappedObject* ip_obj,
                   const MappedObject* addr_obj) override {
        const char* ipname   = ip_obj   ? ip_obj->display.c_str()   : "<unknown>";
        const char* addrname = addr_obj ? addr_obj->display.c_str() : "<unknown>";
        std::fprintf(stderr,
            "[SAMPLE %s] tid=%u t=%" PRIu64 " ip=0x%" PRIx64 " (%s) "
            "addr=0x%" PRIx64 " (%s) period=%" PRIu64 "\n",
            op_to_str(op), s.tid, s.time, s.ip, ipname, s.addr, addrname, s.period);
    }

    void on_mmap(const MappedObject& obj) override {
        std::fprintf(stderr,
            "[MMAP ] %s  0x%" PRIx64 "-0x%" PRIx64 "  (%s)\n",
            obj.display.c_str(), obj.start, obj.end, obj.filename.c_str());
    }

    void on_snapshot(const StatsAggregator& stats) override {
        std::fprintf(stderr,
            "---- snapshot: total_samples=%" PRIu64 "  lost=%" PRIu64 " ----\n",
            stats.total_samples(), stats.lost_samples());

        auto code = stats.code_snapshot();
        std::fprintf(stderr, "  CODE (by IP-resolved object):\n");
        for (const auto& o : code) {
            if (o.total_reads + o.total_writes == 0) continue;
            std::fprintf(stderr, "    %-24s R=%" PRIu64 " W=%" PRIu64 "\n",
                         o.display.c_str(), o.total_reads, o.total_writes);
        }
        auto data = stats.data_snapshot();
        std::fprintf(stderr, "  DATA (by ADDR-resolved object):\n");
        for (const auto& o : data) {
            if (o.total_reads + o.total_writes == 0) continue;
            std::fprintf(stderr, "    %-24s R=%" PRIu64 " W=%" PRIu64 "\n",
                         o.display.c_str(), o.total_reads, o.total_writes);
        }
    }

    void on_lost(uint64_t n_lost) override {
        std::fprintf(stderr, "[LOST ] %" PRIu64 " samples dropped by kernel\n", n_lost);
    }

    void on_exit(int child_exit_code) override {
        std::fprintf(stderr, "[EXIT ] child returned %d\n", child_exit_code);
    }
};

}  // namespace

std::unique_ptr<OutputSink> make_text_sink() {
    return std::make_unique<TextSink>();
}

}  // namespace memtracer
