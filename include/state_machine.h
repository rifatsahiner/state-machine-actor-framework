#ifndef STATE_MACHINE_H
#define STATE_MACHINE_H

#include "uOS_defs.h"

namespace uOS {

class StateMachine {
public:
    // definition for state-handler function
    enum class HandleResult : uint_fast8_t { Handled, Unhandled, Trans, Super, Ignored };
    using State = HandleResult (StateMachine::*)(const Event&);

    StateMachine() : _currentState{&StateMachine::top}, _tempTargetState{&StateMachine::top} {}
    virtual ~StateMachine() {};

    // Copying a live machine is meaningless. Moving one through a StateMachine&
    // would copy the state pointers and leave the derived object behind.
    StateMachine(const StateMachine&) = delete;
    StateMachine& operator=(const StateMachine&) = delete;
    StateMachine(StateMachine&&) = delete;
    StateMachine& operator=(StateMachine&&) = delete;

    void init(void);
    void dispatch(const Event& event);
    bool isIn(const State);

protected:
    State _getState(void) const { return _currentState; }

    // conceptual UML top state that ignores all events
    HandleResult top(const Event& event) {
        (void)event;
        return HandleResult::Ignored;
    }

    // termination psude-state -> 🔘
    // Entry is handled. Every other signal goes to top. Override for a custom termination.
    virtual HandleResult _terminationState(const Event& event) {
        switch (event.signal) {
            case ENTRY_SIG:
                return _handled();
            default:
                return _super(&StateMachine::top);
        }
    }

    // macro-like functions for event handle returns (handled/unhandled/transiton/superstate)
    static constexpr HandleResult _handled(void) { return HandleResult::Handled; }
    static constexpr HandleResult _unhandled(void) { return HandleResult::Unhandled; } // because of a guard
    template <typename T> constexpr HandleResult _trans(T target) {
        static_assert(std::is_base_of_v<StateMachine, remove_ptr_to_member_func_t<T>>);

        // store target in temp and return result as state transition
        _tempTargetState = static_cast<State>(target);
        return HandleResult::Trans;
    }
    template <typename T> constexpr HandleResult _super(T superstate) {
        static_assert(std::is_base_of_v<StateMachine, remove_ptr_to_member_func_t<T>>);

        // store superstate in temp and return result as delegated to superstate
        _tempTargetState = static_cast<State>(superstate);
        return HandleResult::Super;
    }

    // reserved signals accoridng to UML semantics
    enum ReservedSignals : SignalId {
        ENTRY_SIG = static_cast<SignalId>(1), // signal for entry actions
        EXIT_SIG,                             // signal for exit actions
        INIT_SIG                              // signal for nested initial transitions
    };

private:
    State _currentState;    // current active state (state-variable)
    State _tempTargetState; // temporary state: target of transition or superstate

    State _procInitTrans(void);

    // top initial transition that every UML state machine must provide
    virtual HandleResult _topInitialTrans(void) = 0;

private:
    // custom type trait that gets class from a pointer to member function in 'State' form
    // used for type checking state handler functions
    template <typename> struct remove_ptr_to_member_func;

    template <typename T> struct remove_ptr_to_member_func<HandleResult (T::*)(const Event&)> {
        using type = T;
    };

    template <typename U> using remove_ptr_to_member_func_t = typename remove_ptr_to_member_func<U>::type;
};

} // namespace uOS

#endif // STATE_MACHINE_H