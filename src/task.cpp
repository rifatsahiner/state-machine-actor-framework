
#include "uOS.h"

#include "detail/logger.h"

namespace uOS {


Task::Task(const TaskId taskId, std::string taskInstName) : _id{taskId}, _instName{std::move(taskInstName)} {}

Task::~Task() {
    Logger::detachTask(_id);
}

void Task::init(void){
    _taskName = _getTaskName();
    _log = Logger::openTask(_taskName, _instName, _id);
    _stateNameMap = _getStateNameMap();

    LOG(LogLevel::LOG_INFO, "Instance {} is initialized", _instName);

    // note: this function is needed because these pure virtual functions cannot be called in constructor
}

void Task::start(void)
{
    if(_isRunning.load(std::memory_order_relaxed) == false){
        StateMachine::init();
        resume(true);
    } else {
        LOG(LogLevel::LOG_ERROR, "Start task function called for already running task instance {}", _instName);
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
    
    ASSERT_FOR_FATAL(false);
}

#ifdef uOS_ENABLE_CLI
void Task::cliEventSink(std::ostream& out, SignalId signalId, const nlohmann::json& json, std::optional<TaskId> sourceTaskId) {
    (void)signalId;
    (void)json;
    (void)sourceTaskId;

    out << "event injector handler is not implemented for this task. id: " << (unsigned)_id << "\n";
}
#endif

Task::HandleResult Task::_terminationState(const Event& event) {
    HandleResult res;

    // call self-delete on entry and discard all events
    switch (event.signal) {
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

#ifdef uOS_ENABLE_CLI
void Task::_registerCliMenu(std::unique_ptr<cli::Menu>&& menuPtr) { FW::registerCliMenu(_id, std::move(menuPtr)); }
#endif


}   // namespace uOS
