#pragma once
// ekit - system.hpp
//
// Systems are classes with an Execute(World&) method that explicitly declare
// their data dependencies with `using Reads = ...` and `using Writes = ...`:
//
//   struct MoveSystem {
//       using Reads  = ekit::TypeList<Position>;
//       using Writes = ekit::TypeList<Velocity>;
//       void Execute(ekit::World& world) {
//           world.Query<Position, Velocity>()
//                .ForEach([](Position& p, Velocity& v) { p.x += v.x; });
//       }
//   };
//
// The Scheduler analyzes these declarations to build a dependency graph and
// executes independent systems in parallel.

#include "world.hpp"

#include <typeindex>
#include <vector>

namespace ekit {

// ---------------------------------------------------------------------------
// Dependency declarations
// ---------------------------------------------------------------------------

namespace detail {

// Reads / Writes extraction. Default to an empty TypeList when the system does
// not declare a dependency (it is then treated as fully independent).
template<typename T, typename = void>
struct SystemReads {
    using type = TypeList<>;
};
template<typename T>
struct SystemReads<T, std::void_t<typename T::Reads>> {
    using type = typename T::Reads;
};
template<typename T>
using SystemReads_t = typename SystemReads<T>::type;

template<typename T, typename = void>
struct SystemWrites {
    using type = TypeList<>;
};
template<typename T>
struct SystemWrites<T, std::void_t<typename T::Writes>> {
    using type = typename T::Writes;
};
template<typename T>
using SystemWrites_t = typename SystemWrites<T>::type;

template<typename List, typename WorldT>
struct AppendTypeIds;
template<typename... Ts, typename WorldT>
struct AppendTypeIds<TypeList<Ts...>, WorldT> {
    static void Apply(WorldT& world, std::vector<ComponentTypeId>& out) {
        (out.push_back(world.template GetComponentTypeId<Ts>()), ...);
    }
};

} // namespace detail

// ---------------------------------------------------------------------------
// System interface
// ---------------------------------------------------------------------------

// Type-erased system. Scheduler stores systems through this interface.
class ISystem {
public:
    virtual ~ISystem() = default;

    virtual void Execute(World& world) = 0;

    // Resolves the system's declared Reads/Writes against the world. Throws a
    // clear error when a declared component was never registered.
    virtual void GetDependencies(World& world, std::vector<ComponentTypeId>& reads,
                                 std::vector<ComponentTypeId>& writes) = 0;

    virtual const char* GetName() const = 0;
};

namespace detail {

template<typename T>
class SystemWrapper final : public ISystem {
public:
    explicit SystemWrapper(T system)
        : system_(std::move(system)) {}

    void Execute(World& world) override {
        system_.Execute(world);
    }

    void GetDependencies(World& world, std::vector<ComponentTypeId>& reads,
                         std::vector<ComponentTypeId>& writes) override {
        AppendTypeIds<SystemReads_t<T>, World>::Apply(world, reads);
        AppendTypeIds<SystemWrites_t<T>, World>::Apply(world, writes);
    }

    const char* GetName() const override {
        return typeid(T).name();
    }

private:
    T system_;
};

} // namespace detail
} // namespace ekit
