#ifndef uOS_LOG_STREAM_H
#define uOS_LOG_STREAM_H

#include "uOS_defs.h"

#include <memory>
#include <string_view>

// Quill formats on its backend thread. These codecs cover the standard types
// that show up in log arguments. A type without a codec needs one before it
// can be passed to LOG / SLOG / LOG_TO.
#include "quill/LogMacros.h"
#include "quill/Logger.h"
#include "quill/std/Array.h"
#include "quill/std/Chrono.h"
#include "quill/std/FilesystemPath.h"
#include "quill/std/Optional.h"
#include "quill/std/Vector.h"

#include <utility>

namespace uOS {

// Copyable handle to one log file and its UI view. Copies share the file.
// The file closes when the last copy is destroyed, while the backend is running.
class LogStream {
public:
    struct Impl;

    LogStream() noexcept;
    ~LogStream();
    LogStream(const LogStream&) noexcept;
    LogStream& operator=(const LogStream&) noexcept;
    LogStream(LogStream&&) noexcept;
    LogStream& operator=(LogStream&&) noexcept;

    explicit operator bool() const noexcept;

    // fileLevel is the file threshold, uiLevel is the TUI or the console.
    // A statement below both is dropped before its arguments are copied.
    void setLevels(LogLevel fileLevel, LogLevel uiLevel);
    LogLevel fileLevel() const noexcept;
    LogLevel uiLevel() const noexcept;

    // Block until the backend has written this stream's queued lines.
    void flush() const;

    std::string_view typeName() const noexcept;
    quill::Logger* native() const noexcept;

    // Impl is private. Callers outside this library cannot build a non-empty stream.
    explicit LogStream(std::shared_ptr<Impl> impl) noexcept;

private:
    std::shared_ptr<Impl> _impl;

    friend class Logger;
};

LogStream systemLog();
void prepareLogThread();

} // namespace uOS

// level must be a constant uOS::LogLevel. LOG uses the enclosing object's
// logStream() (Task, ChildStateMachine, or a member that stores a LogStream).
// Free functions use SLOG. LOG_TO takes an explicit stream.
#define UOS_LOG_TO(stream, level, fmt, ...)                                                                \
    do {                                                                                                   \
        auto _uos_stream = (stream);                                                                       \
        static_assert((level) == ::uOS::LogLevel::LOG_TRACE_L3 || (level) == ::uOS::LogLevel::LOG_TRACE_L2 || \
                          (level) == ::uOS::LogLevel::LOG_TRACE_L1 || (level) == ::uOS::LogLevel::LOG_DEBUG ||  \
                          (level) == ::uOS::LogLevel::LOG_INFO || (level) == ::uOS::LogLevel::LOG_NOTICE ||    \
                          (level) == ::uOS::LogLevel::LOG_WARNING || (level) == ::uOS::LogLevel::LOG_ERROR || \
                          (level) == ::uOS::LogLevel::LOG_CRITICAL,                                          \
                      "LOG level must be a constant uOS::LogLevel other than LOG_NONE");                    \
        if (auto* _uos_logger = _uos_stream.native()) {                                                    \
            if constexpr ((level) == ::uOS::LogLevel::LOG_TRACE_L3) {                                      \
                QUILL_LOG_TRACE_L3(_uos_logger, fmt, ##__VA_ARGS__);                                       \
            } else if constexpr ((level) == ::uOS::LogLevel::LOG_TRACE_L2) {                               \
                QUILL_LOG_TRACE_L2(_uos_logger, fmt, ##__VA_ARGS__);                                       \
            } else if constexpr ((level) == ::uOS::LogLevel::LOG_TRACE_L1) {                               \
                QUILL_LOG_TRACE_L1(_uos_logger, fmt, ##__VA_ARGS__);                                       \
            } else if constexpr ((level) == ::uOS::LogLevel::LOG_DEBUG) {                                  \
                QUILL_LOG_DEBUG(_uos_logger, fmt, ##__VA_ARGS__);                                          \
            } else if constexpr ((level) == ::uOS::LogLevel::LOG_INFO) {                                   \
                QUILL_LOG_INFO(_uos_logger, fmt, ##__VA_ARGS__);                                           \
            } else if constexpr ((level) == ::uOS::LogLevel::LOG_NOTICE) {                                 \
                QUILL_LOG_NOTICE(_uos_logger, fmt, ##__VA_ARGS__);                                         \
            } else if constexpr ((level) == ::uOS::LogLevel::LOG_WARNING) {                                \
                QUILL_LOG_WARNING(_uos_logger, fmt, ##__VA_ARGS__);                                        \
            } else if constexpr ((level) == ::uOS::LogLevel::LOG_ERROR) {                                  \
                QUILL_LOG_ERROR(_uos_logger, fmt, ##__VA_ARGS__);                                          \
            } else if constexpr ((level) == ::uOS::LogLevel::LOG_CRITICAL) {                               \
                QUILL_LOG_CRITICAL(_uos_logger, fmt, ##__VA_ARGS__);                                       \
            }                                                                                              \
        }                                                                                                  \
    } while (0)

#define LOG(level, fmt, ...) UOS_LOG_TO(this->logStream(), level, fmt, ##__VA_ARGS__)
#define SLOG(level, fmt, ...) UOS_LOG_TO(::uOS::systemLog(), level, fmt, ##__VA_ARGS__)
#define LOG_TO(stream, level, fmt, ...) UOS_LOG_TO(stream, level, fmt, ##__VA_ARGS__)

#endif // uOS_LOG_STREAM_H
