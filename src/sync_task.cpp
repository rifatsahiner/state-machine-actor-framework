
#include "uOS.h"


namespace uOS {


void SyncTask::putEvent(std::shared_ptr<const Event>&& event)
{    
    std::lock_guard<std::mutex> lock(_queueMutex);
    _eventQueue.emplace(std::move(event));
}

#ifndef NDEBUG
bool SyncTask::isIdle(void) {
    std::lock_guard<std::mutex> lock(_queueMutex);
    return _eventQueue.empty();
}
#endif

void SyncTask::stop(bool waitJoin) 
{    
    (void)waitJoin; // suppress unused warning
    
    _isRunning.store(false, std::memory_order_relaxed);
    LOG(LogLevel::LOG_INFO, "Task stopped");
}

void SyncTask::resume(bool cleanStart) {
    if(_isRunning.load(std::memory_order_relaxed) == false){
        if(cleanStart == true) {
            // clear event queue
            std::lock_guard<std::mutex> lock(_queueMutex);
            std::queue<std::shared_ptr<const Event>> empty;
            std::swap(_eventQueue, empty);
        }

        _isRunning.store(true, std::memory_order_relaxed);
        LOG(LogLevel::LOG_INFO, "Task started/resumed");
    } else {
        LOG(LogLevel::LOG_ERROR, "Resume called for already running task");
    }
}

void SyncTask::runCycle(void)
{
    while (_isRunning.load(std::memory_order_relaxed)) {
        std::shared_ptr<const Event> currentEvent;
        {
            std::lock_guard<std::mutex> lock(_queueMutex);
            if(!_eventQueue.empty()){
                currentEvent = std::move(_eventQueue.front());
                _eventQueue.pop();
            } else {
                // no event to process, exit cycle
                break;
            }
        }
        // LOG(LogLevel::LOG_TRACE, "get event -> signalId: {}", currentEvent->signal);
        dispatch(*currentEvent);
    }
}

void SyncTask::_selfStop(void) { FW::stopTask(_id); }

void SyncTask::_selfDelete(void) { FW::deleteTask(_id); }


}   // namespace uOS
