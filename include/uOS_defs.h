#ifndef uOS_DEFS_H
#define uOS_DEFS_H

#include <cstdint>
#include <optional>
#include <memory>
#include <cassert>

#define uOS_FW_MAX_TASK_COUNT 128
#define uOS_FW_MAX_TIMER_COUNT 256


namespace uOS {


// timerId
#if uOS_FW_MAX_TIMER_COUNT > 65536
using TimerId = uint_fast32_t;
#else
using TimerId = uint_fast16_t;
#endif

// Not a live timer. Allocation never returns this value.
constexpr TimerId IDLE_TIMER_ID = 0;

// taskId
#if uOS_FW_MAX_TASK_COUNT > 256
using TaskId = uint_fast16_t;
#define uOS_FW_SYNC_TASK_THREAD_POOL_SIZE 8
#else
using TaskId = uint_fast8_t;
#endif

// Not a live task. Allocation never returns this value. Distinct from isTaskIdle.
constexpr TaskId NO_TASK_ID = 0;

// signal
using SignalId = uint_fast16_t;

// published tick signals
constexpr SignalId SIG_TICK_SEC = static_cast<SignalId>(4);
constexpr SignalId SIG_TICK_MIN = static_cast<SignalId>(5);
constexpr SignalId SIG_TICK_HOUR = static_cast<SignalId>(6);

// Offsets for user signals
constexpr SignalId INT_SIGNAL_START = static_cast<SignalId>(7);
constexpr SignalId CHILD_SIGNAL_START = static_cast<SignalId>(64);
constexpr SignalId EXT_SIGNAL_START = static_cast<SignalId>(96);

// event
struct Event {
    SignalId signal;
    std::optional<TaskId> source;

    constexpr Event(SignalId s = 0, std::optional<TaskId> src = std::nullopt)
        : signal(s), source(src) {}

    virtual ~Event() = default;
};

// event utility
template <class T = Event>
std::shared_ptr<T> new_e(std::optional<SignalId> signalId = std::nullopt)
{
    static_assert(std::is_base_of_v<Event, T>);
    
    auto ptr = std::make_shared<T>();
    if (signalId) {
        ptr->signal = signalId.value();
    }
    return ptr;
}

template <class T>
const T& recast_e(const Event& eventRef) {
    static_assert(std::is_base_of_v<Event, T>);
#ifndef NDEBUG
    // exact runtime type check in debug mode
    if (typeid(eventRef) != typeid(T)) {
        throw std::runtime_error("Type mismatch in recast_e: Expected exactly " + 
                                 std::string(typeid(T).name()) + 
                                 ", got " + std::string(typeid(eventRef).name()));
    }
#endif
    return static_cast<const T&>(eventRef);
}

// Same values as quill::LogLevel, minus Backtrace. LOG_TRACE is TRACE_L1.
// LOG_NONE is a threshold that drops every statement.
enum class LogLevel : uint8_t {
    LOG_TRACE_L3 = 0,
    LOG_TRACE_L2,
    LOG_TRACE_L1,
    LOG_TRACE = LOG_TRACE_L1,
    LOG_DEBUG,
    LOG_INFO,
    LOG_NOTICE,
    LOG_WARNING,
    LOG_ERROR,
    LOG_CRITICAL,
    LOG_NONE = 10
};

// define the unreachable intrinsic based on compiler
#if defined(_MSC_VER)
    #define FW_UNREACHABLE() __assume(0)
#elif defined(__GNUC__) || defined(__clang__)
    #define FW_UNREACHABLE() __builtin_unreachable()
#else
    #define FW_UNREACHABLE()
#endif

// custom assert for fatal errors that uses regulear assert in debug mode and aborts in release mode
#ifdef NDEBUG
    // Release Build: Abort and hint to the compiler that this is unreachable
    #define ASSERT_FOR_FATAL(expr) \
        do {                             \
            if (!(expr)) {               \
                std::abort();            \
                FW_UNREACHABLE();        \
            }                            \
        } while (0)
#else
    // Debug Build: Use standard assert for breakpoints
    #define ASSERT_FOR_FATAL(expr) assert(expr)
#endif


}   // namespace uOS


#endif  // uOS_DEFS_H