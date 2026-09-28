#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ime::unix_service {

// The PyTorch libtorch build that drives an AMD or NVIDIA GPU. TorchSharp
// publishes no ROCm package, so the manager installs the accelerator libraries
// next to the CPU trainer instead of publishing another trainer build: the
// trainer loads whatever libtorch sits in its own directory.
struct LibTorchBackend {
    std::string name;
    std::string version;
    std::string url;
    std::string sha256;
    std::uint64_t size;
    std::vector<std::string> kernel_directories;
};

const LibTorchBackend* libtorch_backend_for(const std::string& name);
const LibTorchBackend* libtorch_backend_for_vendor(const std::string& vendor);

std::filesystem::path libtorch_cache_directory(const LibTorchBackend& backend);

// True when the trainer directory already carries this backend's libraries.
bool trainer_uses_libtorch(const std::filesystem::path& trainer_directory, const LibTorchBackend& backend);

// Downloads the archive, verifies it and extracts the libraries the trainer
// needs into the per-user cache; repeated calls reuse the cache.
void install_libtorch(const LibTorchBackend& backend);

// Hard-links (or copies) the cached libtorch into the installed trainer.
bool apply_libtorch_to_trainer(const std::filesystem::path& trainer_directory, const LibTorchBackend& backend);

// Detects the GPU and makes the installed trainer use it; machines without an
// accelerator keep the CPU build untouched.
void ensure_trainer_libtorch(const std::filesystem::path& trainer_directory);

}  // namespace ime::unix_service
