#pragma once

// RAII wrappers for Win32 handles. Include only from .cpp files.

#include <windows.h>

#include <cstdint>

namespace vaultik {

class UniqueHandle {
public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE h) : h_(h) {}
    ~UniqueHandle() { reset(); }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    UniqueHandle(UniqueHandle&& other) noexcept : h_(other.release()) {}
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) reset(other.release());
        return *this;
    }

    HANDLE get() const { return h_; }
    bool valid() const { return h_ != nullptr && h_ != INVALID_HANDLE_VALUE; }

    HANDLE release() {
        HANDLE h = h_;
        h_ = INVALID_HANDLE_VALUE;
        return h;
    }

    void reset(HANDLE h = INVALID_HANDLE_VALUE) {
        if (valid()) CloseHandle(h_);
        h_ = h;
    }

private:
    HANDLE h_ = INVALID_HANDLE_VALUE;
};

class FindHandle {
public:
    explicit FindHandle(HANDLE h) : h_(h) {}
    ~FindHandle() {
        if (h_ != INVALID_HANDLE_VALUE) FindClose(h_);
    }
    FindHandle(const FindHandle&) = delete;
    FindHandle& operator=(const FindHandle&) = delete;

private:
    HANDLE h_;
};

inline std::uint64_t filetime_to_u64(const FILETIME& ft) {
    return (static_cast<std::uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

inline FILETIME u64_to_filetime(std::uint64_t v) {
    FILETIME ft;
    ft.dwLowDateTime = static_cast<DWORD>(v & 0xFFFFFFFFu);
    ft.dwHighDateTime = static_cast<DWORD>(v >> 32);
    return ft;
}

}  // namespace vaultik
