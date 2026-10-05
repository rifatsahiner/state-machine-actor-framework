#ifndef UOS_DETAIL_PARENT_DATA_H
#define UOS_DETAIL_PARENT_DATA_H

#include <type_traits>

namespace uOS::detail {

// A reference ParentData is stored as a pointer. The argument has to be an
// lvalue, or that pointer would point at a temporary.
template <class Parent, class Arg>
concept ParentReferenceIsLvalue = !std::is_lvalue_reference_v<Parent> || std::is_lvalue_reference_v<Arg>;

template <class Parent, class Arg>
concept ParentBindable = ParentReferenceIsLvalue<Parent, Arg> && std::is_constructible_v<Parent, Arg>;

// void ParentData takes no parent argument. A non-void ParentData takes one,
// and that argument has to satisfy ParentBindable.
// An empty pack must fail this constraint. Naming a concept parameter for the
// first argument would be ill-formed for an empty pack, not false, so the
// first-type lookup sits behind sizeof...(Args) > 0.
template <class... Args>
struct ParentFirst;

template <class First, class... Rest>
struct ParentFirst<First, Rest...> {
    using type = First;
};

template <class Parent, class... Args>
concept ParentFactoryArgs = std::is_void_v<Parent> ||
                            (sizeof...(Args) > 0 && ParentBindable<Parent, typename ParentFirst<Args...>::type>);

} // namespace uOS::detail

#endif // UOS_DETAIL_PARENT_DATA_H
