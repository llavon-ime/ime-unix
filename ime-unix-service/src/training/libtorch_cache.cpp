#include "libtorch_cache.hpp"

#include "gpu_vendor.hpp"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <system_error>

#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace ime::unix_service {

namespace {

// PyTorch's official libtorch builds: ROCm 7.0 and CUDA 12.8, both LibTorch
// 2.10, which is the ABI TorchSharp 0.106 expects.
const LibTorchBackend kRocm{
    "rocm",
    "2.10.0+rocm7.0",
    "https://download.pytorch.org/libtorch/rocm7.0/libtorch-shared-with-deps-2.10.0%2Brocm7.0.zip",
    "d8561904e2cee6af1be8083ede61cef82fdeb2586697bcff66488bea1539e0e1",
    4850819106,
    {"rocblas/library/", "hipblaslt/library/", "hipsparselt/library/"},
};

const LibTorchBackend kCuda{
    "cuda",
    "2.10.0+cu128",
    "https://download.pytorch.org/libtorch/cu128/libtorch-shared-with-deps-2.10.0%2Bcu128.zip",
    "429aa9fead3cf3d557e7c310442a1fae3879cdc14a469ff452043b39b61666a9",
    3917843662,
    {},
};

fs::path cache_root() {
    if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg && *xdg) return fs::path(xdg) / "llavon-ime";
    const char* home = std::getenv("HOME");
    if (!home || !*home) throw std::runtime_error("cannot locate the per-user cache: HOME is not set");
    return fs::path(home) / ".cache" / "llavon-ime";
}

// Runs an external tool and reports whether it succeeded; a missing tool or a
// non-zero exit is not fatal for callers that validate the result themselves.
bool run(const char* program, const std::vector<std::string>& arguments, const fs::path& stdout_file = {}) {
    const pid_t pid = ::fork();
    if (pid < 0) throw std::runtime_error("cannot fork external tool");
    if (pid == 0) {
        if (!stdout_file.empty()) {
            const int fd = ::open(stdout_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
            if (fd < 0 || ::dup2(fd, STDOUT_FILENO) < 0) _exit(127);
            ::close(fd);
        }
        std::vector<std::string> owned{program};
        owned.insert(owned.end(), arguments.begin(), arguments.end());
        std::vector<char*> argv;
        for (auto& value : owned) argv.push_back(value.data());
        argv.push_back(nullptr);
        ::execvp(program, argv.data());
        _exit(127);
    }
    int status = 0;
    while (::waitpid(pid, &status, 0) == -1) {}
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

std::string sha256_hex(const fs::path& file) {
    auto hash_file = file; hash_file += ".sha256";
    try {
#ifdef __APPLE__
        const bool ok = run("shasum", {"-a", "256", file.string()}, hash_file);
#else
        const bool ok = run("sha256sum", {file.string()}, hash_file);
#endif
        if (!ok) throw std::runtime_error("cannot compute the archive SHA-256");
        std::ifstream input(hash_file);
        std::string actual;
        input >> actual;
        fs::remove(hash_file);
        return actual;
    } catch (...) {
        fs::remove(hash_file);
        throw;
    }
}

bool has_entries(const fs::path& directory) {
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(directory, error)) {
        (void)entry;
        return true;
    }
    return false;
}

bool under_home(const fs::path& directory) {
    const char* home = std::getenv("HOME");
    if (home == nullptr || !*home) return false;
    std::error_code error;
    const auto relative = fs::relative(fs::weakly_canonical(directory, error), fs::weakly_canonical(home), error);
    if (error) return false;
    const auto text = relative.generic_string();
    return !text.empty() && !text.starts_with("..");
}

}  // namespace

const LibTorchBackend* libtorch_backend_for(const std::string& name) {
    if (name == kRocm.name) return &kRocm;
    if (name == kCuda.name) return &kCuda;
    return nullptr;
}

const LibTorchBackend* libtorch_backend_for_vendor(const std::string& vendor) {
    if (vendor == "amd") return &kRocm;
    if (vendor == "nvidia") return &kCuda;
    return nullptr;
}

fs::path libtorch_cache_directory(const LibTorchBackend& backend) {
    return cache_root() / "lora-libtorch" / (backend.name + "-" + backend.version);
}

bool trainer_uses_libtorch(const fs::path& trainer_directory, const LibTorchBackend& backend) {
    const auto library = trainer_directory / (backend.name == "rocm" ? "libtorch_hip.so" : "libtorch_cuda.so");
    if (!fs::is_regular_file(library)) return false;
    for (const auto& kernel : backend.kernel_directories)
        if (!has_entries(trainer_directory / kernel)) return false;
    return true;
}

void install_libtorch(const LibTorchBackend& backend) {
    const auto root = libtorch_cache_directory(backend);
    const auto library_directory = root / "libtorch" / "lib";
    if (trainer_uses_libtorch(library_directory, backend)) {
        std::cout << "libtorch " << backend.version << " is already cached\n";
        return;
    }
    fs::create_directories(root);
    const auto archive = root / ("libtorch-" + backend.version + ".zip.partial");
    try {
        std::cout << "downloading libtorch " << backend.version << " ("
                  << (backend.size / (1024 * 1024)) << " MB, only once)\n";
        if (!run("curl", {"--fail", "--location", "--retry", "3", "--output", archive.string(), backend.url}))
            throw std::runtime_error("cannot download the libtorch archive");
        std::error_code error;
        if (fs::file_size(archive, error) != backend.size || error)
            throw std::runtime_error("libtorch download has an unexpected size");
        if (sha256_hex(archive) != backend.sha256)
            throw std::runtime_error("libtorch download failed checksum verification");
        std::cout << "extracting libtorch\n";
        std::vector<std::string> patterns{"libtorch/lib/*.so*", "libtorch/build-version"};
        for (const auto& kernel : backend.kernel_directories)
            patterns.push_back("libtorch/lib/" + kernel + "*");
        std::vector<std::string> bsdtar{"-xf", archive.string(), "-C", root.string()};
        bsdtar.insert(bsdtar.end(), patterns.begin(), patterns.end());
        std::vector<std::string> unzip{"-q", "-o", archive.string()};
        unzip.insert(unzip.end(), patterns.begin(), patterns.end());
        unzip.push_back("-d");
        unzip.push_back(root.string());
        // libarchive's bsdtar is part of the base system; unzip is the fallback.
        // The completeness check below is what validates the extraction.
        if (!run("bsdtar", bsdtar)) run("unzip", unzip);
        fs::remove(archive);
        if (!trainer_uses_libtorch(library_directory, backend))
            throw std::runtime_error("the extracted libtorch is incomplete");
    } catch (...) {
        fs::remove(archive);
        throw;
    }
}

bool apply_libtorch_to_trainer(const fs::path& trainer_directory, const LibTorchBackend& backend) {
    if (trainer_uses_libtorch(trainer_directory, backend)) return true;
    const auto source = libtorch_cache_directory(backend) / "libtorch" / "lib";
    if (!trainer_uses_libtorch(source, backend)) return false;
    std::error_code error;
    for (const auto& entry : fs::recursive_directory_iterator(source, error)) {
        if (!entry.is_regular_file()) continue;
        const auto relative = fs::relative(entry.path(), source, error);
        if (error) throw std::runtime_error("cannot walk the libtorch cache");
        const auto target = trainer_directory / relative;
        fs::create_directories(target.parent_path());
        fs::remove(target, error);
        error.clear();
        fs::create_hard_link(entry.path(), target, error);
        if (error) {
            error.clear();
            fs::copy_file(entry.path(), target, fs::copy_options::overwrite_existing);
        }
    }
    return trainer_uses_libtorch(trainer_directory, backend);
}

void ensure_trainer_libtorch(const fs::path& trainer_directory) {
    // Only the per-user installation is meant to carry accelerator libraries:
    // packaging runs outside the home directory (and often as root) and must
    // not fetch several gigabytes.
    if (!under_home(trainer_directory)) return;
    const auto vendor = gpu_vendor();
    const auto* backend = libtorch_backend_for_vendor(vendor);
    if (backend == nullptr) return;
    if (!fs::is_regular_file(trainer_directory / "llavon-lora")) return;
    if (trainer_uses_libtorch(trainer_directory, *backend)) return;
    std::cout << "detected a " << vendor << " GPU: installing the " << backend->name << " libtorch\n";
    install_libtorch(*backend);
    if (!apply_libtorch_to_trainer(trainer_directory, *backend))
        throw std::runtime_error("cannot install the libtorch next to the trainer");
}

}  // namespace ime::unix_service
