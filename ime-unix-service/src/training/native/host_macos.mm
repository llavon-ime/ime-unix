#include "host_macos.hpp"
#include <CoreFoundation/CoreFoundation.h>
#include <cstdlib>
#include <unistd.h>

namespace llavon::lora {
std::string requestMacHost(const std::string& request) {
    if (request.size() > 4096) return R"({"error":"要求過長"})";
    const char* override = std::getenv("LLAVON_IME_SETTINGS_PORT");
    const auto name = override && *override ? std::string(override) : "org.llavon-ime.settings." + std::to_string(getuid());
    auto cfName = CFStringCreateWithCString(nullptr, name.c_str(), kCFStringEncodingUTF8);
    auto port = CFMessagePortCreateRemote(nullptr, cfName); CFRelease(cfName);
    if (!port) return R"({"error":"尚未連接輸入法，請先啟用拉風輸入法。"})";
    auto data = CFDataCreate(nullptr, reinterpret_cast<const UInt8*>(request.data()), static_cast<CFIndex>(request.size()));
    CFDataRef reply = nullptr;
    const auto status = CFMessagePortSendRequest(port, 1, data, 1, 2, kCFRunLoopDefaultMode, &reply);
    CFRelease(data); CFRelease(port);
    if (status != kCFMessagePortSuccess || !reply) return R"({"error":"輸入法尚未回應，請稍後重試。"})";
    const std::string result(reinterpret_cast<const char*>(CFDataGetBytePtr(reply)), static_cast<std::size_t>(CFDataGetLength(reply)));
    CFRelease(reply); return result;
}
}
