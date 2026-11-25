#pragma once

#include "master.hpp"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

struct sqlite3;

namespace luo {
struct Reply {
    int status;
    Json body;
};

class Jobs {
public:
    explicit Jobs(const Config& config);
    ~Jobs();
    Reply submit(const std::string& id, const std::string& profile);
    Reply status(const std::string& id);
    Reply offer();
    Reply resume(const std::string& id, const Json& result);
    Json topology();
    std::string metrics();
    bool ready() const;
    bool end_if_idle();
    void drain();
    bool drained() const;
private:
    void run();
    void stop_topology(Json& job);
    void recover(Json& job);
    void restore_topology(const Json& job);
    void record(Json& job, const char* stage, const char* error = "");
    void save(const Json& job, bool done = false);
    Json read(const char* sql, const std::string& id = {});
    Json active();
    bool worker_online() const;

    const Config& config_;
    std::unique_ptr<sqlite3, void(*)(sqlite3*)> db_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::chrono::steady_clock::time_point heartbeat_{};
    std::atomic_bool ready_{false};
    std::atomic_bool draining_{false};
    std::atomic_bool drained_{false};
    std::atomic_uint64_t accepted_{0}, completed_{0}, failed_{0}, recovery_failed_{0};
    std::thread runner_;
};
}
