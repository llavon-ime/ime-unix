#include "commit_store.hpp"
#include "libtorch_cache.hpp"
#include "lora_presets.hpp"
#include "numeric_dataset.hpp"
#include "sqlite.hpp"

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <limits>
#include <set>
#include <sstream>
#include <thread>
#include <unordered_set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <vector>


namespace fs = std::filesystem;

namespace {

using Options = std::map<std::string, std::string>;

Options parse(int argc, char** argv) {
    Options options;
    for (int i = 2; i < argc; ++i) {
        const std::string name = argv[i];
        if (!name.starts_with("--") || i + 1 >= argc || !options.emplace(name, argv[i + 1]).second)
            throw std::invalid_argument("expected unique --option value pairs");
        ++i;
    }
    return options;
}

std::string require(const Options& options, const char* key) {
    const auto found = options.find(key);
    if (found == options.end() || found->second.empty()) throw std::invalid_argument(std::string("missing ") + key);
    return found->second;
}

std::string optional(const Options& options, const char* key, const char* fallback) {
    const auto found = options.find(key);
    return found == options.end() ? fallback : found->second;
}

int integer_option(const Options& options, const char* key, int fallback, int minimum, int maximum) {
    const auto text = optional(options, key, "");
    if (text.empty()) return fallback;
    int value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc() || parsed.ptr != text.data() + text.size() || value < minimum || value > maximum)
        throw std::invalid_argument(std::string("invalid ") + key);
    return value;
}

double real_option(const Options& options, const char* key, double fallback, double minimum, double maximum) {
    const auto text = optional(options, key, "");
    if (text.empty()) return fallback;
    char* end = nullptr;
    const double value = std::strtod(text.c_str(), &end);
    if (end != text.c_str() + text.size() || !std::isfinite(value) || value < minimum || value > maximum)
        throw std::invalid_argument(std::string("invalid ") + key);
    return value;
}

// Passwords never travel in argv, where any local process could read them.
// They come from an environment variable, a private file, or the terminal.
std::string password_option(const Options& options) {
    if (options.contains("--password-env")) {
        const char* value = std::getenv(require(options, "--password-env").c_str());
        if (value == nullptr || *value == '\0') throw std::runtime_error("password environment variable is not set");
        return value;
    }
    if (options.contains("--password-file")) {
        std::ifstream input(require(options, "--password-file"));
        if (!input) throw std::runtime_error("cannot read password file");
        std::string line;
        std::getline(input, line);
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (line.empty()) throw std::runtime_error("password file is empty");
        return line;
    }
    if (options.contains("--password-fd")) {
        const auto text = require(options, "--password-fd");
        int fd = 0;
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), fd);
        if (parsed.ec != std::errc() || parsed.ptr != text.data() + text.size() || fd < 3)
            throw std::invalid_argument("invalid --password-fd");
        std::string line;
        char ch = 0;
        while (true) {
            const auto count = ::read(fd, &ch, 1);
            if (count == 1) { if (ch == '\n') break; line.push_back(ch); continue; }
            if (count < 0 && errno == EINTR) continue;
            break;
        }
        ::close(fd);
        if (line.empty()) throw std::runtime_error("password was not provided");
        return line;
    }
    if (::isatty(STDIN_FILENO)) {
        std::cerr << "password: " << std::flush;
        termios original{};
        const bool hidden = ::tcgetattr(STDIN_FILENO, &original) == 0;
        if (hidden) {
            termios masked = original;
            masked.c_lflag &= ~static_cast<tcflag_t>(ECHO);
            ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &masked);
        }
        std::string line;
        std::getline(std::cin, line);
        if (hidden) ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &original);
        std::cerr << '\n';
        if (line.empty()) throw std::runtime_error("password required");
        return line;
    }
    throw std::runtime_error("password required; pass --password-env or --password-file");
}

// A locked cipher still serves legacy plaintext rows; a configured password
// always demands a matching one.
void unlock(ime::unix_service::CommitCipher& cipher, sqlite3* db, const Options& options) {
    if (ime::unix_service::read_commit_protection(db).configured) cipher.unlock(db, password_option(options));
}

std::string as_argument(double value) {
    std::ostringstream output;
    output << std::setprecision(17) << value;
    return output.str();
}

bool has_pinned_trainer_stamp(const fs::path& executable) {
    if (!fs::is_regular_file(executable)) return false;
    try {
        return nlohmann::json::parse(std::ifstream(executable.parent_path() / "trainer-release.json")).at("commit")
               == LLAVON_IME_LORA_PINNED_COMMIT;
    } catch (...) { return false; }
}

// The trainer archive is a directory of the executable plus TorchSharp's
// native libraries. Installing only the executable leaves TorchSharp failing
// at initialization, so every candidate must carry them.
bool has_trainer_libraries(const fs::path& directory, int depth = 0) {
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(directory, error)) {
        const auto name = entry.path().filename().string();
        if (name.ends_with(".so") || name.find(".so.") != std::string::npos || name.ends_with(".dylib")) return true;
        if (depth < 3 && entry.is_directory() && has_trainer_libraries(entry.path(), depth + 1)) return true;
    }
    return false;
}

bool trainer_usable(const fs::path& executable) {
    return fs::is_regular_file(executable) && has_pinned_trainer_stamp(executable) &&
           has_trainer_libraries(executable.parent_path());
}

// The release pinned by the submodule is installed either by the GUI/CLI into
// the per-user state directory or by the install scripts into the system
// directory. Prefer whichever carries the matching release stamp, then fall
// back to the per-user path so its update flow stays the default.
fs::path installed_trainer(const fs::path& db_path) {
    if (const char* override = std::getenv("LLAVON_IME_LORA_CLI_PATH"); override && *override)
        return fs::absolute(override);
    const fs::path managed = db_path.parent_path() / "tools" / "lora" / "llavon-lora";
    const fs::path system = LLAVON_IME_INSTALLED_LORA_TRAINER_PATH;
    if (trainer_usable(managed)) return managed;
    if (trainer_usable(system)) return system;
    return fs::is_regular_file(managed) ? managed : system;
}

class Database {
public:
    explicit Database(const fs::path& path)
        : db_(ime::unix_service::sqlite::open(path, SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX)) {
        sqlite3_busy_timeout(db_.get(), 3000);
        exec("PRAGMA foreign_keys=ON");
        ime::unix_service::initialize_commit_database(db_.get());
    }
    sqlite3* get() const { return db_.get(); }
    void exec(const char* sql) {
        if (sqlite3_exec(db_.get(), sql, nullptr, nullptr, nullptr) != SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(db_.get()));
    }
private:
    ime::unix_service::sqlite::Database db_;
};

class Statement {
public:
    Statement(sqlite3* db, const char* sql) : db_(db), stmt_(ime::unix_service::sqlite::prepare(db, sql)) {}
    void bind(int index, const std::string& value) {
        if (sqlite3_bind_text(stmt_.get(), index, value.c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(db_));
    }
    int next() {
        const auto status = sqlite3_step(stmt_.get());
        if (status != SQLITE_ROW && status != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db_));
        return status;
    }
    const char* text(int index) const { return reinterpret_cast<const char*>(sqlite3_column_text(stmt_.get(), index)); }
    sqlite3_stmt* get() const { return stmt_.get(); }
private:
    sqlite3* db_;
    ime::unix_service::sqlite::Statement stmt_;
};

void run(const fs::path& program, const std::vector<std::string>& args) {
    if (!fs::is_regular_file(program)) throw std::runtime_error("trainer executable not found: " + program.string());
    const pid_t pid = ::fork();
    if (pid < 0) throw std::runtime_error("cannot fork trainer");
    if (pid == 0) {
        std::vector<std::string> owned{program.string()};
        owned.insert(owned.end(), args.begin(), args.end());
        std::vector<char*> argv;
        for (auto& value : owned) argv.push_back(value.data());
        argv.push_back(nullptr);
        ::execv(argv[0], argv.data());
        _exit(127);
    }
    int status = 0;
    while (::waitpid(pid, &status, 0) == -1) {
        if (errno != EINTR) throw std::runtime_error("cannot wait for trainer");
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        throw std::runtime_error("trainer failed (exit status " +
                                 std::to_string(WIFEXITED(status) ? WEXITSTATUS(status) : -1) + ")");
}

void run_tool(const char* program, const std::vector<std::string>& args, const fs::path& stdout_file = {}) {
    const pid_t pid = ::fork();
    if (pid < 0) throw std::runtime_error("cannot fork external tool");
    if (pid == 0) {
        if (!stdout_file.empty()) {
            const int fd = ::open(stdout_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
            if (fd < 0 || ::dup2(fd, STDOUT_FILENO) < 0) _exit(127);
            ::close(fd);
        }
        std::vector<std::string> owned{program};
        owned.insert(owned.end(), args.begin(), args.end());
        std::vector<char*> argv;
        for (auto& value : owned) argv.push_back(value.data());
        argv.push_back(nullptr);
        ::execvp(program, argv.data());
        _exit(127);
    }
    int status;
    while (::waitpid(pid, &status, 0) == -1) {
        if (errno != EINTR) throw std::runtime_error("cannot wait for external tool");
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        throw std::runtime_error(std::string(program) + " failed");
}

std::string sha256_hex(const fs::path& file) {
    auto hash_file = file; hash_file += ".sha256";
    try {
#ifdef __APPLE__
        run_tool("shasum", {"-a", "256", file.string()}, hash_file);
#else
        run_tool("sha256sum", {file.string()}, hash_file);
#endif
        std::ifstream input(hash_file);
        std::string actual;
        input >> actual;
        fs::remove(hash_file);
        return actual;
    } catch (...) { fs::remove(hash_file); throw; }
}

bool sha256_matches(const fs::path& file, const std::string& expected) {
    return sha256_hex(file) == expected;
}

// Downloaded trainer archives are kept so repeat installs (packaging, CI, or
// another target directory) do not fetch the 158 MB release again. The archive
// is still verified against the manifest SHA-256 before it is reused.
fs::path trainer_cache_dir() {
    if (const char* override = std::getenv("LLAVON_IME_LORA_TRAINER_CACHE"); override && *override)
        return fs::absolute(override);
    if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg && *xdg)
        return fs::path(xdg) / "llavon-ime" / "lora-trainer";
    const char* home = std::getenv("HOME");
    if (!home || !*home) return {};
    return fs::path(home) / ".cache" / "llavon-ime" / "lora-trainer";
}

bool archived_trainer(const fs::path& archive, const std::string& hash, std::uint64_t size) {
    return fs::is_regular_file(archive) && fs::file_size(archive) == size && sha256_matches(archive, hash);
}

// The rolling `latest` manifest only tracks the newest trainer release, so a
// pinned submodule commit falls off it as soon as the trainer cuts a newer
// version. Look the release that actually declares the pinned commit up
// through the public release listing instead of waiting for a manifest that
// will never match. The immutable manifest is still the authority: the caller
// re-validates the version, commit, asset name, and SHA-256.
nlohmann::json trainer_release_for_commit(const fs::path& output, std::string_view expected_commit) {
    const auto listing = output / "releases.json.partial";
    try {
        run_tool("curl", {"--fail", "--location", "--retry", "3",
                          "--header", "Accept: application/vnd.github+json",
                          "--header", "Cache-Control: no-cache",
                          "--output", listing.string(),
                          "https://api.github.com/repos/llavon-ime/lora-trainer/releases?per_page=50"});
        const auto releases = nlohmann::json::parse(std::ifstream(listing));
        fs::remove(listing);
        if (!releases.is_array()) return nlohmann::json::object();
        for (const auto& release : releases) {
            const auto target = release.value("target_commitish", "");
            if (target.empty() || std::string_view(target).rfind(expected_commit, 0) != 0) continue;
            const auto tag = release.value("tag_name", "");
            if (tag.rfind("v", 0) != 0 || tag.size() < 2 ||
                tag.find_first_not_of("0123456789.", 1) != std::string::npos) continue;
            return nlohmann::json{{"schema", 1}, {"trainerApi", 1}, {"version", tag.substr(1)},
                                  {"commit", std::string(expected_commit)}};
        }
    } catch (...) {
        // The listing is an optimisation for pinned commits that fell off the
        // rolling manifest; a rate limit or network error still produces the
        // caller's pinned-commit diagnosis.
    }
    fs::remove(listing);
    return nlohmann::json::object();
}

void check_model(const fs::path& output) {
    fs::create_directories(output);
    const auto metadata = output / "metadata.partial";
    try {
        run_tool("curl", {"--fail", "--location", "--retry", "3", "--output", metadata.string(),
                          "https://huggingface.co/api/models/tony65535/llavon-ime-llama-250m/revision/main"});
        const auto remote = nlohmann::json::parse(std::ifstream(metadata)).at("sha").get<std::string>();
        if (remote.size() != 40 || remote.find_first_not_of("0123456789abcdef") != std::string::npos)
            throw std::runtime_error("invalid Hugging Face model revision");
        std::string local;
        std::ifstream current(output / "current.revision");
        current >> local;
        const bool complete = local.size() == 40 && fs::is_regular_file(output / local / "config.json") &&
            fs::is_regular_file(output / local / "ime_vocab.json") &&
            fs::is_regular_file(output / local / "model.safetensors");
        std::cout << "revision=" << remote << " installed=" << (complete ? local : "none")
                  << " update-available=" << (!complete || local != remote ? "true" : "false") << '\n';
    } catch (...) { fs::remove(metadata); throw; }
    fs::remove(metadata);
}

// Prefer the immutable commit manifest. The version-tagged manifest and its
// asset checksum are still re-validated before installing anything.
nlohmann::json fetch_pinned_candidate(const fs::path& output, std::string_view expected_commit) {
    const auto manifest_file = output / "commit-release.json.partial";
    try {
        // The immutable commit tag remains available after `latest` moves and
        // does not consume the anonymous GitHub API quota shared by AUR/CI.
        run_tool("curl", {"--fail", "--location", "--retry", "3", "--output", manifest_file.string(),
                          "https://github.com/llavon-ime/lora-trainer/releases/download/commit-" +
                          std::string(expected_commit) + "/latest.json"});
        auto candidate = nlohmann::json::parse(std::ifstream(manifest_file));
        fs::remove(manifest_file);
        if (candidate.is_object() && candidate.value("commit", "") == expected_commit) return candidate;
    } catch (...) {}
    fs::remove(manifest_file);
    return nlohmann::json::object();
}

// Compatibility fallback for older trainer releases without a commit tag.
nlohmann::json fetch_rolling_candidate(const fs::path& output, std::string_view expected_commit, int attempt) {
    const auto manifest_file = output / "release.json.partial";
    try {
        run_tool("curl", {"--fail", "--location", "--retry", "3", "--header", "Cache-Control: no-cache",
                          "--output", manifest_file.string(),
                          "https://github.com/llavon-ime/lora-trainer/releases/download/latest/latest.json?commit=" +
                          std::string(expected_commit) + "&attempt=" + std::to_string(attempt)});
        auto candidate = nlohmann::json::parse(std::ifstream(manifest_file));
        fs::remove(manifest_file);
        if (candidate.is_object() && candidate.value("commit", "") == expected_commit) return candidate;
    } catch (...) {}
    fs::remove(manifest_file);
    return nlohmann::json::object();
}

// Reports the installed trainer and the release of the pinned commit without
// changing anything, so the manager page can show the version and whether an
// update is available (the same fields the Windows manager exposes).
void check_trainer(const fs::path& output) {
    fs::create_directories(output);
    constexpr std::string_view expected_commit = LLAVON_IME_LORA_PINNED_COMMIT;
    const bool usable = trainer_usable(output / "llavon-lora");
    std::string installed_version, installed_commit;
    try {
        const auto stamp = nlohmann::json::parse(std::ifstream(output / "trainer-release.json"));
        installed_version = stamp.value("version", "");
        installed_commit = stamp.value("commit", "");
    } catch (...) {}
    auto candidate = fetch_pinned_candidate(output, expected_commit);
    if (candidate.value("commit", "") != expected_commit)
        candidate = fetch_rolling_candidate(output, expected_commit, 1);
    if (candidate.value("commit", "") != expected_commit)
        candidate = trainer_release_for_commit(output, expected_commit);
    const std::string release_version =
        candidate.value("commit", "") == expected_commit ? candidate.value("version", "") : std::string{};
    const bool update = !usable || installed_commit != expected_commit ||
                        (!release_version.empty() && installed_version != release_version);
    std::cout << "installed=" << (usable ? "true" : "false")
              << " version=" << (installed_version.empty() ? "none" : installed_version)
              << " commit=" << (installed_commit.empty() ? "none" : installed_commit)
              << " release=" << (release_version.empty() ? "unknown" : release_version)
              << " update-available=" << (update ? "true" : "false") << '\n';
}

void install_trainer(const fs::path& output) {
    // Every platform declares the name so the function still compiles where no
    // release exists (macOS x86_64); the check below reports those.
    constexpr std::string_view platform =
#if defined(__linux__) && defined(__x86_64__)
        "linux-x64-cpu";
#elif defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__))
        "osx-arm64-cpu";
#else
        "";
#endif
    if (platform.empty())
        throw std::runtime_error("LoRA Trainer release is unavailable for this platform");
    fs::create_directories(output);
    const auto manifest_file = output / "release.json.partial";
    const auto pinned_manifest_file = output / "pinned-release.json.partial";
    const auto archive = output / "trainer.tar.gz.partial";
    const auto staging = output / "trainer.staging";
    fs::remove_all(staging);
    try {
        constexpr std::string_view expected_commit = LLAVON_IME_LORA_PINNED_COMMIT;
        // An intact installation of the pinned release is left alone: the
        // stamp records the executable SHA-256, so a repeat install neither
        // downloads nor re-extracts anything. A stamp written before hashes
        // were recorded is upgraded in place instead of re-downloading.
        try {
            const auto stamp_path = output / "trainer-release.json";
            const auto stamp = nlohmann::json::parse(std::ifstream(stamp_path));
            const auto installed = output / "llavon-lora";
            if (stamp.at("commit") == expected_commit && fs::is_regular_file(installed) && has_trainer_libraries(output)) {
                const auto hash = sha256_hex(installed);
                const auto recorded = stamp.contains("sha256") ? stamp.at("sha256").get<std::string>() : std::string{};
                if (recorded.empty() || recorded == hash) {
                    if (recorded.empty()) {
                        std::ofstream file(stamp_path, std::ios::trunc);
                        file << nlohmann::json{{"version", stamp.at("version")}, {"commit", stamp.at("commit")},
                                               {"sha256", hash}}.dump() << '\n';
                    }
                    std::cout << "trainer=" << installed << " version=" << stamp.at("version").get<std::string>()
                              << " already-installed=true\n";
                    return;
                }
            }
        } catch (...) {}
        int attempts = 30;
        if (const char* setting = std::getenv("LLAVON_IME_LORA_RELEASE_ATTEMPTS")) {
            const std::string value(setting);
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), attempts);
            if (parsed.ec != std::errc() || parsed.ptr != value.data() + value.size() || attempts < 1 || attempts > 30)
                throw std::invalid_argument("invalid release retry count");
        }
        auto candidate = fetch_pinned_candidate(output, expected_commit);
        for (int attempt = 1; candidate.value("commit", "") != expected_commit && attempt <= attempts; ++attempt) {
            candidate = fetch_rolling_candidate(output, expected_commit, attempt);
            if (candidate.value("commit", "") == expected_commit) break;
            std::cerr << "Waiting for LoRA Trainer release of pinned commit " << expected_commit << '\n';
            // The pinned commit falls off the rolling manifest as soon as the
            // trainer cuts a newer release, so consult the release listing
            // before sleeping on a manifest that may never match again.
            if (attempt == 1 || attempt % 5 == 0) {
                candidate = trainer_release_for_commit(output, expected_commit);
                if (candidate.value("commit", "") == expected_commit) break;
            }
            if (attempt < attempts) std::this_thread::sleep_for(std::chrono::seconds(30));
        }
        if (!candidate.is_object() || candidate.value("commit", "") != expected_commit)
            candidate = trainer_release_for_commit(output, expected_commit);
        if (!candidate.is_object() || candidate.value("commit", "") != expected_commit)
            throw std::runtime_error("no LoRA Trainer release for pinned submodule commit " + std::string(expected_commit));
        if (candidate.at("schema") != 1 || candidate.at("trainerApi") != 1)
            throw std::runtime_error("unsupported trainer release manifest");
        const std::string version = candidate.at("version").get<std::string>();
        run_tool("curl", {"--fail", "--location", "--retry", "3", "--output", pinned_manifest_file.string(),
                          "https://github.com/llavon-ime/lora-trainer/releases/download/v" + version + "/latest.json"});
        const auto manifest = nlohmann::json::parse(std::ifstream(pinned_manifest_file));
        if (manifest.at("schema") != 1 || manifest.at("trainerApi") != 1 ||
            manifest.at("commit") != expected_commit || manifest.at("version") != version)
            throw std::runtime_error("immutable trainer release differs from pinned submodule commit");
        if (version.empty() || version.find_first_not_of("0123456789.") != std::string::npos)
            throw std::runtime_error("invalid trainer release version");
        const auto& asset = manifest.at("assets").at(std::string(platform));
        const std::string name = asset.at("name").get<std::string>();
        const std::string url = asset.at("url").get<std::string>();
        const std::string hash = asset.at("sha256").get<std::string>();
        if (name != "llavon-lora-" + version + "-" + std::string(platform) + ".tar.gz" ||
            url != "https://github.com/llavon-ime/lora-trainer/releases/download/v" + version + "/" + name ||
            hash.size() != 64 || hash.find_first_not_of("0123456789abcdef") != std::string::npos)
            throw std::runtime_error("invalid trainer release asset");
        const auto cache = trainer_cache_dir();
        const auto cached = cache.empty() ? fs::path{} : cache / name;
        const std::uint64_t size = asset.at("size").get<std::uint64_t>();
        // Reuse a verified archive when one is cached; only a cache miss
        // downloads the release again.
        fs::path source_archive = archive;
        if (!cached.empty() && archived_trainer(cached, hash, size)) {
            source_archive = cached;
        } else {
            run_tool("curl", {"--fail", "--location", "--retry", "3", "--output", archive.string(), url});
            if (!archived_trainer(archive, hash, size))
                throw std::runtime_error("trainer download failed checksum verification");
            if (!cached.empty()) {
                fs::create_directories(cache);
                auto partial = cached; partial += ".partial";
                fs::remove(partial);
                fs::copy_file(archive, partial, fs::copy_options::overwrite_existing);
                fs::rename(partial, cached);
                source_archive = cached;
            }
        }
        // The release is a directory: the executable plus the TorchSharp
        // native libraries it loads at startup. Extract and validate the whole
        // tree, then publish it in place of the previous installation.
        fs::create_directories(staging);
        run_tool("tar", {"-xzf", source_archive.string(), "-C", staging.string()});
        const auto binary = staging / "llavon-lora";
        if (!fs::is_regular_file(binary)) throw std::runtime_error("trainer executable is missing from release");
        if (!has_trainer_libraries(staging))
            throw std::runtime_error("trainer release is missing its native libraries");
        if (::chmod(binary.c_str(), 0755) != 0) throw std::runtime_error("cannot mark the trainer executable");
        const auto version_file = output / "trainer-version.json.partial";
        run_tool(binary.c_str(), {"--version", "--json"}, version_file);
        const auto actual = nlohmann::json::parse(std::ifstream(version_file));
        fs::remove(version_file);
        // The release manifest currently reports API 1; verify the actual CLI.
        if (actual.at("trainerApi").get<int>() != 2)
            throw std::runtime_error("trainer API is incompatible (expected 2)");
        { std::ofstream file(staging / "trainer-release.json", std::ios::trunc);
          file << nlohmann::json{{"version", version}, {"commit", expected_commit},
                                 {"sha256", sha256_hex(binary)}}.dump() << '\n';
          if (!file) throw std::runtime_error("cannot store trainer release information"); }
        std::vector<fs::path> obsolete;
        for (const auto& entry : fs::directory_iterator(output)) obsolete.push_back(entry.path());
        for (const auto& path : obsolete) {
            const auto filename = path.filename();
            if (filename == archive.filename() || filename == manifest_file.filename() ||
                filename == pinned_manifest_file.filename() || filename == staging.filename()) continue;
            fs::remove_all(path);
        }
        for (const auto& entry : fs::directory_iterator(staging))
            fs::rename(entry.path(), output / entry.path().filename());
        fs::remove_all(staging);
        std::cout << "trainer=" << (output / "llavon-lora") << " version=" << version << '\n';
        // Give the fresh installation the accelerator libraries of this machine
        // (a no-op on machines without one). A failure here only costs GPU
        // training, so it is reported instead of failing the installation.
        try {
            ime::unix_service::ensure_trainer_libtorch(output);
        } catch (const std::exception& error) {
            std::cerr << "warning: cannot install the GPU libtorch: " << error.what() << '\n';
        }
    } catch (...) { fs::remove_all(staging); fs::remove(archive); fs::remove(manifest_file);
                    fs::remove(pinned_manifest_file); throw; }
    fs::remove(archive);
    fs::remove(manifest_file);
    fs::remove(pinned_manifest_file);
}

void fetch_model(const fs::path& output) {
    const fs::path metadata = output / "metadata.partial";
    fs::create_directories(output);
    const std::string repository = "tony65535/llavon-ime-llama-250m";
    try {
        run_tool("curl", {"--fail", "--location", "--retry", "3", "--output", metadata.string(),
                          "https://huggingface.co/api/models/" + repository + "/revision/main?blobs=true"});
        std::ifstream stream(metadata);
        const auto manifest = nlohmann::json::parse(stream);
        const std::string revision = manifest.at("sha").get<std::string>();
        if (revision.size() != 40 || revision.find_first_not_of("0123456789abcdef") != std::string::npos)
            throw std::runtime_error("invalid Hugging Face model revision");
        const auto destination = output / revision;
        fs::create_directories(destination);
        const auto& siblings = manifest.at("siblings");
        for (const auto name : {"config.json", "ime_vocab.json", "model.safetensors"}) {
            const auto found = std::find_if(siblings.begin(), siblings.end(),
                [&](const nlohmann::json& sibling) { return sibling.value("rfilename", "") == name; });
            if (found == siblings.end()) throw std::runtime_error("missing checkpoint asset");
            const auto size = found->at("size").get<std::uint64_t>();
            const auto file = destination / name;
            const bool weights = std::string_view(name) == "model.safetensors";
            const std::string expected_hash = weights ? found->at("lfs").at("sha256").get<std::string>() : "";
            if (fs::is_regular_file(file) && fs::file_size(file) == size &&
                (!weights || sha256_matches(file, expected_hash))) continue;
            auto partial = file; partial += ".partial";
            run_tool("curl", {"--fail", "--location", "--retry", "3", "--output", partial.string(),
                              "https://huggingface.co/" + repository + "/resolve/" + revision + "/" + name});
            if (!fs::is_regular_file(partial) || fs::file_size(partial) != size)
                throw std::runtime_error(std::string("incomplete checkpoint asset: ") + name);
            if (weights && !sha256_matches(partial, expected_hash))
                throw std::runtime_error("checkpoint SHA-256 mismatch");
            fs::rename(partial, file);
        }
        std::ofstream current(output / "current.revision.partial");
        current << revision << '\n'; current.close();
        fs::rename(output / "current.revision.partial", output / "current.revision");
        fs::remove(metadata);
        std::cout << "model-dir=" << destination << " revision=" << revision << '\n';
    } catch (...) { fs::remove(metadata); throw; }
}

void list(sqlite3* db, const Options& options) {
    ime::unix_service::CommitCipher cipher;
    unlock(cipher, db, options);
    for (const auto& record : ime::unix_service::read_commits(db, "pending", cipher, 0, 1000000)) {
        std::string readings;
        for (const auto& reading : record.readings) {
            if (!readings.empty()) readings += ' ';
            readings += reading;
        }
        std::cout << nlohmann::json{{"id", record.id}, {"committed_at", record.committed_at},
                                    {"context", record.context}, {"answer", record.answer},
                                    {"readings", readings}}.dump() << '\n';
    }
}

void change_state(Database& db, std::string_view action, const std::string& id) {
    // Deleting overwrites the freed pages so the typed text does not linger.
    if (action == "delete") db.exec("PRAGMA secure_delete=ON");
    const char* sql = action == "exclude" ? "UPDATE commits SET state='excluded' WHERE id=? AND state='pending'" :
                      action == "delete" ? "DELETE FROM commits WHERE id=? AND state='pending'" : nullptr;
    if (!sql) throw std::invalid_argument("unknown record action");
    Statement query(db.get(), sql);
    query.bind(1, id);
    (void)query.next();
    if (sqlite3_changes(db.get()) != 1)
        throw std::runtime_error(action == "delete" ? "record not found or not pending"
                                                    : "record not found or not eligible for exclusion");
}

// Removes the readable training dataset (and any partial file) as soon as a
// run no longer needs it. Only these two application-owned files are touched.
void discard_plaintext_dataset(const fs::path& dataset) {
    std::error_code error;
    fs::remove(dataset, error);
    auto partial = dataset; partial += ".partial";
    fs::remove(partial, error);
}

void ensure_run_history(Database& db) {    std::set<std::string> columns;
    Statement info(db.get(), "PRAGMA table_info(lora_runs)");
    while (info.next() == SQLITE_ROW) columns.insert(info.text(1));
    for (const auto& [name, definition] : std::vector<std::pair<std::string, std::string>>{
             {"rank", "INTEGER NOT NULL DEFAULT 8"}, {"alpha", "REAL NOT NULL DEFAULT 16"},
             {"dropout", "REAL NOT NULL DEFAULT 0"}, {"target_modules", "TEXT NOT NULL DEFAULT 'q_proj,v_proj'"},
             {"optimizer_steps", "INTEGER NOT NULL DEFAULT 0"}, {"parent_id", "INTEGER"},
             {"cumulative_record_count", "INTEGER NOT NULL DEFAULT 0"},
             {"training_request_json", "TEXT"}}) {
        if (!columns.contains(name)) db.exec(("ALTER TABLE lora_runs ADD COLUMN " + name + " " + definition).c_str());
    }
}

void publish_run(Database& db, const ime::unix_service::NumericDataset& dataset,
                 const fs::path& adapter, const fs::path& model, const std::string& revision,
                 int rank, double alpha, double dropout, const std::string& modules,
                 std::int64_t parent_id, std::int64_t base_cumulative, const std::string& request_json) {
    int steps = 0;
    if (fs::is_regular_file(adapter / "training_state.json"))
        steps = nlohmann::json::parse(std::ifstream(adapter / "training_state.json")).at("step").get<int>();
    db.exec("BEGIN IMMEDIATE");
    try {
        Statement update(db.get(), "UPDATE commits SET state='trained' WHERE id=? AND state='pending'");
        for (const auto& id : dataset.included_ids) {
            sqlite3_reset(update.get()); sqlite3_clear_bindings(update.get());
            update.bind(1, id);
            (void)update.next();
            if (sqlite3_changes(db.get()) != 1) throw std::runtime_error("training records changed during export");
        }
        Statement insert(db.get(), "INSERT INTO lora_runs(base_revision,adapter_path,model_path,record_count,rank,alpha,dropout,target_modules,optimizer_steps,parent_id,cumulative_record_count,training_request_json) VALUES (?,?,?,?,?,?,?,?,?,?,?,?)");
        insert.bind(1, revision); insert.bind(2, adapter.string()); insert.bind(3, model.string());
        sqlite3_bind_int64(insert.get(), 4, static_cast<sqlite3_int64>(dataset.included_ids.size()));
        sqlite3_bind_int(insert.get(), 5, rank);
        sqlite3_bind_double(insert.get(), 6, alpha);
        sqlite3_bind_double(insert.get(), 7, dropout);
        insert.bind(8, modules);
        sqlite3_bind_int(insert.get(), 9, steps);
        // A run that branches from an earlier adapter records that parent; a
        // run from the base model keeps no parent at all.
        if (parent_id) sqlite3_bind_int64(insert.get(), 10, parent_id);
        else sqlite3_bind_null(insert.get(), 10);
        sqlite3_bind_int64(insert.get(), 11, base_cumulative + static_cast<sqlite3_int64>(dataset.included_ids.size()));
        insert.bind(12, request_json);
        (void)insert.next();
        db.exec("COMMIT");
    } catch (...) { sqlite3_exec(db.get(), "ROLLBACK", nullptr, nullptr, nullptr); throw; }
}

// The full parameter set of a run. Every run records these fields in its
// training_request_json so the history can show what produced an adapter and
// so a later run can continue exactly from it, mirroring the Windows manager.
struct TrainingParameters {
    int rank = 8;
    double alpha = 16;
    double dropout = 0;
    int batch = 1;
    int accumulation = 1;
    int epochs = 5;
    int max_steps = -1;
    double learning_rate = 0.0001;
    double weight_decay = 0;
    int warmup = 0;
    double norm = 1;
    int save_every = 0;
    int seed = 42;
    int shuffle = 1;
    int max_length = 384;
    std::string device = "auto";
    std::string dtype = "float32";
    std::string modules = "q_proj,v_proj";
};

std::int64_t integer64_option(const Options& options, const char* key, std::int64_t fallback,
                              std::int64_t minimum, std::int64_t maximum) {
    const auto text = optional(options, key, "");
    if (text.empty()) return fallback;
    std::int64_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc() || parsed.ptr != text.data() + text.size() || value < minimum || value > maximum)
        throw std::invalid_argument(std::string("invalid ") + key);
    return value;
}

// Non-advanced strengths always train the preset structure and ignore the
// individual fields, exactly like the Windows manager resets them.
TrainingParameters resolve_parameters(const Options& options, ime::unix_service::LoraTrainingStrength strength) {
    TrainingParameters parameters;
    if (strength != ime::unix_service::LoraTrainingStrength::advanced) {
        const auto preset = ime::unix_service::lora_training_preset(strength);
        parameters.epochs = preset.epochs;
        parameters.learning_rate = preset.learning_rate;
        return parameters;
    }
    parameters.device = optional(options, "--device", "auto");
    if (parameters.device != "cpu" && parameters.device != "cuda" && parameters.device != "mps" && parameters.device != "auto")
        throw std::invalid_argument("invalid --device");
    parameters.dtype = optional(options, "--dtype", "float32");
    if (parameters.dtype != "float32" && parameters.dtype != "bfloat16") throw std::invalid_argument("invalid --dtype");
    parameters.modules = optional(options, "--target-modules", "q_proj,v_proj");
    if (parameters.modules.empty() || parameters.modules.find_first_not_of("abcdefghijklmnopqrstuvwxyz_,") != std::string::npos)
        throw std::invalid_argument("invalid --target-modules");
    parameters.max_length = integer_option(options, "--max-seq-length", 384, 2, 384);
    parameters.rank = integer_option(options, "--rank", 8, 1, INT_MAX);
    parameters.alpha = real_option(options, "--alpha", 16, std::numeric_limits<double>::denorm_min(),
                                   std::numeric_limits<double>::max());
    parameters.dropout = real_option(options, "--dropout", 0, 0, 1);
    if (parameters.dropout >= 1) throw std::invalid_argument("invalid --dropout");
    parameters.batch = integer_option(options, "--batch-size", 1, 1, INT_MAX);
    parameters.accumulation = integer_option(options, "--gradient-accumulation", 1, 1, INT_MAX);
    parameters.epochs = integer_option(options, "--epochs", 5, 1, INT_MAX);
    parameters.max_steps = integer_option(options, "--max-steps", -1, -1, INT_MAX);
    if (parameters.max_steps == 0) throw std::invalid_argument("invalid --max-steps");
    parameters.learning_rate = real_option(options, "--learning-rate", 0.0001,
                                           std::numeric_limits<double>::denorm_min(),
                                           std::numeric_limits<double>::max());
    parameters.weight_decay = real_option(options, "--weight-decay", 0, 0, std::numeric_limits<double>::max());
    parameters.warmup = integer_option(options, "--warmup-steps", 0, 0, INT_MAX);
    parameters.norm = real_option(options, "--max-grad-norm", 1, 0, std::numeric_limits<double>::max());
    parameters.save_every = integer_option(options, "--save-every", 0, 0, INT_MAX);
    parameters.seed = integer_option(options, "--seed", 42, INT_MIN, INT_MAX);
    parameters.shuffle = integer_option(options, "--shuffle", 1, 0, 1);
    return parameters;
}

// The adapter a run continues from. Without --base-run-id the newest run is
// used (the historical behaviour); an explicit 0 starts from the base model
// and an explicit ID branches from that earlier run.
struct TrainingBase {
    std::int64_t id = 0;
    std::string revision;
    int rank = 0;
    double alpha = 0;
    double dropout = 0;
    std::string modules;
    std::string adapter_path;
    std::int64_t cumulative = 0;
};

TrainingBase resolve_training_base(Database& db, const Options& options) {
    TrainingBase base;
    const bool explicit_choice = options.contains("--base-run-id");
    const std::int64_t requested = integer64_option(options, "--base-run-id", 0, 0,
                                                    std::numeric_limits<std::int64_t>::max());
    Statement query(db.get(), explicit_choice
        ? "SELECT id,base_revision,rank,alpha,dropout,target_modules,adapter_path,"
          "COALESCE(cumulative_record_count,0) FROM lora_runs WHERE id=?"
        : "SELECT id,base_revision,rank,alpha,dropout,target_modules,adapter_path,"
          "COALESCE(cumulative_record_count,0) FROM lora_runs ORDER BY id DESC LIMIT 1");
    if (explicit_choice) sqlite3_bind_int64(query.get(), 1, requested);
    if (query.next() != SQLITE_ROW) {
        // An explicit ID that does not exist is an error; an absent run
        // history or an explicit 0 both mean the base model.
        if (explicit_choice && requested != 0) throw std::runtime_error("training base run not found");
        return base;
    }
    base.id = sqlite3_column_int64(query.get(), 0);
    base.revision = query.text(1);
    base.rank = sqlite3_column_int(query.get(), 2);
    base.alpha = sqlite3_column_double(query.get(), 3);
    base.dropout = sqlite3_column_double(query.get(), 4);
    base.modules = query.text(5);
    base.adapter_path = query.text(6);
    base.cumulative = sqlite3_column_int64(query.get(), 7);
    return base;
}

// Resolves the installed trainer and refuses a stale or incomplete install
// unless a development override names the executable.
fs::path verified_trainer(const fs::path& db_path) {
    const fs::path trainer = installed_trainer(db_path);
    const char* override = std::getenv("LLAVON_IME_LORA_CLI_PATH");
    if (!override || !*override) {
        if (!has_pinned_trainer_stamp(trainer))
            throw std::runtime_error("installed trainer does not match the pinned submodule commit; reinstall the pinned trainer");
        if (!has_trainer_libraries(trainer.parent_path()))
            throw std::runtime_error("installed trainer is missing its native libraries; reinstall the pinned trainer");
    }
    return trainer;
}

// Converts an adapter into the quantized GGUF the input method loads. The
// intermediate f16 file is removed again; only the adapter is kept.
void export_model(const fs::path& trainer, const fs::path& model_dir, const fs::path& adapter,
                  const fs::path& gguf) {
    auto f16 = gguf;
    f16.replace_filename("personalized-f16.gguf");
    run(trainer, {"export-gguf", "--model-config", (model_dir / "config.json").string(),
                  "--model", model_dir.string(), "--vocab-file", (model_dir / "ime_vocab.json").string(),
                  "--adapter", adapter.string(), "--outfile", f16.string(), "--outtype", "f16",
                  "--quantize", "Q4_K_M", "--quantized-outfile", gguf.string(), "--force"});
    if (!fs::is_regular_file(gguf) || fs::file_size(gguf) == 0)
        throw std::runtime_error("trainer did not produce a GGUF model");
    fs::remove(f16);
}

// Re-exports the GGUF of a finished run from its retained adapter, mirroring
// the Windows manager's ensure_model_exported.
void export_run_model(Database& db, std::int64_t run_id, const fs::path& model_dir,
                      const fs::path& db_path) {
    ensure_run_history(db);
    Statement query(db.get(), "SELECT base_revision,adapter_path,model_path FROM lora_runs WHERE id=?");
    sqlite3_bind_int64(query.get(), 1, run_id);
    if (query.next() != SQLITE_ROW) throw std::runtime_error("training run not found");
    const std::string revision = query.text(0);
    const fs::path adapter = query.text(1);
    const fs::path gguf = query.text(2);
    if (!fs::is_regular_file(adapter / "adapter_model.safetensors"))
        throw std::runtime_error("run adapter is missing");
    const fs::path training_model_dir = model_dir.parent_path() / revision;
    if (!fs::is_regular_file(training_model_dir / "config.json") ||
        !fs::is_regular_file(training_model_dir / "ime_vocab.json") ||
        !fs::is_regular_file(training_model_dir / "model.safetensors"))
        throw std::runtime_error("run's base checkpoint is missing");
    const auto trainer = verified_trainer(db_path);
    export_model(trainer, training_model_dir, adapter, gguf);
    std::cout << "model=" << gguf << '\n';
}

// Reports how many records a training run would actually use, without
// touching the database or the trainer: the same conversion the run performs,
// written to a throwaway file. The page shows this exact count in its
// confirmation and marks the skipped records in the review list.
void count_dataset(sqlite3* db, const Options& options, const fs::path& model_dir, const fs::path& tables,
                   const ime::unix_service::CommitCipher& cipher) {
    const int max_length = integer_option(options, "--max-seq-length", 384, 2, 384);
    const bool manual_only = integer_option(options, "--only-manually-selected", 0, 0, 1) != 0;
    auto temporary = fs::temp_directory_path() /
        ("llavon-ime-count-" + std::to_string(::getpid()) + ".jsonl");
    const auto decryption = cipher.decryption();
    const auto dataset = ime::unix_service::write_numeric_dataset(
        db, tables, model_dir / "config.json", temporary, max_length, nullptr, &decryption, manual_only);
    fs::remove(temporary);
    // The skipped ids let the manager mark records that will never train; the
    // list is capped so a huge pending set cannot flood the page.
    constexpr std::size_t kMaxSkippedIds = 500;
    nlohmann::json skipped_ids = nlohmann::json::array();
    for (std::size_t index = 0; index < dataset.skipped_ids.size() && index < kMaxSkippedIds; ++index)
        skipped_ids.push_back(dataset.skipped_ids[index]);
    std::cout << nlohmann::json{{"trainable", dataset.included_ids.size()},
                                {"skipped", dataset.skipped},
                                {"samples", dataset.samples},
                                {"skipped_ids", std::move(skipped_ids)}}.dump() << '\n';
}

void train(Database& db, const Options& options, const fs::path& model_dir, const fs::path& tables,
            const fs::path& output, const fs::path& db_path, const ime::unix_service::CommitCipher& cipher) {
    if (options.contains("--trainer"))
        throw std::invalid_argument("--trainer is no longer supported; use the pinned trainer installer");
    const fs::path trainer = verified_trainer(db_path);
    // Make sure the trainer can use this machine's GPU; machines without an
    // accelerator are left alone, and a failure only costs GPU training.
    try {
        ime::unix_service::ensure_trainer_libtorch(trainer.parent_path());
    } catch (const std::exception& error) {
        std::cerr << "warning: cannot install the GPU libtorch: " << error.what() << '\n';
    }
    const auto strength_name = optional(options, "--strength", "advanced");
    const auto strength = ime::unix_service::lora_strength_from_name(strength_name);
    if (!strength) throw std::invalid_argument("invalid --strength");
    ensure_run_history(db);
    auto parameters = resolve_parameters(options, *strength);
    auto base = resolve_training_base(db, options);
    if (base.id != 0) {
        if (*strength != ime::unix_service::LoraTrainingStrength::advanced) {
            // A preset continues the adapter it was started from; the adapter
            // structure cannot change between runs.
            parameters.rank = base.rank;
            parameters.alpha = base.alpha;
            parameters.dropout = base.dropout;
            parameters.modules = base.modules;
        } else if (parameters.rank != base.rank || parameters.alpha != base.alpha ||
                   parameters.dropout != base.dropout || parameters.modules != base.modules) {
            throw std::runtime_error("parameters differ from the base adapter");
        }
    }
    std::string revision = require(options, "--revision");
    fs::path training_model_dir = model_dir;
    if (revision.size() != 40 || revision.find_first_not_of("0123456789abcdef") != std::string::npos ||
        (model_dir.filename().string().size() == 40 && model_dir.filename() != revision))
        throw std::invalid_argument("--revision must match the pinned checkpoint directory");
    if (base.id != 0) {
        revision = base.revision;
        training_model_dir = model_dir.parent_path() / revision;
        if (!fs::is_regular_file(training_model_dir / "config.json") ||
            !fs::is_regular_file(training_model_dir / "ime_vocab.json") ||
            !fs::is_regular_file(training_model_dir / "model.safetensors"))
            throw std::runtime_error("base run's base checkpoint is missing");
    }
    const bool manual_only = integer_option(options, "--only-manually-selected", 0, 0, 1) != 0;
    const bool stabilize = integer_option(options, "--stabilize-intruders", 0, 0, 1) != 0;
    std::unordered_set<std::string> selected;
    if (options.contains("--selected-ids")) {
        const auto selection = nlohmann::json::parse(std::ifstream(require(options, "--selected-ids")));
        if (!selection.is_object() || !selection.contains("selected") || !selection.contains("reviewed") ||
            !selection["selected"].is_array() || !selection["reviewed"].is_array() ||
            selection["selected"].empty()) throw std::invalid_argument("at least one record must be selected");
        const auto read_ids = [](const nlohmann::json& ids) {
            std::unordered_set<std::string> result;
            for (const auto& id : ids) {
                if (!id.is_string()) throw std::invalid_argument("invalid training record ID");
                const auto value = id.get<std::string>();
                if (value.size() != 32 || value.find_first_not_of("0123456789abcdef") != std::string::npos ||
                    !result.insert(value).second) throw std::invalid_argument("invalid training record ID");
            }
            return result;
        };
        selected = read_ids(selection["selected"]);
        const auto reviewed = read_ids(selection["reviewed"]);
        for (const auto& id : selected) if (!reviewed.contains(id))
            throw std::invalid_argument("selected record was not reviewed");
        db.exec("BEGIN IMMEDIATE");
        try {
            Statement pending(db.get(), "SELECT id FROM commits WHERE state='pending'");
            std::set<std::string> available;
            while (pending.next() == SQLITE_ROW) available.insert(pending.text(0));
            Statement exclude(db.get(), "UPDATE commits SET state='excluded' WHERE id=? AND state='pending'");
            for (const auto& id : reviewed) {
                if (selected.contains(id) || !available.contains(id)) continue;
                exclude.bind(1, id); (void)exclude.next();
                sqlite3_reset(exclude.get()); sqlite3_clear_bindings(exclude.get());
            }
            db.exec("COMMIT");
        } catch (...) { sqlite3_exec(db.get(), "ROLLBACK", nullptr, nullptr, nullptr); throw; }
    }
    if (fs::exists(output)) throw std::runtime_error("output directory already exists");
    fs::create_directories(output);
    const auto version_file = output / "trainer-version.json";
    run_tool(trainer.c_str(), {"--version", "--json"}, version_file);
    const auto trainer_version = nlohmann::json::parse(std::ifstream(version_file));
    if (trainer_version.at("trainerApi").get<int>() != 2)
        throw std::runtime_error("trainer API is incompatible (expected 2)");
    const auto dataset_path = output / "training.jsonl";
    const auto decryption = cipher.decryption();
    const auto dataset = ime::unix_service::write_numeric_dataset(db.get(), tables, training_model_dir / "config.json",
        dataset_path, parameters.max_length, options.contains("--selected-ids") ? &selected : nullptr, &decryption,
        manual_only);
    if (dataset.included_ids.empty()) throw std::runtime_error("no trainable pending Bopomofo records");
    std::cout << "trainable=" << dataset.included_ids.size() << " skipped=" << dataset.skipped << std::endl;
    // The manager keeps the full parameter set of every run so the history
    // page and a later branched run agree on what was trained.
    const nlohmann::json request{
        {"preset_version", 1},
        {"strength", ime::unix_service::lora_strength_name(*strength)},
        {"only_manually_selected", manual_only},
        {"stabilize_intruders", stabilize},
        {"parent_id", base.id},
        {"base_model_revision", revision},
        {"trainer_commit", LLAVON_IME_LORA_PINNED_COMMIT},
        {"trainer_version", trainer_version.value("version", "")},
        {"record_count", dataset.included_ids.size()},
        {"skipped_record_count", dataset.skipped},
        {"pad_token_id", dataset.pad_token_id},
        {"rank", parameters.rank}, {"alpha", parameters.alpha}, {"dropout", parameters.dropout},
        {"batch_size", parameters.batch}, {"gradient_accumulation", parameters.accumulation},
        {"epochs", parameters.epochs}, {"max_steps", parameters.max_steps},
        {"learning_rate", parameters.learning_rate}, {"weight_decay", parameters.weight_decay},
        {"warmup_steps", parameters.warmup}, {"max_gradient_norm", parameters.norm},
        {"save_every", parameters.save_every}, {"device", parameters.device},
        {"seed", parameters.seed}, {"shuffle", parameters.shuffle != 0},
        {"max_sequence_length", parameters.max_length}, {"dtype", parameters.dtype},
        {"target_modules", parameters.modules},
    };
    { std::ofstream record(output / "training_request.json", std::ios::trunc);
      record << request.dump(2) << '\n';
      if (!record) throw std::runtime_error("cannot store the training request"); }
    // With forgetting mitigation enabled the raw training output is kept apart
    // so the stabilization step can rewrite it into the final adapter, exactly
    // like the Windows manager.
    const auto raw_adapter = stabilize ? output / "adapter-before-stabilization" : output / "adapter";
    const auto adapter = output / "adapter";
    const auto gguf = output / "personalized-Q4_K_M.gguf";
    // The numeric dataset is readable text, so it only lives while this run
    // needs it; a killed process leaves at most the partial file, which the
    // next run and the manager remove.
    try {
        run(trainer, {"validate", "--train-data", dataset_path.string(), "--vocab-size",
                       std::to_string(dataset.vocab_size), "--max-seq-length", std::to_string(parameters.max_length)});
        std::vector<std::string> args{
            "train", "--model-config", (training_model_dir / "config.json").string(), "--model", training_model_dir.string(),
            "--train-data", dataset_path.string(), "--output-dir", raw_adapter.string(),
            "--target-modules", parameters.modules, "--pad-token-id", std::to_string(dataset.pad_token_id),
            "--max-seq-length", std::to_string(parameters.max_length), "--rank", std::to_string(parameters.rank),
            "--alpha", as_argument(parameters.alpha), "--dropout", as_argument(parameters.dropout),
            "--batch-size", std::to_string(parameters.batch), "--gradient-accumulation", std::to_string(parameters.accumulation),
            "--epochs", std::to_string(parameters.epochs), "--max-steps", std::to_string(parameters.max_steps),
            "--learning-rate", as_argument(parameters.learning_rate), "--weight-decay", as_argument(parameters.weight_decay),
            "--warmup-steps", std::to_string(parameters.warmup), "--max-grad-norm", as_argument(parameters.norm),
            "--save-every", std::to_string(parameters.save_every), "--seed", std::to_string(parameters.seed),
            "--device", parameters.device, "--dtype", parameters.dtype
        };
        if (!parameters.shuffle) args.push_back("--no-shuffle");
        if (base.id != 0) {
            // Continue the adapter the user selected; a run from the base
            // model starts a fresh adapter instead.
            const fs::path path = base.adapter_path;
            if (!fs::is_regular_file(path / "adapter_model.safetensors"))
                throw std::runtime_error("base run's adapter is missing");
            args.insert(args.end(), {"--resume-adapter", path.string()});
        }
        run(trainer, args);
        if (stabilize) {
            // Suppress the top intruder dimension of each matrix; the trainer
            // keeps the alpha/rank scaling while the adapter rank may grow.
            run(trainer, {"stabilize-adapter", "--model", training_model_dir.string(),
                          "--adapter", raw_adapter.string(), "--output-dir", adapter.string(),
                          "--scale", "0.9", "--force"});
        }
        export_model(trainer, training_model_dir, adapter, gguf);
        discard_plaintext_dataset(dataset_path);
    } catch (...) {
        discard_plaintext_dataset(dataset_path);
        throw;
    }
    // The run record carries the adapter that was actually written: the
    // stabilization step can change rank and alpha, and the next run has to
    // continue from those values.
    int published_rank = parameters.rank;
    double published_alpha = parameters.alpha;
    try {
        const auto final_config = nlohmann::json::parse(std::ifstream(adapter / "adapter_config.json"));
        published_rank = final_config.value("r", parameters.rank);
        published_alpha = final_config.value("lora_alpha", parameters.alpha);
    } catch (...) {}
    publish_run(db, dataset, adapter, gguf, revision, published_rank, published_alpha, parameters.dropout,
                parameters.modules, base.id, base.cumulative, request.dump());
    if (stabilize) {
        std::error_code ignored;
        fs::remove_all(raw_adapter, ignored);
    }
    std::cout << "model=" << gguf << '\n';
}

}  // namespace

int main(int argc, char** argv) {
    try {
        ::umask(0077);  // Datasets and adapter outputs contain user typing.
        if (argc < 2) throw std::invalid_argument("usage: llavon-ime-lora check-model|fetch-model|check-trainer|install-trainer|protection-status|configure-password|set-recording|reset-conversation-data|list|exclude|delete|dataset|count-dataset|train|export-model [--option value ...]");
        const auto options = parse(argc, argv);
        if (std::string_view(argv[1]) == "install-trainer") {
            install_trainer(fs::absolute(require(options, "--output-dir")));
            return EXIT_SUCCESS;
        }
        if (std::string_view(argv[1]) == "check-trainer") {
            check_trainer(fs::absolute(require(options, "--output-dir")));
            return EXIT_SUCCESS;
        }
        if (std::string_view(argv[1]) == "check-model") {
            check_model(fs::absolute(require(options, "--output-dir")));
            return EXIT_SUCCESS;
        }
        if (std::string_view(argv[1]) == "fetch-model") {
            fetch_model(fs::absolute(require(options, "--output-dir")));
            return EXIT_SUCCESS;
        }
        const auto db_path = optional(options, "--db", "");
        const auto database = db_path.empty() ? ime::unix_service::CommitStore::default_path() : fs::path(db_path);
        const std::string_view action = argv[1];
        if (action == "protection-status") {
            ime::unix_service::CommitStore store(database);
            const auto status = store.protection_status();
            std::cout << nlohmann::json{{"configured", status.configured}, {"enabled", status.enabled}}.dump() << '\n';
            return EXIT_SUCCESS;
        }
        if (action == "configure-password") {
            ime::unix_service::CommitStore store(database);
            store.configure_password(password_option(options));
            std::cout << "configured=1 enabled=1\n";
            return EXIT_SUCCESS;
        }
        if (action == "set-recording") {
            const auto enabled = require(options, "--enabled");
            if (enabled != "0" && enabled != "1") throw std::invalid_argument("invalid --enabled");
            ime::unix_service::CommitStore store(database);
            store.set_recording_enabled(enabled == "1");
            std::cout << "enabled=" << enabled << '\n';
            return EXIT_SUCCESS;
        }
        if (action == "reset-conversation-data") {
            ime::unix_service::CommitStore store(database);
            store.reset_conversation_data();
            std::cout << "cleared=1\n";
            return EXIT_SUCCESS;
        }
        Database db(database);
        if (action == "list") list(db.get(), options);
        else if (action == "exclude" || action == "delete") change_state(db, action, require(options, "--id"));
        else if (action == "export-model") {
            const auto run_id = integer64_option(options, "--run-id", 0, 1,
                                                 std::numeric_limits<std::int64_t>::max());
            export_run_model(db, run_id, fs::absolute(require(options, "--model-dir")), database);
        }
        else if (action == "count-dataset") {
            const fs::path model_dir = fs::absolute(require(options, "--model-dir"));
            const fs::path tables = fs::absolute(require(options, "--tables-dir"));
            ime::unix_service::CommitCipher cipher;
            unlock(cipher, db.get(), options);
            count_dataset(db.get(), options, model_dir, tables, cipher);
        }
        else if (action == "dataset" || action == "train") {
            const fs::path model_dir = fs::absolute(require(options, "--model-dir"));
            const fs::path tables = fs::absolute(require(options, "--tables-dir"));
            ime::unix_service::CommitCipher cipher;
            unlock(cipher, db.get(), options);
            if (action == "train") train(db, options, model_dir, tables, fs::absolute(require(options, "--output-dir")),
                                          database, cipher);
            else {
                const auto decryption = cipher.decryption();
                const auto dataset = ime::unix_service::write_numeric_dataset(
                    db.get(), tables, model_dir / "config.json", require(options, "--output"), 384, nullptr,
                    &decryption, integer_option(options, "--only-manually-selected", 0, 0, 1) != 0);
                if (dataset.included_ids.empty()) throw std::runtime_error("no trainable pending Bopomofo records");
                std::cout << "trainable=" << dataset.included_ids.size() << " skipped=" << dataset.skipped << '\n';
            }
        } else throw std::invalid_argument("unknown action");
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "llavon-ime-lora: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
