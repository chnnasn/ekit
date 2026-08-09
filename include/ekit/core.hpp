#pragma once
// ekit - a friendly, explicit, modern C++20 ECS library.
//
// core.hpp - common vocabulary types and compile-time utilities shared by all
// modules (Entity ids, component ids, TypeList, exceptions, type ids).

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <typeinfo>
#include <utility>

namespace ekit {

// ---------------------------------------------------------------------------
// Identifiers
// ---------------------------------------------------------------------------

// Index of an entity inside the World's entity storage.
using EntityId = std::uint32_t;

// Generation counter of an entity. It is bumped every time an entity slot is
// recycled so stale handles can be detected (dangling-handle safety).
using EntityGeneration = std::uint16_t;

// Runtime identifier of a registered component type. Assigned when the user
// explicitly calls world.RegisterComponent<T>().
using ComponentTypeId = std::uint32_t;

inline constexpr ComponentTypeId kInvalidComponentTypeId = 0;

// ---------------------------------------------------------------------------
// Exceptions
// ---------------------------------------------------------------------------

// Base exception type for all ekit runtime errors. Messages are written to be
// actionable: they tell the caller exactly what to fix.
class EkitException : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// ---------------------------------------------------------------------------
// TypeList
// ---------------------------------------------------------------------------

// Compile-time list of types. Used by queries, system dependency declarations
// (Reads / Writes) and component batches.
template<typename... Ts>
struct TypeList {};

namespace detail {

template<typename T>
struct IsTypeList : std::false_type {};
template<typename... Ts>
struct IsTypeList<TypeList<Ts...>> : std::true_type {};

template<typename T>
struct AlwaysFalse : std::false_type {};

template<typename A, typename B>
struct TypeListCat;
template<typename... As, typename... Bs>
struct TypeListCat<TypeList<As...>, TypeList<Bs...>> {
    using type = TypeList<As..., Bs...>;
};
template<typename A, typename B>
using TypeListCat_t = typename TypeListCat<A, B>::type;

template<typename List>
struct IsEmptyList : std::false_type {};
template<>
struct IsEmptyList<TypeList<>> : std::true_type {};

inline constexpr std::size_t kNpos = static_cast<std::size_t>(-1);

// Process-wide, monotonically increasing type id. Used for events and other
// runtime-keyed registries that do not need explicit registration.
inline std::atomic<std::size_t>& NextTypeId() {
    static std::atomic<std::size_t> counter{1};
    return counter;
}


// Process-wide component type id counter. Component ids are shared by all
// worlds so that ComponentTypeIdOf<T> is globally consistent.
inline std::atomic<ComponentTypeId>& NextComponentTypeId() {
    static std::atomic<ComponentTypeId> counter{1};
    return counter;
}

template<typename T>
std::size_t TypeIdOf() {
    static const std::size_t id = NextTypeId().fetch_add(1);
    return id;
}

} // namespace detail
} // namespace ekit

