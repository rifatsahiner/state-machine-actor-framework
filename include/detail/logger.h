
#ifndef LOGGER_H
#define LOGGER_H

#include <string>
#include <thread>
#include <functional>
#include <filesystem>
#include <fstream>
#include <shared_mutex>

#include <fmt/format.h>

#include "uOS_defs.h"

#ifdef uOS_ENABLE_TUI
class FLogView;
class FLogViewMulti;
namespace finalcut { class FApplication; }
#endif


namespace uOS {


class Logger {
    public:
        static void init(int, char*[], std::optional<std::function<void(void)>> = std::nullopt);
        static void start(void);
        static void stop(void);
        static void addTaskLogger(const TaskId, const std::string&, const std::string&);
        static void removeTaskLogger(const TaskId);
        static void logFw(const char*, const char*, const int, const LogLevel, const std::string&);
        static void logTask(const char*, const char*, int, LogLevel, const std::string&, TaskId, std::string_view);
        static bool isActive(void);
    
    private:
        struct TaskLogStream {
            std::ofstream fileStream;
            std::mutex fileMutex;
            const std::filesystem::path folderPath;
            const std::string fileName;
            uint_fast32_t fileLineCount = 0;
            uint_fast8_t overflowCount = 0;

            TaskLogStream(std::ofstream&& stream, std::filesystem::path&& path, std::string&& file) : fileStream(std::move(stream)), folderPath(std::move(path)), fileName(std::move(file)) { }
        };

#ifdef uOS_ENABLE_TUI
        static inline finalcut::FApplication* _tui {nullptr};
        static inline FLogView* _loggerFw;
        static inline FLogViewMulti* _loggerTasks;
        static inline std::thread _loggerThread;
#endif
        static inline std::filesystem::path _logFolderPath;
        static inline std::unordered_map<TaskId, TaskLogStream> _taskFileMap;
        static inline std::shared_mutex _taskFileMapMutex;
        static inline std::unique_ptr<TaskLogStream> _systemFile;

        static inline void _logCommon(const char*, const char*, const int, const LogLevel, const std::string&, std::optional<TaskId> = std::nullopt, std::string_view = "");
        static inline void _fileSizer(TaskLogStream& logStream);
        static void _systemSignalHandler(int signum);
};

template <typename... LogStrArgs>
void log__(const char* file, const char* function, int line, LogLevel level, fmt::format_string<LogStrArgs...> logStr, LogStrArgs&&... logStrArgs) {
    // if TUI not active and this is TRACE, skip
    if((Logger::isActive() == false) && level == LogLevel::LOG_TRACE){
        return;
    }
    
    // format logStr
    auto formattedStr = fmt::format(logStr, std::forward<LogStrArgs>(logStrArgs)...);

    // sent to logger
    Logger::logFw(file, function, line, level, formattedStr);
}


}   // namesapce uOS

#endif
