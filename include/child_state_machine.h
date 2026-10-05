#ifndef CHILD_STATE_MACHINE_H
#define CHILD_STATE_MACHINE_H

#include <memory>
#include <type_traits>
#include <utility>

#include "detail/parent_data.h"
#include "log_stream.h"
#include "state_machine.h"

namespace uOS {

// ParentData is the view of the parent task:
//   void                         no parent view
//   an lvalue reference          pointer to that task member; pass it by name
//   an object (struct or tuple)  stored by value; a temporary view may be moved in
// createChild rejects a temporary when ParentData is a reference.
template <class Parent = void> class ChildStateMachine : public StateMachine {
public:
    using ParentData = Parent;

    static_assert(!std::is_rvalue_reference_v<Parent>, "ParentData cannot be an rvalue reference");

    ChildStateMachine(const ChildStateMachine&) = delete;
    ChildStateMachine(ChildStateMachine&&) = delete;
    ChildStateMachine& operator=(const ChildStateMachine&) = delete;
    ChildStateMachine& operator=(ChildStateMachine&&) = delete;

protected:
    ChildStateMachine(TaskId parentTaskId, LogStream log)
        requires std::is_void_v<Parent>
        : _parentTaskId(parentTaskId), _log(std::move(log)), _parentData{} {}

    // Arg&& keeps the value category. A reference parameter would bind a
    // temporary and hide it from this constraint.
    template <class Arg>
        requires(!std::is_void_v<Parent> && detail::ParentBindable<Parent, Arg>)
    ChildStateMachine(Arg&& parent, TaskId parentTaskId, LogStream parentStream)
        : _parentTaskId(parentTaskId), _log(std::move(parentStream)), _parentData(makeParentData(std::forward<Arg>(parent))) {}

    TaskId parentTaskId() const noexcept { return _parentTaskId; }

    // Reference to this machine's stream. Copy it to keep a handle.
    const LogStream& logStream() const { return _log; }

    // termination psude-state -> 🔘
    // Override for a custom termination.
    HandleResult _terminationState(const Event& event) override {
        if (event.signal == ENTRY_SIG) {
            LOG(LogLevel::LOG_INFO, "ENTRY for TERMINATION-STATE");
        }
        return StateMachine::_terminationState(event);
    }

    void postToParent(std::shared_ptr<const Event> event) const { FW::postEvent(_parentTaskId, std::move(event)); }

    TimerId postToParentIn(uint_fast16_t intervalMs, std::shared_ptr<const Event> event) const {
        return FW::postEventIn(intervalMs, _parentTaskId, std::move(event));
    }

    template <class P = Parent>
        requires(!std::is_void_v<P> && std::is_same_v<P, Parent> && std::is_lvalue_reference_v<P>)
    std::remove_reference_t<P>& parentData() noexcept {
        return *_parentData;
    }

    template <class P = Parent>
        requires(!std::is_void_v<P> && std::is_same_v<P, Parent> && std::is_lvalue_reference_v<P>)
    const std::remove_reference_t<P>& parentData() const noexcept {
        return *_parentData;
    }

    template <class P = Parent>
        requires(!std::is_void_v<P> && std::is_same_v<P, Parent> && !std::is_lvalue_reference_v<P>)
    P& parentData() noexcept {
        return _parentData;
    }

    template <class P = Parent>
        requires(!std::is_void_v<P> && std::is_same_v<P, Parent> && !std::is_lvalue_reference_v<P>)
    const P& parentData() const noexcept {
        return _parentData;
    }

private:
    struct EmptyParent {};

    using Stored = std::conditional_t<std::is_void_v<Parent>, EmptyParent,
                                      std::conditional_t<std::is_lvalue_reference_v<Parent>, std::remove_reference_t<Parent>*, Parent>>;

    template <class Arg> static Stored makeParentData(Arg&& parent) {
        if constexpr (std::is_lvalue_reference_v<Parent>) {
            return std::addressof(parent);
        } else {
            return Stored(std::forward<Arg>(parent));
        }
    }

    TaskId _parentTaskId{};
    LogStream _log;
    Stored _parentData;
};

} // namespace uOS

#endif // CHILD_STATE_MACHINE_H
