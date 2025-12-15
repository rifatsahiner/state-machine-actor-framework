
#include <cassert>

#include "uOS.h"


namespace uOS {


Task::Task(const TaskId taskId, const std::string& taskInstName) : _id{taskId}, _instName{taskInstName} {}

Task::~Task() {
    Logger::removeTaskLogger(_id);
}

void Task::init(void){
    // get task specific data from overriden functions
    _taskName = _getTaskName();
    _stateNameMap = _getStateNameMap();

    // register task to logger
    Logger::addTaskLogger(_id, _taskName, _instName);

    LOG(LogLevel::LOG_INFO, "Instance {} is initialized", _instName);

    // note: this function is needed because these pure virtual functions cannot be called in constructor
}

void Task::start(void)
{
    assert(_isRunning == false);

    StateMachine::init();
    resume(true);
}

void Task::putEvent(std::shared_ptr<const Event>&& newEvent)
{    
    // lock the queue
    std::lock_guard<std::mutex> lock(_threadMutex);

    // put event into te queue
    _eventQueue.emplace(std::move(newEvent));
    
    _taskCv.notify_one();  // Always notify (safe: spurious wakes are harmless)
}

#ifndef NDEBUG
bool Task::isIdle(void) {
    std::lock_guard<std::mutex> lock(_threadMutex);
    // std::cout << "queue: " << _eventQueue.size() << ", processing: " << _isProcessing.load(std::memory_order_relaxed) << std::endl;
    return _eventQueue.empty() && _isProcessing.load(std::memory_order_relaxed) == false;
}
#endif

void Task::stop(bool waitJoin) 
{    
    {
        std::lock_guard<std::mutex> lock(_threadMutex);
        _isRunning = false;
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

void Task::resume(bool cleanStart) {
    if(_isRunning == false)
    {
        if(cleanStart == true) {
            // clear event queue
            std::lock_guard<std::mutex> lock(_threadMutex);
            std::queue<std::shared_ptr<const Event>> empty;
            std::swap(_eventQueue, empty);
        }

        _isRunning = true;
        _taskThead = std::thread {&Task::_taskLoop, this};

        std::ostringstream oss;
        oss << _taskThead.get_id();  // thread id is not formatable, so use ostringstream
        LOG(LogLevel::LOG_INFO, "Task started with thread id: {}", oss.str());
    }
}

std::string Task::getName(void) const {
    return _taskName + "-" + _instName;
}

std::string_view Task::getState(void) const
{
    State currentState = _getState();
    for(const auto& [state, name] : _stateNameMap) {
        if(currentState == state) {
            return name;
        }
    }
    
    assert(false);
}

void Task::cliEventSink(std::ostream& out, SignalId signalId, const nlohmann::json& json, std::optional<TaskId> sourceTaskId) {
    (void)signalId;
    (void)json;
    (void)sourceTaskId;

    out << "event injector handler is not implemented for this task. id: " << (unsigned)_id << "\n";
}

Task::HandleResult Task::_terminationState(const std::shared_ptr<const uOS::Event>& event){
    HandleResult res;

    // call self-delete on entry and discard all events
    switch(event->signal) {
        case ENTRY_SIG : {
            LOG(LogLevel::LOG_INFO, "ENTRY for TERMINATION-STATE");
            _selfDelete();
            res = _handled();
            break;
        }

        default: {
            res = _super(&Task::top);
            break;
        }
    }

    return res;
}

void Task::_subscribe(SignalId signalId) { FW::subscribe(signalId, _id); }

void Task::_subscribe(std::initializer_list<SignalId> signalList) { FW::subscribe(signalList, _id); }

void Task::_unsubscribe(SignalId signalId) { FW::unsubscribe(signalId, _id); }

void Task::_unsubscribe(std::initializer_list<SignalId> signalList) { FW::unsubscribe(signalList, _id); }

void Task::_selfPost(std::shared_ptr<const Event> event)
{
    putEvent(std::move(event));
}

TimerId Task::_selfPostIn(uint_fast16_t intervalMs, std::shared_ptr<const Event> event) { return FW::postEventIn(intervalMs, _id, std::move(event)); }

TimerId Task::_selfPostEvery(uint_fast16_t intervalMs, std::shared_ptr<const Event> event) { return FW::postEventEvery(intervalMs, _id, std::move(event)); }

void Task::_selfStop(void) { FW::stop(_id); }

void Task::_selfDelete(void) { FW::delete_(_id); }

void Task::_registerCliMenu(std::unique_ptr<cli::Menu>&& menuPtr) { FW::registerCliMenu(_id, std::move(menuPtr)); }

//////////////////////////////////////////////////////
//                                                  //
//////////////////////////////////////////////////////

void Task::_taskLoop(void)
{
    std::unique_lock<std::mutex> threadLock(_threadMutex);

    LOG(LogLevel::LOG_INFO, "Task thread loop entry");

    while (true) {
        // Wait while empty and running (atomic under mutex)
        while (_eventQueue.empty() && _isRunning) {
            _taskCv.wait(threadLock);
        }

        if (!_isRunning) {
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


}   // namespace uOS
