#pragma once
// ekit - query.hpp
//
// Fluent, LINQ/Unity-DOTS style queries:
//
//   world.Query<Position, Velocity>()
//        .Where([](Position& p, Velocity& v) { return v.x > 0.f; })
//        .ForEach([](Entity e, Position& p, Velocity& v) { ... });
//
// Combinators:
//   With<Ts...>()     - require additional components (also passed to the
//                       callable, appended after the queried ones).
//   Without<Ts...>()  - require the entities to NOT have these components.
//   Optional<Ts...>() - entities may or may not have these; passed as pointers.
//   Where(predicate)  - runtime filter over the full component set. The
//                       predicate receives the same argument list as the
//                       ForEach callable and must return bool.
//
// The whole chain is composed at compile time: no type erasure, no per-entity
// virtual calls. Iteration always uses the smallest matching storage as the
// driver (classic sparse-set query optimization).

#include "core.hpp"
#include "entity.hpp"
#include "component.hpp"
#include "parallel.hpp"

#include <algorithm>
#include <limits>
#include <tuple>

namespace ekit {

class World; // forward declaration only; Query is instantiated after World.

namespace detail {

// Default predicate: when no Where() filter is present.
struct EmptyPredicate {
    template<typename... Args>
    bool operator()(Args&&...) const {
        return true;
    }
};

// True when every type in the list is a declared component.
template<typename List>
struct AllComponents;
template<typename... Ts>
struct AllComponents<TypeList<Ts...>>
    : std::bool_constant<(IsComponent<Ts>::value && ...)> {};

// True when the entity has every component in the list.
template<typename List>
struct AllPresent;
template<typename... Ts>
struct AllPresent<TypeList<Ts...>> {
    template<typename WorldT>
    static bool Check(WorldT* world, Entity e) {
        return (world->template GetStorage<Ts>().Contains(e.GetIndex()) && ...);
    }
};

// True when the entity has at least one component in the list.
template<typename List>
struct AnyPresent;
template<typename... Ts>
struct AnyPresent<TypeList<Ts...>> {
    template<typename WorldT>
    static bool Check(WorldT* world, Entity e) {
        return ((world->template GetStorage<Ts>().Contains(e.GetIndex())) || ...);
    }
};

// Iterates the smallest of the required storages and yields each matching
// entity. Presence/exclusion filters are applied by the caller's visitor.
template<typename WorldT, typename List>
struct IterateWithDriver;

template<typename WorldT, typename... Ts>
struct IterateWithDriver<WorldT, TypeList<Ts...>> {
    template<typename F>
    static void Run(WorldT* world, F&& f) {
        IterateImpl(world, MinSize(world), f, TypeList<Ts...>{});
    }

    static std::size_t MinSize(WorldT* world) {
        std::size_t m = (std::numeric_limits<std::size_t>::max)();
        ((m = (std::min)(m, world->template GetStorage<Ts>().Size())), ...);
        return m;
    }

    // Runs fn over only the [begin, end) slice of the chosen driver storage.
    template<typename F>
    static void RunParallel(WorldT* world, std::size_t begin, std::size_t end, F& f) {
        IterateImplParallel(world, MinSize(world), begin, end, f, TypeList<Ts...>{});
    }

    template<typename F, typename Driver, typename... Rest>
    static void IterateImpl(WorldT* world, std::size_t min_size, F& f, TypeList<Driver, Rest...>) {
        auto& storage = world->template GetStorage<Driver>();
        if (storage.Size() == min_size) {
            const std::size_t count = storage.Size();
            for (std::size_t i = 0; i < count; ++i) {
                const Entity e = world->GetEntity(storage.EntityAt(i));
                f(e);
            }
        } else {
            if constexpr (sizeof...(Rest) > 0) {
                IterateImpl(world, min_size, f, TypeList<Rest...>{});
            }
        }
    }

    template<typename F, typename Driver, typename... Rest>
    static void IterateImplParallel(WorldT* world, std::size_t min_size, std::size_t begin,
                                    std::size_t end, F& f, TypeList<Driver, Rest...>) {
        auto& storage = world->template GetStorage<Driver>();
        if (storage.Size() == min_size) {
            for (std::size_t i = begin; i < end; ++i) {
                const Entity e = world->GetEntity(storage.EntityAt(i));
                f(e);
            }
        } else {
            if constexpr (sizeof...(Rest) > 0) {
                IterateImplParallel(world, min_size, begin, end, f, TypeList<Rest...>{});
            }
        }
    }
};

// Invokes a callable (ForEach callback or Where predicate) with the current
// entity's components. Two signatures are supported, detected at compile time:
//   (Entity, T0&..., U0*...)   - entity first
//   (T0&..., U0*...)           - components only
// where T are required components (by reference) and U are optional components
// (as pointers, nullptr when absent).
template<typename F, typename WorldT, typename... Reqs, typename... Opts>
auto InvokeCallable(F& f, WorldT* world, Entity e, TypeList<Reqs...>, TypeList<Opts...>) {
    auto refs = std::tuple<Reqs&...>(world->template GetStorage<Reqs>().Get(e.GetIndex())...);
    auto ptrs = std::tuple<Opts*...>(world->template GetStorage<Opts>().TryGet(e.GetIndex())...);
    auto all = std::tuple_cat(std::make_tuple(e), refs, ptrs);

    if constexpr (std::is_invocable_v<F&, Entity, Reqs&..., Opts*...>) {
        return std::apply(f, all);
    } else if constexpr (std::is_invocable_v<F&, Reqs&..., Opts*...>) {
        return std::apply(
            [&](auto&&, auto&&... rest) {
                return f(std::forward<decltype(rest)>(rest)...);
            },
            all);
    } else {
        static_assert(
            AlwaysFalse<F>::value,
            "ekit: unsupported callable signature. Expected (Entity, T&..., U*...) "
            "or (T&..., U*...) where T are the queried components (by reference) "
            "and U are the optional components (as pointers).");
    }
}

} // namespace detail

template<typename WorldT, typename PredicateT, typename RequiredList,
         typename OptionalList, typename ExcludedList>
class Query {
    static_assert(detail::IsTypeList<RequiredList>::value, "ekit: internal error: RequiredList must be a TypeList.");
    static_assert(detail::IsTypeList<OptionalList>::value, "ekit: internal error: OptionalList must be a TypeList.");
    static_assert(detail::IsTypeList<ExcludedList>::value, "ekit: internal error: ExcludedList must be a TypeList.");
    static_assert(detail::AllComponents<RequiredList>::value && detail::AllComponents<OptionalList>::value &&
                      detail::AllComponents<ExcludedList>::value,
                  "ekit: every type used in a Query must be declared as a component. "
                  "Add 'EKIT_COMPONENT(T)' after declaring T (or specialize ekit::IsComponent<T>).");
    static_assert(!detail::IsEmptyList<RequiredList>::value,
                  "ekit: Query requires at least one component type. "
                  "Use world.Query<Position, Velocity>() or add With<T>() to a non-empty query.");

public:
    explicit Query(WorldT& world)
        : world_(&world) {}

    // ------------------------------------------------------------------
    // Fluent combinators. Each one returns a new Query of a different type;
    // the whole chain is compiled into a single inlined loop.
    // ------------------------------------------------------------------

    template<typename F>
    auto Where(F&& pred) const
        -> Query<WorldT, std::decay_t<F>, RequiredList, OptionalList, ExcludedList> {
        Query<WorldT, std::decay_t<F>, RequiredList, OptionalList, ExcludedList> q(*world_);
        q.predicate_ = std::forward<F>(pred);
        return q;
    }

    template<typename... Ts>
    auto With() const -> Query<WorldT, PredicateT, detail::TypeListCat_t<RequiredList, TypeList<Ts...>>,
                               OptionalList, ExcludedList> {
        using NewRequired = detail::TypeListCat_t<RequiredList, TypeList<Ts...>>;
        return Query<WorldT, PredicateT, NewRequired, OptionalList, ExcludedList>(*world_, predicate_);
    }

    template<typename... Ts>
    auto Without() const -> Query<WorldT, PredicateT, RequiredList, OptionalList,
                                  detail::TypeListCat_t<ExcludedList, TypeList<Ts...>>> {
        using NewExcluded = detail::TypeListCat_t<ExcludedList, TypeList<Ts...>>;
        return Query<WorldT, PredicateT, RequiredList, OptionalList, NewExcluded>(*world_, predicate_);
    }

    template<typename... Ts>
    auto Optional() const -> Query<WorldT, PredicateT, RequiredList,
                                   detail::TypeListCat_t<OptionalList, TypeList<Ts...>>, ExcludedList> {
        using NewOptional = detail::TypeListCat_t<OptionalList, TypeList<Ts...>>;
        return Query<WorldT, PredicateT, RequiredList, NewOptional, ExcludedList>(*world_, predicate_);
    }

    // ------------------------------------------------------------------
    // Execution
    // ------------------------------------------------------------------

    // Visits every matching entity. Internal; prefer ForEach / Count.
    template<typename Visitor>
    void Visit(Visitor&& visit) const {
        detail::IterateWithDriver<WorldT, RequiredList>::Run(world_, [&](Entity e) {
            if (!detail::AllPresent<RequiredList>::Check(world_, e)) {
                return;
            }
            if (detail::AnyPresent<ExcludedList>::Check(world_, e)) {
                return;
            }
            if constexpr (!std::is_same_v<PredicateT, detail::EmptyPredicate>) {
                if (!detail::InvokeCallable(predicate_, world_, e, RequiredList{}, OptionalList{})) {
                    return;
                }
            }
            visit(e);
        });
    }

    // Iterates all matching entities. The callback receives the queried
    // components by reference (optional components as pointers), optionally
    // preceded by the Entity handle:
    //   .ForEach([](Entity e, Position& p, Velocity& v) { ... });
    //   .ForEach([](Position& p, Velocity& v) { ... });
    template<typename F>
    void ForEach(F&& func) const {
        Visit([&](Entity e) {
            detail::InvokeCallable(func, world_, e, RequiredList{}, OptionalList{});
        });
    }

    // Like ForEach but splits the driver storage into chunks and runs them
    // concurrently on the supplied thread pool. The callback (and any Where
    // predicate) must be safe to invoke from multiple threads; in practice it
    // should only touch the entity's own components (no shared mutable state).
    template<typename F>
    void ForEachParallel(ThreadPool& pool, F&& func) const {
        const std::size_t count =
            detail::IterateWithDriver<WorldT, RequiredList>::MinSize(world_);
        if (count == 0) {
            return;
        }
        auto visit = [&](Entity e) {
            if (!detail::AllPresent<RequiredList>::Check(world_, e)) {
                return;
            }
            if (detail::AnyPresent<ExcludedList>::Check(world_, e)) {
                return;
            }
            if constexpr (!std::is_same_v<PredicateT, detail::EmptyPredicate>) {
                if (!detail::InvokeCallable(predicate_, world_, e, RequiredList{}, OptionalList{})) {
                    return;
                }
            }
            detail::InvokeCallable(func, world_, e, RequiredList{}, OptionalList{});
        };
        detail::ParallelFor(pool, count, [&](std::size_t begin, std::size_t end) {
            detail::IterateWithDriver<WorldT, RequiredList>::RunParallel(
                world_, begin, end, visit);
        });
    }

    // Alias for ForEach (for EnTT/entt-style muscle memory).
    template<typename F>
    void Each(F&& func) const {
        ForEach(std::forward<F>(func));
    }

    // Number of entities matching this query (including the Where filter).
    std::size_t Count() const {
        std::size_t count = 0;
        Visit([&](Entity) { ++count; });
        return count;
    }

private:
    Query(WorldT& world, PredicateT pred)
        : world_(&world), predicate_(std::move(pred)) {}

    template<typename, typename, typename, typename, typename>
    friend class Query;

    WorldT* world_;
    mutable PredicateT predicate_;
};

} // namespace ekit

