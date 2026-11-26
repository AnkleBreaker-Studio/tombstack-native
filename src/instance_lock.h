#ifndef TOMBSTONE_SRC_INSTANCE_LOCK_H
#define TOMBSTONE_SRC_INSTANCE_LOCK_H

#include <filesystem>

namespace tombstone {

/**
 * Which of several processes of the same game sharing one data directory owns
 * its per-session state. The first to take the exclusive lock on
 * `<data_dir>/instance.lock` owns the session marker, the session log, the
 * native crash dump and the drain of what earlier sessions left pending; any
 * other process (a second instance, a launcher relaunch, a client and a server
 * on one machine) leaves them alone, so it never reports the owner's live
 * session as an unclean shutdown or deletes the owner's marker. The operating
 * system releases the lock when the holder exits, crashed or not.
 *
 * Windows: LockFileEx; elsewhere: flock. Both lock per open file, so two locks
 * in one process also exclude each other. Never throws.
 */
class InstanceLock {
public:
    InstanceLock() noexcept = default;
    ~InstanceLock();

    InstanceLock(const InstanceLock &) = delete;
    InstanceLock &operator=(const InstanceLock &) = delete;
    InstanceLock(InstanceLock &&) = delete;
    InstanceLock &operator=(InstanceLock &&) = delete;

    /** Try once, without waiting. True when this process now owns `dir`. */
    bool try_acquire(const std::filesystem::path &dir) noexcept;

    bool held() const noexcept { return held_; }

    /** Release now (also done by the destructor and by the OS at exit). */
    void release() noexcept;

private:
#if defined(_WIN32)
    void *handle_{nullptr};
#else
    int fd_{-1};
#endif
    bool held_{false};
};

}  // namespace tombstone

#endif  // TOMBSTONE_SRC_INSTANCE_LOCK_H
