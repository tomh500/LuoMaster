#include "master.hpp"

#include <algorithm>
#include <fstream>
#include <functional>
#include <iostream>
#include <set>
#include <syncstream>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace luo {
bool valid_id(const std::string& id) {
    return !id.empty() && id.size() <= 64 && std::ranges::all_of(id, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_';
    });
}

bool within(const fs::path& root, const fs::path& path) {
    auto part = path.begin();
    for (const auto& parent : root) {
        if (part == path.end() || *part != parent) return false;
        ++part;
    }
    return true;
}

std::string read_token(const fs::path& path) {
    if (is_link(path) || !fs::is_regular_file(path)) throw std::runtime_error("invalid token file");
#ifndef _WIN32
    struct stat permissions{};
    if (::stat(path.c_str(), &permissions) != 0 || (permissions.st_mode & 0137) != 0)
        throw std::runtime_error("token file permissions must be 0600 or 0640 (read-only also accepted)");
#endif
    if (fs::file_size(path) > 130) throw std::runtime_error("invalid token length");
    std::ifstream input(path);
    std::string token;
    std::getline(input, token);
    if (!token.empty() && token.back() == '\r') token.pop_back();
    if (input.bad() || input.peek() != std::char_traits<char>::eof() || token.size() < 32 ||
        token.size() > 128 || !std::ranges::all_of(token, [](char c) { return c >= '!' && c <= '~'; }))
        throw std::runtime_error("invalid token format");
    return token;
}

namespace {
fs::path absolute_path(const Json& value) {
    const auto text = value.get<std::string>();
    if (text.find('\0') != std::string::npos) throw std::runtime_error("invalid path");
    fs::path path{std::u8string(text.begin(), text.end())};
    if (!path.is_absolute()) throw std::runtime_error("absolute config paths are required");
    if (is_link(path)) throw std::runtime_error("configured path is a link");
    return fs::canonical(path);
}

std::vector<fs::path> relative_paths(const Json& json) {
    if (!json.is_array()) throw std::runtime_error("backup paths must be arrays");
    std::vector<fs::path> result;
    for (const auto& value : json) {
        const auto txt = value.get<std::string>();
        fs::path path{std::u8string(txt.begin(), txt.end())};
        if (txt.empty() || txt.find_first_of("\\:\0", 0, 3) != std::string::npos ||
            path.has_root_path() || std::ranges::any_of(path, [](const auto& part) {
                return part.empty() || part == "." || part == "..";
            })) throw std::runtime_error("invalid relative backup path");
        result.push_back(path);
    }
    return result;
}

bool overlap(const fs::path& a, const fs::path& b) {
    return within(a, b) || within(b, a);
}

std::set<std::string> actions(const Json& json) {
    const auto parsed = json.get<std::set<std::string>>();
    for (const auto& action : parsed) {
        if (action != "help" && action != "topology" && action != "jobs" && action != "status" && action != "backup" && action != "metrics")
            throw std::runtime_error("invalid service action; local console privileges cannot be delegated");
    }
    return parsed;
}
}

Config load_config(const fs::path& file, bool archive_worker) {
    if (fs::file_size(file) > 65536) throw std::runtime_error("configuration exceeds 64 KiB");
    std::ifstream input(file);
    const auto data = Json::parse(input);
    Config config;
    config.state_dir = absolute_path(data.at("state_dir"));
    config.archive_dir = absolute_path(data.at("archive_dir"));
    config.staging_dir = absolute_path(data.value("staging_dir", data.at("archive_dir")));
    config.lifecycle = absolute_path(data.at("lifecycle"));
    if (!archive_worker && data.contains("economy_config")) {
        config.economy_config = absolute_path(data.at("economy_config"));
    }
    if (!fs::is_directory(config.state_dir) || !fs::is_directory(config.archive_dir) || !fs::is_directory(config.staging_dir) ||
        !fs::is_regular_file(config.lifecycle) || overlap(config.state_dir, config.archive_dir) ||
        overlap(config.state_dir, config.staging_dir) ||
        (config.staging_dir != config.archive_dir && overlap(config.staging_dir, config.archive_dir)))
        throw std::runtime_error("configuration requires separate state and archive directories and a lifecycle executable");
    config.worker_token = read_token(absolute_path(data.at("worker_token_file")));
    if (!archive_worker) {
        config.observer_token = read_token(absolute_path(data.at("observer_token_file")));
        config.backup_token = read_token(absolute_path(data.at("backup_token_file")));
        if (std::set{config.observer_token, config.backup_token, config.worker_token}.size() != 3)
            throw std::runtime_error("tokens must be distinct");
        config.proxy_actions = actions(data.value("proxy_actions", Json::array({"status", "backup"})));
        if (data.contains("qq_commands")) {
            const auto& qq = data.at("qq_commands");
            config.qq_token = read_token(absolute_path(qq.at("token_file")));
            config.qq_actions = actions(qq.at("actions"));
            if (config.qq_token == config.observer_token || config.qq_token == config.backup_token || config.qq_token == config.worker_token)
                throw std::runtime_error("QQ commands require a distinct token");
        }
    }
    config.port = data.value("port", 18776);
    config.http_workers = data.value("http_workers", 2);
    config.action_timeout = std::chrono::seconds(data.value("action_timeout_seconds", 120));
    config.archive_timeout = std::chrono::seconds(data.value("archive_timeout_seconds", 900));
    if (config.port < 1024 || config.port > 65535 || config.http_workers < 1 || config.http_workers > 16 ||
        config.action_timeout.count() < 1 || config.action_timeout.count() > 600 ||
        config.archive_timeout.count() < 2 || config.archive_timeout.count() > 86400)
        throw std::runtime_error("port, http_workers or timeouts out of range");

    std::map<std::string, Node> nodes;
    const auto& configured_nodes = data.at("nodes");
    if (!configured_nodes.is_array()) throw std::runtime_error("nodes must be an array");
    for (const auto& spec : configured_nodes) {
        const auto id = spec.at("id").get<std::string>();
        if (!valid_id(id) || !nodes.emplace(id, Node{id, spec.at("depends_on").get<std::vector<std::string>>(),
                                                   spec.at("save").get<bool>()}).second)
            throw std::runtime_error("invalid or repeated node ID");
    }
    if (nodes.empty() || nodes.size() > 32) throw std::runtime_error("provide 1 to 32 managed nodes");
    std::set<std::string> visiting, visited;
    std::function<void(const std::string&)> visit = [&](const std::string& id) {
        if (visited.contains(id)) return;
        const auto it = nodes.find(id);
        if (it == nodes.end() || !visiting.insert(id).second)
            throw std::runtime_error("missing or cyclic topology dependency");
        for (const auto& dep : it->second.dependencies) visit(dep);
        visiting.erase(id);
        visited.insert(id);
        config.nodes.push_back(it->second);
    };
    for (const auto& [id, node] : nodes) visit(id);

    const auto& profiles = data.at("profiles");
    if (!profiles.is_object() || profiles.empty() || profiles.size() > 32)
        throw std::runtime_error("provide 1 to 32 backup profiles");
    for (const auto& [name, spec] : profiles.items()) {
        Profile profile{absolute_path(spec.at("source")), relative_paths(spec.at("include")),
                        relative_paths(spec.value("exclude", Json::array()))};
        if (!valid_id(name) || !fs::is_directory(profile.source) || profile.include.empty() ||
            overlap(profile.source, config.state_dir) || overlap(profile.source, config.archive_dir) ||
            overlap(profile.source, config.staging_dir))
            throw std::runtime_error("invalid backup profile or overlapping source, state and archive paths");
        for (std::size_t i = 0; i < profile.include.size(); ++i) {
            for (std::size_t j = 0; j < i; ++j) {
                if (overlap(profile.include[i], profile.include[j]))
                    throw std::runtime_error("backup include paths overlap");
            }
        }
        config.profiles.emplace(name, std::move(profile));
    }
    // Recovery must use the same topology, source selections and artifact location.
    config.generation = sha256_text(Json::array({data.at("nodes"), profiles, data.at("archive_dir"),
                                                data.value("staging_dir", data.at("archive_dir")),
                                                data.at("lifecycle")}).dump());
    return config;
}

std::int64_t unix_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

void audit(const char* event, const Json& details) {
    auto record = details;
    record["event"] = event;
    record["time_unix_ms"] = unix_ms();
    std::osyncstream(std::cerr) << record.dump(-1, ' ', false, Json::error_handler_t::replace) << '\n';
}
}
