#ifndef uOS_SYNC_TASK_SCANNER_H
#define uOS_SYNC_TASK_SCANNER_H

#include "uOS_defs.h"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

namespace uOS {

// thread_count = taskCount / uOS_FW_SYNC_TASKS_PER_THREAD + 1, including taskCount 0.
size_t syncScanThreadCount(size_t taskCount);

struct SyncScanRange {
    size_t begin;
    size_t end;
};

// Contiguous slice of threadIndex in [0, threadCount). Lengths differ by at most one.
SyncScanRange syncScanSlice(size_t taskCount, size_t threadCount, size_t threadIndex);

// Persistent workers for one sync-task scan. The caller runs one slice.
// The scanner does not know about tasks, locks, or logging.
class SyncTaskScanner {
public:
    using SliceFn = void (*)(void* ctx, size_t begin, size_t end);
    using StartFn = void (*)();

    // extraWorkers threads are started immediately. onStart runs once on each, before it waits.
    explicit SyncTaskScanner(size_t extraWorkers, StartFn onStart = nullptr);
    ~SyncTaskScanner();

    SyncTaskScanner(const SyncTaskScanner&) = delete;
    SyncTaskScanner& operator=(const SyncTaskScanner&) = delete;

    // threadCount 0 uses syncScanThreadCount(taskCount), then clamps to extraWorkers + 1.
    // One thread runs the whole range on the caller and does not wake the pool.
    // taskCount 0 returns without calling fn. A slice exception is rethrown after every slice finishes.
    void run(size_t taskCount, void* ctx, SliceFn fn, size_t threadCount = 0);

    // Join the workers. FW::run calls this before tasks are destroyed.
    void shutdown();

private:
    void workerMain(size_t workerIndex);

    size_t _extraWorkers;
    StartFn _onStart;
    std::vector<std::thread> _workers;
    std::mutex _mutex;
    std::condition_variable _cvStart;
    std::condition_variable _cvDone;
    bool _running{true};
    uint64_t _generation{0};
    size_t _jobTasks{0};
    size_t _jobThreads{0};
    void* _jobCtx{nullptr};
    SliceFn _jobFn{nullptr};
    size_t _finished{0};
    std::exception_ptr _error{nullptr};
};

} // namespace uOS

#endif // uOS_SYNC_TASK_SCANNER_H
