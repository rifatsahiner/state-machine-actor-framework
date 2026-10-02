// Quill owns rotating files and, when the TUI is off, the console.
// Each line is formatted once: neither the console sink nor the TUI sink
// sets its own pattern, so both receive the same log_statement.
// Still ours: the TUI widget and its UTF-8 conversion, the TUI-only stdout/stderr
// capture (finalcut keeps the terminal; printf and fprintf are stored in the
// system file at the current system file level), the crash handler plus
// backward-cpp, and the folder deletion sweep.

#include "detail/logger.h"

#ifdef uOS_ENABLE_TUI
#include "detail/flogview.h"

#include <final/output/tty/ftermcap.h>
#endif

#include <backward.hpp>

#include "quill/Backend.h"
#include "quill/Frontend.h"
#include "quill/LogMacros.h"
#include "quill/Logger.h"
#include "quill/bundled/fmt/format.h"
#include "quill/filters/Filter.h"
#include "quill/sinks/ConsoleSink.h"
#include "quill/sinks/RotatingFileSink.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <filesystem>
#include <limits>
#include <mutex>
#include <poll.h>
#include <pthread.h>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

namespace {

std::atomic<bool> g_backendUp{false};
std::atomic<int> g_crashSig{0};
std::atomic_flag g_inCrash = ATOMIC_FLAG_INIT;
std::atomic<uint8_t> g_systemFileLevel{static_cast<uint8_t>(uOS::LogLevel::LOG_INFO)};

int g_savedOut = -1;
int g_savedErr = -1;
int g_outPipe[2] = {-1, -1};
int g_errPipe[2] = {-1, -1};
std::thread g_tee;
bool g_teeOn = false;

std::mutex g_liveMu;
std::unordered_set<std::string> g_live;

std::mutex g_captureMu;
std::vector<std::string> g_capture;
bool g_captureOn = false;

std::mutex g_taskMu;
std::unordered_map<uOS::TaskId, uOS::LogStream> g_tasks;
uOS::LogStream g_systemStream;
std::filesystem::path g_systemPath;
std::shared_ptr<quill::Sink> g_systemFileSink;
quill::Logger* g_stdio = nullptr;

#ifdef uOS_ENABLE_TUI
finalcut::FApplication* g_tui = nullptr;
FLogViewMulti* g_tuiView = nullptr;
std::thread g_tuiThread;
#endif
std::atomic<bool> g_tuiOn{false};

std::filesystem::path g_logRoot;

quill::LogLevel toQuill(uOS::LogLevel level) { return static_cast<quill::LogLevel>(level); }

uOS::LogLevel quieter(uOS::LogLevel a, uOS::LogLevel b) { return static_cast<uint8_t>(a) < static_cast<uint8_t>(b) ? a : b; }

quill::PatternFormatterOptions logPattern() {
    return quill::PatternFormatterOptions{"%(time) %(log_level:<8) %(logger) %(short_source_location) %(message)", "%Y-%m-%d %H:%M:%S.%Qus",
                                          quill::Timezone::LocalTime};
}

void writeAllFd(int fd, const char* data, std::size_t size) {
    while (size > 0 && fd >= 0) {
        const ssize_t n = ::write(fd, data, size);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return;
        }
        data += n;
        size -= static_cast<std::size_t>(n);
    }
}

void noteLive(const std::filesystem::path& path) {
    std::lock_guard lock(g_liveMu);
    g_live.insert(path.string());
}

void noteDead(const std::filesystem::path& path) {
    std::lock_guard lock(g_liveMu);
    g_live.erase(path.string());
}

bool protects(const std::filesystem::path& base, const std::filesystem::path& candidate) {
    if (base.parent_path() != candidate.parent_path()) {
        return false;
    }
    if (base == candidate) {
        return true;
    }
    const std::string baseName = base.filename().string();
    const std::string candName = candidate.filename().string();
    const auto dot = baseName.rfind('.');
    const std::string stem = dot == std::string::npos ? baseName : baseName.substr(0, dot);
    const std::string ext = dot == std::string::npos ? std::string{} : baseName.substr(dot);
    const std::string prefix = stem + ".";
    if (candName.size() <= prefix.size() + ext.size()) {
        return false;
    }
    if (candName.compare(0, prefix.size(), prefix) != 0) {
        return false;
    }
    if (candName.compare(candName.size() - ext.size(), ext.size(), ext) != 0) {
        return false;
    }
    const auto index = candName.substr(prefix.size(), candName.size() - prefix.size() - ext.size());
    return !index.empty() && std::all_of(index.begin(), index.end(), [](unsigned char c) { return c >= '0' && c <= '9'; });
}

bool isProtected(const std::filesystem::path& candidate) {
    for (const auto& live : g_live) {
        if (protects(live, candidate)) {
            return true;
        }
    }
    return false;
}

void sweepFolder(const std::filesystem::path& folder, const uOS::LogLimits& limits) {
    std::lock_guard lock(g_liveMu);
    std::error_code ec;
    struct Entry {
        std::filesystem::path path;
        std::uintmax_t bytes;
        std::filesystem::file_time_type when;
    };
    std::vector<Entry> files;
    std::uintmax_t total = 0;
    for (const auto& dir : std::filesystem::directory_iterator(folder, ec)) {
        if (ec || !dir.is_regular_file(ec)) {
            continue;
        }
        const auto size = dir.file_size(ec);
        if (ec) {
            continue;
        }
        files.push_back(Entry{dir.path(), size, dir.last_write_time(ec)});
        total += size;
    }

    auto older = [](const Entry& a, const Entry& b) {
        if (a.when != b.when) {
            return a.when < b.when;
        }
        return a.path.string() < b.path.string();
    };

    while ((total > limits.maxFolderBytes || files.size() > limits.maxFolderFiles) && !files.empty()) {
        auto victim = files.end();
        for (auto it = files.begin(); it != files.end(); ++it) {
            if (isProtected(it->path)) {
                continue;
            }
            if (victim == files.end() || older(*it, *victim)) {
                victim = it;
            }
        }
        if (victim == files.end()) {
            break;
        }
        std::error_code rm;
        std::filesystem::remove(victim->path, rm);
        if (!rm && total >= victim->bytes) {
            total -= victim->bytes;
        }
        files.erase(victim);
    }
}

class CaptureFilter : public quill::Filter {
public:
    CaptureFilter() : quill::Filter("ui-capture") {}

    bool filter(quill::MacroMetadata const*, uint64_t, std::string_view, std::string_view, std::string_view, quill::LogLevel, std::string_view,
                std::string_view log_statement) noexcept override {
        if (!g_captureOn) {
            return true;
        }
        std::string_view line = log_statement;
        if (!line.empty() && line.back() == '\n') {
            line.remove_suffix(1);
        }
        std::lock_guard lock(g_captureMu);
        g_capture.emplace_back(line);
        return true;
    }
};

#ifdef uOS_ENABLE_TUI
std::wstring utf8ToWstring(std::string_view utf8) {
    const auto* ptr = reinterpret_cast<const unsigned char*>(utf8.data());
    const auto* end = ptr + utf8.size();
    std::wstring out;
    out.reserve(utf8.size());
    while (ptr < end) {
        uint32_t codepoint = 0;
        std::size_t seqlen = 0;
        const unsigned char lead = *ptr;
        if (lead < 0x80) {
            codepoint = lead;
            seqlen = 1;
        } else if ((lead >> 5) == 0x6 && ptr + 1 < end) {
            codepoint = (lead & 0x1F) << 6;
            codepoint |= (ptr[1] & 0x3F);
            seqlen = 2;
        } else if ((lead >> 4) == 0xE && ptr + 2 < end) {
            codepoint = (lead & 0x0F) << 12;
            codepoint |= (ptr[1] & 0x3F) << 6;
            codepoint |= (ptr[2] & 0x3F);
            seqlen = 3;
        } else if ((lead >> 3) == 0x1E && ptr + 3 < end) {
            codepoint = (lead & 0x07) << 18;
            codepoint |= (ptr[1] & 0x3F) << 12;
            codepoint |= (ptr[2] & 0x3F) << 6;
            codepoint |= (ptr[3] & 0x3F);
            seqlen = 4;
        } else {
            codepoint = '?';
            seqlen = 1;
        }
        out.push_back(static_cast<wchar_t>(codepoint));
        ptr += seqlen;
    }
    return out;
}

class TuiSink final : public quill::Sink {
public:
    explicit TuiSink(uint16_t viewId) : _viewId(viewId) {}

    void write_log(quill::MacroMetadata const*, uint64_t, std::string_view, std::string_view, std::string const&, std::string_view,
                   quill::LogLevel log_level, std::string_view, std::string_view, std::vector<std::pair<std::string, std::string>> const*,
                   std::string_view, std::string_view log_statement) override {
        // TODO: hand the line to the finalcut thread instead of drawing from the backend thread.
        // The widget mutex covers the log list, not finalcut's own redraw.
        if (!g_tuiOn.load(std::memory_order_acquire) || g_tuiView == nullptr) {
            return;
        }
        std::string_view line = log_statement;
        if (!line.empty() && line.back() == '\n') {
            line.remove_suffix(1);
        }
        g_tuiView->log(utf8ToWstring(line), static_cast<uOS::LogLevel>(static_cast<uint8_t>(log_level)), _viewId);
    }

    void flush_sink() override {}

private:
    uint16_t _viewId;
};
#endif

std::filesystem::path currentExecutablePath() {
#if defined(__linux__)
    char buffer[4096];
    const ssize_t len = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (len > 0) {
        buffer[len] = '\0';
        return std::filesystem::path(buffer);
    }
    return {};
#elif defined(__APPLE__)
    uint32_t size = 1024;
    std::vector<char> buf(size);
    if (_NSGetExecutablePath(buf.data(), &size) != 0) {
        if (size == 0) {
            return {};
        }
        buf.resize(size);
        if (_NSGetExecutablePath(buf.data(), &size) != 0) {
            return {};
        }
    }
    if (char* resolved = ::realpath(buf.data(), nullptr)) {
        std::filesystem::path path(resolved);
        std::free(resolved);
        return path;
    }
    return std::filesystem::path(buf.data());
#else
    return {};
#endif
}

bool dirWritable(const std::filesystem::path& p) {
    std::error_code ec;
    return std::filesystem::is_directory(p, ec) && !ec && (::access(p.c_str(), W_OK | X_OK) == 0);
}

bool ensureWritableDir(const std::filesystem::path& p) {
    std::error_code ec;
    std::filesystem::create_directories(p, ec);
    return !ec && dirWritable(p);
}

std::string stampLocal() {
    const auto now = std::chrono::system_clock::now();
    const auto sec = std::chrono::system_clock::to_time_t(now);
    const auto usec = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()) % 1000000;
    std::tm tm{};
    localtime_r(&sec, &tm);
    return fmtquill::format("{:04}-{:02}-{:02}_{:02}{:02}{:02}.{:06}", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
                            static_cast<int>(usec.count()));
}

std::string sanitize(std::string_view name) {
    std::string out;
    out.reserve(name.size());
    for (char c : name) {
        if (c == '/' || c == '\0') {
            out.push_back('_');
        } else {
            out.push_back(c);
        }
    }
    return out;
}

std::filesystem::path uniqueLogPath(const std::filesystem::path& folder, const std::string& stamp, const std::string& body) {
    std::error_code ec;
    auto path = folder / (stamp + body + ".log");
    if (!std::filesystem::exists(path, ec)) {
        return path;
    }
    for (int n = 1; n < 1000; ++n) {
        path = folder / (stamp + body + "_" + std::to_string(n) + ".log");
        if (!std::filesystem::exists(path, ec)) {
            return path;
        }
    }
    return path;
}

std::uintmax_t rotationBytes(const uOS::LogLimits& limits) { return std::max(limits.maxFileBytes, std::uintmax_t{512}); }

// Caps the live index chain (name.log, name.1.log, ...). sweepFolder deletes other closed
// files in the folder. The chain stays protected so Quill can rename it; sweep must not
// delete a member of it.
// TODO: roll each segment onto a new unique file and protect only the open file, so
// sweepFolder is the only deleter. An unlimited Quill backup count is not that design:
// protects() keeps name.N.log live, Quill renames the whole chain, and a live stream
// would grow without a bound.
uint32_t backupCount(const uOS::LogLimits& limits) {
    const auto bytes = rotationBytes(limits);
    std::uintmax_t byBytes = bytes == 0 ? 1 : limits.maxFolderBytes / bytes;
    if (byBytes == 0) {
        byBytes = 1;
    }
    const std::uintmax_t files = limits.maxFolderFiles == 0 ? 1 : limits.maxFolderFiles;
    std::uintmax_t n = std::min(files, byBytes);
    if (n < 1) {
        n = 1;
    }
    if (n > std::numeric_limits<uint32_t>::max()) {
        n = std::numeric_limits<uint32_t>::max();
    }
    return static_cast<uint32_t>(n);
}

quill::RotatingFileSinkConfig rotatingConfig(const uOS::LogLimits& limits) {
    quill::RotatingFileSinkConfig cfg;
    const auto bytes = rotationBytes(limits);
    const auto capped = std::min(bytes, std::uintmax_t{std::numeric_limits<std::size_t>::max()});
    cfg.set_rotation_max_file_size(static_cast<std::size_t>(capped));
    cfg.set_max_backup_files(backupCount(limits));
    cfg.set_rotation_naming_scheme(quill::RotatingFileSinkConfig::RotationNamingScheme::Index);
    // Each path is unique, so there is no earlier index chain to recover or delete.
    // "w" with remove_old_files(false) skips Quill's open-time directory walk.
    cfg.set_open_mode("w");
    cfg.set_remove_old_files(false);
    // Routine fsync stays off. A zero minimum interval would fsync every backend flush
    // and stall callers when the queue fills. The crash path fsyncs the trace itself.
    // Quill still fsyncs before it renames a segment.
    return cfg;
}

void appendToSystemFile(std::string_view text) {
    if (g_systemPath.empty() || text.empty()) {
        return;
    }
    const int fd = ::open(g_systemPath.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC);
    if (fd < 0) {
        return;
    }
    writeAllFd(fd, text.data(), text.size());
    ::fsync(fd);
    ::close(fd);
}

void logStdio(const std::string& line) {
    if (g_stdio == nullptr) {
        return;
    }
    const auto level = static_cast<uOS::LogLevel>(g_systemFileLevel.load(std::memory_order_relaxed));
    if (level == uOS::LogLevel::LOG_NONE) {
        return;
    }
    QUILL_LOG_DYNAMIC(g_stdio, toQuill(level), "{}", line);
}

#ifdef uOS_ENABLE_TUI
int writeSavedOut(const char* data, std::size_t size) {
    if (g_savedOut < 0 || data == nullptr) {
        return -1;
    }
    std::size_t left = size;
    while (left > 0) {
        const ssize_t n = ::write(g_savedOut, data, left);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        data += n;
        left -= static_cast<std::size_t>(n);
    }
    return 0;
}

void directTuiToTerminal() {
    finalcut::FTermcap::setPutCharFunction([](int ch) -> int {
        const char c = static_cast<char>(ch);
        return writeSavedOut(&c, 1) < 0 ? -1 : ch;
    });
    finalcut::FTermcap::setPutStringFunction(
        [](const char* s, std::uint32_t len) -> int { return writeSavedOut(s, len) < 0 ? -1 : static_cast<int>(len); });
}
#endif

void consumePipe(int fd, std::string& pending, const char* tag) {
    char buf[4096];
    while (true) {
        const ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return;
        }
        if (n == 0) {
            return;
        }
        pending.append(buf, buf + n);
        std::size_t start = 0;
        for (std::size_t i = 0; i < pending.size(); ++i) {
            if (pending[i] != '\n') {
                continue;
            }
            logStdio(std::string(tag) + pending.substr(start, i - start));
            start = i + 1;
        }
        pending.erase(0, start);
    }
}

void teeMain() {
    uOS::prepareLogThread();
    std::string outPending;
    std::string errPending;
    bool outOpen = g_outPipe[0] >= 0;
    bool errOpen = g_errPipe[0] >= 0;
    while (outOpen || errOpen) {
        pollfd fds[2]{};
        nfds_t count = 0;
        int outIndex = -1;
        int errIndex = -1;
        if (outOpen) {
            outIndex = static_cast<int>(count);
            fds[count].fd = g_outPipe[0];
            fds[count].events = POLLIN;
            ++count;
        }
        if (errOpen) {
            errIndex = static_cast<int>(count);
            fds[count].fd = g_errPipe[0];
            fds[count].events = POLLIN;
            ++count;
        }
        const int rc = ::poll(fds, count, 200);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        auto take = [&](int index, int fd, std::string& pending, const char* tag, bool& openFlag) {
            if (index < 0) {
                return;
            }
            if (fds[index].revents & (POLLIN | POLLHUP | POLLERR)) {
                consumePipe(fd, pending, tag);
                if (fds[index].revents & (POLLHUP | POLLERR)) {
                    if (!pending.empty()) {
                        logStdio(std::string(tag) + pending);
                        pending.clear();
                    }
                    openFlag = false;
                }
            }
        };
        take(outIndex, g_outPipe[0], outPending, "STDOUT ", outOpen);
        take(errIndex, g_errPipe[0], errPending, "STDERR ", errOpen);
    }
}

void startTee() {
    g_savedOut = ::dup(STDOUT_FILENO);
    g_savedErr = ::dup(STDERR_FILENO);
    ASSERT_FOR_FATAL(g_savedOut >= 0 && g_savedErr >= 0);
#ifdef uOS_ENABLE_TUI
    directTuiToTerminal();
#endif
    ASSERT_FOR_FATAL(::pipe(g_outPipe) == 0);
    ASSERT_FOR_FATAL(::pipe(g_errPipe) == 0);
    ::fcntl(g_outPipe[0], F_SETFL, O_NONBLOCK);
    ::fcntl(g_errPipe[0], F_SETFL, O_NONBLOCK);
    ASSERT_FOR_FATAL(::dup2(g_outPipe[1], STDOUT_FILENO) >= 0);
    ASSERT_FOR_FATAL(::dup2(g_errPipe[1], STDERR_FILENO) >= 0);
    g_teeOn = true;
    g_tee = std::thread(teeMain);
}

void stopTee() {
    if (!g_teeOn) {
        return;
    }
    ::dup2(g_savedOut, STDOUT_FILENO);
    ::dup2(g_savedErr, STDERR_FILENO);
    if (g_outPipe[1] >= 0) {
        ::close(g_outPipe[1]);
        g_outPipe[1] = -1;
    }
    if (g_errPipe[1] >= 0) {
        ::close(g_errPipe[1]);
        g_errPipe[1] = -1;
    }
    if (g_tee.joinable()) {
        g_tee.join();
    }
    if (g_outPipe[0] >= 0) {
        ::close(g_outPipe[0]);
        g_outPipe[0] = -1;
    }
    if (g_errPipe[0] >= 0) {
        ::close(g_errPipe[0]);
        g_errPipe[0] = -1;
    }
    g_teeOn = false;
}

void drainFd(int fd) {
    if (fd < 0) {
        return;
    }
    char buf[4096];
    while (true) {
        const ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return;
        }
        if (n == 0) {
            return;
        }
        appendToSystemFile(std::string_view(buf, static_cast<std::size_t>(n)));
    }
}

void reRaise(int sig) {
    std::signal(sig, SIG_DFL);
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, sig);
    pthread_sigmask(SIG_UNBLOCK, &set, nullptr);
    raise(sig);
}

void onAlarm(int) {
    const int sig = g_crashSig.load(std::memory_order_relaxed);
    reRaise(sig == 0 ? SIGABRT : sig);
}

void crashHandler(int sig) {
    if (g_inCrash.test_and_set(std::memory_order_acq_rel)) {
        reRaise(sig);
        return;
    }
    g_crashSig.store(sig, std::memory_order_relaxed);
    ::alarm(20);

    std::cout.flush();
    std::cerr.flush();
    std::fflush(stdout);
    std::fflush(stderr);
    if (g_teeOn) {
        drainFd(g_outPipe[0]);
        drainFd(g_errPipe[0]);
    }

    const char* name = "signal";
    switch (sig) {
        case SIGSEGV:
            name = "Segmentation fault";
            break;
        case SIGABRT:
            name = "Abort";
            break;
        case SIGILL:
            name = "Illegal instruction";
            break;
        case SIGBUS:
            name = "Bus error";
            break;
        default:
            break;
    }

    // TODO: replace backward-cpp here with an async-signal-safe trace. StackTrace and the
    // ostringstream allocate, which is not safe in a signal handler.
    backward::StackTrace st;
    st.load_here(32);
    backward::Printer printer;
    printer.object = false;
    printer.address = false;
    printer.snippet = true;
    printer.color_mode = backward::ColorMode::never;
    std::ostringstream trace;
    trace << "\n" << name << " (signal " << sig << "). backtrace:\n";
    printer.print(st, trace);
    const std::string text = trace.str();
    appendToSystemFile(text);

    printer.color_mode = backward::ColorMode::automatic;
    if (g_savedErr >= 0) {
        writeAllFd(g_savedErr, text.data(), text.size());
    } else {
        printer.print(st, stderr);
    }

    std::signal(sig, SIG_DFL);
    raise(sig);
}

void installCrashSignals() {
    struct sigaction alarmAction{};
    alarmAction.sa_handler = onAlarm;
    sigemptyset(&alarmAction.sa_mask);
    sigaction(SIGALRM, &alarmAction, nullptr);

    struct sigaction action{};
    action.sa_handler = crashHandler;
    sigemptyset(&action.sa_mask);
    for (int sig : {SIGSEGV, SIGABRT, SIGILL, SIGBUS}) {
        sigaction(sig, &action, nullptr);
    }
}

#ifdef uOS_ENABLE_TUI
void tuiThreadFunc() {
    uOS::prepareLogThread();
    finalcut::FApplication::getApplicationObject()->exec();
}
#endif

} // namespace

namespace uOS {

struct LogStream::Impl {
    quill::Logger* logger = nullptr;
    std::shared_ptr<quill::Sink> fileSink;
    std::shared_ptr<quill::Sink> uiSink;
    std::string typeName;
    std::filesystem::path path;
    std::filesystem::path folder;
    LogLimits limits{};
    std::atomic<uint8_t> fileLevel{static_cast<uint8_t>(LogLevel::LOG_INFO)};
    std::atomic<uint8_t> uiLevel{static_cast<uint8_t>(LogLevel::LOG_INFO)};
    bool system = false;

    ~Impl() {
        if (quill::Logger::is_current_thread_backend_thread()) {
            return;
        }
        quill::Logger* dying = logger;
        logger = nullptr;
        if (dying && g_backendUp.load(std::memory_order_acquire)) {
            try {
                if (dying->is_valid_logger()) {
                    quill::Frontend::remove_logger_blocking(dying, 0);
                }
            } catch (...) {
            }
        }
        fileSink.reset();
        uiSink.reset();
        if (!path.empty()) {
            noteDead(path);
            sweepFolder(folder, limits);
        }
    }
};

} // namespace uOS

namespace {

void attachCapture(const std::shared_ptr<quill::Sink>& sink) {
    if (g_captureOn && sink) {
        sink->add_filter(std::make_unique<CaptureFilter>());
    }
}

} // namespace

namespace uOS {

LogStream Logger::makeStream(const std::filesystem::path& folder, const std::filesystem::path& path, std::string loggerName, std::string typeName,
                             uint16_t viewId, LogLimits limits, bool system, TaskId id) {
    (void)viewId;
    ASSERT_FOR_FATAL(ensureWritableDir(folder));
    quill::FileEventNotifier notifier;
    notifier.after_close = [folder, limits](const std::filesystem::path&) { sweepFolder(folder, limits); };

    auto fileSink = quill::Frontend::create_or_get_sink<quill::RotatingFileSink>(path.string(), rotatingConfig(limits), std::move(notifier));
    fileSink->set_log_level_filter(toQuill(uOS::LogLevel::LOG_INFO));
    noteLive(path);
    sweepFolder(folder, limits);

    std::shared_ptr<quill::Sink> uiSink;
    if (g_tuiOn) {
#ifdef uOS_ENABLE_TUI
        uiSink = quill::Frontend::create_or_get_sink<TuiSink>("tui:" + path.string(), viewId);
#endif
    } else {
        uiSink = quill::Frontend::create_or_get_sink<quill::ConsoleSink>("console:" + path.string());
    }
    if (uiSink) {
        uiSink->set_log_level_filter(toQuill(uOS::LogLevel::LOG_INFO));
        attachCapture(uiSink);
    }

    std::vector<std::shared_ptr<quill::Sink>> sinks;
    sinks.push_back(fileSink);
    if (uiSink) {
        sinks.push_back(uiSink);
    }

    quill::Logger* logger = nullptr;
    try {
        logger = quill::Frontend::create_logger(loggerName, std::move(sinks), logPattern(), quill::ClockSourceType::System);
    } catch (const quill::QuillError&) {
        loggerName += "#" + std::to_string(static_cast<unsigned>(id));
        sinks.clear();
        sinks.push_back(fileSink);
        if (uiSink) {
            sinks.push_back(uiSink);
        }
        logger = quill::Frontend::create_logger(loggerName, std::move(sinks), logPattern(), quill::ClockSourceType::System);
    }
    logger->set_log_level(toQuill(LogLevel::LOG_INFO));
    if (system) {
        g_systemFileSink = fileSink;
        detail::systemLogger.store(logger, std::memory_order_release);
    }

    auto impl = std::make_shared<uOS::LogStream::Impl>();
    impl->logger = logger;
    impl->fileSink = std::move(fileSink);
    impl->uiSink = std::move(uiSink);
    impl->typeName = std::move(typeName);
    impl->path = path;
    impl->folder = folder;
    impl->limits = limits;
    impl->system = system;
    return LogStream{std::move(impl)};
}

LogStream::LogStream() noexcept = default;
LogStream::~LogStream() = default;
LogStream::LogStream(const LogStream&) noexcept = default;
LogStream& LogStream::operator=(const LogStream&) noexcept = default;
LogStream::LogStream(LogStream&&) noexcept = default;
LogStream& LogStream::operator=(LogStream&&) noexcept = default;

LogStream::LogStream(std::shared_ptr<Impl> impl) noexcept : _impl(std::move(impl)) {}

LogStream::operator bool() const noexcept { return native() != nullptr; }

quill::Logger* LogStream::native() const noexcept { return _impl ? _impl->logger : nullptr; }

std::string_view LogStream::typeName() const noexcept {
    if (!_impl) {
        return {};
    }
    return _impl->typeName;
}

void LogStream::setLevels(LogLevel fileLevel, LogLevel uiLevel) {
    if (!_impl || !_impl->logger) {
        return;
    }
    if (_impl->fileSink) {
        _impl->fileSink->set_log_level_filter(toQuill(fileLevel));
    }
    if (_impl->uiSink) {
        _impl->uiSink->set_log_level_filter(toQuill(uiLevel));
    }
    _impl->fileLevel.store(static_cast<uint8_t>(fileLevel), std::memory_order_relaxed);
    _impl->uiLevel.store(static_cast<uint8_t>(uiLevel), std::memory_order_relaxed);
    if (_impl->system) {
        g_systemFileLevel.store(static_cast<uint8_t>(fileLevel), std::memory_order_relaxed);
    }
    if (_impl->logger->is_valid_logger()) {
        _impl->logger->set_log_level(toQuill(quieter(fileLevel, uiLevel)));
    }
}

LogLevel LogStream::fileLevel() const noexcept {
    if (!_impl) {
        return LogLevel::LOG_INFO;
    }
    return static_cast<LogLevel>(_impl->fileLevel.load(std::memory_order_relaxed));
}

LogLevel LogStream::uiLevel() const noexcept {
    if (!_impl) {
        return LogLevel::LOG_INFO;
    }
    return static_cast<LogLevel>(_impl->uiLevel.load(std::memory_order_relaxed));
}

void LogStream::flush() const {
    if (!_impl || !_impl->logger || !g_backendUp.load(std::memory_order_acquire)) {
        return;
    }
    if (quill::Logger::is_current_thread_backend_thread()) {
        return;
    }
    try {
        if (_impl->logger->is_valid_logger()) {
            _impl->logger->flush_log(0);
        }
    } catch (...) {
    }
}

const LogStream& systemLog() { return g_systemStream; }

void prepareLogThread() { quill::Frontend::preallocate(); }

void Logger::init(int argc, char* argv[], const std::string& appName, std::optional<std::function<void(void)>> quitCb) {
    const std::string_view logsRootFolderName = "LOGS";
    const std::string stateAppName = appName.empty() ? std::string("uOS") : appName;
    auto xdgStateLogs = [&]() {
        if (const char* xdg = std::getenv("XDG_STATE_HOME"); xdg && xdg[0] != '\0') {
            return std::filesystem::path(xdg) / stateAppName / logsRootFolderName;
        }
        if (const char* home = std::getenv("HOME"); home && home[0] != '\0') {
            return std::filesystem::path(home) / ".local" / "state" / stateAppName / logsRootFolderName;
        }
        std::error_code ec;
        return std::filesystem::temp_directory_path(ec) / stateAppName / logsRootFolderName;
    };

    bool usedExeDir = false;
    bool rootCreated = false;
    if (const auto exe = currentExecutablePath(); !exe.empty()) {
        g_logRoot = exe.parent_path() / logsRootFolderName;
        if (dirWritable(g_logRoot)) {
            usedExeDir = true;
        } else if (!std::filesystem::exists(g_logRoot) && ensureWritableDir(g_logRoot)) {
            rootCreated = true;
            usedExeDir = true;
        } else if (dirWritable(g_logRoot)) {
            usedExeDir = true;
        }
    }
    if (!usedExeDir) {
        g_logRoot = xdgStateLogs();
        rootCreated = ensureWritableDir(g_logRoot);
        ASSERT_FOR_FATAL(dirWritable(g_logRoot));
    }

#ifdef uOS_ENABLE_TUI
    if (quitCb.has_value()) {
        g_tui = new finalcut::FApplication(argc, argv);
        g_tui->initTerminal();
        g_tuiView = new FLogViewMulti(g_tui, 1000);
        finalcut::FWidget::setMainWidget(g_tuiView);
        g_tuiView->setText(L"Logger");
        g_tuiView->unsetShadow();
        g_tuiView->setResizable(true);
        g_tuiView->setGeometry(finalcut::FPoint{1, 1}, finalcut::FSize{g_tui->getDesktopWidth(), g_tui->getDesktopHeight()});
        g_tuiView->registerOnQuit(quitCb.value());
        g_tuiView->show();
        g_tuiView->createView(kSystemViewId, "system");
        g_tuiOn = true;
    }
#else
    (void)argc;
    (void)argv;
    (void)quitCb;
#endif

    if (const char* capture = std::getenv("UOS_LOG_CAPTURE"); capture && capture[0] == '1') {
        g_captureOn = true;
    }

    quill::Backend::start(quill::BackendOptions{});
    g_backendUp.store(true, std::memory_order_release);
    prepareLogThread();

    const auto systemFolder = g_logRoot / "system";
    const bool systemCreated = !dirWritable(systemFolder);
    const auto systemPath = uniqueLogPath(systemFolder, stampLocal(), "");
    g_systemPath = systemPath;
    {
        std::lock_guard lock(g_taskMu);
        g_systemStream = makeStream(systemFolder, systemPath, "system", "system", kSystemViewId, {}, true, 0);
    }

    if (g_tuiOn && g_systemFileSink) {
        g_stdio = quill::Frontend::create_logger("stdio", g_systemFileSink, logPattern(), quill::ClockSourceType::System);
        g_stdio->set_log_level(quill::LogLevel::TraceL3);
        startTee();
    }

    installCrashSignals();

    SLOG(LogLevel::LOG_INFO, "{}", g_tuiOn ? "logger initialized with TUI" : "logger initialized without TUI");
    if (rootCreated) {
        SLOG(LogLevel::LOG_INFO, "root log folder created");
    }
    if (systemCreated) {
        SLOG(LogLevel::LOG_INFO, "system log folder created");
    }
}

void Logger::start(void) {
#ifdef uOS_ENABLE_TUI
    if (g_tui != nullptr) {
        g_tuiThread = std::thread(tuiThreadFunc);
    }
#endif
}

void Logger::stop(void) {
    // Flush while the widget and the backend are both still alive.
    if (auto sys = systemLog()) {
        sys.flush();
    }
    {
        std::lock_guard lock(g_taskMu);
        for (auto& [id, stream] : g_tasks) {
            (void)id;
            stream.flush();
        }
    }
    if (g_stdio != nullptr && g_backendUp.load(std::memory_order_acquire) && !quill::Logger::is_current_thread_backend_thread()) {
        try {
            g_stdio->flush_log(0);
        } catch (...) {
        }
    }

#ifdef uOS_ENABLE_TUI
    // Join finalcut before restoring stdout. exec() still writes through the tee.
    if (g_tui) {
        g_tui->quit();
        if (g_tuiThread.joinable()) {
            g_tuiThread.join();
        }
    }
#endif
    stopTee();
    // TuiSink::write_log runs only on the backend thread. Publish that the widget is
    // going away, join that thread, and only then delete it.
    g_tuiOn.store(false, std::memory_order_release);
    detail::systemLogger.store(nullptr, std::memory_order_release);
    g_backendUp.store(false, std::memory_order_release);
    quill::Backend::stop();
#ifdef uOS_ENABLE_TUI
    if (g_tui) {
        finalcut::FTermcap::setDefaultPutCharFunction();
        finalcut::FTermcap::setDefaultPutStringFunction();
        delete g_tui;
        g_tui = nullptr;
        g_tuiView = nullptr;
    }
#endif
}

LogStream Logger::openTask(std::string_view actorName, std::string_view instName, TaskId id, LogLimits limits) {
    const std::string type = sanitize(actorName);
    const std::string inst = sanitize(instName);
    const auto folder = g_logRoot / type;
    const auto file = uniqueLogPath(folder, stampLocal(), "_" + inst);
    const std::string loggerName = type + "/" + inst;
    LogStream stream = makeStream(folder, file, loggerName, type, static_cast<uint16_t>(id), limits, false, id);

    {
        std::lock_guard lock(g_taskMu);
        g_tasks.insert_or_assign(id, stream);
    }

#ifdef uOS_ENABLE_TUI
    if (g_tuiOn && g_tuiView != nullptr) {
        g_tuiView->createView(static_cast<uint16_t>(id), loggerName);
    }
#endif
    return stream;
}

void Logger::detachTask(TaskId id) {
    {
        std::lock_guard lock(g_taskMu);
        g_tasks.erase(id);
    }
#ifdef uOS_ENABLE_TUI
    if (g_tuiOn && g_tuiView != nullptr) {
        g_tuiView->removeView(static_cast<uint16_t>(id));
    }
#endif
}

void Logger::setSystemLogLevels(LogLevel fileLevel, LogLevel uiLevel) {
    if (auto sys = systemLog()) {
        sys.setLevels(fileLevel, uiLevel);
    }
}

void Logger::setTaskLogLevels(TaskId id, LogLevel fileLevel, LogLevel uiLevel) {
    std::lock_guard lock(g_taskMu);
    auto it = g_tasks.find(id);
    if (it == g_tasks.end()) {
        return;
    }
    it->second.setLevels(fileLevel, uiLevel);
}

LogLevel Logger::systemFileLogLevel() {
    auto sys = systemLog();
    return sys ? sys.fileLevel() : LogLevel::LOG_INFO;
}

LogLevel Logger::systemUiLogLevel() {
    auto sys = systemLog();
    return sys ? sys.uiLevel() : LogLevel::LOG_INFO;
}

LogLevel Logger::taskFileLogLevel(TaskId id) {
    std::lock_guard lock(g_taskMu);
    auto it = g_tasks.find(id);
    if (it == g_tasks.end()) {
        return LogLevel::LOG_INFO;
    }
    return it->second.fileLevel();
}

LogLevel Logger::taskUiLogLevel(TaskId id) {
    std::lock_guard lock(g_taskMu);
    auto it = g_tasks.find(id);
    if (it == g_tasks.end()) {
        return LogLevel::LOG_INFO;
    }
    return it->second.uiLevel();
}

std::filesystem::path Logger::logRoot() { return g_logRoot; }

std::vector<std::string> Logger::capturedUi() {
    std::lock_guard lock(g_captureMu);
    return g_capture;
}

} // namespace uOS
