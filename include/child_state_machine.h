#ifndef CHILD_STATE_MACHINE_H
#define CHILD_STATE_MACHINE_H

#include <memory>
#include <type_traits>
#include <utility>

#include "state_machine.h"

namespace uOS {

// Included from uOS.h after class FW is defined.
// The Shared& constructor and parentData() are member templates so
// ChildStateMachine<void> never forms a void& parameter.
template <class Shared = void> class ChildStateMachine : public StateMachine {
public:
    ChildStateMachine(const ChildStateMachine&) = delete;
    ChildStateMachine(ChildStateMachine&&) = delete;
    ChildStateMachine& operator=(const ChildStateMachine&) = delete;
    ChildStateMachine& operator=(ChildStateMachine&&) = delete;

protected:
    explicit ChildStateMachine(TaskId parentTaskId)
        requires std::is_void_v<Shared>
        : _parentTaskId(parentTaskId) {}

    template <class S = Shared>
        requires(!std::is_void_v<S> && std::is_same_v<S, Shared>)
    ChildStateMachine(std::type_identity_t<S>& shared, TaskId parentTaskId) : _parentTaskId(parentTaskId), _shared(&shared) {}

    TaskId parentTaskId() const noexcept { return _parentTaskId; }

    void postToParent(std::shared_ptr<const Event> event) const { FW::postEvent(_parentTaskId, std::move(event)); }

    TimerId postToParentIn(uint_fast16_t intervalMs, std::shared_ptr<const Event> event) const {
        return FW::postEventIn(intervalMs, _parentTaskId, std::move(event));
    }

    template <class S = Shared>
        requires(!std::is_void_v<S> && std::is_same_v<S, Shared>)
    S& parentData() noexcept {
        return *_shared;
    }

    template <class S = Shared>
        requires(!std::is_void_v<S> && std::is_same_v<S, Shared>)
    const S& parentData() const noexcept {
        return *_shared;
    }

private:
    struct EmptyShared {};

    TaskId _parentTaskId{};
    std::conditional_t<std::is_void_v<Shared>, EmptyShared, Shared*> _shared{};
};

} // namespace uOS

#endif // CHILD_STATE_MACHINE_H
