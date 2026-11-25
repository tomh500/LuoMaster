#include "jobs.hpp"

#include <sqlite3.h>
#include <openssl/rand.h>
#include <openssl/crypto.h>

#include <algorithm>
#include <array>

namespace luo {
using namespace std::chrono_literals;

namespace {
void sql_check(int code, sqlite3* db, int expected = SQLITE_OK) {
    if (code != expected) throw std::runtime_error(sqlite3_errmsg(db));
}

class Statement {
public:
    Statement(sqlite3* db, const char* sql) : db_(db) {
        sql_check(sqlite3_prepare_v2(db, sql, -1, &statement_, nullptr), db);
    }
    ~Statement() { sqlite3_finalize(statement_); }
    void bind(int column, const std::string& arg) {
        sql_check(sqlite3_bind_text(statement_, column, arg.c_str(), -1, SQLITE_TRANSIENT), db_);
    }
    void bind(int column, bool arg) {
        sql_check(sqlite3_bind_int(statement_, column, arg ? 1 : 0), db_);
    }
    void write() { sql_check(sqlite3_step(statement_), db_, SQLITE_DONE); }
    Json read() {
        const auto result = sqlite3_step(statement_);
        if (result == SQLITE_DONE) return nullptr;
        sql_check(result, db_, SQLITE_ROW);
        return Json::parse(reinterpret_cast<const char*>(sqlite3_column_text(statement_, 0)));
    }
private:
    sqlite3* db_;
    sqlite3_stmt* statement_ = nullptr;
};

Json public_job(const Json& job) {
    Json result;
    for (const auto key : {"id", "profile", "phase", "error", "created_at_ms", "updated_at_ms", "archive", "sha256"}) {
        if (job.contains(key)) result[key] = job.at(key);
    }
    return result;
}

Reply error(int code, const char* message) { return {code, {{"error", message}}}; }

std::string make_lease() {
    std::array<unsigned char, 32> random{};
    if (RAND_bytes(random.data(), static_cast<int>(random.size())) != 1)
        throw std::runtime_error("failed to generate backup lease");
    return sha256_text(std::string(reinterpret_cast<const char*>(random.data()), random.size()));
}

bool hex_digest(const std::string& hash) {
    return hash.size() == 64 && std::ranges::all_of(hash, [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
}

Jobs::Jobs(const Config& config) : config_(config), db_(nullptr, [](sqlite3* db) { sqlite3_close(db); }) {
    const auto database = config_.state_dir / "jobs.sqlite3";
    for (const auto& path : {database, config_.state_dir / "jobs.sqlite3-journal"}) {
        if (is_link(path)) throw std::runtime_error("journal path is a link");
    }
    sqlite3* db = nullptr;
    const auto filename = database.u8string();
    const int result = sqlite3_open_v2(reinterpret_cast<const char*>(filename.c_str()), &db,
                                       SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOFOLLOW, nullptr);
    db_.reset(db);
    sql_check(result, db);
    sql_check(sqlite3_exec(db, "PRAGMA journal_mode=DELETE; PRAGMA synchronous=FULL;"
                               "CREATE TABLE IF NOT EXISTS jobs(id TEXT PRIMARY KEY, done INTEGER NOT NULL, payload TEXT NOT NULL);"
                               "CREATE UNIQUE INDEX IF NOT EXISTS one_active_job ON jobs(done) WHERE done = 0;",
                           nullptr, nullptr, nullptr), db);
    runner_ = std::thread([this] { run(); });
}

Jobs::~Jobs() {
    drain();
    runner_.join();
}

void Jobs::save(const Json& job, bool done) {
    Statement record(db_.get(), "INSERT INTO jobs VALUES (?1, ?2, ?3) "
                               "ON CONFLICT(id) DO UPDATE SET done=excluded.done, payload=excluded.payload");
    record.bind(1, job.at("id").get<std::string>());
    record.bind(2, done);
    record.bind(3, job.dump());
    record.write();
}

Json Jobs::read(const char* sql, const std::string& id) {
    Statement query(db_.get(), sql);
    if (!id.empty()) query.bind(1, id);
    return query.read();
}

Json Jobs::active() { return read("SELECT payload FROM jobs WHERE done=0"); }

bool Jobs::worker_online() const { return std::chrono::steady_clock::now() - heartbeat_ < 5s; }
bool Jobs::ready() const { return ready_ && !draining_; }
bool Jobs::drained() const { return drained_; }

bool Jobs::end_if_idle() {
    std::lock_guard lock(mutex_);
    if (!active().is_null()) return false;
    drain();
    return true;
}

void Jobs::drain() {
    draining_ = true;
    changed_.notify_all();
}

Reply Jobs::submit(const std::string& id, const std::string& profile) {
    std::lock_guard lock(mutex_);
    const auto job = read("SELECT payload FROM jobs WHERE id=?1", id);
    if (!job.is_null()) {
        if (job.at("profile") != profile) return error(409, "idempotency_key_conflict");
        return {202, public_job(job)};
    }
    if (!config_.profiles.contains(profile)) return error(404, "unknown_profile");
    if (!ready()) return error(503, "master_unavailable");
    if (!active().is_null()) return error(409, "maintenance_in_progress");
    if (!worker_online()) return error(503, "backup_worker_unavailable");
    const auto now = unix_ms();
    Json request = {{"id", id}, {"profile", profile}, {"phase", "accepted"}, {"generation", config_.generation},
                    {"lease", make_lease()}, {"created_at_ms", now}, {"updated_at_ms", now}};
    save(request);
    ++accepted_;
    audit("backup_accepted", {{"id", id}, {"profile", profile}});
    changed_.notify_one();
    return {202, public_job(request)};
}

Reply Jobs::status(const std::string& id) {
    std::lock_guard lock(mutex_);
    const auto job = read("SELECT payload FROM jobs WHERE id=?1", id);
    return job.is_null() ? error(404, "unknown_job") : Reply{200, public_job(job)};
}

Reply Jobs::offer() {
    std::lock_guard lock(mutex_);
    heartbeat_ = std::chrono::steady_clock::now();
    const auto job = active();
    if (job.is_null() || job.at("phase") != "awaiting_backup" || unix_ms() >= job.at("deadline_ms").get<std::int64_t>())
        return {204, nullptr};
    return {200, {{"id", job.at("id")}, {"profile", job.at("profile")}, {"lease", job.at("lease")},
                  {"deadline_ms", job.at("deadline_ms")}, {"generation", job.at("generation")}}};
}

Reply Jobs::resume(const std::string& id, const Json& receipt) {
    if (!receipt.is_object() || !receipt.contains("lease") || !receipt.at("lease").is_string() ||
        !receipt.contains("result") || !receipt.at("result").is_string()) return error(400, "invalid_receipt");
    const auto result = receipt.at("result").get<std::string>();
    if (result == "verified") {
        if (receipt.size() != 3 || !receipt.contains("sha256") || !receipt.at("sha256").is_string() ||
            !hex_digest(receipt.at("sha256").get<std::string>())) return error(400, "invalid_checksum");
    } else if (result != "failed" || receipt.size() != 2) {
        return error(400, "invalid_receipt");
    }
    std::lock_guard lock(mutex_);
    auto job = read("SELECT payload FROM jobs WHERE id=?1", id);
    if (job.is_null()) return error(404, "unknown_job");
    const auto lease = receipt.at("lease").get<std::string>();
    const auto expected = job.at("lease").get<std::string>();
    if (lease.size() != expected.size() || CRYPTO_memcmp(lease.data(), expected.data(), expected.size()) != 0)
        return error(409, "invalid_lease");
    if (job.contains("receipt")) {
        return job.at("receipt") == receipt ? Reply{202, public_job(job)} : error(409, "receipt_conflict");
    }
    if (job.at("phase") != "awaiting_backup" || unix_ms() >= job.at("deadline_ms").get<std::int64_t>())
        return error(409, "lease_closed");
    job["receipt"] = receipt;
    record(job, "resume_requested");
    changed_.notify_one();
    return {202, public_job(job)};
}

Json Jobs::topology() {
    std::lock_guard lock(mutex_);
    Json nodes = Json::array();
    for (const auto& node : config_.nodes) nodes.push_back({{"id", node.id}, {"depends_on", node.dependencies}});
    const auto job = active();
    return {{"nodes", nodes}, {"maintenance", job.is_null() ? Json(nullptr) : public_job(job)},
            {"worker_online", worker_online()}};
}

std::string Jobs::metrics() {
    std::lock_guard lock(mutex_);
    return "# TYPE luo_master_ready gauge\nluo_master_ready " + std::to_string(ready()) +
        "\n# TYPE luo_master_worker_online gauge\nluo_master_worker_online " + std::to_string(worker_online()) +
        "\n# TYPE luo_master_backup_total counter\nluo_master_backup_total{result=\"accepted\"} " + std::to_string(accepted_) +
        "\nluo_master_backup_total{result=\"succeeded\"} " + std::to_string(completed_) +
        "\nluo_master_backup_total{result=\"failed\"} " + std::to_string(failed_) +
        "\nluo_master_backup_total{result=\"recovery_required\"} " + std::to_string(recovery_failed_) + '\n';
}

void Jobs::record(Json& job, const char* stage, const char* error_code) {
    job["phase"] = stage;
    job["updated_at_ms"] = unix_ms();
    if (*error_code) job["error"] = error_code;
    const bool done = job.at("phase") == "succeeded" || job.at("phase") == "failed";
    save(job, done);
    audit("backup_phase", public_job(job));
}

void Jobs::stop_topology(Json& job) {
    auto snapshot = Json::object();
    for (const auto& node : config_.nodes) {
        const int code = lifecycle(config_, "status", node);
        if (code != 0 && code != 3) throw std::runtime_error("unknown initial status: " + node.id);
        snapshot[node.id] = (code == 0);
    }
    {
        std::lock_guard lock(mutex_);
        job["snapshot"] = snapshot;
        record(job, "stopping");
    }
    for (auto node = config_.nodes.rbegin(); node != config_.nodes.rend(); ++node) {
        if (!snapshot.at(node->id).get<bool>()) continue;
        if (node->save && lifecycle(config_, "save", *node) != 0)
            throw std::runtime_error("save failed: " + node->id);
        if (lifecycle(config_, "stop", *node) != 0)
            throw std::runtime_error("stop failed: " + node->id);
    }
    for (const auto& node : config_.nodes) {
        if (lifecycle(config_, "status", node) != 3) throw std::runtime_error("node still active: " + node.id);
    }
    std::lock_guard lock(mutex_);
    job["deadline_ms"] = unix_ms() + std::chrono::duration_cast<std::chrono::milliseconds>(config_.archive_timeout).count();
    record(job, "awaiting_backup");
}

void Jobs::restore_topology(const Json& job) {
    if (!job.contains("snapshot")) return;
    for (const auto& node : config_.nodes) {
        const int code = lifecycle(config_, "status", node);
        if (job.at("snapshot").at(node.id).get<bool>()) {
            if (code == 0) continue;
            if (code != 3 || lifecycle(config_, "start", node) != 0 || lifecycle(config_, "status", node) != 0)
                throw std::runtime_error("cannot confirm node restored: " + node.id);
        } else if (code != 3) {
            throw std::runtime_error("cannot confirm originally stopped node is down: " + node.id);
        }
    }
}

void Jobs::recover(Json& job) {
    try {
        if (job.at("generation") != config_.generation)
            throw std::runtime_error("operation configuration changed, restore original configuration");
        {
            std::lock_guard lock(mutex_);
            record(job, "restoring"); // Close the offer before waiting for any archive writes to finish.
        }
        std::unique_ptr<InstanceLock> archive_lock;
        const auto lock_deadline = std::chrono::steady_clock::now() + 5s;
        while (!archive_lock) {
            try { archive_lock = std::make_unique<InstanceLock>(config_.staging_dir / ".archive-io.lock"); }
            catch (const std::exception&) {
                if (std::chrono::steady_clock::now() >= lock_deadline) throw;
                std::this_thread::sleep_for(50ms);
            }
        }
        const auto id = job.at("id").get<std::string>();
        const auto staged = staged_archive(config_, id);
        const auto final = config_.archive_dir / ("backup-" + sha256_text(id) + ".zip");
        auto outcome = job.value("error", std::string{});
        std::string hash;
        if (job.contains("receipt") && job.at("receipt").at("result") == "verified") {
            hash = job.at("receipt").at("sha256").get<std::string>();
            try {
                const auto& candidate = fs::exists(final) ? final : staged;
                if (is_link(candidate) || !fs::is_regular_file(candidate) || sha256_file(candidate) != hash)
                    throw std::runtime_error("staged archive digest mismatch");
            } catch (const std::exception& e) {
                audit("archive_verification_failed", {{"id", id}, {"detail", e.what()}});
                outcome = "archive_verification_failed";
            }
        } else if (outcome.empty()) {
            outcome = job.contains("receipt") ? "archive_failed" : "interrupted";
        }
        restore_topology(job);
        if (outcome.empty()) {
            if (!fs::exists(final)) publish(staged, final);
#ifndef _WIN32
            sync_file(final.parent_path());
#endif
            job["archive"] = final.filename().string();
            job["sha256"] = hash;
        }
        fs::remove(staged);
        std::lock_guard lock(mutex_);
        if (outcome.empty()) {
            job.erase("error");
            record(job, "succeeded");
            ++completed_;
        } else {
            record(job, "failed", outcome.c_str());
            ++failed_;
        }
        ready_ = true;
    } catch (const std::exception& e) {
        ready_ = false;
        ++recovery_failed_;
        audit("topology_recovery_failed", {{"id", job.at("id")}, {"detail", e.what()}});
        std::lock_guard lock(mutex_);
        record(job, "recovery_required");
    }
}

void Jobs::run() {
    try {
        std::unique_lock lock(mutex_);
        auto job = active();
        lock.unlock();
        if (!job.is_null()) recover(job);
        else ready_ = true;
        lock.lock();
        while (!draining_ || ready_) {
            job = active();
            if (job.is_null() || job.at("phase") != "accepted") {
                if (draining_) break;
                changed_.wait_for(lock, 250ms);
                continue;
            }
            lock.unlock();
            try {
                stop_topology(job);
                const auto deadline = std::chrono::steady_clock::now() + config_.archive_timeout;
                lock.lock();
                changed_.wait_until(lock, deadline, [&] {
                    job = active();
                    return job.at("phase") == "resume_requested";
                });
                if (!job.contains("receipt")) {
                    job["error"] = "archive_timeout";
                    record(job, "restoring");
                }
                lock.unlock();
            } catch (const std::exception& e) {
                if (lock.owns_lock()) lock.unlock();
                job["error"] = "stop_or_journal_failed";
                audit("topology_stop_failed", {{"id", job.at("id")}, {"detail", e.what()}});
            }
            recover(job);
            lock.lock();
        }
    } catch (const std::exception& e) {
        audit("coordinator_failed", {{"detail", e.what()}});
        ready_ = false;
    }
    drained_ = true;
}
}
