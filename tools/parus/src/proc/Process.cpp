#include <parus_tool/proc/Process.hpp>

#include <cstdio>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace parus_tool::proc {

int run_argv(const std::vector<std::string>& argv) {
    if (argv.empty()) return 1;
#if defined(_WIN32)
    std::vector<const char*> cargs;
    cargs.reserve(argv.size() + 1);
    for (const auto& a : argv) cargs.push_back(a.c_str());
    cargs.push_back(nullptr);
    const int rc = _spawnvp(_P_WAIT, argv[0].c_str(), cargs.data());
    return (rc < 0) ? 1 : rc;
#else
    std::vector<char*> cargs;
    cargs.reserve(argv.size() + 1);
    for (const auto& a : argv) cargs.push_back(const_cast<char*>(a.c_str()));
    cargs.push_back(nullptr);

    pid_t pid = -1;
    const int sp = posix_spawnp(&pid, argv[0].c_str(), nullptr, nullptr, cargs.data(), environ);
    if (sp != 0) return 1;

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return 1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
#endif
}

bool run_argv_capture_stdout(const std::vector<std::string>& argv, std::string& out, int& exit_code) {
    out.clear();
    exit_code = 1;
    if (argv.empty()) return false;

#if defined(_WIN32)
    return false;
#else
    int pipefd[2] = {-1, -1};
    if (pipe(pipefd) != 0) return false;

    std::vector<char*> cargs;
    cargs.reserve(argv.size() + 1);
    for (const auto& a : argv) cargs.push_back(const_cast<char*>(a.c_str()));
    cargs.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, pipefd[0]);
    posix_spawn_file_actions_addclose(&actions, pipefd[1]);

    pid_t pid = -1;
    const int sp = posix_spawnp(&pid, argv[0].c_str(), &actions, nullptr, cargs.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(pipefd[1]);

    if (sp != 0) {
        close(pipefd[0]);
        return false;
    }

    char buf[4096];
    for (;;) {
        const ssize_t n = read(pipefd[0], buf, sizeof(buf));
        if (n <= 0) break;
        out.append(buf, static_cast<size_t>(n));
    }
    close(pipefd[0]);

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return false;
    if (WIFEXITED(status)) exit_code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) exit_code = 128 + WTERMSIG(status);
    else exit_code = 1;
    return true;
#endif
}

} // namespace parus_tool::proc
