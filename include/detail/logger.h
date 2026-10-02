#ifndef LOGGER_H
#define LOGGER_H

#include "log_stream.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace uOS {

struct LogLimits {
    std::uintmax_t maxFileBytes = 64ull * 1024ull * 1024ull;
    std::uintmax_t maxFolderBytes = 1024ull * 1024ull * 1024ull;
    std::size_t maxFolderFiles = 4096;
};

inline constexpr uint16_t kSystemViewId = 0xFFFE;

class Logger {
public:
    static void init(int argc, char* argv[], const std::string& appName = "uOS", std::optional<std::function<void(void)>> quitCb = std::nullopt);
    static void start(void);
    static void stop(void);

    static LogStream openTask(std::string_view actorName, std::string_view instName, TaskId id, LogLimits limits = {});
    static void detachTask(TaskId id);

    static void setSystemLogLevels(LogLevel fileLevel, LogLevel uiLevel);
    static void setTaskLogLevels(TaskId id, LogLevel fileLevel, LogLevel uiLevel);
    static LogLevel systemFileLogLevel();
    static LogLevel systemUiLogLevel();
    static LogLevel taskFileLogLevel(TaskId id);
    static LogLevel taskUiLogLevel(TaskId id);

    static std::filesystem::path logRoot();
    // Lines handed to the TUI or the console since init. Empty unless UOS_LOG_CAPTURE is set.
    static std::vector<std::string> capturedUi();
};

} // namespace uOS

#endif
