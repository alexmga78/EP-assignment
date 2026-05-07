// JSONL output sink: one self-contained JSON object per line on stdout.
//
// Record kinds:
//   {"type":"mmap",     "start":..., "end":..., "prot":..., "display":"...", "filename":"..."}
//   {"type":"sample",   "ts":..., "op":"R"|"W", "ip":..., "addr":..., "period":...,
//                       "ip_obj":"...", "ip_obj_start":..., "ip_obj_end":...,
//                       "addr_obj":"...", "addr_obj_start":..., "addr_obj_end":...}
//   {"type":"lost",     "n":...}
//   {"type":"snapshot", "total_samples":..., "lost":..., "window_ns":...,
//                       "code":[{"display":"...","start":...,"end":...,"buckets":[[r,w], ...]} ...],
//                       "data":[ ... same shape ... ]}
//   {"type":"exit",     "code":...}
//
// scripts/live_plot.py reads this stream from stdin and renders.

#include "output.hpp"

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <string>

namespace memtracer {

namespace {

// Append a JSON-escaped copy of s to out. Handles control characters and
// the four characters that need escaping per RFC 8259.
void append_json_string(std::string& out, const std::string& s) {
    out.push_back('"');
    for (char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b";  break;
        case '\f': out += "\\f";  break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:
            if ((unsigned char)c < 0x20) {
                char buf[8]; std::snprintf(buf, sizeof(buf), "\\u%04x", (unsigned)(unsigned char)c);
                out += buf;
            } else {
                out.push_back(c);
            }
        }
    }
    out.push_back('"');
}

void append_uint(std::string& out, uint64_t v) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%" PRIu64, v);
    out += buf;
}

class JsonlSink final : public OutputSink {
public:
    void on_sample(const ParsedSample& s,
                   MemAccessOp         op,
                   const MappedObject* ip_obj,
                   const MappedObject* addr_obj) override {
        std::string line; line.reserve(256);
        line += R"({"type":"sample","ts":)";   append_uint(line, s.time);
        line += R"(,"op":")";                  line += op_to_str(op);
        line += R"(","ip":)";                  append_uint(line, s.ip);
        line += R"(,"addr":)";                 append_uint(line, s.addr);
        line += R"(,"period":)";               append_uint(line, s.period);
        if (ip_obj) {
            line += R"(,"ip_obj":)";           append_json_string(line, ip_obj->display);
            line += R"(,"ip_obj_start":)";     append_uint(line, ip_obj->start);
            line += R"(,"ip_obj_end":)";       append_uint(line, ip_obj->end);
        }
        if (addr_obj) {
            line += R"(,"addr_obj":)";         append_json_string(line, addr_obj->display);
            line += R"(,"addr_obj_start":)";   append_uint(line, addr_obj->start);
            line += R"(,"addr_obj_end":)";     append_uint(line, addr_obj->end);
        }
        line += "}\n";
        std::fwrite(line.data(), 1, line.size(), stdout);
    }

    void on_mmap(const MappedObject& obj) override {
        std::string line; line.reserve(128);
        line += R"({"type":"mmap","start":)";  append_uint(line, obj.start);
        line += R"(,"end":)";                  append_uint(line, obj.end);
        line += R"(,"prot":)";                 append_uint(line, obj.prot);
        line += R"(,"display":)";              append_json_string(line, obj.display);
        line += R"(,"filename":)";             append_json_string(line, obj.filename);
        line += "}\n";
        std::fwrite(line.data(), 1, line.size(), stdout);
    }

    void on_snapshot(const StatsAggregator& stats) override {
        std::string line; line.reserve(2048);
        line += R"({"type":"snapshot","total_samples":)";
        append_uint(line, stats.total_samples());
        line += R"(,"lost":)";                 append_uint(line, stats.lost_samples());
        line += R"(,"window_ns":)";            append_uint(line, stats.window_ns());

        auto emit_objects = [&](const char* key, const std::vector<ObjectStats>& v) {
            line += R"(,")"; line += key; line += R"(":[)";
            bool first_obj = true;
            for (const auto& o : v) {
                if (o.total_reads + o.total_writes == 0) continue;
                if (!first_obj) line.push_back(',');
                first_obj = false;
                line += R"({"display":)";       append_json_string(line, o.display);
                line += R"(,"start":)";         append_uint(line, o.start);
                line += R"(,"end":)";           append_uint(line, o.end);
                line += R"(,"reads":)";         append_uint(line, o.total_reads);
                line += R"(,"writes":)";        append_uint(line, o.total_writes);
                line += R"(,"buckets":[)";
                for (size_t i = 0; i < o.buckets.size(); ++i) {
                    if (i) line.push_back(',');
                    line.push_back('[');
                    append_uint(line, o.buckets[i].reads);
                    line.push_back(',');
                    append_uint(line, o.buckets[i].writes);
                    line.push_back(']');
                }
                line += "]}";
            }
            line.push_back(']');
        };
        emit_objects("code", stats.code_snapshot());
        emit_objects("data", stats.data_snapshot());

        line += "}\n";
        std::fwrite(line.data(), 1, line.size(), stdout);
        std::fflush(stdout);
    }

    void on_lost(uint64_t n_lost) override {
        std::string line = R"({"type":"lost","n":)";
        append_uint(line, n_lost);
        line += "}\n";
        std::fwrite(line.data(), 1, line.size(), stdout);
    }

    void on_exit(int child_exit_code) override {
        std::string line = R"({"type":"exit","code":)";
        append_uint(line, (uint64_t)(int64_t)child_exit_code);
        line += "}\n";
        std::fwrite(line.data(), 1, line.size(), stdout);
        std::fflush(stdout);
    }
};

}  // namespace

// Forward declaration; defined in output_text.cpp.
std::unique_ptr<OutputSink> make_text_sink();

std::unique_ptr<OutputSink> make_sink(const Options& opts) {
    switch (opts.sink) {
    case SinkKind::Text:  return make_text_sink();
    case SinkKind::Jsonl: return std::make_unique<JsonlSink>();
    }
    return std::make_unique<JsonlSink>();
}

}  // namespace memtracer
