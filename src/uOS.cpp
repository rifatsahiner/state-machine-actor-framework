
#include <algorithm>
#include <chrono>
#include <csignal>
#include <iterator>
#ifdef uOS_ENABLE_CLI
#include <fstream>

#include <cli/cli.h>
#include <cli/standaloneasioremotecli.h>
#include <cli/standaloneasioscheduler.h>

#include <nlohmann/json.hpp>
#endif

#include "uOS.h"

#include "detail/logger.h"
#include "detail/sync_task_scanner.h"

using namespace std::chrono_literals;

namespace uOS {

namespace {

volatile std::sig_atomic_t g_stopSignal = 0;

void runSyncSnapshot(void* ctx, size_t begin, size_t end) {
    auto* snapshot = static_cast<std::vector<SyncTask*>*>(ctx);
    for (size_t i = begin; i < end; ++i) {
        (*snapshot)[i]->runCycle();
    }
}

void logStopSignal() {
    if (g_stopSignal == 0) {
        return;
    }
    switch (g_stopSignal) {
        case SIGINT:
            SLOG(LogLevel::LOG_WARNING, "SIGINT received, stopping FW");
            break;
        case SIGTERM:
            SLOG(LogLevel::LOG_WARNING, "SIGTERM received, stopping FW");
            break;
        case SIGHUP:
            SLOG(LogLevel::LOG_WARNING, "SIGHUP received, stopping FW");
            break;
        case SIGQUIT:
            SLOG(LogLevel::LOG_WARNING, "SIGQUIT received, stopping FW");
            break;
        default:
            SLOG(LogLevel::LOG_ERROR, "Unknown signal received: {}", static_cast<int>(g_stopSignal));
            break;
    }
}

} // namespace

#ifdef uOS_ENABLE_CLI
// required for forward decleration
std::unique_ptr<cli::Cli> FW::_cli;
std::unique_ptr<cli::StandaloneAsioScheduler> FW::_cliScheduler;
std::unique_ptr<cli::StandaloneAsioCliTelnetServer> FW::_cliServer;
std::unordered_map<std::string, cli::CmdHandler> FW::_menuHandlerMap;
#endif

void FW::init(int argc, char* argv[], const std::string& appName) {
#ifdef uOS_ENABLE_TUI
    bool tui = true;
#endif
#ifdef uOS_ENABLE_CLI
    bool cli = true;
    int cliPort = 42069; // default CLI port
#endif

    // init usage lists
    _taskIdUsageList.fill(false);
    _freeTimerHead = 0;
    _freeTimerCount = uOS_FW_MAX_TIMER_COUNT - 1;
    for (uint_fast16_t i = 0; i < _freeTimerCount; ++i) {
        _freeTimerIds[i] = static_cast<TimerId>(i + 1);
    }
    _taskMap.clear();
    _syncTaskList.clear();
    _syncTaskList.reserve(uOS_FW_MAX_TASK_COUNT);
    _syncScanActive = false;
    _syncScanGraveyard.clear();
    _subsMap.clear();
    _deleteList.clear();

    _isRunning = true;

    // parse command line arguments
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
#ifdef uOS_ENABLE_TUI
        if (arg == "--no-tui") {
            tui = false;
            continue;
        }
#endif
#ifdef uOS_ENABLE_CLI
        if (arg == "--no-cli") {
            cli = false;
            continue;
        }
        if (arg == "--cli-port" && (i + 1) < argc) {
            try {
                size_t pos;
                std::string port_str = argv[i + 1];
                int port = std::stoi(port_str, &pos);
                if (pos == port_str.length() && port > 0 && port <= 65535) {
                    cliPort = port;
                    i++; // skip next argument as it is the port number
                }
            } catch (...) {
            }
            continue;
        }
#endif
    }

    // set interrupt signal handlers
#ifdef uOS_ENABLE_TUI
    if (tui == false)
        std::signal(SIGINT, _sigintHandler); // set if TUI is not active
#else
    std::signal(SIGINT, _sigintHandler);
#endif
    std::signal(SIGTERM, _sigintHandler);
    std::signal(SIGHUP, _sigintHandler);
    std::signal(SIGQUIT, _sigintHandler);

    // init logger
#ifdef uOS_ENABLE_TUI
    if (tui) {
        Logger::init(argc, argv, appName, &FW::kill); // todo: can set tui name with appName?
    } else {
        Logger::init(argc, argv, appName);
    }
#else
    Logger::init(argc, argv, appName);
#endif

#ifdef uOS_ENABLE_CLI
    // init cli if activated
    if (cli) {
        // setup cli
        // prepare root manu
        auto rootMenu = std::make_unique<cli::Menu>(appName);
        auto fwMenu = std::make_unique<cli::Menu>("Framework");
        fwMenu->Insert("list-tasks", &FW::_cliListAllTasks, "List current tasks");
        fwMenu->Insert("search-task", &FW::_cliListTasksWithSearch, "Search task name with given string (case sensitive)");
        fwMenu->Insert("post-sig", &FW::_cliPostSignal, "post a signal only event to a task <signalId> <targetTaskId>");
        fwMenu->Insert("post-sig-src", &FW::_cliPostSignalWithSource,
                       "post a signal only event to a task with source task info <signalId> <targetTaskId> <sourceTaskId>");
        fwMenu->Insert("pub-sig", &FW::_cliPublishSignal, "publish a signal only event <signalId>");
        fwMenu->Insert("pub-sig-src", &FW::_cliPublishSignalWithSource,
                       "publish a signal only event with source task info <signalId> <sourceTaskId>");
        fwMenu->Insert("post-evt", &FW::_cliPostEvent, "post an event to a task with parameters <signalId> <parameterJsonPath> <targetTaskId>");
        fwMenu->Insert("post-evt-src", &FW::_cliPostEventWithSource,
                       "post an event to a task with parameters and source task info <signalId> <parameterJsonPath> <targetTaskId> <sourceTaskId>");
        fwMenu->Insert("pub-evt", &FW::_cliPublishEvent, "publish an event with parameters <signalId> <parameterJsonPath>");
        fwMenu->Insert("pub-evt-src", &FW::_cliPublishEventWithSource,
                       "publish an event with parameters and source task info <signalId> <parameterJsonPath> <sourceTaskId>");
        rootMenu->Insert(std::move(fwMenu));

        // create cli object and assing wrong command handler to avoid cli using console
        _cliRootMenu = rootMenu.get();
        _cli = std::make_unique<cli::Cli>(std::move(rootMenu));
        _cli->WrongCommandHandler([](std::ostream& out, const std::string& cmd) {
            (void)out;
            SLOG(uOS::LogLevel::LOG_WARNING, "Unknown CLI command or incorrect parameters: {}", cmd);
        });

        // create scheduler and telnet server
        _cliScheduler = std::make_unique<cli::StandaloneAsioScheduler>();

        try {
            _cliServer = std::make_unique<cli::StandaloneAsioCliTelnetServer>(*_cli, *_cliScheduler, cliPort);
            _cliServer->ExitAction([](auto& out) { out << "Closing CLI connection...\n"; });
            SLOG(uOS::LogLevel::LOG_INFO, "CLI started on port: {}", cliPort);
        } catch (const std::system_error& e) {
            // Handle binding failure (e.g., port in use)
            SLOG(uOS::LogLevel::LOG_ERROR, "Failed to start CLI server on port {}: {}", cliPort, e.what());
            _cliServer.reset(); // Ensure it's null if failed
        } catch (const std::exception& e) {
            // Catch any other unexpected exceptions
            SLOG(uOS::LogLevel::LOG_ERROR, "Unexpected error starting CLI server: {}", e.what());
            _cliServer.reset();
        }
    } else {
        _cli.reset();
    }
#endif

    // lock memory so we're never swapped out to disk   ????
    // mlockall(MCL_CURRENT | MCL_FUTURE);          // uncomment when supported
}

int32_t FW::run() {
    const Event TICK_SEC_EVENT{SIG_TICK_SEC, std::nullopt};
    const Event TICK_MIN_EVENT{SIG_TICK_MIN, std::nullopt};
    const Event TICK_HOUR_EVENT{SIG_TICK_HOUR, std::nullopt};
    uint_fast8_t s = 0;
    uint_fast8_t m = 0;
    uint_fast8_t h = 0;
    std::chrono::milliseconds::rep maxLoopDuration = 0;
    uint_fast32_t loopOverrunCount = 0;

    // onStartup() aşağıdaki satırlar
    // struct termios tio;                        // modified terminal attributes
    // tcgetattr(0, &l_tsav);             // save the current terminal attributes
    // tcgetattr(0, &tio);              // obtain the current terminal attributes
    // tio.c_lflag &= ~(ICANON | ECHO);      // disable the canonical mode & echo
    // tcsetattr(0, TCSANOW, &tio);                     // set the new attributes
    // QF_setTickRate(DPP::BSP_TICKS_PER_SEC);       // set the desired tick rate

    // set thread priority ??? aşağıdaki satırlar
    // struct sched_param sparam;
    // sparam.sched_priority = sched_get_priority_max(SCHED_FIFO);
    // pthread_setschedparam(pthread_self(), SCHED_FIFO, &sparam) == 0;

    // Pool covers a full task table. The FW thread is one of the scan threads.
    const size_t extraScanWorkers = syncScanThreadCount(static_cast<size_t>(uOS_FW_MAX_TASK_COUNT)) - 1;
    SyncTaskScanner syncScanner(extraScanWorkers, prepareLogThread);

    // start logger
    Logger::start();
    SLOG(uOS::LogLevel::LOG_INFO, "Welcome to uOS application...");
    SLOG(uOS::LogLevel::LOG_INFO, "FW run loop entry");

    std::vector<SyncTask*> syncSnapshot;
    syncSnapshot.reserve(uOS_FW_MAX_TASK_COUNT);
    auto wakeup = std::chrono::steady_clock::now();

    //
    // main framework loop
    //
    while (_isRunning) {
        // Registered sync tasks. Thread count is snapshot size / TASKS_PER_THREAD + 1.
        // A post into another slice may run this tick or the next.
        bool scanning = false;
        {
            // Unique lock: the copy is a pointer memcpy, and it publishes _syncScanActive.
            std::unique_lock lock(_taskMutex);
            syncSnapshot.assign(_syncTaskList.begin(), _syncTaskList.end());
            if (!syncSnapshot.empty()) {
                _syncScanActive = true;
                scanning = true;
            }
        }

        if (scanning) {
            // Destroyed before timers, including when a slice throws.
            struct ScanGuard {
                std::vector<SyncTask*>* snapshot;
                explicit ScanGuard(std::vector<SyncTask*>* snapshot_) : snapshot(snapshot_) {}
                ScanGuard(const ScanGuard&) = delete;
                ScanGuard& operator=(const ScanGuard&) = delete;
                ~ScanGuard() {
                    _finishSyncScan();
                    snapshot->clear();
                }
            } guard{&syncSnapshot};

            syncScanner.run(syncSnapshot.size(), &syncSnapshot, runSyncSnapshot);
        }

        // handle timers
        _handleTick();

        // handle time events
        // check for second
        if (++s == 50) {
            _publishEvent(std::make_shared<const Event>(TICK_SEC_EVENT));
            s = 0;

            // check for minute
            if (++m == 60) {
                _publishEvent(std::make_shared<const Event>(TICK_MIN_EVENT));
                m = 0;

                // check for hour
                if (++h == 60) {
                    _publishEvent(std::make_shared<const Event>(TICK_HOUR_EVENT));
                    h = 0;
                }
            }
        }

#ifdef uOS_ENABLE_CLI
        // handle cli
        if (_cli) {
            _cliScheduler->PollOne();
        }
#endif

        // handle self delete request from tasks
        _handleDelete();

        // handle loop duration stats
        auto loopDuration = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - wakeup).count();
        if (maxLoopDuration < loopDuration) {
            maxLoopDuration = loopDuration;
        }
        if (loopDuration > 18) {
            loopOverrunCount++;
            SLOG(uOS::LogLevel::LOG_WARNING, "Loop overruned {} th time with {} ms. Max loop duration: {} ms.", loopOverrunCount, loopDuration,
                maxLoopDuration);
        }

        // sleep_remaining of 20ms loop period
        wakeup += 20ms;
        std::this_thread::sleep_until(wakeup);
    }

    logStopSignal();

    // Join scan workers before any task destructor runs.
    syncScanner.shutdown();

    // terminate all task loops
    SLOG(uOS::LogLevel::LOG_INFO, "FW run loop exit. Stopping all tasks");
    {
        std::shared_lock readlock(_taskMutex);
        for (const auto& taskPair : _taskMap) {
            taskPair.second->stop(true);
        }
    } // drop the shared lock before any destructor can post

    std::unordered_map<TaskId, std::shared_ptr<Task>> dyingTasks;
    std::vector<std::shared_ptr<Task>> dyingHeld;
    {
        std::unique_lock writeLock(_taskMutex);
        _syncTaskList.clear();
        _syncScanActive = false;
        _syncScanGraveyard.swap(dyingHeld);
        _taskMap.swap(dyingTasks);
    } // map is empty: _findTask() now returns nullptr

    // Graveyard destructors may postEvent. The map is already empty, so that misses.
    _retireTasks(std::move(dyingHeld));
    dyingTasks.clear(); // NsaUeTask::~NsaUeTask may postEvent; that is now a no-op

    // terminate logger
    SLOG(LogLevel::LOG_INFO, "uOS Bye bye!");
    Logger::stop();

    // onCleanup aşağıdaki satırlar
    // tcsetattr(0, TCSANOW, &l_tsav);   // restore the saved terminal attributes

    // hangi mutex bu, neden destroy ediliyor ???
    // pthread_mutex_destroy(&QF_pThreadMutex_);

    return 0;
}

void FW::kill(void) {
    SLOG(LogLevel::LOG_INFO, "Stopping FW");
    _isRunning = false;
}

void FW::setSystemLogLevels(LogLevel fileLevel, LogLevel uiLevel) { Logger::setSystemLogLevels(fileLevel, uiLevel); }

void FW::setTaskLogLevels(TaskId id, LogLevel fileLevel, LogLevel uiLevel) { Logger::setTaskLogLevels(id, fileLevel, uiLevel); }

LogLevel FW::systemFileLogLevel() { return Logger::systemFileLogLevel(); }

LogLevel FW::systemUiLogLevel() { return Logger::systemUiLogLevel(); }

LogLevel FW::taskFileLogLevel(TaskId id) { return Logger::taskFileLogLevel(id); }

LogLevel FW::taskUiLogLevel(TaskId id) { return Logger::taskUiLogLevel(id); }

std::shared_ptr<Task> FW::_requireTask(TaskId taskId) {
    std::shared_lock lock(_taskMutex);
    auto it = _taskMap.find(taskId);
    ASSERT_FOR_FATAL(it != _taskMap.cend());
    return it->second;
}

std::shared_ptr<Task> FW::_findTask(TaskId taskId) {
    std::shared_lock lock(_taskMutex);
    auto it = _taskMap.find(taskId);
    if (it == _taskMap.cend()) {
        return nullptr;
    }
    return it->second;
}

void FW::startTask(TaskId taskId) {
    auto task = _requireTask(taskId);

    SLOG(LogLevel::LOG_INFO, "Starting task with id: {}", taskId);
    task->start();
}

void FW::stopTask(TaskId taskId, bool waitShutdown) {
    auto task = _requireTask(taskId);

    SLOG(LogLevel::LOG_INFO, "Stoping task with id: {}", taskId);
    task->stop(waitShutdown);
}

std::string_view FW::getTaskState(TaskId taskId) {
    auto task = _requireTask(taskId);
    return task->getState();
}

bool FW::isTaskIdle(TaskId taskId) {
    auto task = _requireTask(taskId);
#ifndef NDEBUG
    return task->isIdle();
#else
    (void)task;
    return true;
#endif
}

void FW::resumeTask(TaskId taskId, bool cleanStart) {
    auto task = _requireTask(taskId);

    SLOG(LogLevel::LOG_INFO, "Resuming task with id: {}", taskId);
    task->resume(cleanStart);
}

void FW::_retireTasks(std::vector<std::shared_ptr<Task>> dying) {
    // ~Task may re-enter FW and take _taskMutex. TaskRelease frees the id after
    // that destructor returns. Clearing the bit here would race a reused id, and
    // would free an id whose object another shared_ptr still owns.
    dying.clear();
}

void FW::_releaseTaskId(TaskId taskId) {
    std::lock_guard lock(_taskMutex);
    ASSERT_FOR_FATAL(taskId != NO_TASK_ID);
    ASSERT_FOR_FATAL(taskId < uOS_FW_MAX_TASK_COUNT);
    _taskIdUsageList[taskId] = false;
}

void FW::_dropTaskRegistrations(TaskId taskId) {
    {
        auto checkSubsId = [taskId](TaskId subsId) { return subsId == taskId; };
        std::lock_guard<std::mutex> subsLock(_subsMutex);
        for (auto& pair : _subsMap) {
            auto& subsList = pair.second;
            auto it = std::find_if(subsList.begin(), subsList.end(), checkSubsId);
            if (it != subsList.end()) {
                subsList.erase(it);
            }
        }
    }

    std::vector<TimerId> receiverTimers;
    std::lock_guard<std::mutex> timerLock(_timerMutex);
    for (const auto& pair : _timerStorageMap) {
        const TimerNode& node = pair.second;
        if (!node.isPublish && node.receiverTaskId == taskId) {
            receiverTimers.push_back(pair.first);
        }
    }

    // A timer already popped by _handleTick is in storage and not in the queue.
    // That tick holds the storage node and releases it when the receiver is gone.
    for (TimerId timerId : receiverTimers) {
        auto itPrev = _timerQueue.cbefore_begin();
        for (auto itCancel = _timerQueue.begin(); itCancel != _timerQueue.cend(); ++itCancel) {
            if (itCancel->first == timerId) {
                auto itNext = std::next(itCancel);
                if (itNext != _timerQueue.cend()) {
                    itNext->second += itCancel->second;
                }
                _timerQueue.erase_after(itPrev);
                _timerStorageMap.erase(timerId);
                _releaseTimerId(timerId);
                break;
            }
            itPrev = itCancel;
        }
    }
}

void FW::_finishSyncScan(void) {
    std::vector<std::shared_ptr<Task>> dead;
    {
        std::lock_guard lock(_taskMutex);
        _syncScanActive = false;
        dead.swap(_syncScanGraveyard);
    }
    _retireTasks(std::move(dead));
}

void FW::deleteTask(TaskId taskId) {
    if (taskId == NO_TASK_ID) {
        return;
    }

    SLOG(LogLevel::LOG_INFO, "Deleting task with id: {}", taskId);

    // stop task
    stopTask(taskId, true);

    std::shared_ptr<Task> owned;
    {
        std::lock_guard taskLock(_taskMutex);
        auto taskIter = _taskMap.find(taskId);
        ASSERT_FOR_FATAL(taskIter != _taskMap.cend());

#ifdef uOS_ENABLE_CLI
        // get task name before deletion (needed to drop its CLI menu)
        std::string taskName = taskIter->second->getName();
#endif

        owned = std::move(taskIter->second);

        // Unlink the map entry and the sync-list pointer. TaskRelease makes the id available after ~Task.
        if (auto* syncTask = dynamic_cast<SyncTask*>(owned.get())) {
            auto syncTaskIt = std::find(_syncTaskList.begin(), _syncTaskList.end(), syncTask);
            ASSERT_FOR_FATAL(syncTaskIt != _syncTaskList.end());
            _syncTaskList.erase(syncTaskIt);
        }
        ASSERT_FOR_FATAL(_taskMap.erase(taskId) == 1);

        // The scan holds this raw pointer. Park the object; TaskRelease frees the id
        // after the scan joins and ~Task returns. The bit stays set either way.
        if (_syncScanActive) {
            _syncScanGraveyard.push_back(std::move(owned));
        }

#ifdef uOS_ENABLE_CLI
        // remove its cli menu if necessary
        if (_cli) {
            // check if it was last task of its kind
            if (std::none_of(_taskMap.cbegin(), _taskMap.cend(),
                             [name = std::string_view(taskName)](const auto& pair) { return pair.second->getName() == name; })) {
                // post to cli scheduler for thread safety
                _cliScheduler->Post([taskName = std::move(taskName)]() {
                    // if there is a menu for this task, remove it
                    auto it = _menuHandlerMap.find(taskName);
                    if (it != _menuHandlerMap.end()) {
                        it->second.Remove();
                        _menuHandlerMap.erase(it);
                        SLOG(LogLevel::LOG_INFO, "CLI menu for task: {} is removed", taskName);
                    }
                });
            }
        }
#endif
    }

    // Subscriptions and receiver timers, while this id is still reserved.
    // The id is freed later, after ~Task returns.
    _dropTaskRegistrations(taskId);

    // Destructor may re-enter FW. Do this after the registration locks are released.
    if (owned) {
        std::vector<std::shared_ptr<Task>> dying;
        dying.push_back(std::move(owned));
        _retireTasks(std::move(dying));
    }
}

void FW::postEvent(TaskId taskId, std::shared_ptr<const Event> event) {
    if (!_isRunning) {
        return;
    }
    // use find instead of require to avoid throwing exception while posting to a deleted task
    // which happens on shutdown on tasks that post on their destructors
    auto task = _findTask(taskId);
    if (task == nullptr) {
        SLOG(LogLevel::LOG_ERROR, "postEvent: task with id: {} not found for signalId: {}", taskId, event->signal);
        return;
    }
    task->putEvent(std::move(event));
}

TimerId FW::postEventIn(uint_fast16_t intervalMs, TaskId taskId, std::shared_ptr<const Event> event) {
    std::shared_lock lock(_taskMutex);
    ASSERT_FOR_FATAL(_taskMap.find(taskId) != _taskMap.cend());
    lock.unlock();

    // call common function for new timer creation
    return _setupTimedEvent(false, false, intervalMs / 20, std::move(event), taskId);
}

TimerId FW::postEventEvery(uint_fast16_t intervalMs, TaskId taskId, std::shared_ptr<const Event> event) {
    std::shared_lock lock(_taskMutex);
    ASSERT_FOR_FATAL(_taskMap.find(taskId) != _taskMap.cend());
    lock.unlock();

    return _setupTimedEvent(false, true, intervalMs / 20, std::move(event), taskId);
}

void FW::publishEvent(std::shared_ptr<const Event> event) {
    std::lock_guard<std::mutex> lock(_subsMutex);

    // get signal's subs list, return if signal id does not have subs list
    auto subListIter = _subsMap.find(event->signal);

    if (subListIter != _subsMap.cend()) {
        _publishEventCommon(std::move(event), subListIter->second);
    }
}

TimerId FW::publishEventIn(uint_fast16_t intervalMs, std::shared_ptr<const Event> event) {
    return _setupTimedEvent(true, false, intervalMs / 20, std::move(event));
}

TimerId FW::publishEventEvery(uint_fast16_t intervalMs, std::shared_ptr<const Event> event) {
    return _setupTimedEvent(true, true, intervalMs / 20, std::move(event));
}

void FW::cancelTimedEvent(TimerId timerId) {
    if (timerId == IDLE_TIMER_ID) {
        return;
    }

    std::lock_guard<std::mutex> lock(_timerMutex);

    // check if timer id is still present
    if (_timerStorageMap.find(timerId) != _timerStorageMap.cend()) {
        auto itPrev = _timerQueue.cbefore_begin();

        // find timer in queue
        for (auto itCancel = _timerQueue.begin(); itCancel != _timerQueue.cend(); ++itCancel) {
            if (itCancel->first == timerId) {
                // add remeaning downcounter value to next timer
                auto itNext = std::next(itCancel);
                if (itNext != _timerQueue.cend())
                    itNext->second += itCancel->second;

                // clear timer data
                _timerQueue.erase_after(itPrev);
                _timerStorageMap.erase(timerId);
                _releaseTimerId(timerId);

                return;
            } else
                itPrev = itCancel;
        }

        ASSERT_FOR_FATAL(false);
    } else {
        SLOG(LogLevel::LOG_WARNING, "Timer Id: {} not found or already expired for cancel operation", timerId);
    }
}

void FW::subscribe(SignalId signalId, TaskId subsId) {
    // todo: assert -- user-signal-start < signalId < signal-list-count
    std::lock_guard<std::mutex> lock(_subsMutex);

    auto signalSubsListIter = _subsMap.find(signalId);

    // check if signal has active subscribers
    if (signalSubsListIter == _subsMap.cend()) {
        // add signal to the map along with its first subscriber
        std::vector<TaskId> subsList;
        subsList.push_back(subsId);
        _subsMap.emplace(std::move(signalId), std::move(subsList));
    } else {
        std::vector<TaskId>& subsList = signalSubsListIter->second;

        // assert if subs is already on the list
        ASSERT_FOR_FATAL(std::find(subsList.begin(), subsList.end(), subsId) == subsList.end());

        // add task to the list as a new subs
        subsList.push_back(subsId);
    }

    SLOG(LogLevel::LOG_INFO, "Task Id: {} subscribed to signalId: {}", subsId, signalId);
}

void FW::subscribe(std::initializer_list<SignalId> signalList, TaskId subsId) {
    // iterate over initializer list and call subscribe for each signal
    for (SignalId signalId : signalList) {
        subscribe(signalId, subsId);
    }
}

void FW::unsubscribe(SignalId signalId, TaskId subsId) {
    std::lock_guard<std::mutex> lock(_subsMutex);

    // get signal's subs list, assert if signal id does not have subs list
    auto signalSubsListIter = _subsMap.find(signalId);
    ASSERT_FOR_FATAL(signalSubsListIter != _subsMap.cend());

    // get iterator for subsId, assert if subsId is not present
    std::vector<TaskId>& subsList = signalSubsListIter->second;
    auto subsIdIter = std::find(subsList.cbegin(), subsList.cend(), subsId);
    ASSERT_FOR_FATAL(subsIdIter != subsList.cend());

    // remove subsId from list
    subsList.erase(subsIdIter);

    SLOG(LogLevel::LOG_INFO, "Task Id: {} unsubscribed to signalId: {}", subsId, signalId);

    // todo: sinyalde subs kalmadıysa map'den sil
}

void FW::unsubscribe(std::initializer_list<SignalId> signalList, TaskId subsId) {
    // iterate over initializer list and call unsubscribe for each signal
    for (SignalId signalId : signalList) {
        unsubscribe(signalId, subsId);
    }
}

// void FW::stop(TaskId taskId) {
//     // add to stop list
//     std::lock_guard<std::mutex> lock(_listMutex);
//     _stopList.push_back(taskId);
//     SLOG(LogLevel::LOG_INFO, "Self stop is schduled for task id: {}", taskId);
// }

void FW::deleteAsyncTask(TaskId taskId) {
    // stop task first
    stopTask(taskId, false);

    // add to delete list
    std::lock_guard<std::mutex> lock(_listMutex);
    _deleteList.push_back(taskId);
    SLOG(LogLevel::LOG_INFO, "Self delete is schduled for task id: {}", taskId);
}

#ifdef uOS_ENABLE_CLI
void FW::registerCliMenu(TaskId taskId, std::unique_ptr<cli::Menu>&& menuPtr) {
    // check if cli is initialized
    if (_cli) {
        // get task name
        std::shared_lock lock(_taskMutex);
        std::string taskName = _taskMap[taskId]->getName();

        // post to cli scheduler for thread safety
        auto copyableMenuPtrPtr =
            std::make_shared<std::unique_ptr<cli::Menu>>(std::move(menuPtr)); // this is needed to keep lambda copy constructable
        _cliScheduler->Post([taskName = std::move(taskName), copyableMenuPtrPtr]() {
            // check if this task had already inserted menu
            if (_menuHandlerMap.find(taskName) == _menuHandlerMap.end()) {
                // insert menu and save menu handler
                _menuHandlerMap[taskName] = _cliRootMenu->Insert(std::move(*copyableMenuPtrPtr));
                SLOG(LogLevel::LOG_INFO, "CLI menu for task: {} is registered", taskName);
            } else {
                SLOG(LogLevel::LOG_INFO, "CLI menu for task: {} is already registered", taskName);
            }
        });
    } else {
        SLOG(LogLevel::LOG_WARNING, "CLI scheduler is not initialized. Cannot register CLI menu for task id: {}", taskId);
    }
}
#endif

//////////////////////////////////////////////////////
//                                                  //
//////////////////////////////////////////////////////

void FW::_publishEvent(std::shared_ptr<const Event>&& event) {
    std::lock_guard<std::mutex> lock(_subsMutex);

    // get signal's subs list, return if signal id does not have subs list
    auto subListIter = _subsMap.find(event->signal);

    if (subListIter != _subsMap.cend()) {
        _publishEventCommon(std::move(event), subListIter->second);
    }
}

void FW::_publishEventCommon(std::shared_ptr<const Event>&& event, std::vector<TaskId>& subsList) {
    std::shared_lock lock(_taskMutex);

    // for each subs, send a copy of the event (send original event to last subs)
    for (TaskId taskId : subsList) {
        auto it = _taskMap.find(taskId);
        if (it != _taskMap.cend()) {
            if (taskId != subsList.back()) {
                std::shared_ptr<const Event> eventCopy = event;
                it->second->putEvent(std::move(eventCopy));
            } else {
                it->second->putEvent(std::move(event));
            }
        } else {
            // if task is not found, log a warning
            SLOG(LogLevel::LOG_WARNING, "Task with id: {} not found on publishing signalId: {}", taskId, event->signal);
        }
    }
}

TaskId FW::_getAvailableTaskId(void) {
    // Id 0 is NO_TASK_ID and is never issued.
    for (uint_fast16_t i = 1; i < uOS_FW_MAX_TASK_COUNT; i++) {
        if (_taskIdUsageList[i] == false) {
            _taskIdUsageList[i] = true;
            return static_cast<TaskId>(i);
        }
    }

    ASSERT_FOR_FATAL(false);
}

TimerId FW::_getAvailableTimerId(void) {
    ASSERT_FOR_FATAL(_freeTimerCount > 0);

    TimerId id = _freeTimerIds[_freeTimerHead];
    _freeTimerHead = static_cast<uint_fast16_t>((_freeTimerHead + 1) % uOS_FW_MAX_TIMER_COUNT);
    --_freeTimerCount;
    return id;
}

void FW::_releaseTimerId(TimerId timerId) {
    ASSERT_FOR_FATAL(timerId != IDLE_TIMER_ID);
    ASSERT_FOR_FATAL(timerId < uOS_FW_MAX_TIMER_COUNT);
    ASSERT_FOR_FATAL(_freeTimerCount < uOS_FW_MAX_TIMER_COUNT - 1);

    uint_fast16_t tail = static_cast<uint_fast16_t>((_freeTimerHead + _freeTimerCount) % uOS_FW_MAX_TIMER_COUNT);
    _freeTimerIds[tail] = timerId;
    ++_freeTimerCount;
}

void FW::_insertTimerQueue(TimerId timerId, uint_fast16_t interval) {
    uint16_t accuPrev{0};
    uint16_t accuNext;

    // lock the queue --> bunu çağıran yerlerde kilitlendi
    // std::lock_guard<std::mutex> lock(_timerMutex);

    auto itPrev = _timerQueue.cbefore_begin();
    auto itNext = _timerQueue.begin();

    for (; itNext != _timerQueue.cend(); ++itNext) {
        accuNext = accuPrev + itNext->second;
        if (interval < accuNext)
            break;
        else {
            accuPrev = accuNext;
            itPrev = itNext;
        }
    }

    // yeni timer'ı interval değerini aşan son iteratörden bir önceye koy
    // sonraki timer'ın delta değerini yeni giren timer'ın delta değeri kadar azalt
    _timerQueue.insert_after(itPrev, std::make_pair(timerId, interval - accuPrev));
    if (itNext != _timerQueue.cend())
        itNext->second = accuNext - interval;
}

TimerId FW::_setupTimedEvent(bool isPublish, bool isPeriodic, uint_fast16_t interval, std::shared_ptr<const Event>&& eventPtr,
                             TaskId receiverTaskId) {
    TimerNode timerNode{isPublish, isPeriodic, receiverTaskId, interval, std::move(eventPtr)};

    // lock timer related resources
    std::lock_guard<std::mutex> lock(_timerMutex);

    // acquire timer id
    TimerId timerId = _getAvailableTimerId();

    // insert timer to delta queue
    _insertTimerQueue(timerId, interval);

    // store timer data
    _timerStorageMap.emplace(std::move(timerId), std::move(timerNode));
    // todo: assert if emplace fails

    // std::cout << "timer created with timer id: " << timerId << std::endl;
    SLOG(LogLevel::LOG_TRACE, "Timer created with timerId: {}", timerId);
    return timerId;
}

void FW::_handleTick(void) // todo: inline yap
{
    std::unique_lock<std::mutex> lock(_timerMutex); // scooped lock ile hem taskMap mutex hem de timerMutex kilitlenebilir

    if (_timerQueue.empty() == false) {
        std::vector<TimerId> expiredTimers;
        TimerNode timerNode;

        // get head timer and decrement downcounter
        auto head = _timerQueue.begin();
        head->second--;

        // check if head expired
        while (head->second == 0) {
            // save expired timer id and remove from queue
            expiredTimers.push_back(head->first);
            _timerQueue.pop_front();

            // get new head or
            if (_timerQueue.empty() == false)
                head = _timerQueue.begin();
            else
                break;
        }

        // unlock the mutex until there is any timer map/queue access
        lock.unlock();

        // handle expired timers
        for (TimerId timerId : expiredTimers) {
            // std::cout << "expired timer with timer id: " << unsigned(timerId) << std::endl;
            SLOG(LogLevel::LOG_TRACE, "Timer expired with timerId: {}", timerId);

            // get stored timer node with the timer key
            // todo: find thread safe değil burası kontrol edilecek
            TimerNode& timerNode = _timerStorageMap.find(timerId)->second; // find is thread-safe

            // event'i post et
            // todo: event'i move edersem periyodik olarak tekrar çağırıldığında event kaybolmuyor mu?
            bool receiverGone = false;
            if (timerNode.isPublish) {
                if (timerNode.isPeriodic == false)
                    _publishEvent(std::move(timerNode.eventPtr));
                else {
                    std::shared_ptr<const Event> eventCopy = timerNode.eventPtr;
                    _publishEvent(std::move(eventCopy));
                }
            } else {
                std::shared_lock taskMapLock(_taskMutex);
                auto it = _taskMap.find(timerNode.receiverTaskId);
                if (it != _taskMap.cend()) {
                    if (timerNode.isPeriodic == false) {
                        it->second->putEvent(std::move(timerNode.eventPtr));
                    } else {
                        std::shared_ptr<const Event> eventCopy = timerNode.eventPtr;
                        it->second->putEvent(std::move(eventCopy));
                    }
                } else {
                    receiverGone = true;
                    SLOG(LogLevel::LOG_WARNING, "Timer expired for deleted task with id: {} ", timerNode.receiverTaskId);
                }
            }

            // A periodic timer for a live receiver is armed again. Anything else,
            // including a periodic timer whose task is gone, releases the timer id.
            lock.lock();
            if (timerNode.isPeriodic && !receiverGone) {
                _insertTimerQueue(timerId,
                                  (timerNode.interval - 1)); // -tam nedenini çözemedim ama 20ms fazla çıkıyor periyodiklerde, o yüzden -1 yaptım
            } else {
                _timerStorageMap.erase(timerId);
                _releaseTimerId(timerId);
            }
            lock.unlock();
        }
    }
}

void FW::_handleDelete(void) // todo: inline yap
{
    std::lock_guard<std::mutex> lock(_listMutex);

    // if(_stopList.empty() == false){
    //     for(TaskId taskId : _stopList){
    //         stopTask(taskId);
    //     }
    //     _stopList.clear();
    // }

    if (_deleteList.empty() == false) {
        for (TaskId taskId : _deleteList) {
            deleteTask(taskId);
        }
        _deleteList.clear();
    }
}

void FW::_sigintHandler(int signum) {
    // No SLOG here. systemLog and Quill are not safe to call from a signal handler.
    // The run thread logs g_stopSignal after the loop sees _isRunning == false.
    g_stopSignal = signum;
    _isRunning = false;
}

#ifdef uOS_ENABLE_CLI
void FW::_cliPostSignal(std::ostream& out, int signalId, int targetTaskId) {
    if (static_cast<SignalId>(signalId) < INT_SIGNAL_START) {
        out << "invalid signal id" << std::endl;
        return;
    }

    std::shared_lock lock(_taskMutex);
    auto taskIter = _taskMap.find(static_cast<TaskId>(targetTaskId));
    if (taskIter != _taskMap.cend()) {
        auto evt = new_e(static_cast<SignalId>(signalId));
        postEvent(static_cast<TaskId>(targetTaskId), evt);
        out << "signal id: " << (unsigned)signalId << " -> task id: " << (unsigned)targetTaskId << std::endl;
    } else {
        out << "invalid task id" << std::endl;
    }
}

void FW::_cliPostSignalWithSource(std::ostream& out, int signalId, int targetTaskId, int sourceTaskId) {
    if (static_cast<SignalId>(signalId) < INT_SIGNAL_START) {
        out << "invalid signal id" << std::endl;
        return;
    }

    std::shared_lock lock(_taskMutex);
    auto taskIter = _taskMap.find(static_cast<TaskId>(targetTaskId));
    if (taskIter != _taskMap.cend()) {
        auto evt = new_e(static_cast<SignalId>(signalId));
        evt->source = static_cast<TaskId>(sourceTaskId);
        postEvent(static_cast<TaskId>(targetTaskId), evt);
        out << "signal id: " << (unsigned)signalId << " -> task id: " << (unsigned)targetTaskId << " w/ source id: " << (unsigned)sourceTaskId
            << std::endl;
    } else {
        out << "invalid task id" << std::endl;
    }
}

void FW::_cliPublishSignal(std::ostream& out, int signalId) {
    auto subListIter = _subsMap.find(static_cast<SignalId>(signalId));
    if (subListIter != _subsMap.cend()) {
        auto evt = new_e(static_cast<SignalId>(signalId));
        publishEvent(evt);
        out << "event published w/ signal id: " << (unsigned)signalId << '\n';
    } else {
        out << "this signal has no subscribers" << std::endl;
    }
}

void FW::_cliPublishSignalWithSource(std::ostream& out, int signalId, int sourceTaskId) {
    auto subListIter = _subsMap.find(static_cast<SignalId>(signalId));
    if (subListIter != _subsMap.cend()) {
        auto evt = new_e(static_cast<SignalId>(signalId));
        evt->source = static_cast<TaskId>(sourceTaskId);
        publishEvent(evt);
        out << "event published w/ signal id: " << (unsigned)signalId << " and source id: " << (unsigned)sourceTaskId << '\n';
    } else {
        out << "this signal has no subscribers" << std::endl;
    }
}

void FW::_cliPostEvent(std::ostream& out, int signalId, const std::string& jsonPath, int targetTaskId) {
    _cliPostEventCommon(out, static_cast<SignalId>(signalId), jsonPath, static_cast<TaskId>(targetTaskId));
}

void FW::_cliPostEventWithSource(std::ostream& out, int signalId, const std::string& jsonPath, int targetTaskId, int sourceTaskId) {
    _cliPostEventCommon(out, static_cast<SignalId>(signalId), jsonPath, static_cast<TaskId>(targetTaskId), static_cast<TaskId>(sourceTaskId));
}

void FW::_cliPublishEvent(std::ostream& out, int signalId, const std::string& jsonPath) {
    _cliPubEventCommon(out, static_cast<SignalId>(signalId), jsonPath);
}

void FW::_cliPublishEventWithSource(std::ostream& out, int signalId, const std::string& jsonPath, int sourceTaskId) {
    _cliPubEventCommon(out, static_cast<SignalId>(signalId), jsonPath, static_cast<TaskId>(sourceTaskId));
}

void FW::_cliListAllTasks(std::ostream& out) {
    std::shared_lock lock(_taskMutex);
    for (const auto& pair : _taskMap) {
        out << "id: " << (unsigned)pair.first << " name: " << pair.second->getName() << "\n";
    }
}

void FW::_cliListTasksWithSearch(std::ostream& out, const std::string& searchStr) {
    std::shared_lock lock(_taskMutex);
    for (const auto& pair : _taskMap) {
        if (pair.second->getName().find(searchStr) != std::string::npos) {
            out << "id: " << (unsigned)pair.first << " name: " << pair.second->getName() << "\n";
        }
    }
}

void FW::_cliPostEventCommon(std::ostream& out, SignalId signalId, const std::string& jsonPath, TaskId targetTaskId,
                             std::optional<TaskId> sourceTaskId) {
    if (signalId < INT_SIGNAL_START) {
        out << "invalid signal id" << std::endl;
        return;
    }

    std::shared_lock lock(_taskMutex);
    auto taskIter = _taskMap.find(targetTaskId);
    if (taskIter != _taskMap.cend()) {
        // Read the JSON file
        std::ifstream file(jsonPath);
        if (file.is_open()) {
            nlohmann::json jsonData;
            try {
                // json parse
                file >> jsonData;
                // send to task
                taskIter->second->cliEventSink(out, signalId, jsonData, sourceTaskId);
            } catch (const nlohmann::json::parse_error& e) {
                out << "Json parse error: " << e.what() << '\n';
            }
            file.close();
        } else {
            out << "Could not open file\n";
        }
    } else {
        out << "invalid task id" << std::endl;
    }
}

void FW::_cliPubEventCommon(std::ostream& out, SignalId signalId, const std::string& jsonPath, std::optional<TaskId> sourceTaskId) {
    auto subListIter = _subsMap.find(signalId);
    if (subListIter != _subsMap.cend()) {
        // Read the JSON file
        std::ifstream file(jsonPath);
        if (file.is_open()) {
            nlohmann::json jsonData;
            try {
                // json parse
                file >> jsonData;
                // send to all subs
                std::shared_lock lock(_taskMutex);
                for (TaskId taskId : subListIter->second) {
                    _taskMap.find(taskId)->second->cliEventSink(out, signalId, jsonData, sourceTaskId);
                }
            } catch (const nlohmann::json::parse_error& e) {
                out << "Json parse error: " << e.what() << '\n';
            }
            file.close();
        } else {
            out << "Could not open file\n";
        }
    } else {
        out << "this signal has no subscribers" << std::endl;
    }
}
#endif

} // namespace uOS
