#pragma once

#include <filesystem>
#include <fstream>
#include <string>

namespace ime::unix_service {

// Reports the accelerator the installed trainer should drive. AMD GPUs train
// through ROCm and NVIDIA GPUs through CUDA; Apple Silicon already carries the
// Metal backend in the CPU build, so it needs nothing extra.
inline std::string gpu_vendor() {
#if defined(__APPLE__)
    return "apple";
#else
    std::error_code error;
    bool amd = false;
    bool nvidia = false;
    for (const auto& entry : std::filesystem::directory_iterator("/sys/class/drm", error)) {
        const auto name = entry.path().filename().string();
        if (!name.starts_with("card") || name.find('-') != std::string::npos) continue;
        std::ifstream vendor(entry.path() / "device" / "vendor");
        std::string value;
        if (!(vendor >> value)) continue;
        if (value == "0x1002") amd = true;
        else if (value == "0x10de") nvidia = true;
    }
    if (amd) return "amd";
    if (nvidia) return "nvidia";
    return "none";
#endif
}

}  // namespace ime::unix_service
