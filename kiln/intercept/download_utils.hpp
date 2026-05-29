#pragma once
#include <string>
#include <vector>
#include <expected>
#include <ostream>

namespace kiln {

class Interpreter;

// Download progress display, handles both TTY and non-TTY output.
// TTY:     \r[download] 42% (1234/5678 bytes)  (in-place updates)
// non-TTY: ruler + '*' per percentage point     (CI-friendly, no VT100)
// Constructed with nullptr stream to disable progress entirely.
class DownloadProgress {
public:
    explicit DownloadProgress(std::ostream* stream);

    // Configure curl progress callbacks on the handle.
    // Prints the non-TTY ruler header if applicable.
    void apply(void* curl);

    // Print final newline after a successful download.
    void finish();

private:
    static int curl_callback(void* clientp, long long dltotal, long long dlnow, long long ultotal, long long ulnow);

    std::ostream* stream_;
    bool is_tty_;
    int last_pct_ = -1;
};

// Download a file from a URL, optionally verifying a hash.
// If progress_stream is non-null, show download progress.
// Returns error string on failure.
std::expected<void, std::string> download_url(const std::string& url, const std::string& dest_file,
                                              const std::string& hash_algo = "", // "SHA256", "MD5", or empty
                                              const std::string& hash_value = "", std::ostream* progress_stream = nullptr);

// Extract an archive to a destination directory using libarchive.
std::expected<void, std::string> extract_archive(const std::string& archive_path, const std::string& dest_dir);

// Clone a git repository.
std::expected<void, std::string> git_clone(const std::string& repo_url, const std::string& dest_dir, const std::string& tag = "",
                                           bool shallow = false);

// Replace <SOURCE_DIR>, <BINARY_DIR>, <INSTALL_DIR> tokens in command strings.
void replace_tokens(std::vector<std::string>& command, const std::vector<std::pair<std::string, std::string>>& replacements);

// Run a list of commands sequentially, aborting on failure.
std::expected<void, std::string> run_steps(const std::vector<std::vector<std::string>>& commands, const std::string& working_dir = "");

} // namespace kiln
