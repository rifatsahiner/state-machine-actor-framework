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

    const uOS::TaskId namedId = uOS::FW::createTask<NamedTask>("n1");
    uOS::FW::deleteTask(namedId);
    const fs::path named = newest(root / "NamedTask");
    expect(!named.empty(), "createTask opens a folder from _getTaskName");
    expect(contains(named, "Instance n1 is initialized"), "init line uses the instance name");
    expect(contains(named, "NamedTask/n1"), "logger name uses the virtual type name");

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
