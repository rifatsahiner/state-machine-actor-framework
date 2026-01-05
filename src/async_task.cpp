
#include "uOS.h"


namespace uOS {


AsyncTask::~AsyncTask() {
    if(_taskThead.joinable()) {
        LOG(LogLevel::LOG_ERROR, "Async task destructor called while task thread is still joinable");
        _isRunning.store(false, std::memory_order_relaxed);
        _taskCv.notify_one();
        _taskThead.join();
    }
}

void AsyncTask::putEvent(std::shared_ptr<const Event>&& newEvent)
{    
    // lock the queue
    std::lock_guard<std::mutex> lock(_threadMutex);

    // put event into te queue
    _eventQueue.emplace(std::move(newEvent));
    
    _taskCv.notify_one();  // Always notify (safe: spurious wakes are harmless)
}

#ifndef NDEBUG
bool AsyncTask::isIdle(void) {
    std::lock_guard<std::mutex> lock(_threadMutex);
    // std::cout << "queue: " << _eventQueue.size() << ", processing: " << _isProcessing.load(std::memory_order_relaxed) << std::endl;
    return _eventQueue.empty() && _isProcessing.load(std::memory_order_relaxed) == false;
}
#endif


void AsyncTask::stop(bool waitJoin) 
{    
    {
        // std::lock_guard<std::mutex> lock(_threadMutex);
        _isRunning.store(false, std::memory_order_relaxed);
        _taskCv.notify_one();  // Wake if waiting (predicate will see !_isRunning)
    }

    if(waitJoin == true){
        if(_taskThead.joinable()) {
            LOG(LogLevel::LOG_INFO, "Waiting for task thread to join");
            _taskThead.join();
        } else {
            LOG(LogLevel::LOG_WARNING, "Task thread is not joinable, may be already stopped or not started");
        }
    }

    LOG(LogLevel::LOG_INFO, "Task stopped");
}

void AsyncTask::resume(bool cleanStart) {
    if(_isRunning.load(std::memory_order_relaxed) == false)
    {
        if(cleanStart == true) {
            // clear event queue
            std::lock_guard<std::mutex> lock(_threadMutex);
            std::queue<std::shared_ptr<const Event>> empty;
            std::swap(_eventQueue, empty);
        }

        _isRunning.store(true, std::memory_order_relaxed);
        _taskThead = std::thread {&AsyncTask::_taskLoop, this};

        std::ostringstream oss;
        oss << _taskThead.get_id();  // thread id is not formatable, so use ostringstream
        LOG(LogLevel::LOG_INFO, "Task started with thread id: {}", oss.str());
    }
}

void AsyncTask::_taskLoop(void)
{
    std::unique_lock<std::mutex> threadLock(_threadMutex);

    LOG(LogLevel::LOG_INFO, "Task thread loop entry");

    while (true) {
        // Wait while empty and running (atomic under mutex)
        while (_eventQueue.empty() && _isRunning.load(std::memory_order_relaxed)) {
            _taskCv.wait(threadLock);
        }

        if (!_isRunning.load(std::memory_order_relaxed)) {
            break;
        }

#ifndef NDEBUG
        _isProcessing.store(true, std::memory_order_relaxed);
#endif

        // Queue not empty: pop while holding
        auto currentEvent = std::move(_eventQueue.front());
        _eventQueue.pop();

        threadLock.unlock();  // Release for dispatch (non-blocking)

        LOG(LogLevel::LOG_TRACE, "get event -> signalId: {}", currentEvent->signal);
        dispatch(*currentEvent);

#ifndef NDEBUG
        _isProcessing.store(false, std::memory_order_relaxed);
#endif

        threadLock.lock();  // Re-acquire for next iteration's check
    }
      
    threadLock.unlock();
    LOG(LogLevel::LOG_INFO, "Task thread loop exit");
}

void AsyncTask::_selfStop(void) { FW::stopTask(_id, false); }

void AsyncTask::_selfDelete(void) { FW::deleteAsyncTask(_id); }


}    // namespace uOS

