#include "detail/logger.h"
#include "uOS.h"

#include <cstdlib>
#include <iostream>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

int g_fails = 0;

void expect(bool cond, const char* msg) {
    if (!cond) {
        std::cerr << "FAIL " << msg << "\n";
        ++g_fails;
    }
}

class IdTask : public uOS::SyncTask {
public:
    IdTask(uOS::TaskId id, std::string inst) : SyncTask(id, std::move(inst)) {}

private:
    std::string _getTaskName(void) const override { return "IdTask"; }
    std::vector<std::pair<State, std::string_view>> _getStateNameMap(void) const override { return {}; }
    HandleResult _topInitialTrans(void) override { return HandleResult::Ignored; }
};

} // namespace

int main() {
    ::setenv("UOS_LOG_CAPTURE", "1", 1);
    uOS::Logger::init(0, nullptr, "task-id-test");

    uOS::FW::deleteTask(uOS::NO_TASK_ID);

    const uOS::TaskId first = uOS::FW::createTask<IdTask>("a");
    expect(first != uOS::NO_TASK_ID, "first createTask is not zero");
    uOS::FW::deleteTask(first);

    const uOS::TaskId again = uOS::FW::createTask<IdTask>("b");
    expect(again != uOS::NO_TASK_ID, "id after delete is not zero");
    uOS::FW::deleteTask(again);

    const uOS::TaskId left = uOS::FW::createTask<IdTask>("c");
    const uOS::TaskId right = uOS::FW::createTask<IdTask>("d");
    expect(left != uOS::NO_TASK_ID && right != uOS::NO_TASK_ID && left != right, "two live tasks get distinct non-zero ids");
    uOS::FW::deleteTask(left);
    uOS::FW::deleteTask(right);

    constexpr int liveCap = uOS_FW_MAX_TASK_COUNT - 1;
    std::vector<uOS::TaskId> ids;
    ids.reserve(static_cast<std::size_t>(liveCap));
    std::set<uOS::TaskId> seen;
    bool allNonZero = true;
    for (int i = 0; i < liveCap; ++i) {
        const uOS::TaskId id = uOS::FW::createTask<IdTask>(std::to_string(i));
        if (id == uOS::NO_TASK_ID) {
            allNonZero = false;
        }
        seen.insert(id);
        ids.push_back(id);
    }
    expect(allNonZero, "full allocation never returns zero");
    expect(seen.size() == ids.size(), "full allocation ids are unique");

    for (uOS::TaskId id : ids) {
        uOS::FW::deleteTask(id);
    }

    uOS::Logger::stop();

    if (g_fails != 0) {
        std::cerr << g_fails << " failed\n";
        return 1;
    }
    std::cout << "ok\n";
    return 0;
}
