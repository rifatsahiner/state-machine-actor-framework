#include "detail/logger.h"
#include "uOS.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

int g_fails = 0;

void expect(bool cond, const char* msg) {
    if (!cond) {
        std::cerr << "FAIL " << msg << "\n";
        ++g_fails;
    }
}

std::string readFile(const fs::path& path) {
    std::ifstream in(path);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool contains(const fs::path& path, std::string_view needle) { return readFile(path).find(needle) != std::string::npos; }

fs::path newest(const fs::path& dir) {
    fs::path best;
    fs::file_time_type when{};
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (ec || !entry.is_regular_file()) {
            continue;
        }
        const auto stamp = entry.last_write_time(ec);
        if (best.empty() || stamp >= when) {
            best = entry.path();
            when = stamp;
        }
    }
    return best;
}

std::size_t fileCount(const fs::path& dir) {
    std::size_t count = 0;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (!ec && entry.is_regular_file()) {
            ++count;
        }
    }
    return count;
}

bool capturedHas(std::string_view needle) {
    for (const auto& line : uOS::Logger::capturedUi()) {
        if (line.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

int capturedCount(std::string_view needle) {
    int count = 0;
    for (const auto& line : uOS::Logger::capturedUi()) {
        if (line.find(needle) != std::string::npos) {
            ++count;
        }
    }
    return count;
}

class Probe : public uOS::ChildStateMachine<> {
public:
    Probe(uOS::TaskId id, uOS::LogStream log) : ChildStateMachine(id, std::move(log)) {}
    void go() { LOG(uOS::LogLevel::LOG_INFO, "from-child"); }

private:
    HandleResult _topInitialTrans(void) override { return HandleResult::Ignored; }
};

class NamedTask : public uOS::SyncTask {
public:
    NamedTask(uOS::TaskId id, std::string inst) : SyncTask(id, std::move(inst)) {}

private:
    std::string _getTaskName(void) const override { return "NamedTask"; }
    std::vector<std::pair<State, std::string_view>> _getStateNameMap(void) const override { return {}; }
    HandleResult _topInitialTrans(void) override { return HandleResult::Ignored; }
};

// deleteTask unlinks the task before the destructor. Subscribing here leaves
// an id on the signal that is no longer in the task map.
class PubProbe : public uOS::SyncTask {
public:
    static constexpr uOS::SignalId kSignal = 200;

    PubProbe(uOS::TaskId id, std::string inst) : SyncTask(id, std::move(inst)) {}
    ~PubProbe() { _subscribe(kSignal); }

private:
    std::string _getTaskName(void) const override { return "PubProbe"; }
    std::vector<std::pair<State, std::string_view>> _getStateNameMap(void) const override { return {}; }
    HandleResult _topInitialTrans(void) override { return HandleResult::Ignored; }
};

} // namespace

int main() {
    ::setenv("UOS_LOG_CAPTURE", "1", 1);
    uOS::Logger::init(0, nullptr, "log-stream-test");
    const fs::path root = uOS::Logger::logRoot();

    SLOG(uOS::LogLevel::LOG_INFO, "hello-system");
    SLOG(uOS::LogLevel::LOG_TRACE, "system-trace-hidden");

    uOS::LogLimits tiny;
    tiny.maxFileBytes = 1800;
    tiny.maxFolderFiles = 8;
    auto stream = uOS::Logger::openTask("AmfTask", "amf1", 3, tiny);
    LOG_TO(stream, uOS::LogLevel::LOG_INFO, "hello-task");
    LOG_TO(stream, uOS::LogLevel::LOG_TRACE, "task-trace-hidden");
    stream.flush();
    uOS::systemLog().flush();

    const fs::path sys = newest(root / "system");
    const fs::path task = newest(root / "AmfTask");
    expect(!sys.empty() && !task.empty(), "log files exist");
    expect(contains(sys, "hello-system"), "system info");
    expect(!contains(sys, "system-trace-hidden"), "system trace dropped");
    expect(contains(task, "hello-task"), "task info");
    expect(!contains(task, "task-trace-hidden"), "task trace dropped");
    expect(contains(task, "AmfTask/amf1"), "logger name on the line");
    expect(contains(task, "INFO"), "info level word");
    expect(capturedHas("hello-system") && capturedHas("hello-task"), "ui got info lines");
    expect(!capturedHas("system-trace-hidden") && !capturedHas("task-trace-hidden"), "ui dropped trace");

    stream.setLevels(uOS::LogLevel::LOG_TRACE, uOS::LogLevel::LOG_INFO);
    LOG_TO(stream, uOS::LogLevel::LOG_TRACE, "file-only-trace");
    stream.flush();
    expect(contains(task, "file-only-trace"), "file-only trace is in the file");
    expect(contains(task, "TRACE_L1"), "trace uses Quill's level name");
    expect(!capturedHas("file-only-trace"), "file-only trace stays off the ui");

    stream.setLevels(uOS::LogLevel::LOG_INFO, uOS::LogLevel::LOG_TRACE);
    LOG_TO(stream, uOS::LogLevel::LOG_TRACE, "ui-only-trace");
    stream.flush();
    expect(!contains(task, "ui-only-trace"), "ui-only trace stays out of the file");
    expect(capturedHas("ui-only-trace"), "ui-only trace reaches the ui");

    auto copy = stream;
    LOG_TO(copy, uOS::LogLevel::LOG_INFO, "from-copy");
    copy.flush();
    expect(contains(task, "from-copy"), "copied stream writes the same file");

    Probe probe(3, stream);
    probe.go();
    stream.flush();
    expect(contains(task, "from-child"), "child state machine uses the parent stream");

    const std::string blob(1200, 'x');
    LOG_TO(stream, uOS::LogLevel::LOG_INFO, "{}", blob);
    LOG_TO(stream, uOS::LogLevel::LOG_INFO, "{}", blob);
    stream.flush();
    bool rotated = false;
    for (const auto& entry : fs::directory_iterator(root / "AmfTask")) {
        if (entry.path().string().find(".1.log") != std::string::npos) {
            rotated = true;
        }
    }
    expect(rotated, "size rotation opens a .1 segment");

    uOS::LogLimits sweep;
    sweep.maxFolderFiles = 1;
    {
        auto first = uOS::Logger::openTask("SweepTask", "a", 10, sweep);
        uOS::Logger::detachTask(10);
        (void)first;
    }
    auto second = uOS::Logger::openTask("SweepTask", "b", 11, sweep);
    uOS::Logger::detachTask(11);
    (void)second;
    expect(fileCount(root / "SweepTask") == 1, "folder file cap deletes the oldest closed file");

    uOS::LogLimits trimLim;
    trimLim.maxFolderFiles = 2;
    trimLim.maxFolderBytes = 1024ull * 1024ull * 1024ull * 1024ull;
    const fs::path trimDir = root / "TrimTask";
    std::error_code trimRm;
    fs::remove_all(trimDir, trimRm);
    {
        auto oldest = uOS::Logger::openTask("TrimTask", "a", 30, trimLim);
        uOS::Logger::detachTask(30);
        (void)oldest;
    }
    {
        auto middle = uOS::Logger::openTask("TrimTask", "b", 31, trimLim);
        uOS::Logger::detachTask(31);
        (void)middle;
    }
    {
        auto newestTask = uOS::Logger::openTask("TrimTask", "c", 32, trimLim);
        uOS::Logger::detachTask(32);
        (void)newestTask;
    }
    expect(fileCount(trimDir) == 2, "third open drops one closed file");
    bool sawA = false;
    bool sawB = false;
    bool sawC = false;
    for (const auto& entry : fs::directory_iterator(trimDir)) {
        const auto name = entry.path().filename().string();
        sawA = sawA || name.ends_with("_a.log");
        sawB = sawB || name.ends_with("_b.log");
        sawC = sawC || name.ends_with("_c.log");
    }
    expect(!sawA && sawB && sawC, "oldest closed file is the one removed");

    uOS::LogLimits keep;
    keep.maxFolderFiles = 100000;
    keep.maxFolderBytes = 1024ull * 1024ull * 1024ull * 1024ull;
    const fs::path keepDir = root / "KeepTask";
    const std::size_t keepBefore = fs::exists(keepDir) ? fileCount(keepDir) : 0;
    {
        auto a = uOS::Logger::openTask("KeepTask", "a", 20, keep);
        auto b = uOS::Logger::openTask("KeepTask", "b", 21, keep);
        auto c = uOS::Logger::openTask("KeepTask", "c", 22, keep);
        uOS::Logger::detachTask(20);
        uOS::Logger::detachTask(21);
        uOS::Logger::detachTask(22);
        (void)a;
        (void)b;
        (void)c;
    }
    expect(fileCount(keepDir) == keepBefore + 3, "closing a task log does not delete it");

    // Same limits on every stream so they share one folder record. The closed
    // files stay empty: an init line in each would push a correct trim over the
    // byte cap and the test could no longer see a phantom rotation count.
    uOS::LogLimits rot;
    rot.maxFileBytes = 512;
    rot.maxFolderFiles = 1000;
    rot.maxFolderBytes = 51200;
    // Same rule as backupCount(): one live chain is the active file plus this many backups.
    const std::uintmax_t segment = rot.maxFileBytes < 512 ? 512 : rot.maxFileBytes;
    std::uintmax_t byBytes = segment == 0 ? 1 : rot.maxFolderBytes / segment;
    if (byBytes == 0) {
        byBytes = 1;
    }
    const std::uintmax_t byFiles = rot.maxFolderFiles == 0 ? 1 : rot.maxFolderFiles;
    std::uintmax_t backups = byBytes < byFiles ? byBytes : byFiles;
    if (backups < 1) {
        backups = 1;
    }
    constexpr int kClosedLogs = 30;
    constexpr int kRotateWrites = 250;
    const std::size_t steadyFiles = static_cast<std::size_t>(kClosedLogs) + static_cast<std::size_t>(backups) + 1;
    const fs::path rotDir = root / "RotateTask";
    std::error_code rotRm;
    fs::remove_all(rotDir, rotRm);
    for (int i = 0; i < kClosedLogs; ++i) {
        const auto id = static_cast<uOS::TaskId>(100 + i);
        auto closed = uOS::Logger::openTask("RotateTask", "c" + std::to_string(i), id, rot);
        uOS::Logger::detachTask(id);
        (void)closed;
    }
    {
        auto live = uOS::Logger::openTask("RotateTask", "live", 200, rot);
        const std::string blob(400, 'y');
        // Each line is one segment. The loop has to pass backupCount or the file
        // count never reaches the steady chain and a missing backup delete stays hidden.
        for (int n = 0; n < kRotateWrites; ++n) {
            LOG_TO(live, uOS::LogLevel::LOG_INFO, "{}", blob);
            live.flush();
        }
        std::size_t totalFiles = 0;
        std::uintmax_t totalBytes = 0;
        int closedBases = 0;
        for (const auto& entry : fs::directory_iterator(rotDir)) {
            if (!entry.is_regular_file()) {
                continue;
            }
            ++totalFiles;
            totalBytes += entry.file_size();
            const auto name = entry.path().filename().string();
            const auto dot = name.rfind('.');
            if (dot == std::string::npos || name.substr(dot) != ".log") {
                continue;
            }
            const auto stem = name.substr(0, dot);
            const auto mark = stem.rfind("_c");
            if (mark == std::string::npos || mark + 2 >= stem.size()) {
                continue;
            }
            bool digits = true;
            for (std::size_t i = mark + 2; i < stem.size(); ++i) {
                if (stem[i] < '0' || stem[i] > '9') {
                    digits = false;
                    break;
                }
            }
            if (digits) {
                ++closedBases;
            }
        }
        if (closedBases != kClosedLogs || totalFiles != steadyFiles || totalBytes > rot.maxFolderBytes) {
            std::cerr << "rotate files=" << totalFiles << " steady=" << steadyFiles << " bytes=" << totalBytes << " closed=" << closedBases
                      << "\n";
        }
        expect(closedBases == kClosedLogs, "rotation leaves the other closed logs");
        expect(totalFiles == steadyFiles, "rotation holds the closed logs plus the full backup chain");
        expect(totalBytes <= rot.maxFolderBytes, "rotation stays within the byte cap");
        uOS::Logger::detachTask(200);
        (void)live;
    }

    const uOS::TaskId namedId = uOS::FW::createTask<NamedTask>("n1");
    uOS::FW::deleteTask(namedId);
    const fs::path named = newest(root / "NamedTask");
    expect(!named.empty(), "createTask opens a folder from _getTaskName");
    expect(contains(named, "Instance n1 is initialized"), "init line uses the instance name");
    expect(contains(named, "NamedTask/n1"), "logger name uses the virtual type name");

    const uOS::TaskId probeId = uOS::FW::createTask<PubProbe>("p1");
    uOS::FW::deleteTask(probeId);
    uOS::FW::publishEvent(uOS::new_e(PubProbe::kSignal));
    uOS::systemLog().flush();
    const int missingWarnings = capturedCount("not found on publishing");
    expect(missingWarnings == 1, "publish to a missing task warns while running");
    uOS::FW::kill();
    uOS::FW::publishEvent(uOS::new_e(PubProbe::kSignal));
    uOS::systemLog().flush();
    expect(capturedCount("not found on publishing") == missingWarnings, "publish after kill does not warn");

    uOS::Logger::stop();

    const fs::path sysAfter = newest(root / "system");
    expect(contains(sysAfter, "hello-system"), "stop leaves the system line on disk");

    if (g_fails != 0) {
        std::cerr << g_fails << " failed\n";
        return 1;
    }
    std::cout << "ok\n";
    return 0;
}
