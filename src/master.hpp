#pragma once

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace luo {
namespace fs = std::filesystem;
using Json = nlohmann::json;

struct Node {
    std::string id;
    std::vector<std::string> dependencies;
    bool save;
};

struct Profile {
    fs::path source;
    std::vector<fs::path> include;
    std::vector<fs::path> exclude;
};

struct Config {
    fs::path economy_config;
    fs::path state_dir;
    fs::path archive_dir;
    fs::path staging_dir;
    fs::path lifecycle;
    std::vector<Node> nodes;
    std::map<std::string, Profile> profiles;
    std::string observer_token;
    std::string backup_token;
    std::string worker_token;
    std::string qq_token;
    std::set<std::string> proxy_actions;
    std::set<std::string> qq_actions;
    std::string generation;
    int port;
    int http_workers;
    std::chrono::seconds action_timeout;
    std::chrono::seconds archive_timeout;
};

bool valid_id(const std::string& id);
bool is_link(const fs::path& path);
bool within(const fs::path& root, const fs::path& path);
using CheckDeadline = std::function<void()>;
std::string sha256_file(const fs::path& path, const CheckDeadline& check = {});
std::string sha256_text(const std::string& text);
std::string read_token(const fs::path& path);
fs::path staged_archive(const Config& config, const std::string& id);
Config load_config(const fs::path& file, bool archive_worker = false);
int lifecycle(const Config& config, const std::string& action, const Node& node);
void sync_file(const fs::path& path);
void publish(const fs::path& staged, const fs::path& final);
std::string archive_profile(const Profile& profile, const fs::path& output, const CheckDeadline& deadline);
std::int64_t unix_ms();
void audit(const char* event, const Json& details = Json::object());

class InstanceLock {
public:
    explicit InstanceLock(const fs::path& path);
    ~InstanceLock();
    InstanceLock(const InstanceLock&) = delete;
    InstanceLock& operator=(const InstanceLock&) = delete;
private:
#ifdef _WIN32
    void* handle_;
#else
    int fd_;
#endif
};
}
