#pragma once

#include <cerrno>
#include <unistd.h>
#include <utility>

namespace llavon::ime {

// Owns one descriptor. Borrow with get(); transfer ownership with release().
class UniqueFd final {
public:
    UniqueFd() = default;
    explicit UniqueFd(int fd) noexcept : fd_(fd) {}
    ~UniqueFd() { reset(); }

    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;
    UniqueFd(UniqueFd&& other) noexcept : fd_(other.release()) {}
    UniqueFd& operator=(UniqueFd&& other) noexcept {
        if (this != &other) reset(other.release());
        return *this;
    }

    [[nodiscard]] int get() const noexcept { return fd_; }
    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }
    [[nodiscard]] int release() noexcept { return std::exchange(fd_, -1); }
    void reset(int fd = -1) noexcept {
        if (fd_ == fd) return;
        const int old = std::exchange(fd_, fd);
        if (old >= 0) {
            // Cleanup must not replace the error from the failing operation.
            const int saved_errno = errno;
            (void)::close(old);
            errno = saved_errno;
        }
    }

private:
    int fd_ = -1;
};

}  // namespace llavon::ime
