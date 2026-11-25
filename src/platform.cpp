#include "master.hpp"

#include <cerrno>
#include <stdexcept>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <csignal>
#include <fcntl.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace luo {
bool is_link(const fs::path& path) {
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return true;
#endif
    return fs::is_symlink(fs::symlink_status(path));
}

InstanceLock::InstanceLock(const fs::path& path) {
    if (is_link(path)) throw std::runtime_error("lock path is a link");
#ifdef _WIN32
    handle_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                          FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle_ == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot acquire exclusive directory lock");
#else
    fd_ = open(path.c_str(), O_CREAT | O_RDONLY | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd_ < 0) throw std::runtime_error("cannot open directory lock");
    if (flock(fd_, LOCK_EX | LOCK_NB) != 0) {
        close(fd_);
        throw std::runtime_error("cannot acquire exclusive directory lock");
    }
#endif
}

InstanceLock::~InstanceLock() {
#ifdef _WIN32
    CloseHandle(handle_);
#else
    close(fd_);
#endif
}

int lifecycle(const Config& config, const std::string& action, const Node& node) {
#ifdef _WIN32
    auto widen = [](const std::string& arg) { return std::wstring(arg.begin(), arg.end()); };
    auto command = L"\"" + config.lifecycle.wstring() + L"\" " + widen(action) + L" " + widen(node.id);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(config.lifecycle.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &startup, &process))
        throw std::runtime_error("could not start lifecycle helper");
    CloseHandle(process.hThread);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(config.action_timeout).count();
    const DWORD wait = WaitForSingleObject(process.hProcess, static_cast<DWORD>(ms));
    DWORD code = 255;
    if (wait == WAIT_OBJECT_0) {
        GetExitCodeProcess(process.hProcess, &code);
    } else {
        TerminateProcess(process.hProcess, 124);
        WaitForSingleObject(process.hProcess, 1000);
    }
    CloseHandle(process.hProcess);
    if (wait != WAIT_OBJECT_0) throw std::runtime_error("lifecycle helper timed out or failed");
    return code <= 255 ? static_cast<int>(code) : 255;
#else
    std::string executable = config.lifecycle.string();
    char* arguments[] = {executable.data(), const_cast<char*>(action.c_str()),
                         const_cast<char*>(node.id.c_str()), nullptr};
    posix_spawnattr_t attr;
    int spawn_error = posix_spawnattr_init(&attr);
    if (spawn_error != 0) throw std::runtime_error("cannot initialize lifecycle process attributes");
    spawn_error = posix_spawnattr_setpgroup(&attr, 0);
    if (spawn_error == 0) spawn_error = posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
    pid_t child{};
    if (spawn_error == 0) spawn_error = posix_spawn(&child, executable.c_str(), nullptr, &attr, arguments, environ);
    posix_spawnattr_destroy(&attr);
    if (spawn_error != 0) throw std::runtime_error("could not start lifecycle helper");
    const auto deadline = std::chrono::steady_clock::now() + config.action_timeout;
    int status{};
    do {
        const auto result = waitpid(child, &status, WNOHANG);
        if (result == child) return WIFEXITED(status) ? WEXITSTATUS(status) : 255;
        if (result < 0 && errno != EINTR) throw std::runtime_error("could not wait for lifecycle helper");
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    } while (std::chrono::steady_clock::now() < deadline);
    kill(-child, SIGKILL);
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    throw std::runtime_error("lifecycle helper timed out");
#endif
}

void sync_file(const fs::path& path) {
#ifdef _WIN32
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot open archive to flush");
    const BOOL synced = FlushFileBuffers(handle);
    CloseHandle(handle);
    if (!synced) throw std::runtime_error("cannot flush archive");
#else
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) throw std::runtime_error("cannot open archive or directory to flush");
    const int synced = fsync(fd);
    close(fd);
    if (synced != 0) throw std::runtime_error("cannot flush archive or directory");
#endif
}

void publish(const fs::path& staged, const fs::path& final) {
#ifdef _WIN32
    if (!MoveFileExW(staged.c_str(), final.c_str(), MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("cannot publish archive");
#else
    if (link(staged.c_str(), final.c_str()) != 0) throw std::runtime_error("cannot publish archive");
    sync_file(final.parent_path());
    fs::remove(staged);
    sync_file(staged.parent_path());
#endif
}
}
