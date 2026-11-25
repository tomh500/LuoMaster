#include "master.hpp"
#include <luo/master_client.hpp>

#include <atomic>
#include <clocale>
#include <csignal>
#include <iostream>
#include <thread>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace {
using namespace luo;
using namespace std::chrono_literals;
static_assert(std::atomic_bool::is_always_lock_free);
std::atomic_bool shutdown_requested{false};
void signal_stop(int) { shutdown_requested.store(true, std::memory_order_relaxed); }

void attempt(const Config& config, sdk::MasterClient& client) {
    std::string id;
    Json receipt;
    std::int64_t deadline_ms{};
    {
        // Acquire the fence before fetching a lease. After Master closes the offer,
        // a racing worker can no longer start archiving.
        const InstanceLock fence(config.staging_dir / ".archive-io.lock");
        const auto response = client.offer();
        if (response.status == 204) return;
        if (response.status != 200) throw std::runtime_error("worker offer request was rejected");
        const auto& offer = response.body;
        id = offer.at("id").get<std::string>();
        if (!valid_id(id)) throw std::runtime_error("invalid lease ID");
        deadline_ms = offer.at("deadline_ms").get<std::int64_t>();
        receipt = {{"lease", offer.at("lease")}, {"result", "failed"}};
        try {
            if (offer.at("generation") != config.generation) throw std::runtime_error("Master and worker configuration differ");
            const auto staged = staged_archive(config, id);
            fs::remove(staged);
            auto next_check = std::chrono::steady_clock::now();
            const auto local_deadline = next_check + std::chrono::milliseconds(std::max<std::int64_t>(0, deadline_ms - unix_ms()));
            const auto check = [&] {
                if (shutdown_requested || unix_ms() >= deadline_ms || std::chrono::steady_clock::now() >= local_deadline)
                    throw std::runtime_error("archive cancelled or lease expired");
                if (std::chrono::steady_clock::now() < next_check) return;
                next_check = std::chrono::steady_clock::now() + 500ms;
                const auto current = client.offer();
                if (current.status != 200 || current.body.at("id") != id || current.body.at("lease") != offer.at("lease"))
                    throw std::runtime_error("Master closed the archive offer");
            };
            check();
            const auto hash = archive_profile(config.profiles.at(offer.at("profile").get<std::string>()), staged, check);
#ifndef _WIN32
            if (config.staging_dir != config.archive_dir) {
                fs::permissions(staged, fs::perms::group_read | fs::perms::group_write, fs::perm_options::add);
                sync_file(staged);
            }
#endif
            receipt["sha256"] = hash;
            receipt["result"] = "verified";
            audit("worker_verified", {{"id", id}, {"sha256", receipt.at("sha256")}});
        } catch (const std::exception& e) {
            audit("worker_backup_failed", {{"id", id}, {"detail", e.what()}});
        }
    }
    // Retry the same receipt if the response is lost after Master commits it.
    for (unsigned retry = 0; retry != 5; ++retry) {
        try {
            const auto response = client.resume(id, receipt);
            if (response.status != 202) audit("worker_resume_rejected", {{"id", id}, {"status", response.status}});
            else audit("worker_resume_requested", {{"id", id}, {"result", receipt.at("result")}});
            return;
        } catch (const std::exception& e) {
            if (retry == 4 || unix_ms() >= deadline_ms) throw std::runtime_error(e.what());
            std::this_thread::sleep_for(250ms);
        }
    }
}
}

int main(int argc, char** argv) {
#ifdef _WIN32
    std::setlocale(LC_CTYPE, ".UTF-8");
#else
    std::setlocale(LC_CTYPE, "");
#endif
#ifndef _WIN32
    umask(0077);
#endif
    if (argc != 3 || std::string_view(argv[1]) != "--config") {
        std::cerr << "usage: luobackup-worker --config FILE\n";
        return 2;
    }
    std::signal(SIGINT, signal_stop);
    std::signal(SIGTERM, signal_stop);
    try {
        const auto config = luo::load_config(luo::fs::path(argv[2]), true);
        const luo::InstanceLock singleton(config.staging_dir / ".worker.lock");
        luo::sdk::MasterClient client(config.port, config.worker_token);
        luo::audit("backup_worker_started");
        bool healthy = true;
        while (!shutdown_requested) {
            try { attempt(config, client); healthy = true; }
            catch (const std::exception& e) {
                if (healthy) luo::audit("worker_poll_failed", {{"detail", e.what()}});
                healthy = false;
            }
            std::this_thread::sleep_for(250ms);
        }
        return 0;
    } catch (const std::exception& e) {
        luo::audit("worker_start_failed", {{"detail", e.what()}});
        return 1;
    }
}
