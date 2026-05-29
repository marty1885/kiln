#include "download_utils.hpp"
#include "../printing.hpp"
#include "../utils.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <curl/curl.h>
#include <archive.h>
#include <archive_entry.h>

namespace kiln {

// ── DownloadProgress ──────────────────────────────────────────────────

DownloadProgress::DownloadProgress(std::ostream* stream) : stream_(stream), is_tty_(stream && is_color_enabled(*stream)) {}

void DownloadProgress::apply(void* curl) {
    if (!stream_) return;
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, &DownloadProgress::curl_callback);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, this);
    if (!is_tty_) {
        *stream_ << "  0        10        20        30        40        50        60        70        80        90        100\n"
                 << "  |----+----|----+----|----+----|----+----|----+----|----+----|----+----|----+----|----+----|----+----|\n"
                 << "  " << std::flush;
    }
}

void DownloadProgress::finish() {
    if (!stream_) return;
    *stream_ << "\n";
}

int DownloadProgress::curl_callback(void* clientp, long long dltotal, long long dlnow, long long /*ultotal*/, long long /*ulnow*/) {
    auto* self = static_cast<DownloadProgress*>(clientp);
    if (dltotal > 0) {
        int pct = static_cast<int>(dlnow * 100 / dltotal);
        if (pct != self->last_pct_) {
            if (self->is_tty_) {
                *self->stream_ << "\r[download] " << pct << "% (" << dlnow << "/" << dltotal << " bytes)" << std::flush;
            } else {
                for (int p = self->last_pct_ + 1; p <= pct; ++p) { *self->stream_ << '*'; }
                self->stream_->flush();
            }
            self->last_pct_ = pct;
        }
    }
    return 0;
}

// ── download_url ──────────────────────────────────────────────────────

std::expected<void, std::string> download_url(const std::string& url, const std::string& dest_file, const std::string& hash_algo,
                                              const std::string& hash_value, std::ostream* progress_stream) {
    // If we have a hash and the file exists, check if it already matches
    if (!hash_algo.empty() && !hash_value.empty() && std::filesystem::exists(dest_file)) {
        std::ifstream existing(dest_file, std::ios::binary);
        if (existing) {
            std::string content((std::istreambuf_iterator<char>(existing)), std::istreambuf_iterator<char>());
            std::string existing_hash;
            if (hash_algo == "SHA256") {
                existing_hash = sha256(content).to_string();
            } else if (hash_algo == "MD5") {
                existing_hash = md5(content).to_string();
            }
            std::string normalized_expected = hash_value;
            std::transform(normalized_expected.begin(), normalized_expected.end(), normalized_expected.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            if (existing_hash == normalized_expected) { return {}; }
        }
    }

    struct DownloadData {
        std::string buffer;
    };
    DownloadData dl_data;

    auto write_callback = +[](char* ptr, size_t size, size_t nmemb, void* userdata) -> size_t {
        auto* data = static_cast<DownloadData*>(userdata);
        size_t total = size * nmemb;
        data->buffer.append(ptr, total);
        return total;
    };

    CURL* curl = curl_easy_init();
    if (!curl) { return std::unexpected<std::string>("Failed to initialize curl"); }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &dl_data);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    // Match CMake: set User-Agent to "curl/<version>"
    curl_version_info_data* cv = curl_version_info(CURLVERSION_FIRST);
    std::string ua = std::string("curl/") + (cv ? cv->version : LIBCURL_VERSION);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, ua.c_str());

    DownloadProgress progress(progress_stream);
    progress.apply(curl);

    CURLcode res = curl_easy_perform(curl);

    if (res == CURLE_OK) { progress.finish(); }

    curl_easy_cleanup(curl);

    if (res != CURLE_OK) { return std::unexpected<std::string>("Download failed for \"" + url + "\": " + curl_easy_strerror(res)); }

    // Verify hash
    if (!hash_algo.empty() && !hash_value.empty()) {
        std::string computed;
        if (hash_algo == "SHA256") {
            computed = sha256(dl_data.buffer).to_string();
        } else if (hash_algo == "MD5") {
            computed = md5(dl_data.buffer).to_string();
        } else {
            return std::unexpected<std::string>("Unsupported hash algorithm: " + hash_algo);
        }
        std::string normalized_expected = hash_value;
        std::transform(normalized_expected.begin(), normalized_expected.end(), normalized_expected.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (computed != normalized_expected) {
            return std::unexpected<std::string>("Hash mismatch for \"" + url + "\"\n  expected: " + normalized_expected
                                                + "\n  actual:   " + computed);
        }
    }

    // Write to file
    std::filesystem::path out_path(dest_file);
    std::filesystem::create_directories(out_path.parent_path());
    std::ofstream out(out_path, std::ios::binary);
    if (!out) { return std::unexpected<std::string>("Could not write to: " + dest_file); }
    out.write(dl_data.buffer.data(), static_cast<std::streamsize>(dl_data.buffer.size()));

    return {};
}

std::expected<void, std::string> extract_archive(const std::string& archive_path, const std::string& dest_dir) {
    if (!std::filesystem::exists(archive_path)) { return std::unexpected<std::string>("Archive does not exist: " + archive_path); }

    std::filesystem::create_directories(dest_dir);

    struct archive* a = archive_read_new();
    archive_read_support_format_all(a);
    archive_read_support_filter_all(a);

    int r = archive_read_open_filename(a, archive_path.c_str(), 16384);
    if (r != ARCHIVE_OK) {
        std::string err = archive_error_string(a);
        archive_read_free(a);
        return std::unexpected<std::string>("Could not open archive: " + archive_path + ": " + err);
    }

    struct archive* ext = archive_write_disk_new();
    int flags = ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_PERM | ARCHIVE_EXTRACT_ACL | ARCHIVE_EXTRACT_FFLAGS;
    archive_write_disk_set_options(ext, flags);
    archive_write_disk_set_standard_lookup(ext);

    struct archive_entry* entry;
    while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
        std::string pathname = archive_entry_pathname(entry);
        std::filesystem::path full_path = std::filesystem::path(dest_dir) / pathname;
        archive_entry_set_pathname(entry, full_path.string().c_str());

        r = archive_write_header(ext, entry);
        if (r != ARCHIVE_OK) {
            std::cerr << "warning: " << archive_error_string(ext) << "\n";
        } else if (archive_entry_size(entry) > 0) {
            const void* buff;
            size_t size;
            la_int64_t offset;
            while ((r = archive_read_data_block(a, &buff, &size, &offset)) == ARCHIVE_OK) {
                r = archive_write_data_block(ext, buff, size, offset);
                if (r != ARCHIVE_OK) {
                    std::cerr << "warning: " << archive_error_string(ext) << "\n";
                    break;
                }
            }
        }
        archive_write_finish_entry(ext);
    }

    archive_write_close(ext);
    archive_write_free(ext);
    archive_read_close(a);
    archive_read_free(a);

    return {};
}

std::expected<void, std::string> git_clone(const std::string& repo_url, const std::string& dest_dir, const std::string& tag, bool shallow) {
    // Strategy: try --branch first (works for branches/tags, efficient for shallow).
    // If that fails, fall back to full clone + checkout (works for commit hashes).

    if (!tag.empty()) {
        std::vector<std::string> cmd = {"git", "clone"};
        if (shallow) {
            cmd.push_back("--depth");
            cmd.push_back("1");
        }
        cmd.push_back("--branch");
        cmd.push_back(tag);
        cmd.push_back("--");
        cmd.push_back(repo_url);
        cmd.push_back(dest_dir);

        auto result = run_command(cmd);
        if (result.exit_code == 0) { return {}; }

        // --branch failed (likely a commit hash). Fall back to clone + checkout.
        // Clean up any partial clone first.
        std::error_code ec;
        std::filesystem::remove_all(dest_dir, ec);
    }

    // Clone without --branch (gets default branch)
    std::vector<std::string> cmd = {"git", "clone", "--", repo_url, dest_dir};
    auto result = run_command(cmd);
    if (result.exit_code != 0) { return std::unexpected<std::string>("git clone failed for " + repo_url + ":\n" + result.output); }

    // Checkout the specific ref if requested
    if (!tag.empty()) {
        auto checkout = run_command(std::vector<std::string>{"git", "-C", dest_dir, "checkout", tag});
        if (checkout.exit_code != 0) { return std::unexpected<std::string>("git checkout failed for " + tag + ":\n" + checkout.output); }
    }

    return {};
}

void replace_tokens(std::vector<std::string>& command, const std::vector<std::pair<std::string, std::string>>& replacements) {
    for (auto& arg : command) {
        for (const auto& [token, value] : replacements) { arg = kiln::replace_all(std::move(arg), token, value); }
    }
}

std::expected<void, std::string> run_steps(const std::vector<std::vector<std::string>>& commands, const std::string& working_dir) {
    for (const auto& cmd : commands) {
        if (cmd.empty()) continue;

        auto result = run_command(cmd, working_dir);
        if (result.exit_code != 0) {
            std::string cmd_str;
            for (const auto& a : cmd) {
                if (!cmd_str.empty()) cmd_str += ' ';
                cmd_str += a;
            }
            return std::unexpected<std::string>("Command failed: " + cmd_str + "\n" + result.output);
        }
    }
    return {};
}

} // namespace kiln
