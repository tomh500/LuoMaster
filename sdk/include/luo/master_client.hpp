#pragma once

#include <luo/detail/http.hpp>
#include <nlohmann/json.hpp>

#include <string>

namespace luo::sdk {
struct Response {
    int status;
    nlohmann::json body;
};

// Use one client per thread. The endpoint is always the local IPv4 loopback.
class MasterClient {
public:
    MasterClient(int port, const std::string& token) : http_("127.0.0.1", port) {
        http_.set_connection_timeout(1);
        http_.set_read_timeout(2);
        http_.set_write_timeout(2);
        http_.set_default_headers({{"Authorization", "Bearer " + token}});
    }
    Response topology() { return unpack(http_.Get("/v1/topology")); }
    Response status(const std::string& id) { return unpack(http_.Get("/v1/backups/" + validate(id))); }
    Response submit(const std::string& id, const std::string& profile) {
        return unpack(http_.Post("/v1/backups", {{"Idempotency-Key", validate(id)}},
                                 nlohmann::json{{"profile", profile}}.dump(), "application/json"));
    }
    Response offer() { return unpack(http_.Get("/v1/backup-worker/job")); }
    Response resume(const std::string& id, const nlohmann::json& receipt) {
        return unpack(http_.Post("/v1/backups/" + validate(id) + "/resume", receipt.dump(), "application/json"));
    }
private:
    static const std::string& validate(const std::string& id) {
        if (id.empty() || id.size() > 64 || id.find_first_not_of(
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != std::string::npos)
            throw std::invalid_argument("invalid operation ID");
        return id;
    }
    static Response unpack(const httplib::Result& result) {
        if (!result) throw std::runtime_error("cannot reach local Master: " + httplib::to_string(result.error()));
        return {result->status, result->body.empty() ? nlohmann::json(nullptr) :
                    nlohmann::json::parse(result->body, nullptr, false)};
    }
    httplib::Client http_;
};
}
