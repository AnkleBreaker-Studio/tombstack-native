#include "instance_lock.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
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
            return true;  // cannot lock here at all: behave as the only process
        }
        OVERLAPPED region = {};
        if (LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0,
                       &region) == 0) {
            const DWORD error = GetLastError();
            CloseHandle(handle);
            return error != ERROR_LOCK_VIOLATION;  // only a lock held elsewhere contests it
        }
        handle_ = handle;
        held_ = true;
        return true;
    } catch (...) {
        return true;
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
            return true;  // cannot lock here at all: behave as the only process
        }
        if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
            const int error = errno;
            close(fd);
#if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
            const bool contested = error == EWOULDBLOCK || error == EAGAIN;
#else
            const bool contested = error == EAGAIN;
#endif
            return !contested;  // only a lock held elsewhere contests it (not ENOLCK...)
        }
        fd_ = fd;
        held_ = true;
        return true;
    } catch (...) {
        return true;
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
