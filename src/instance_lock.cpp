#include "instance_lock.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace tombstone {

namespace {
constexpr const char *kLockFileName = "instance.lock";
}  // namespace

InstanceLock::~InstanceLock() { release(); }

#if defined(_WIN32)

bool InstanceLock::try_acquire(const std::filesystem::path &dir) noexcept {
    if (held_) {
        return true;
    }
    try {
        const std::filesystem::path path = dir / kLockFileName;
        HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            return false;
        }
        OVERLAPPED region = {};
        if (LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0,
                       &region) == 0) {
            CloseHandle(handle);
            return false;
        }
        handle_ = handle;
        held_ = true;
        return true;
    } catch (...) {
        return false;
    }
}

void InstanceLock::release() noexcept {
    if (handle_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(handle_));  // closing the handle drops the lock
        handle_ = nullptr;
    }
    held_ = false;
}

#else

bool InstanceLock::try_acquire(const std::filesystem::path &dir) noexcept {
    if (held_) {
        return true;
    }
    try {
        const std::filesystem::path path = dir / kLockFileName;
        const int fd = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (fd < 0) {
            return false;
        }
        if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
            close(fd);
            return false;
        }
        fd_ = fd;
        held_ = true;
        return true;
    } catch (...) {
        return false;
    }
}

void InstanceLock::release() noexcept {
    if (fd_ >= 0) {
        close(fd_);  // closing the descriptor drops the lock
        fd_ = -1;
    }
    held_ = false;
}

#endif

}  // namespace tombstone
