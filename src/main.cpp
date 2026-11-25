#include "jobs.hpp"

#include <luo/detail/http.hpp>
#include <openssl/crypto.h>

#include <atomic>
#include <csignal>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string_view>

#ifdef LUO_MASTER_ECONOMY
#include "economy_routes.hpp"
#include <mysql.h>
#include <set>
#endif

#ifndef _WIN32
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {
using namespace luo;
using namespace std::chrono_literals;
static_assert(std::atomic_bool::is_always_lock_free);
std::atomic_bool shutdown_requested{false};
void signal_stop(int) { shutdown_requested.store(true, std::memory_order_relaxed); }

bool allowed(const httplib::Request& req, const std::string& token) {
    if (token.empty() || req.get_header_value_count("Authorization") != 1) return false;
    const auto value = req.get_header_value("Authorization");
    const std::string_view auth(value);
    return auth.starts_with("Bearer ") && auth.size() == token.size() + 7 &&
        CRYPTO_memcmp(auth.data() + 7, token.data(), token.size()) == 0;
}

void reply(httplib::Response& res, Reply value) {
    res.status = value.status;
    res.set_header("Cache-Control", "no-store");
    if (value.status != 204) res.set_content(value.body.dump(), "application/json");
}

const std::set<std::string> local_actions{"help", "topology", "jobs", "status", "backup", "metrics"};

Reply run_command(Jobs& jobs, std::string_view actor, const std::set<std::string>& actions, const std::string& line) {
    if (line.size() > 256 || line.find_first_of("\r\n\0", 0, 3) != std::string::npos)
        return {400, {{"error", "invalid_command"}}};
    std::istringstream words(line);
    std::string action, first, second, extra;
    words >> action >> first >> second >> extra;
    const bool local = actor == "local_console";
    const bool permitted = actions.contains(action) || (local && action == "end");
    audit("operator_command", {{"actor", actor}, {"action", valid_id(action) ? action : "invalid"}, {"authorized", permitted}});
    if (!permitted) return {403, {{"error", "action_not_authorized"}}};
    if (action == "help" && first.empty()) {
        auto available = actions;
        if (local) available.insert("end");
        return {200, {{"actions", available}, {"syntax", {"status ID", "backup ID PROFILE"}}}};
    }
    if (action == "topology" && first.empty()) return {200, jobs.topology()};
    if (action == "jobs" && first.empty()) return {200, {{"maintenance", jobs.topology().at("maintenance")}}};
    if (action == "metrics" && first.empty()) return {200, {{"metrics", jobs.metrics()}}};
    if (action == "status" && valid_id(first) && second.empty()) return jobs.status(first);
    if (action == "backup" && valid_id(first) && valid_id(second) && extra.empty()) return jobs.submit(first, second);
    if (action == "end" && first.empty()) {
        if (!jobs.end_if_idle()) return {409, {{"error", "maintenance_not_finished"}}};
        shutdown_requested = true;
        return {200, {{"status", "draining"}}};
    }
    return {400, {{"error", "invalid_command_arguments"}}};
}

void console(Jobs& jobs) {
    std::cout << "Luo Master console. Type help for commands.\n";
    while (!shutdown_requested) {
        std::cout << "> " << std::flush;
#ifndef _WIN32
        pollfd input{STDIN_FILENO, POLLIN, 0};
        while (!shutdown_requested && poll(&input, 1, 100) == 0) {}
        if (shutdown_requested) break;
#endif
        std::string line;
        if (!std::getline(std::cin, line)) break;
        try {
            const auto result = run_command(jobs, "local_console", local_actions, line);
            std::cout << result.status << ' ' << result.body.dump() << '\n';
        } catch (const std::exception& e) {
            audit("console_command_failed", {{"detail", e.what()}});
            std::cout << "500 internal_error\n";
        }
    }
    shutdown_requested = true;
}

#ifndef _WIN32
class ConsoleSocket {
public:
    ConsoleSocket(const fs::path& path, Jobs& jobs) : path_(path) {
        struct stat info{};
        if (!path_.is_absolute() || stat(path_.parent_path().c_str(), &info) != 0 ||
            !S_ISDIR(info.st_mode) || info.st_uid != geteuid() || (info.st_mode & 0077) != 0 ||
            fs::exists(fs::symlink_status(path_)))
            throw std::runtime_error("console socket needs an unused path inside a private, owned directory");
        server.set_address_family(AF_UNIX).set_keep_alive_max_count(1).set_payload_max_length(1024)
            .set_read_timeout(2).set_write_timeout(2);
        server.new_task_queue = [] { return new httplib::ThreadPool(1, 0, 8); };
        server.Post("/console", [&](const auto& req, auto& res) {
            const auto body = Json::parse(req.body, nullptr, false);
            if (!body.is_object() || body.size() != 1 || !body.contains("command") || !body.at("command").is_string())
                return reply(res, {400, {{"error", "invalid_request"}}});
            reply(res, run_command(jobs, "local_console", local_actions, body.at("command").template get<std::string>()));
        });
        if (!server.bind_to_port(path_.string(), 1)) throw std::runtime_error("cannot bind private console socket");
    }
    ~ConsoleSocket() {
        std::error_code error;
        fs::remove(path_, error);
    }
    httplib::Server server;
private:
    fs::path path_;
};

int attach(const fs::path& socket) {
    if (!socket.is_absolute()) throw std::runtime_error("an absolute console socket path is required");
    httplib::Client client(socket.string());
    client.set_address_family(AF_UNIX);
    client.set_connection_timeout(1);
    client.set_read_timeout(3);
    std::cout << "Luo Master console. Type help for commands.\n";
    std::string line;
    while (std::cout << "> " << std::flush && std::getline(std::cin, line)) {
        const auto result = client.Post("/console", Json{{"command", line}}.dump(), "application/json");
        if (!result) throw std::runtime_error("cannot reach the private console socket");
        std::cout << result->status << ' ' << result->body << '\n';
        const auto body = Json::parse(result->body, nullptr, false);
        if (result->status == 200 && body.is_object() && body.value("status", std::string{}) == "draining") break;
    }
    return 0;
}
#endif

int serve(const fs::path& path, bool interactive, const fs::path& socket) {
    const auto config = load_config(path);
#ifdef LUO_MASTER_ECONOMY
    Json economy;
    if (!config.economy_config.empty()) {
        if (config.http_workers < 2) throw std::runtime_error("economy requires at least two HTTP workers");
        try {
#ifndef _WIN32
            struct stat permissions{};
            if (::stat(config.economy_config.c_str(), &permissions) != 0 || (permissions.st_mode & 0137) != 0)
                throw std::runtime_error("invalid private economy file permissions");
#endif
            if (fs::file_size(config.economy_config) > 65536) throw std::runtime_error("configuration exceeds 64 KiB");
            std::ifstream input(config.economy_config);
            economy = Json::parse(input);
            if (economy.at("clients").empty() || !economy.at("clients").is_array()) {
                throw std::runtime_error("no economy clients");
            }
            std::set used{config.worker_token, config.observer_token, config.backup_token, config.qq_token};
            for (const auto& client : economy.at("clients")) {
                const auto token = client.at("token").get<std::string>();
                if (token.size() < 32 || !used.insert(token).second) throw std::runtime_error("invalid token");
            }
            economy["request_slots"] = config.http_workers - 1;
        } catch (const std::exception&) {
            throw std::runtime_error("invalid private economy configuration");
        }
    }
    struct MySqlLibrary {
        explicit MySqlLibrary(bool enabled) : enabled(enabled) {
            if (enabled && mysql_library_init(0, nullptr, nullptr)) throw std::runtime_error("MySQL initialization failed");
        }
        ~MySqlLibrary() { if (enabled) mysql_library_end(); }
        bool enabled;
    } mysql(!economy.is_null());
#else
    if (!config.economy_config.empty()) throw std::runtime_error("build Master with LUO_ECONOMY_ROOT to enable economy");
#endif
    const InstanceLock state_lock(config.state_dir / ".master.lock");
    const InstanceLock archive_lock(config.archive_dir / ".master.lock");
    const InstanceLock staging_lock(config.staging_dir / ".coordinator.lock");
    httplib::Server server;
    server.new_task_queue = [&] { return new httplib::ThreadPool(config.http_workers, 0, 64); };
    server.set_read_timeout(2).set_write_timeout(2).set_keep_alive_max_count(1).set_payload_max_length(2048);
#ifdef LUO_MASTER_ECONOMY
    if (!economy.is_null()) luo::economy::register_routes(server, economy);
#endif
    if (!server.bind_to_port("127.0.0.1", config.port)) throw std::runtime_error("could not bind the loopback port");
    Jobs jobs(config);
    const auto can_read = [&](const auto& req, const std::string& action) {
        return allowed(req, config.observer_token) ||
               (allowed(req, config.backup_token) && config.proxy_actions.contains(action));
    };
    const Reply denied{401, {{"error", "unauthorized"}}};
    const Reply bad_json{400, {{"error", "invalid_request"}}};
    server.set_exception_handler([](const auto&, auto& response, std::exception_ptr failure) {
        try { std::rethrow_exception(failure); }
        catch (const std::exception& e) { audit("request_failed", {{"detail", e.what()}}); }
        catch (...) { audit("request_failed"); }
        reply(response, {500, {{"error", "internal_error"}}});
    });
    server.Get("/healthz", [](const auto&, auto& res) { reply(res, {200, {{"status", "ok"}}}); });
    server.Get("/readyz", [&](const auto&, auto& res) {
        const bool ready = jobs.ready();
        reply(res, {ready ? 200 : 503, {{"ready", ready}}});
    });
    server.Get("/metrics", [&](const auto& req, auto& res) {
        if (!can_read(req, "metrics")) return reply(res, denied);
        res.set_content(jobs.metrics(), "text/plain; version=0.0.4");
    });
    server.Get("/v1/topology", [&](const auto& req, auto& res) {
        reply(res, can_read(req, "topology") ? Reply{200, jobs.topology()} : denied);
    });
    server.Get(R"(/v1/backups/([A-Za-z0-9_-]{1,64}))", [&](const auto& req, auto& res) {
        reply(res, can_read(req, "status") ? jobs.status(req.matches[1].str()) : denied);
    });
    server.Post("/v1/backups", [&](const auto& req, auto& res) {
        if (!allowed(req, config.backup_token)) return reply(res, denied);
        if (!config.proxy_actions.contains("backup")) {
            audit("operator_command", {{"actor", "proxy"}, {"action", "backup"}, {"authorized", false}});
            return reply(res, {403, {{"error", "action_not_authorized"}}});
        }
        const auto id = req.get_header_value("Idempotency-Key");
        const auto body = Json::parse(req.body, nullptr, false);
        if (req.get_header_value_count("Idempotency-Key") != 1 || !valid_id(id) || !body.is_object() ||
            body.size() != 1 || !body.contains("profile") || !body.at("profile").is_string())
            return reply(res, bad_json);
        audit("operator_command", {{"actor", "proxy"}, {"action", "backup"}, {"authorized", true}, {"id", id}});
        reply(res, jobs.submit(id, body.at("profile").template get<std::string>()));
    });
    server.Get("/v1/backup-worker/job", [&](const auto& req, auto& res) {
        reply(res, allowed(req, config.worker_token) ? jobs.offer() : denied);
    });
    server.Post(R"(/v1/backups/([A-Za-z0-9_-]{1,64})/resume)", [&](const auto& req, auto& res) {
        if (!allowed(req, config.worker_token)) return reply(res, denied);
        reply(res, jobs.resume(req.matches[1].str(), Json::parse(req.body, nullptr, false)));
    });
    server.Post("/v1/commands", [&](const auto& req, auto& res) {
        const bool qq = allowed(req, config.qq_token);
        if (!qq && !allowed(req, config.backup_token)) return reply(res, denied);
        const auto body = Json::parse(req.body, nullptr, false);
        if (!body.is_object() || body.size() != 1 || !body.contains("command") || !body.at("command").is_string())
            return reply(res, bad_json);
        reply(res, run_command(jobs, qq ? "qq_bot" : "proxy", qq ? config.qq_actions : config.proxy_actions,
                               body.at("command").template get<std::string>()));
    });
#ifndef _WIN32
    std::unique_ptr<ConsoleSocket> local_socket;
    if (!socket.empty()) local_socket = std::make_unique<ConsoleSocket>(socket, jobs);
    std::jthread local_listener;
    if (local_socket) {
        local_listener = std::jthread([&] { local_socket->server.listen_after_bind(); });
        local_socket->server.wait_until_ready();
    }
#else
    if (!socket.empty()) throw std::runtime_error("private console sockets require Linux");
#endif
    std::jthread monitor([&](std::stop_token token) {
        while (!token.stop_requested() && !shutdown_requested) std::this_thread::sleep_for(50ms);
        if (token.stop_requested()) return;
        jobs.drain();
        while (!token.stop_requested() && !jobs.drained()) std::this_thread::sleep_for(50ms);
        if (!token.stop_requested()) server.stop();
    });
    audit("master_listening", {{"port", config.port}, {"http_workers", config.http_workers}});
    if (interactive) {
        bool clean = false;
        std::jthread listener([&] { clean = server.listen_after_bind(); });
        server.wait_until_ready();
        console(jobs);
        listener.join();
        return clean ? 0 : 1;
    }
    const bool clean = server.listen_after_bind();
    monitor.request_stop();
#ifndef _WIN32
    if (local_socket) {
        local_socket->server.stop();
        local_listener.join();
    }
#endif
    return clean ? 0 : 1;
}
}

int main(int argc, char** argv) {
#ifndef _WIN32
    umask(0077);
#endif
    const bool attach_console = argc == 3 && std::string_view(argv[1]) == "--attach";
    const bool interactive = argc == 4 && std::string_view(argv[3]) == "--console";
    const bool console_socket = argc == 5 && std::string_view(argv[3]) == "--console-socket";
    if ((!attach_console && argc != 3 && !interactive && !console_socket) ||
        (!attach_console && std::string_view(argv[1]) != "--config")) {
        std::cerr << "usage: luo-master --config FILE [--console | --console-socket PATH]\n"
                     "       luo-master --attach PATH\n";
        return 2;
    }
    std::signal(SIGINT, signal_stop);
    std::signal(SIGTERM, signal_stop);
    try {
        if (attach_console) {
#ifndef _WIN32
            return attach(luo::fs::path(argv[2]));
#else
            throw std::runtime_error("private console sockets require Linux");
#endif
        }
        return serve(luo::fs::path(argv[2]), interactive, console_socket ? luo::fs::path(argv[4]) : luo::fs::path{});
    }
    catch (const std::exception& e) {
        luo::audit("master_start_failed", {{"detail", e.what()}});
        return 1;
    }
}
