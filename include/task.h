#ifndef TASK_H
#define TASK_H

#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <atomic>
#include <string_view>

#include "state_machine.h"
#include "detail/logger.h"
#include "detail/json_fwd.h"


namespace cli { class Menu; }


namespace uOS {


class Task : public StateMachine
{
    public:
        // framework service methods, do not use in user task code
        void init(void);
        void start(void);
        virtual void stop(bool waitJoin = true) = 0;
        virtual void resume(bool cleanStart = true) = 0;
        virtual void putEvent(std::shared_ptr<const Event>&&) = 0;

        enum class Type {Sync, Async};

        virtual Type getType(void) const = 0;
        std::string getName(void) const;
        std::string_view getState(void) const;
#ifndef NDEBUG
        virtual bool isIdle(void) = 0;
#endif
        virtual void cliEventSink(std::ostream& out, SignalId signalId, const nlohmann::json& json, std::optional<TaskId> sourceTaskId = std::nullopt);

        ~Task();

    protected:
        Task(const TaskId, const std::string&);
        
        Task(const Task& other) = delete;
        Task& operator= (const Task& rhs) = delete;
        Task(Task&&) = delete;
        Task& operator=(Task&&) = delete;

        // termination psude-state -> 🔘
        // user tasks can override this if a custom termination needed
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
        void _registerCliMenu(std::unique_ptr<cli::Menu>&&);

        const TaskId _id;
        std::queue<std::shared_ptr<const Event>> _eventQueue;
        std::atomic<bool> _isRunning {false};
        
        template <class T = Event>
        std::shared_ptr<T> new_e(std::optional<SignalId> signalId = std::nullopt)
        {
            std::shared_ptr<T> e = uOS::new_e<T>(signalId);
            e->source = _id;
            return e;
        }

    private:
        // task name and state-name map will be provided from tasks
        virtual std::string _getTaskName(void) const = 0;
        virtual std::vector<std::pair<State, std::string_view>> _getStateNameMap(void) const = 0;
        std::string _instName;
        std::string _taskName;
        std::vector<std::pair<State, std::string_view>> _stateNameMap;  // couldnt make State work as a key for unordered_map or map, so using vector of pairs

    protected:
        template <typename... LogStrArgs>
        void log__(const char* file, const char* function, int line, LogLevel level, fmt::format_string<LogStrArgs...> logStr, LogStrArgs&&... logStrArgs) {
            // format logStr
            auto formattedStr = fmt::format(logStr, std::forward<LogStrArgs>(logStrArgs)...);

            // sent to logger
            Logger::logTask(file, function, line, level, formattedStr, _id, _taskName + '-' + _instName);
        } 
};

class AsyncTask : public Task
{
    public:
        void stop(bool waitJoin = true) override;
        void resume(bool cleanStart = true) override;
        void putEvent(std::shared_ptr<const Event>&& event) override;
        Type getType() const override { return Type::Async; }
#ifndef NDEBUG
        bool isIdle(void) override;
#endif

        ~AsyncTask();

    protected:
        using Task::Task;   // inherit constructor

        void _selfStop(void) override;
        void _selfDelete(void) override;

    private:
        std::thread _taskThead;
        std::mutex _threadMutex;
        std::condition_variable _taskCv;
#ifndef NDEBUG
        std::atomic<bool> _isProcessing{false};  // Only in non-release
#endif

        void _taskLoop(void);
};

class SyncTask : public Task
{
    public:
        void stop(bool waitJoin = true) override;
        void resume(bool cleanStart = true) override;
        void putEvent(std::shared_ptr<const Event>&& event) override;
        void runCycle(void);
        Type getType() const override { return Type::Sync; }
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


}   // namespace uOS


#endif  // TASK_H