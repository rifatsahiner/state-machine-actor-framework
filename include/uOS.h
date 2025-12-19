#ifndef uOS_H
#define uOS_H

#include <cassert>
#include <type_traits>

#include <utility>
#include <functional>

#include <string>
#include <array>
#include <vector>
#include <forward_list>
#include <unordered_map>

#include "task.h"


namespace cli {
    class Cli;
    class Menu;
    class CmdHandler;
    namespace detail {
        template <typename t>
        class GenericAsioScheduler;

        template <typename t>
        class CliGenericTelnetServer;
        
        class NewStandaloneAsioLib;
    }
}


namespace uOS {


// static class, wont get instatiated
class FW
{
    public:
        // fw op
        static void init(int argc, char* argv[], const std::string& appName = "uOS-App");
        static int32_t run(void);
        static void kill(void);

        // task op
        static void startTask(TaskId);
        static void stopTask(TaskId, bool waitShutdown = true);     // do not call from state-handler for its own task
        static void resumeTask(TaskId, bool cleanStart);
        static void deleteTask(TaskId);                             // do not call from state-handler for its own task
        static std::string_view getTaskState(TaskId taskId);
        static bool isTaskIdle(TaskId taskId);

        // event delivery
        // all post/publish calls consumes event ownership
        // for better performance consider moving the event on post/publish calls to avoid copying
        static void postEvent(TaskId, std::shared_ptr<const Event>);
        static void publishEvent(std::shared_ptr<const Event>);

        static TimerId postEventIn(uint_fast16_t intervalMs, TaskId, std::shared_ptr<const Event>);
        static TimerId postEventEvery(uint_fast16_t intervalMs, TaskId, std::shared_ptr<const Event>);
        static TimerId publishEventIn(uint_fast16_t intervalMs, std::shared_ptr<const Event>);
        static TimerId publishEventEvery(uint_fast16_t intervalMs, std::shared_ptr<const Event>);
        static void cancelTimedEvent(TimerId);

    private:
        // private definitions
        struct TimerNode {
            bool isPublish;
            bool isPeriodic;
            TaskId receiverTaskId;
            uint_fast16_t interval;
            std::shared_ptr<const Event> eventPtr;
        };

        static volatile inline bool _isRunning = true;

        // task related
        static inline std::array<bool, uOS_FW_MAX_TASK_COUNT> _taskIdUsageList;
        static inline std::unordered_map<TaskId, std::unique_ptr<AsyncTask>> _taskMap;
        static inline std::shared_mutex _taskMutex;
        static inline std::vector<TaskId> _stopList;
        static inline std::vector<TaskId> _deleteList;
        static inline std::mutex _listMutex;

        // timer related
        static inline std::array<bool, uOS_FW_MAX_TIMER_COUNT> _timerIdUsageList{};
        static inline std::unordered_map<TimerId, TimerNode> _timerStorageMap;
        static inline std::forward_list<std::pair<TimerId, uint_fast16_t>> _timerQueue;
        static inline std::mutex _timerMutex;

        // pub-sub related
        static inline std::unordered_map<SignalId, std::vector<TaskId>> _subsMap;
        static inline std::mutex _subsMutex;

        // cli related
        static std::unique_ptr<cli::Cli> _cli;
        static std::unique_ptr<cli::detail::GenericAsioScheduler<cli::detail::NewStandaloneAsioLib>> _cliScheduler;
        static std::unique_ptr<cli::detail::CliGenericTelnetServer<cli::detail::NewStandaloneAsioLib>> _cliServer;
        static std::unordered_map<std::string, cli::CmdHandler> _menuHandlerMap;
        static inline cli::Menu* _cliRootMenu;

        // private functions
        static TaskId _getAvailableTaskId(void);
        static TimerId _getAvailableTimerId(void);
        static void _insertTimerQueue(TimerId, uint_fast16_t);
        static TimerId _setupTimedEvent(bool, bool, uint_fast16_t, std::shared_ptr<const Event>&&, TaskId receiverTaskId = 0);
        static void _handleTick(void);
        static void _handleStopDelete(void);
        static void _publishEvent(std::shared_ptr<const Event>&&);
        static void _publishEventCommon(std::shared_ptr<const Event>&&, std::vector<TaskId>&);
        static void _sigintHandler(int signum);

        static void _cliListAllTasks(std::ostream& out);
        static void _cliListTasksWithSearch(std::ostream& out, const std::string& searchStr);
        static void _cliPostSignal(std::ostream& out, int signalId, int targetTaskId);
        static void _cliPostSignalWithSource(std::ostream& out, int signalId, int targetTaskId, int sourceTaskId);
        static void _cliPublishSignal(std::ostream& out, int signalId);
        static void _cliPublishSignalWithSource(std::ostream& out, int signalId, int sourceTaskId);
        static void _cliPostEvent(std::ostream& out, int signalId, const std::string& jsonPath, int targetTaskId);
        static void _cliPostEventWithSource(std::ostream& out, int signalId, const std::string& jsonPath, int targetTaskId, int sourceTaskId);
        static void _cliPublishEvent(std::ostream& out, int signalId, const std::string& jsonPath);
        static void _cliPublishEventWithSource(std::ostream& out, int signalId, const std::string& jsonPath, int sourceTaskId);

        static void _cliPostEventCommon(std::ostream& out, SignalId signalId, const std::string& jsonPath, TaskId targetTaskId, std::optional<TaskId> sourceTaskId = std::nullopt);
        static void _cliPubEventCommon(std::ostream& out, SignalId signalId, const std::string& jsonPath, std::optional<TaskId> sourceTaskId = std::nullopt);

        // protected functions for Task class
        static void subscribe(SignalId, TaskId);
        static void subscribe(std::initializer_list<SignalId>, TaskId);
        static void unsubscribe(SignalId, TaskId);
        static void unsubscribe(std::initializer_list<SignalId>, TaskId);
        static void stop(TaskId);
        static void delete_(TaskId);
        static void registerCliMenu(TaskId, std::unique_ptr<cli::Menu>&&);
        // todo: static void registerStaticCliMenu(std::unique_ptr<cli::Menu>&&);

        friend class Task;

    public:
        template <class T, class... TaskArgs>
        static TaskId createTask(std::string taskName = "no-name-inst", TaskArgs&&... taskArgs)
        {   
            static_assert(std::is_base_of_v<AsyncTask, T>);
            std::lock_guard lock(_taskMutex);

            TaskId newTaskId = FW::_getAvailableTaskId();
            
            const auto [iter, result] = _taskMap.try_emplace(newTaskId, std::make_unique<T>(newTaskId, std::move(taskName), std::forward<TaskArgs>(taskArgs)...));
            assert(result == true);

            iter->second->init();   // see function body for explanation

            return newTaskId;
        }
};

// log view finalize
// cmake with examples (add final, fmt, etc. as modules)
// todo: address sanitizer ekelencek

// bitset kullanılabilecek yerler var mı?
// custom objelerin operatörlerini overload edebileceğimiz durumlar var mı(bool, &&, >, < vs.)


}   // namespace uOS


#endif  // uOS_H