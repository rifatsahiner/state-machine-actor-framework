#ifndef uOS_H
#define uOS_H

#include <type_traits>

#include <functional>
#include <utility>

#include <array>
#include <forward_list>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "task.h"

#ifdef uOS_ENABLE_CLI
namespace cli {
class Cli;
class Menu;
class CmdHandler;
namespace detail {
template <typename t> class GenericAsioScheduler;

template <typename t> class CliGenericTelnetServer;

class NewStandaloneAsioLib;
} // namespace detail
} // namespace cli
#endif

namespace uOS {

// static class, wont get instatiated
class FW {
public:
    // fw op
    static void init(int argc, char* argv[], const std::string& appName = "uOS-App");
    static int32_t run(void);
    static void kill(void);

    static void setSystemLogLevels(LogLevel fileLevel, LogLevel uiLevel);
    static void setTaskLogLevels(TaskId id, LogLevel fileLevel, LogLevel uiLevel);
    static LogLevel systemFileLogLevel();
    static LogLevel systemUiLogLevel();
    static LogLevel taskFileLogLevel(TaskId id);
    static LogLevel taskUiLogLevel(TaskId id);

    // task op
    static void startTask(TaskId);
    static void stopTask(TaskId, bool waitShutdown = true);
    static void resumeTask(TaskId, bool cleanStart);
    static void deleteTask(TaskId); // do not call from state-handler for its own task
    static std::string_view getTaskState(TaskId taskId);
    static bool isTaskIdle(TaskId taskId);

    // event delivery
    // all post/publish calls consumes event ownership
    // for better performance consider moving the event pointer on post/publish calls to avoid copying
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

    // task related. Ids 1 .. MAX-1; NO_TASK_ID is never stored.
    static inline std::array<bool, uOS_FW_MAX_TASK_COUNT> _taskIdUsageList;
    static inline std::unordered_map<TaskId, std::shared_ptr<Task>> _taskMap;
    static inline std::vector<std::shared_ptr<SyncTask>> _syncTaskList;
    static inline std::shared_mutex _taskMutex;
    static inline std::vector<TaskId> _deleteList;
    static inline std::mutex _listMutex;

    // timer related. Ids 1 .. MAX-1; IDLE_TIMER_ID is never stored.
    static inline std::array<TimerId, uOS_FW_MAX_TIMER_COUNT> _freeTimerIds{};
    static inline uint_fast16_t _freeTimerHead = 0;
    static inline uint_fast16_t _freeTimerCount = 0;
    static inline std::unordered_map<TimerId, TimerNode> _timerStorageMap;
    static inline std::forward_list<std::pair<TimerId, uint_fast16_t>> _timerQueue;
    static inline std::mutex _timerMutex;

    // pub-sub related
    static inline std::unordered_map<SignalId, std::vector<TaskId>> _subsMap;
    static inline std::mutex _subsMutex;

#ifdef uOS_ENABLE_CLI
    // cli related
    static std::unique_ptr<cli::Cli> _cli;
    static std::unique_ptr<cli::detail::GenericAsioScheduler<cli::detail::NewStandaloneAsioLib>> _cliScheduler;
    static std::unique_ptr<cli::detail::CliGenericTelnetServer<cli::detail::NewStandaloneAsioLib>> _cliServer;
    static std::unordered_map<std::string, cli::CmdHandler> _menuHandlerMap;
    static inline cli::Menu* _cliRootMenu;
#endif

    // private functions
    static std::shared_ptr<Task> _requireTask(TaskId taskId);
    static std::shared_ptr<Task> _findTask(TaskId taskId);
    static TaskId _getAvailableTaskId(void);
    static TimerId _getAvailableTimerId(void);
    static void _releaseTimerId(TimerId);
    static void _insertTimerQueue(TimerId, uint_fast16_t);
    static TimerId _setupTimedEvent(bool, bool, uint_fast16_t, std::shared_ptr<const Event>&&, TaskId receiverTaskId = NO_TASK_ID);
    static void _handleTick(void);
    static void _handleDelete(void);
    static void _publishEvent(std::shared_ptr<const Event>&&);
    static void _publishEventCommon(std::shared_ptr<const Event>&&, std::vector<TaskId>&);
    static void _sigintHandler(int signum);

#ifdef uOS_ENABLE_CLI
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

    static void _cliPostEventCommon(std::ostream& out, SignalId signalId, const std::string& jsonPath, TaskId targetTaskId,
                                    std::optional<TaskId> sourceTaskId = std::nullopt);
    static void _cliPubEventCommon(std::ostream& out, SignalId signalId, const std::string& jsonPath,
                                   std::optional<TaskId> sourceTaskId = std::nullopt);
#endif

    // protected functions for Task class
    static void subscribe(SignalId, TaskId);
    static void subscribe(std::initializer_list<SignalId>, TaskId);
    static void unsubscribe(SignalId, TaskId);
    static void unsubscribe(std::initializer_list<SignalId>, TaskId);
    static void deleteAsyncTask(TaskId);
#ifdef uOS_ENABLE_CLI
    static void registerCliMenu(TaskId, std::unique_ptr<cli::Menu>&&);
#endif
    // todo: static void registerStaticCliMenu(std::unique_ptr<cli::Menu>&&);

    friend class Task;
    friend class AsyncTask;

public:
    // task logger is not ready until _topInitialTrans. create logging members there and not on task constructor
    template <class T, class... TaskArgs> static TaskId createTask(std::string taskName = "no-name-inst", TaskArgs&&... taskArgs) {
        decltype(_taskMap)::iterator iter;

        static_assert(std::is_base_of_v<Task, T>);

        {
            std::lock_guard lock(_taskMutex);
            bool result;

            TaskId newTaskId = FW::_getAvailableTaskId();

            std::tie(iter, result) =
                _taskMap.try_emplace(newTaskId, std::make_shared<T>(newTaskId, std::move(taskName), std::forward<TaskArgs>(taskArgs)...));
            ASSERT_FOR_FATAL(result == true);
            if (auto syncTaskPtr = std::dynamic_pointer_cast<SyncTask>(iter->second)) {
                _syncTaskList.push_back(std::move(syncTaskPtr));
            }
        }

        iter->second->init(); // see function body for explanation

        return iter->first;
    }
};

} // namespace uOS

#include "child_state_machine.h"

#endif // uOS_H