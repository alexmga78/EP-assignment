#include "signal_handler.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <signal.h>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace memtracer {

std::atomic<bool> SignalHandler::g_running{true};
std::atomic<bool> SignalHandler::g_child_died{false};

namespace {
int g_pipe_r = -1;
int g_pipe_w = -1;

extern "C" void on_term_signal(int) {
    SignalHandler::g_running.store(false, std::memory_order_relaxed);
    if (g_pipe_w >= 0) {
        char b = 1;
        // ssize_t but we don't care if it fails — handler must be reentrant.
        ssize_t n = ::write(g_pipe_w, &b, 1);
        (void)n;
    }
}

extern "C" void on_sigchld(int) {
    SignalHandler::g_child_died.store(true, std::memory_order_relaxed);
    if (g_pipe_w >= 0) {
        char b = 1;
        ssize_t n = ::write(g_pipe_w, &b, 1);
        (void)n;
    }
}
}  // namespace

void SignalHandler::install() {
    if (g_pipe_r >= 0) return;  // already installed

    int fds[2];
    if (::pipe(fds) != 0) {
        throw std::runtime_error(std::string("pipe(self): ") + std::strerror(errno));
    }
    // Make both ends non-blocking + close-on-exec.
    for (int f : fds) {
        int fl = ::fcntl(f, F_GETFL, 0);
        ::fcntl(f, F_SETFL, fl | O_NONBLOCK);
        int cf = ::fcntl(f, F_GETFD, 0);
        ::fcntl(f, F_SETFD, cf | FD_CLOEXEC);
    }
    g_pipe_r = fds[0];
    g_pipe_w = fds[1];

    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    // SA_RESTART so other syscalls (read of /proc, etc.) auto-resume; we rely
    // on the self-pipe wakeup, not on EINTR, to break the poll() loop.
    sa.sa_flags = SA_RESTART;

    sa.sa_handler = on_term_signal;
    sigaction(SIGINT,  &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    sa.sa_handler = on_sigchld;
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, nullptr);
}

int SignalHandler::wakeup_fd() { return g_pipe_r; }

void SignalHandler::drain_wakeups() {
    if (g_pipe_r < 0) return;
    char buf[64];
    while (::read(g_pipe_r, buf, sizeof(buf)) > 0) { /* discard */ }
}

}  // namespace memtracer
