#include "pma/core.hpp"
#include <spawn.h>
#include <sys/wait.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <cerrno>
#include <cstring>
extern char** environ;
namespace pma {
ProcessResult run(const std::vector<std::string>& args, int timeout, size_t limit) {
    if (args.empty()) throw std::runtime_error("Empty subprocess command");
    int pipefd[2];
    if (pipe(pipefd) != 0) throw std::runtime_error("Cannot create subprocess pipe");
    for (int fd : pipefd) fcntl(fd, F_SETFD, FD_CLOEXEC);
    std::vector<char*> argv;
    for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    std::vector<std::string> environment;
    for (char** env = environ; *env; ++env) {
        std::string value(*env);
        if (!value.starts_with("GIT_TERMINAL_PROMPT=") && !value.starts_with("LC_ALL=") && !value.starts_with("GIT_OPTIONAL_LOCKS=")) environment.push_back(value);
    }
    environment.emplace_back("GIT_TERMINAL_PROMPT=0"); environment.emplace_back("GIT_OPTIONAL_LOCKS=0"); environment.emplace_back("LC_ALL=C");
    std::vector<char*> envp; for (auto& value : environment) envp.push_back(value.data()); envp.push_back(nullptr);
    posix_spawn_file_actions_t actions; posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDERR_FILENO);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addclose(&actions, pipefd[0]);
    posix_spawn_file_actions_addclose(&actions, pipefd[1]);
    posix_spawnattr_t attr; posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP); posix_spawnattr_setpgroup(&attr, 0);
    pid_t pid; int error = posix_spawnp(&pid, argv[0], &actions, &attr, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&actions); posix_spawnattr_destroy(&attr); close(pipefd[1]);
    if (error) { close(pipefd[0]); throw std::runtime_error("Cannot start subprocess: " + std::string(std::strerror(error))); }
    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout);
    std::string output; bool truncated = false, eof = false, ended = false, timed_out = false; int status = 0;
    while (!eof || !ended) {
        char buffer[8192]; ssize_t n;
        while ((n = read(pipefd[0], buffer, sizeof(buffer))) > 0) {
            auto available = limit > output.size() ? limit - output.size() : 0;
            output.append(buffer, std::min(available, static_cast<size_t>(n)));
            if (static_cast<size_t>(n) > available) truncated = true;
        }
        if (n == 0) eof = true;
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) eof = true;
        if (!ended) ended = waitpid(pid, &status, WNOHANG) == pid;
        if (eof && ended) break;
        if (std::chrono::steady_clock::now() >= deadline) {
            kill(-pid, SIGKILL); if (!ended) waitpid(pid, &status, 0);
            timed_out = true; break;
        }
        pollfd pfd{pipefd[0], POLLIN, 0}; poll(&pfd, 1, 20);
    }
    close(pipefd[0]);
    return {timed_out ? 124 : (WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status)), output, truncated};
}
ProcessResult git(const fs::path& directory, const std::vector<std::string>& args, int timeout) {
    std::vector<std::string> command{"git", "-c", "core.hooksPath=/dev/null", "-c", "core.fsmonitor=false", "-c", "protocol.ext.allow=never", "-C", directory.string()};
    command.insert(command.end(), args.begin(), args.end());
    return run(command, timeout);
}
std::string git_ok(const fs::path& directory, const std::vector<std::string>& args, int timeout) {
    auto result = git(directory, args, timeout);
    // Git output can contain remote credentials or paths. Do not expose it in the UI/logs.
    if (result.code != 0) throw std::runtime_error("Git " + (args.empty() ? std::string("operation") : args.front()) + " failed (exit " + std::to_string(result.code) + "); check repository access/history or timeout");
    if (result.truncated) throw std::runtime_error("Git output exceeded the safety limit");
    return trim(result.output);
}
}
