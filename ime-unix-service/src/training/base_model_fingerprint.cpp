#include "base_model_fingerprint.hpp"

#include <sodium.h>

#include <array>
#include <cstdint>
#include <fstream>
#include <stdexcept>

namespace ime::unix_service {
namespace {
void append_u64_le(crypto_hash_sha256_state& state, std::uint64_t value) {
    std::array<unsigned char, 8> bytes{};
    for (std::size_t index = 0; index < bytes.size(); ++index)
        bytes[index] = static_cast<unsigned char>((value >> (index * 8)) & 0xffU);
    crypto_hash_sha256_update(&state, bytes.data(), bytes.size());
}
}

bool valid_base_model_fingerprint(std::string_view fingerprint) noexcept {
    return fingerprint.size() == crypto_hash_sha256_BYTES * 2 &&
        fingerprint.find_first_not_of("0123456789abcdef") == std::string_view::npos;
}

std::string compute_base_model_fingerprint(const std::filesystem::path& directory) {
    if (sodium_init() < 0) throw std::runtime_error("unable to initialize base model SHA-256");
    crypto_hash_sha256_state state{};
    crypto_hash_sha256_init(&state);
    constexpr std::string_view format = "llavon-base-model-fingerprint-v1";
    crypto_hash_sha256_update(&state,
        reinterpret_cast<const unsigned char*>(format.data()), format.size());
    constexpr std::array<std::string_view, 3> files{
        "config.json", "ime_vocab.json", "model.safetensors"};
    std::array<unsigned char, 1024 * 1024> buffer{};
    for (const auto filename : files) {
        append_u64_le(state, filename.size());
        crypto_hash_sha256_update(&state,
            reinterpret_cast<const unsigned char*>(filename.data()), filename.size());
        const auto path = directory / filename;
        std::error_code error;
        const auto size = std::filesystem::file_size(path, error);
        if (error) throw std::runtime_error("installed base model is incomplete");
        append_u64_le(state, size);
        std::ifstream input(path, std::ios::binary);
        if (!input) throw std::runtime_error("installed base model is unreadable");
        while (input) {
            input.read(reinterpret_cast<char*>(buffer.data()), buffer.size());
            if (const auto count = input.gcount(); count > 0)
                crypto_hash_sha256_update(&state, buffer.data(), static_cast<std::size_t>(count));
        }
        if (!input.eof()) throw std::runtime_error("installed base model could not be read completely");
    }
    std::array<unsigned char, crypto_hash_sha256_BYTES> digest{};
    crypto_hash_sha256_final(&state, digest.data());
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(digest.size() * 2);
    for (const auto byte : digest) {
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 0x0f]);
    }
    return result;
}

}  // namespace ime::unix_service
