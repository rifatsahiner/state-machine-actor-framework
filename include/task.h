#ifndef TASK_H
#define TASK_H

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "log_stream.h"
#include "state_machine.h"
#ifdef uOS_ENABLE_CLI
#include "detail/json_fwd.h"

namespace cli {
class Menu;
}
#endif

namespace uOS {

class Task : public StateMachine {
public:
    // framework service methods, do not use in user task code
    void init(void);
    void start(void);
    virtual void stop(bool waitJoin = true) = 0;
    virtual void resume(bool cleanStart = true) = 0;
    virtual void putEvent(std::shared_ptr<const Event>&&) = 0;
    std::string getName(void) const;
    std::string_view getState(void) const;
    void setLogLevels(LogLevel fileLevel, LogLevel uiLevel) { _log.setLevels(fileLevel, uiLevel); }
#ifndef NDEBUG
    virtual bool isIdle(void) = 0;
#endif
#ifdef uOS_ENABLE_CLI
    virtual void cliEventSink(std::ostream& out, SignalId signalId, const nlohmann::json& json, std::optional<TaskId> sourceTaskId = std::nullopt);
#endif

    ~Task();

protected:
    Task(TaskId, std::string instName);

    Task(const Task& other) = delete;
    Task& operator=(const Task& rhs) = delete;
    Task(Task&&) = delete;
    Task& operator=(Task&&) = delete;

    // termination psude-state -> 🔘
    // user tasks can override this if a custom termination needed
    // todo: bu pointer event şeklinde kalmış, değiştirilecek
    virtual HandleResult _terminationState(const std::shared_ptr<const Event>&);

    // user task service methods
    void _subscribe(SignalId);
    void _subscribe(std::initializer_list<SignalId>);
    void _unsubscribe(SignalId);
    void _unsubscribe(std::initializer_list<SignalId>);
    void _selfPost(std::shared_ptr<const Event>);
    TimerId _selfPostIn(uint_fast16_t intervalMs, std::shared_ptr<const Event>);
    TimerId _selfPostEvery(uint_fast16_t intervalMs, std::shared_ptr<const Event>);
    virtual void _selfStop(void) = 0;
    virtual void _selfDelete(void) = 0;
    // Reference to this task's stream. Copy it to keep a handle.
    const LogStream& logStream() const { return _log; }
#ifdef uOS_ENABLE_CLI
    void _registerCliMenu(std::unique_ptr<cli::Menu>&&);
#endif

    const TaskId _id;
    std::queue<std::shared_ptr<const Event>> _eventQueue;
    std::atomic<bool> _isRunning{false};

    template <class T = Event> std::shared_ptr<T> new_e(std::optional<SignalId> signalId = std::nullopt) {
        std::shared_ptr<T> e = uOS::new_e<T>(signalId);
        e->source = _id;
        return e;
    }

private:
    virtual std::string _getTaskName(void) const = 0;
    virtual std::vector<std::pair<State, std::string_view>> _getStateNameMap(void) const = 0;
    std::string _instName;
    std::string _taskName;
    LogStream _log;
    std::vector<std::pair<State, std::string_view>>
        _stateNameMap; // couldnt make State work as a key for unordered_map or map, so using vector of pairs
};

class AsyncTask : public Task {
public:
    void stop(bool waitJoin = true) override;
    void resume(bool cleanStart = true) override;
    void putEvent(std::shared_ptr<const Event>&& event) override;
#ifndef NDEBUG
    bool isIdle(void) override;
#endif

    ~AsyncTask();

protected:
    using Task::Task; // inherit constructor

    void _selfStop(void) override;
    void _selfDelete(void) override;

private:
    std::thread _taskThead;
    std::mutex _threadMutex;
    std::condition_variable _taskCv;
#ifndef NDEBUG
    std::atomic<bool> _isProcessing{false}; // Only in non-release
#endif

    void _taskLoop(void);
};

class SyncTask : public Task {
public:
    void stop(bool waitJoin = true) override;
    void resume(bool cleanStart = true) override;
    void putEvent(std::shared_ptr<const Event>&& event) override;
    void runCycle(void);
#ifndef NDEBUG
    bool isIdle(void) override;
#endif

protected:
    using Task::Task;

    void _selfStop(void) override;
    void _selfDelete(void) override;

private:
    std::mutex _queueMutex;
};

} // namespace uOS

#endif // TASK_H