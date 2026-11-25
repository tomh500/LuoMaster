#include "master.hpp"

#include <archive.h>
#include <archive_entry.h>
#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <memory>
#include <stdexcept>

namespace luo {
namespace {
class Digest {
public:
    Digest() {
        if (!ctx_ || EVP_DigestInit_ex(ctx_.get(), EVP_sha256(), nullptr) != 1)
            throw std::runtime_error("cannot initialize SHA-256");
    }
    void add(const void* data, std::size_t size) {
        if (EVP_DigestUpdate(ctx_.get(), data, size) != 1) throw std::runtime_error("cannot update SHA-256");
    }
    std::string finish() {
        std::array<unsigned char, 32> raw{};
        unsigned int length{};
        if (EVP_DigestFinal_ex(ctx_.get(), raw.data(), &length) != 1 || length != raw.size())
            throw std::runtime_error("cannot finalize SHA-256");
        static const char hex[] = "0123456789abcdef";
        std::string answer;
        for (unsigned char byte : raw) {
            answer += hex[byte >> 4];
            answer += hex[byte & 15];
        }
        return answer;
    }
private:
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx_{EVP_MD_CTX_new(), EVP_MD_CTX_free};
};

template<class Consume>
std::uintmax_t stream_file(const fs::path& path, Consume consume) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open selected file");
    std::array<char, 128 * 1024> buffer;
    std::uintmax_t bytes = 0;
    while (input.read(buffer.data(), buffer.size()) || input.gcount() > 0) {
        const auto length = static_cast<std::size_t>(input.gcount());
        consume(buffer.data(), length);
        bytes += length;
    }
    if (input.bad() || !input.eof()) throw std::runtime_error("cannot read selected file");
    return bytes;
}

void check(archive* handle, int result) {
    if (result != ARCHIVE_OK) {
        const auto* reason = archive_error_string(handle);
        throw std::runtime_error(reason ? reason : "ZIP library error");
    }
}

std::string utf8_name(const fs::path& path) {
    const auto name = path.generic_u8string();
    return std::string(name.begin(), name.end());
}

class ZipWriter {
public:
    ZipWriter(const Profile& profile, const fs::path& output, const CheckDeadline& deadline)
        : profile_(profile), deadline_(deadline) {
        if (!zip_) throw std::runtime_error("cannot allocate ZIP writer");
        check(zip_.get(), archive_write_set_format_zip(zip_.get()));
        check(zip_.get(), archive_write_set_format_option(zip_.get(), "zip", "hdrcharset", "UTF-8"));
        check(zip_.get(), archive_write_zip_set_compression_deflate(zip_.get()));
#ifdef _WIN32
        check(zip_.get(), archive_write_open_filename_w(zip_.get(), output.c_str()));
#else
        check(zip_.get(), archive_write_open_filename(zip_.get(), output.c_str()));
#endif
    }

    void walk(const fs::path& relative, unsigned depth = 0) {
        deadline_();
        if (std::ranges::any_of(profile_.exclude, [&](const auto& path) { return within(path, relative); }) ||
            relative.filename() == "session.lock") return;
        const auto path = profile_.source / relative;
        if (depth > 128 || entries_.size() >= 1000000 || is_link(path))
            throw std::runtime_error("selected tree contains a link or exceeds archive limits");
        const auto status = fs::symlink_status(path);
        if (fs::is_directory(status)) {
            add(relative, true, 0);
            for (const auto& child : fs::directory_iterator(path)) walk(relative / child.path().filename(), depth + 1);
            return;
        }
        if (!fs::is_regular_file(status) || fs::hard_link_count(path) != 1)
            throw std::runtime_error("selected path is missing or is an unsupported file");
        add(relative, false, fs::file_size(path));
    }

    void write() {
        if (is_link(profile_.source)) throw std::runtime_error("source directory became a link");
        for (const auto& relative : profile_.include) {
            // Includes can name a nested entry. Check intermediate components too.
            fs::path ancestor = profile_.source;
            for (const auto& part : relative) {
                ancestor /= part;
                if (is_link(ancestor)) throw std::runtime_error("backup include points through a link");
            }
            walk(relative);
        }
        if (entries_.empty()) throw std::runtime_error("backup selection is empty");
        check(zip_.get(), archive_write_close(zip_.get()));
    }

    void verify(const fs::path& path) {
        std::unique_ptr<archive, decltype(&archive_read_free)> reader(archive_read_new(), archive_read_free);
        if (!reader) throw std::runtime_error("cannot allocate ZIP reader");
        check(reader.get(), archive_read_support_format_zip(reader.get()));
#ifdef _WIN32
        check(reader.get(), archive_read_open_filename_w(reader.get(), path.c_str(), 128 * 1024));
#else
        check(reader.get(), archive_read_open_filename(reader.get(), path.c_str(), 128 * 1024));
#endif
        std::array<char, 128 * 1024> buffer;
        archive_entry* entry = nullptr;
        for (;;) {
            deadline_();
            const int status = archive_read_next_header(reader.get(), &entry);
            if (status == ARCHIVE_EOF) break;
            check(reader.get(), status);
            const char* name = archive_entry_pathname_utf8(entry);
            if (!name) throw std::runtime_error("invalid ZIP entry name");
            const auto expected = entries_.find(name);
            if (expected == entries_.end()) throw std::runtime_error("unexpected ZIP entry");
            const auto kind = archive_entry_filetype(entry);
            if (expected->second.empty() ? kind != AE_IFDIR : kind != AE_IFREG)
                throw std::runtime_error("ZIP entry type mismatch");
            if (kind == AE_IFREG) {
                Digest digest;
                for (;;) {
                    deadline_();
                    const auto count = archive_read_data(reader.get(), buffer.data(), buffer.size());
                    if (count < 0) throw std::runtime_error("cannot decompress ZIP entry");
                    if (count == 0) break;
                    digest.add(buffer.data(), static_cast<std::size_t>(count));
                }
                if (digest.finish() != expected->second) throw std::runtime_error("ZIP entry SHA-256 mismatch");
            }
            entries_.erase(expected);
        }
        if (!entries_.empty()) throw std::runtime_error("ZIP has missing entries");
        check(reader.get(), archive_read_close(reader.get()));
    }

private:
    void add(const fs::path& relative, bool directory, std::uintmax_t size) {
        auto name = utf8_name(relative);
        if (name.find_first_of("\\:\r\n") != std::string::npos ||
            size > static_cast<std::uintmax_t>(std::numeric_limits<la_int64_t>::max()))
            throw std::runtime_error("unsupported ZIP entry name or size");
        if (directory) name += '/';
        std::unique_ptr<archive_entry, decltype(&archive_entry_free)> entry(archive_entry_new(), archive_entry_free);
        if (!entry) throw std::runtime_error("cannot allocate ZIP entry");
        archive_entry_set_pathname_utf8(entry.get(), name.c_str());
        archive_entry_set_filetype(entry.get(), directory ? AE_IFDIR : AE_IFREG);
        archive_entry_set_perm(entry.get(), directory ? 0700 : 0600);
        archive_entry_set_size(entry.get(), static_cast<la_int64_t>(size));
        check(zip_.get(), archive_write_header(zip_.get(), entry.get()));
        std::string hash;
        if (!directory) {
            Digest digest;
            const auto bytes = stream_file(profile_.source / relative, [&](const char* data, std::size_t length) {
                deadline_();
                digest.add(data, length);
                if (archive_write_data(zip_.get(), data, length) != static_cast<la_ssize_t>(length))
                    throw std::runtime_error("cannot write ZIP entry");
            });
            if (bytes != size) throw std::runtime_error("selected file changed during backup");
            hash = digest.finish();
        }
        check(zip_.get(), archive_write_finish_entry(zip_.get()));
        if (!entries_.emplace(std::move(name), std::move(hash)).second)
            throw std::runtime_error("duplicate ZIP entry");
    }

    const Profile& profile_;
    const CheckDeadline& deadline_;
    std::unique_ptr<archive, decltype(&archive_write_free)> zip_{archive_write_new(), archive_write_free};
    std::map<std::string, std::string> entries_;
};
}

std::string sha256_text(const std::string& text) {
    Digest digest;
    digest.add(text.data(), text.size());
    return digest.finish();
}

fs::path staged_archive(const Config& config, const std::string& id) {
    return config.staging_dir / ("." + sha256_text(id) + ".part");
}

std::string sha256_file(const fs::path& path, const CheckDeadline& check_deadline) {
    Digest digest;
    stream_file(path, [&](const char* data, std::size_t size) {
        if (check_deadline) check_deadline();
        digest.add(data, size);
    });
    return digest.finish();
}

std::string archive_profile(const Profile& profile, const fs::path& output, const CheckDeadline& deadline) {
    if (fs::exists(output) || is_link(output)) throw std::runtime_error("staged archive path already exists");
    ZipWriter writer(profile, output, deadline);
    writer.write();
    writer.verify(output);
    sync_file(output);
    return sha256_file(output, deadline);
}
}
