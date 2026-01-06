
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
    assert(_isRunning.load(std::memory_order_relaxed) == false);

    StateMachine::init();
    resume(true);
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

void Task::_registerCliMenu(std::unique_ptr<cli::Menu>&& menuPtr) { FW::registerCliMenu(_id, std::move(menuPtr)); }


}   // namespace uOS
