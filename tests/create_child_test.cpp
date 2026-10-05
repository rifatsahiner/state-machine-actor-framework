#include "detail/logger.h"
#include "uOS.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <tuple>
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

class BareChild : public uOS::ChildStateMachine<> {
public:
    BareChild(uOS::TaskId id, uOS::LogStream log) : ChildStateMachine(id, std::move(log)) {}

    void go(const char* text) {
        LOG(uOS::LogLevel::LOG_INFO, "{}", text);
        logStream().flush();
    }

private:
    HandleResult _topInitialTrans(void) override { return HandleResult::Ignored; }
};

// Move is deleted so createChild has to construct in place.
class ImmovableChild : public uOS::ChildStateMachine<> {
public:
    ImmovableChild(uOS::TaskId id, uOS::LogStream log) : ChildStateMachine(id, std::move(log)) {}
    ImmovableChild(ImmovableChild&&) = delete;

private:
    HandleResult _topInitialTrans(void) override { return HandleResult::Ignored; }
};

class RefChild : public uOS::ChildStateMachine<const int&> {
public:
    // Pass the reference by name. std::move would hide the lvalue from the base.
    RefChild(ParentData value, int extra, uOS::TaskId id, uOS::LogStream log)
        : ChildStateMachine(value, id, std::move(log)), _extra(extra) {}

    const int& value() noexcept { return parentData(); }
    int extra() const { return _extra; }

private:
    HandleResult _topInitialTrans(void) override { return HandleResult::Ignored; }
    int _extra;
};

using AggParent = struct {
    int& n;
    std::string& s;
};

class AggChild : public uOS::ChildStateMachine<AggParent> {
public:
    AggChild(ParentData parent, uOS::TaskId id, uOS::LogStream log) : ChildStateMachine(std::move(parent), id, std::move(log)) {}

    AggParent& view() noexcept { return parentData(); }

private:
    HandleResult _topInitialTrans(void) override { return HandleResult::Ignored; }
};

class TupChild : public uOS::ChildStateMachine<std::tuple<int&, int>> {
public:
    TupChild(ParentData parent, uOS::TaskId id, uOS::LogStream log) : ChildStateMachine(std::move(parent), id, std::move(log)) {}

    ParentData& view() noexcept { return parentData(); }

private:
    HandleResult _topInitialTrans(void) override { return HandleResult::Ignored; }
};

class DispatchChild : public uOS::ChildStateMachine<> {
public:
    DispatchChild(int mark, uOS::TaskId id, uOS::LogStream log) : ChildStateMachine(id, std::move(log)), _mark(mark) {}

    int mark() const { return _mark; }
    bool started() const { return _started; }
    bool saw() const { return _saw; }

private:
    HandleResult _topInitialTrans(void) override {
        _started = true;
        return _trans(&DispatchChild::idle);
    }

    HandleResult idle(const uOS::Event& event) {
        if (event.signal == 42) {
            _saw = true;
            return _handled();
        }
        return _super(&DispatchChild::top);
    }

    int _mark;
    bool _started{false};
    bool _saw{false};
};

class FactoryTask : public uOS::SyncTask {
public:
    FactoryTask(uOS::TaskId id, std::string inst) : SyncTask(id, std::move(inst)) {}

    // A failed constraint inside a non-template requires-expression is a hard error on GCC.
    // The check is a concept-style requirement in a function template so rejection is false.
    template <class Parent>
    static constexpr bool canPassRef() {
        return requires(FactoryTask& task, Parent parent) { task.createChild<RefChild>(std::forward<Parent>(parent), 3); };
    }

    // Rejection has to be checked from a function template. In a non-template
    // requires-expression, GCC turns a failed constraint into a hard error.
    template <class Csm, class... Args>
    static constexpr bool canCreate() {
        return requires(FactoryTask& task, Args... args) { task.createChild<Csm>(std::forward<Args>(args)...); };
    }

    static void constraints() {
        static_assert(canPassRef<int&>());
        static_assert(!canPassRef<int>());
        static_assert(canCreate<BareChild>());
        static_assert(canCreate<ImmovableChild>());
        static_assert(requires(FactoryTask& task, int& n, std::string& s) { task.createChild<AggChild>(AggParent{n, s}); });
        static_assert(!canCreate<AggChild>());
        static_assert(!canCreate<RefChild>());
    }

private:
    std::string _getTaskName(void) const override { return "CreateChildTask"; }

    std::vector<std::pair<State, std::string_view>> _getStateNameMap(void) const override {
        return {{static_cast<State>(&FactoryTask::idle), "idle"}};
    }

    HandleResult idle(const uOS::Event&) { return _super(&FactoryTask::top); }

    HandleResult _topInitialTrans(void) override {
        constraints();

        auto bare = createChild<BareChild>();
        bare->go("create-child");

        auto stuck = createChild<ImmovableChild>();
        expect(static_cast<bool>(stuck), "immovable child is constructed in place");

        int n = 1;
        auto ref = createChild<RefChild>(n, 4);
        n = 9;
        expect(ref->value() == 9, "reference parent sees a later write");
        expect(ref->extra() == 4, "reference child keeps its own argument");

        std::string s = "a";
        auto agg = createChild<AggChild>(AggParent{n, s});
        n = 11;
        s = "later";
        expect(agg->view().n == 11 && agg->view().s == "later", "aggregate view aliases task members");

        int live = 3;
        int snap = 4;
        auto tup = createChild<TupChild>(std::tuple<int&, int>{live, snap});
        live = 30;
        snap = 40;
        expect(std::get<0>(tup->view()) == 30, "tuple reference element tracks a later write");
        expect(std::get<1>(tup->view()) == 4, "tuple value element keeps the construction value");

        auto machine = createChild<DispatchChild>(9);
        expect(machine->mark() == 9, "child receives its own argument");
        machine->init();
        expect(machine->started(), "child runs its initial transition");
        machine->dispatch(uOS::Event{42});
        expect(machine->saw(), "child dispatches the event");

        return _trans(&FactoryTask::idle);
    }
};

} // namespace

int main() {
    ::setenv("UOS_LOG_CAPTURE", "1", 1);
    uOS::Logger::init(0, nullptr, "create-child-test");
    const fs::path root = uOS::Logger::logRoot();

    const uOS::TaskId id = uOS::FW::createTask<FactoryTask>("c1");
    uOS::FW::startTask(id);
    uOS::FW::deleteTask(id);

    const fs::path taskLog = newest(root / "CreateChildTask");
    expect(!taskLog.empty(), "createChild task log exists");
    expect(contains(taskLog, "create-child"), "child writes the task log");

    uOS::Logger::stop();

    if (g_fails != 0) {
        std::cerr << g_fails << " failed\n";
        return 1;
    }
    std::cout << "ok\n";
    return 0;
}
