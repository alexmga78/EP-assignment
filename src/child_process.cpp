#include "child_process.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace memtracer {

ChildProcess::ChildProcess(std::vector<std::string> argv) : argv_(std::move(argv)) {
    if (argv_.empty()) throw std::invalid_argument("ChildProcess: empty argv");

    int pipefd[2];
    if (pipe(pipefd) != 0) throw std::runtime_error(std::string("pipe: ") + std::strerror(errno));

    pid_t pid = fork();
    if (pid < 0) {
        ::close(pipefd[0]); ::close(pipefd[1]);
        throw std::runtime_error(std::string("fork: ") + std::strerror(errno));
    }

    if (pid == 0) {
        // Child. Close write end, keep read end for the sync byte.
        ::close(pipefd[1]);
        sync_read_fd_ = pipefd[0];
        run_child_and_never_return();
        // unreachable
    }

    // Parent.
    ::close(pipefd[0]);
    sync_write_fd_ = pipefd[1];
    pid_ = pid;
}

ChildProcess::~ChildProcess() {
    if (sync_write_fd_ >= 0) ::close(sync_write_fd_);
    if (pid_ > 0 && !reaped_) {
        // Best effort: don't leave a zombie behind. Don't kill — caller may have
        // intentionally let it run.
        int status = 0;
        ::waitpid(pid_, &status, WNOHANG);
    }
}

void ChildProcess::run_child_and_never_return() {
    // Block until the parent says go.
    char b = 0;
    ssize_t n;
    do { n = ::read(sync_read_fd_, &b, 1); } while (n < 0 && errno == EINTR);
    ::close(sync_read_fd_);

    if (n != 1) {
        std::fprintf(stderr, "child: parent died before release; aborting\n");
        ::_exit(127);
    }

    // Build argv: vector of std::string -> char* const[]
    std::vector<char*> raw;
    raw.reserve(argv_.size() + 1);
    for (auto& s : argv_) raw.push_back(const_cast<char*>(s.c_str()));
    raw.push_back(nullptr);

    ::execvp(raw[0], raw.data());

    // If we get here, exec failed.
    std::fprintf(stderr, "child: execvp(%s) failed: %s\n", raw[0], std::strerror(errno));
    ::_exit(127);
}

void ChildProcess::release() {
    if (released_) return;
    released_ = true;
    char b = 'G';
    ssize_t n;
    do { n = ::write(sync_write_fd_, &b, 1); } while (n < 0 && errno == EINTR);
    if (n != 1) {
        throw std::runtime_error(std::string("ChildProcess::release: write: ") + std::strerror(errno));
    }
    ::close(sync_write_fd_);
    sync_write_fd_ = -1;
}

int ChildProcess::wait_for_exit() {
    if (reaped_) return exit_code_;
    int status = 0;
    pid_t r;
    do { r = ::waitpid(pid_, &status, 0); } while (r < 0 && errno == EINTR);
    if (r < 0) return -1;
    reaped_ = true;
    if (WIFEXITED(status)) exit_code_ = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) exit_code_ = 128 + WTERMSIG(status);
    else exit_code_ = -1;
    return exit_code_;
}

bool ChildProcess::try_reap(int& exit_code) {
    if (reaped_) { exit_code = exit_code_; return true; }
    int status = 0;
    pid_t r = ::waitpid(pid_, &status, WNOHANG);
    if (r == 0) return false;
    if (r < 0) {
        if (errno == ECHILD) { reaped_ = true; exit_code_ = -1; exit_code = -1; return true; }
        return false;
    }
    reaped_ = true;
    if (WIFEXITED(status)) exit_code_ = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) exit_code_ = 128 + WTERMSIG(status);
    else exit_code_ = -1;
    exit_code = exit_code_;
    return true;
}

}  // namespace memtracer
