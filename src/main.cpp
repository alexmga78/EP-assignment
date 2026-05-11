#include "maps.h"
#include "stats.h"
#include "ipc.h"
#include "perf.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include <fstream>
#include <sstream>

#include <sched.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile sig_atomic_t g_child_exited = 0;

static void sigchld_handler(int) { g_child_exited = 1; }

// MEM_INST_RETIRED.* is a P-core-only event on hybrid Intel CPUs (Alder
// Lake and later): the cpu_core PMU is published under
// /sys/bus/event_source/devices/cpu_core/, and a sample taken on an
// E-core (cpu_atom) simply doesn't fire. Pin the tracee to the cpu_core
// cpumask so every sample lands on a CPU that can produce it. On
// non-hybrid systems the sysfs file is absent and pinning is skipped.
static void pin_to_pcores(pid_t pid)
{
    std::ifstream f("/sys/bus/event_source/devices/cpu_core/cpus");
    if (!f) return;

    std::string line;
    if (!std::getline(f, line)) return;

    cpu_set_t set;
    CPU_ZERO(&set);

    // cpumask format from sysfs: comma-separated list of single CPUs and
    // inclusive ranges, e.g. "0-7,16,20-23".
    std::istringstream ss(line);
    std::string token;
    while (std::getline(ss, token, ',')) {
        std::size_t dash = token.find('-');
        if (dash == std::string::npos) {
            int cpu = std::stoi(token);
            CPU_SET(cpu, &set);
        } else {
            int lo = std::stoi(token.substr(0, dash));
            int hi = std::stoi(token.substr(dash + 1));
            for (int c = lo; c <= hi; ++c)
                CPU_SET(c, &set);
        }
    }

    if (sched_setaffinity(pid, sizeof(set), &set) < 0)
        std::perror("sched_setaffinity (pin to P-cores)");
}

struct Config
{
    // ring_pages controls the mmap size of each perf ring buffer; the
    // kernel requires it to be a power of two and rejects anything else
    // with EINVAL.
    int ring_pages = 512;
    bool no_plot = false;
    std::vector<const char *> child_argv;
};

static void usage(const char *prog)
{
    std::fprintf(stderr,
                 "Usage: %s [--ring-pages N] [--no-plot] -- <program> [args...]\n"
                 "       %s [--ring-pages N] [--no-plot] <program> [args...]\n",
                 prog, prog);
}

static Config parse_args(int argc, char **argv)
{
    Config cfg;
    int i = 1;

    for (; i < argc; ++i)
    {
        std::string arg(argv[i]);
        if (arg == "--")
        {
            ++i;
            break;
        }
        if (arg == "--no-plot")
        {
            cfg.no_plot = true;
            continue;
        }
        if (arg == "--ring-pages" && i + 1 < argc)
        {
            cfg.ring_pages = std::atoi(argv[++i]);
            continue;
        }
        // First non-flag token: the rest of argv belongs to the child.
        break;
    }

    if (i >= argc)
    {
        usage(argv[0]);
        std::exit(EXIT_FAILURE);
    }

    for (; i < argc; ++i)
        cfg.child_argv.push_back(argv[i]);
    cfg.child_argv.push_back(nullptr);

    return cfg;
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    Config cfg = parse_args(argc, argv);

    pid_t child = fork();
    if (child < 0)
    {
        std::perror("fork");
        return EXIT_FAILURE;
    }

    if (child == 0)
    {
        // SIGSTOP / SIGCONT handshake: the parent needs our pid to call
        // perf_event_open() before exec() runs, otherwise the loads
        // performed by ld.so during dynamic linking - which dominate
        // startup - would never be observed.
        raise(SIGSTOP);
        execvp(cfg.child_argv[0],
               const_cast<char *const *>(cfg.child_argv.data()));
        std::perror("execvp");
        _exit(EXIT_FAILURE);
    }

    int wstatus = 0;
    if (waitpid(child, &wstatus, WUNTRACED) < 0)
    {
        std::perror("waitpid");
        kill(child, SIGKILL);
        return EXIT_FAILURE;
    }
    if (!WIFSTOPPED(wstatus))
    {
        std::fprintf(stderr, "Child did not stop as expected\n");
        kill(child, SIGKILL);
        return EXIT_FAILURE;
    }

    pin_to_pcores(child);

    Maps maps;
    maps.seed(child);

    FILE *plot_pipe = nullptr;
    bool  plot_is_pipe = false;
    if (!cfg.no_plot)
    {
        plot_pipe = popen("python3 plot/plotter.py", "w");
        if (!plot_pipe)
        {
            std::perror("popen plotter");
            // Plotting is best-effort: a missing python3 or matplotlib
            // shouldn't abort the trace.
            cfg.no_plot = true;
        }
        else
        {
            plot_is_pipe = true;
        }
    }
    if (!plot_pipe)
    {
        // Ipc unconditionally writes to a FILE*; /dev/null absorbs the
        // NDJSON stream when the live plotter is disabled.
        plot_pipe = fopen("/dev/null", "w");
    }

    Stats stats;
    Ipc ipc(plot_pipe, plot_is_pipe);

    PerfSession perf(child, maps, stats, ipc);
    try
    {
        perf.open(cfg.ring_pages);
    }
    catch (const std::exception &ex)
    {
        std::fprintf(stderr, "Failed to open perf events: %s\n", ex.what());
        kill(child, SIGKILL);
        return EXIT_FAILURE;
    }

    struct sigaction sa{};
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    // SA_RESTART so epoll_wait isn't aborted by every SIGCHLD; the loop
    // polls g_child_exited explicitly. SA_NOCLDSTOP suppresses the
    // signal for stop/continue transitions - only true termination
    // should set the flag.
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, nullptr);

    perf.enable();
    kill(child, SIGCONT);

    int epfd = epoll_create1(0);
    if (epfd < 0)
    {
        std::perror("epoll_create1");
        return EXIT_FAILURE;
    }

    auto add_fd = [&](int fd)
    {
        struct epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.fd = fd;
        epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
    };
    add_fd(perf.load_fd());
    add_fd(perf.store_fd());

    auto last_flush = std::chrono::steady_clock::now();
    constexpr int FLUSH_MS = 100;

    while (!g_child_exited)
    {
        struct epoll_event events[4];
        int n = epoll_wait(epfd, events, 4, FLUSH_MS);

        if (n < 0 && errno != EINTR)
        {
            std::perror("epoll_wait");
            break;
        }

        // Drain both ring buffers on any wakeup. The two events are in
        // a single group and the kernel may signal either fd in response
        // to activity on the other; treating them symmetrically avoids
        // losing records when only one fd is reported readable.
        bool keep_going = perf.drain();

        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(
                now - last_flush)
                .count() >= FLUSH_MS)
        {
            ipc.flush();
            last_flush = now;
        }

        if (!keep_going)
            break;
    }

    // The kernel buffers a final batch of records between the last
    // epoll wakeup and the child's exit; one more drain after disable()
    // collects them. Skipping this loses the tail of every trace.
    perf.disable();
    perf.drain();
    ipc.flush();

    waitpid(child, &wstatus, 0);
    close(epfd);

    std::fprintf(stderr, "\n[tracer] child process exited - collecting final samples...\n");

    std::printf("\n=== Memory Access Summary ===\n\n");

    {
        using Entry = std::pair<std::string, CodeStats>;
        std::vector<Entry> code_vec(stats.code_stats().begin(),
                                    stats.code_stats().end());
        std::sort(code_vec.begin(), code_vec.end(),
            [](const Entry& a, const Entry& b) {
                return (a.second.reads + a.second.writes) >
                       (b.second.reads + b.second.writes);
            });

        std::printf("-- Code objects (instruction side) --\n");
        std::printf("  %-34s %12s %12s\n", "Object", "Reads", "Writes");
        for (auto &[name, cs] : code_vec)
        {
            if (cs.reads == 0 && cs.writes == 0) continue;
            std::printf("  %-34s %12lu %12lu\n",
                        name.c_str(), cs.reads, cs.writes);
        }
    }

    {
        struct Row { std::string label; std::string type_str; uint64_t reads; uint64_t writes; };
        std::vector<Row> rows;

        auto type_name = [](RegionType t) -> const char* {
            switch (t) {
                case RegionType::HEAP:      return "heap";
                case RegionType::STACK:     return "stack";
                case RegionType::FILE_TEXT: return "text";
                case RegionType::FILE_DATA: return "data";
                case RegionType::VDSO:      return "vdso";
                case RegionType::ANON:      return "anon";
                default:                    return "unknown";
            }
        };

        for (auto &[key, rs] : stats.region_stats())
        {
            uint64_t total_r = 0, total_w = 0;
            for (auto &b : rs.buckets) { total_r += b.reads; total_w += b.writes; }
            if (total_r == 0 && total_w == 0) continue;

            // Anonymous mappings (and [heap]/[stack], of which there may
            // be more than one with the same name across threads) all
            // share a display name. Append the address range so distinct
            // mappings remain distinguishable in the printed table.
            std::string label = rs.name;
            if (rs.type == RegionType::ANON  ||
                rs.type == RegionType::HEAP  ||
                rs.type == RegionType::STACK ||
                rs.name == "anon")
            {
                char buf[64];
                std::snprintf(buf, sizeof(buf), " [%lx-%lx]",
                              (unsigned long)rs.start, (unsigned long)rs.end);
                label += buf;
            }

            rows.push_back({label, type_name(rs.type), total_r, total_w});
        }

        std::sort(rows.begin(), rows.end(),
            [](const Row& a, const Row& b) {
                return (a.reads + a.writes) > (b.reads + b.writes);
            });

        std::printf("\n-- Data regions (address side) --\n");
        std::printf("  %-44s %-7s %12s %12s\n", "Region", "Type", "Reads", "Writes");
        for (auto &r : rows)
            std::printf("  %-44s %-7s %12lu %12lu\n",
                        r.label.c_str(), r.type_str.c_str(), r.reads, r.writes);
    }

    return WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : EXIT_FAILURE;
}
