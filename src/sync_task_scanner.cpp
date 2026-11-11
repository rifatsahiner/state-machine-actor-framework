#include "detail/sync_task_scanner.h"

#include <algorithm>
#include <utility>

namespace uOS {

size_t syncScanThreadCount(size_t taskCount) { return taskCount / uOS_FW_SYNC_TASKS_PER_THREAD + 1; }

SyncScanRange syncScanSlice(size_t taskCount, size_t threadCount, size_t threadIndex) {
    SyncScanRange range{0, 0};
    if (threadCount == 0 || threadIndex >= threadCount) {
        return range;
    }

    const size_t base = taskCount / threadCount;
    const size_t rem = taskCount % threadCount;
    range.begin = threadIndex * base + std::min(threadIndex, rem);
    range.end = range.begin + base + (threadIndex < rem ? 1 : 0);
    return range;
}

SyncTaskScanner::SyncTaskScanner(size_t extraWorkers, StartFn onStart) : _extraWorkers(extraWorkers), _onStart(onStart) {
    _workers.reserve(extraWorkers);
    for (size_t i = 0; i < extraWorkers; ++i) {
        _workers.emplace_back([this, i] { workerMain(i); });
    }
}

SyncTaskScanner::~SyncTaskScanner() { shutdown(); }

void SyncTaskScanner::shutdown() {
    std::vector<std::thread> joining;
    {
        std::lock_guard lock(_mutex);
        _running = false;
        joining.swap(_workers);
    }
    _cvStart.notify_all();
    for (auto& thread : joining) {
        if (thread.joinable()) {
            thread.join();
        }
    }
}

void SyncTaskScanner::run(size_t taskCount, void* ctx, SliceFn fn, size_t threadCount) {
    if (taskCount == 0 || fn == nullptr) {
        return;
    }

    size_t threads = threadCount == 0 ? syncScanThreadCount(taskCount) : threadCount;
    if (threads == 0) {
        threads = 1;
    }
    const size_t cap = _extraWorkers + 1;
    if (threads > cap) {
        threads = cap;
    }

    // The FW thread is the only thread. Waking the pool would only add delay.
    if (threads == 1) {
        fn(ctx, 0, taskCount);
        return;
    }

    std::exception_ptr callerError;
    {
        std::lock_guard lock(_mutex);
        _jobTasks = taskCount;
        _jobThreads = threads;
        _jobCtx = ctx;
        _jobFn = fn;
        _finished = 0;
        _error = nullptr;
        ++_generation;
    }
    _cvStart.notify_all();

    const SyncScanRange mine = syncScanSlice(taskCount, threads, threads - 1);
    try {
        fn(ctx, mine.begin, mine.end);
    } catch (...) {
        callerError = std::current_exception();
    }

    std::exception_ptr error;
    {
        std::unique_lock lock(_mutex);
        if (callerError && !_error) {
            _error = std::move(callerError);
        }
        ++_finished;
        if (_finished < threads) {
            _cvDone.wait(lock, [&] { return _finished >= threads; });
        }
        error = _error;
        _error = nullptr;
    }
    if (error) {
        std::rethrow_exception(error);
    }
}

void SyncTaskScanner::workerMain(size_t workerIndex) {
    if (_onStart != nullptr) {
        _onStart();
    }

    uint64_t seen = 0;
    while (true) {
        size_t begin = 0;
        size_t end = 0;
        void* ctx = nullptr;
        SliceFn fn = nullptr;
        {
            std::unique_lock lock(_mutex);
            // workerIndex < threads - 1: the caller runs the last slice.
            _cvStart.wait(lock, [&] { return !_running || (_generation > seen && workerIndex + 1 < _jobThreads); });
            if (!_running) {
                return;
            }
            seen = _generation;
            ctx = _jobCtx;
            fn = _jobFn;
            const SyncScanRange range = syncScanSlice(_jobTasks, _jobThreads, workerIndex);
            begin = range.begin;
            end = range.end;
        }

        std::exception_ptr error;
        try {
            fn(ctx, begin, end);
        } catch (...) {
            error = std::current_exception();
        }

        {
            std::lock_guard lock(_mutex);
            if (error && !_error) {
                _error = std::move(error);
            }
            if (++_finished >= _jobThreads) {
                _cvDone.notify_one();
            }
        }
    }
}

} // namespace uOS
