#pragma once
// ekit - query.hpp
//
// Fluent, LINQ/Unity-DOTS style queries:
//
//   world.Query<Position, Velocity>()
//        .Where([](Position& p, Velocity& v) { return v.x > 0.f; })
//        .ForEach([](Entity e, Position& p, Velocity& v) { ... });
//
// Components may be dense (archetype, SoA columns) or sparse (per-type sparse
// sets). A query mixes both transparently: dense components are matched and
// iterated through archetypes, sparse components are fetched per entity.
//
// Combinators:
//   With<Ts...>()     - require additional components (also passed to the
//                       callable, appended after the queried ones).
//   Without<Ts...>()  - require the entities to NOT have these components.
//   Optional<Ts...>() - entities may or may not have these; passed as pointers.
//   Where(predicate)  - runtime filter over the full component set.
//
// The whole chain is composed at compile time (no type erasure). ForEachBatch
// requires every queried component to be dense (aligned SoA); sparse components
// should be accessed inside the callback via world.TryGet/Get.

#include "core.hpp"
#include "entity.hpp"
#include "component.hpp"
#include "parallel.hpp"

#include <algorithm>
#include <limits>
#include <tuple>
#include <vector>

namespace ekit {

class World; // forward declaration only; Query is instantiated after World.

namespace detail {

struct EmptyPredicate {
    template<typename... Args>
    bool operator()(Args&&...) const {
        return true;
    }
};

// Combines two predicates with logical AND. This lets `Where` chains compose
// instead of silently keeping only the last filter, and it stores lambdas by
// value so capturing lambdas do not need to be default-constructible.
template<typename A, typename B>
struct AndPredicate {
    A first;
    B second;

    AndPredicate(A a, B b)
        : first(std::move(a)), second(std::move(b)) {}

    template<typename... Args>
        requires (std::is_invocable_v<A&, Args...> && std::is_invocable_v<B&, Args...>)
    bool operator()(Args&&... args) {
        return first(std::forward<Args>(args)...) && second(std::forward<Args>(args)...);
    }
};

template<typename List>
struct AllComponents;
template<typename... Ts>
struct AllComponents<TypeList<Ts...>>
    : std::bool_constant<(IsComponent<Ts>::value && ...)> {};

template<typename WorldT, typename List>
struct ResolveIds;
template<typename WorldT, typename... Ts>
struct ResolveIds<WorldT, TypeList<Ts...>> {
    static void Into(WorldT* world, std::vector<ComponentTypeId>& out) {
        (out.push_back(world->template GetComponentTypeId<Ts>()), ...);
    }
};

inline bool ArchetypeContainsAll(const Archetype& a, const std::vector<ComponentTypeId>& ids) {
    for (ComponentTypeId id : ids) {
        if (a.ColumnIndex(id) < 0) {
            return false;
        }
    }
    return true;
}

inline bool ArchetypeContainsAny(const Archetype& a, const std::vector<ComponentTypeId>& ids) {
    for (ComponentTypeId id : ids) {
        if (a.ColumnIndex(id) >= 0) {
            return true;
        }
    }
    return false;
}

// Splits ids into dense (stored in archetypes) and sparse (stored in sparse sets).
template<typename WorldT>
void PartitionIds(WorldT* world, const std::vector<ComponentTypeId>& ids,
                  std::vector<ComponentTypeId>& dense, std::vector<ComponentTypeId>& sparse) {
    for (ComponentTypeId id : ids) {
        (world->IsSparseId(id) ? sparse : dense).push_back(id);
    }
}

inline bool SparseContains(const IComponentStorage* storage, EntityId index) {
    return storage != nullptr && storage->Contains(index);
}

template<typename WorldT>
bool AllSparsePresent(WorldT* world, const std::vector<ComponentTypeId>& ids, EntityId index) {
    for (ComponentTypeId id : ids) {
        const auto& storage = world->SparseStorages()[id];
        if (!storage || !SparseContains(storage.get(), index)) {
            return false;
        }
    }
    return true;
}

template<typename WorldT>
bool AnySparsePresent(WorldT* world, const std::vector<ComponentTypeId>& ids, EntityId index) {
    for (ComponentTypeId id : ids) {
        const auto& storage = world->SparseStorages()[id];
        if (storage && SparseContains(storage.get(), index)) {
            return true;
        }
    }
    return false;
}

// Reference to a required component. When Sparse == false (dense-only query),
// this compiles down to a direct column access with no per-entity branch.
template<bool Sparse, typename T, typename WorldT>
T& ReqAt(WorldT* world, Archetype& a, std::size_t row, ComponentTypeId id) {
    if constexpr (Sparse) {
        if (world->IsSparseId(id)) {
            return world->template GetSparseStorage<T>().Get(a.entities[row]);
        }
    }
    return a.Column<T>(static_cast<std::size_t>(a.ColumnIndex(id)))[row];
}

template<bool Sparse, typename T, typename WorldT>
T* OptAt(WorldT* world, Archetype& a, std::size_t row, ComponentTypeId id) {
    if constexpr (Sparse) {
        if (world->IsSparseId(id)) {
            return world->template GetSparseStorage<T>().TryGet(a.entities[row]);
        }
    }
    const std::ptrdiff_t col = a.ColumnIndex(id);
    return col < 0 ? nullptr : &a.Column<T>(static_cast<std::size_t>(col))[row];
}

template<bool Sparse, typename F, typename WorldT, typename... Reqs, typename... Opts>
auto InvokeArchetype(F& f, Entity e, WorldT* world, Archetype& a, std::size_t row,
                     TypeList<Reqs...>, TypeList<Opts...>) {
    auto refs = std::tuple<Reqs&...>(ReqAt<Sparse, Reqs>(world, a, row, ComponentTypeIdOf<Reqs>)...);
    auto ptrs = std::tuple<Opts*...>(OptAt<Sparse, Opts>(world, a, row, ComponentTypeIdOf<Opts>)...);
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

// Invokes a dense-only batch callable with raw SoA component pointers.
template<typename F, typename... Reqs, typename... Opts>
void InvokeBatch(F& f, Archetype& a, std::size_t base, std::size_t count,
                 TypeList<Reqs...>, TypeList<Opts...>) {
    auto req_ptrs = std::tuple<Reqs*...>(
        a.Column<Reqs>(static_cast<std::size_t>(a.ColumnIndex(ComponentTypeIdOf<Reqs>))) + base...);
    auto opt_ptrs = std::tuple<Opts*...>(
        (a.ColumnIndex(ComponentTypeIdOf<Opts>) >= 0
             ? a.Column<Opts>(static_cast<std::size_t>(a.ColumnIndex(ComponentTypeIdOf<Opts>))) + base
             : nullptr)...);

    if constexpr (std::is_invocable_v<F&, EntityId*, Reqs*..., Opts*..., std::size_t>) {
        auto args = std::tuple_cat(std::make_tuple(a.entities.data() + base),
                                   req_ptrs, opt_ptrs, std::make_tuple(count));
        std::apply(f, args);
    } else if constexpr (std::is_invocable_v<F&, Reqs*..., Opts*..., std::size_t>) {
        auto args = std::tuple_cat(req_ptrs, opt_ptrs, std::make_tuple(count));
        std::apply(f, args);
    } else {
        static_assert(
            AlwaysFalse<F>::value,
            "ekit: unsupported batch callable signature. Expected "
            "(EntityId*, T0*, ..., U0*, ..., std::size_t) or (T0*, ..., U0*, ..., std::size_t).");
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

    template<typename F>
    auto Where(F&& pred) const
        -> Query<WorldT, detail::AndPredicate<PredicateT, std::decay_t<F>>,
                 RequiredList, OptionalList, ExcludedList> {
        using NewPredicate = detail::AndPredicate<PredicateT, std::decay_t<F>>;
        PredicateT previous = [&] {
            if constexpr (std::is_copy_constructible_v<PredicateT>) {
                return predicate_;
            } else {
                return std::move(predicate_);
            }
        }();
        return Query<WorldT, NewPredicate, RequiredList, OptionalList, ExcludedList>(
            *world_, NewPredicate{std::move(previous), std::forward<F>(pred)});
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

    // True when any queried component (required/optional/excluded) is sparse.
    bool QueryHasSparse() const {
        auto has_sparse = [&](const std::vector<ComponentTypeId>& ids) {
            for (ComponentTypeId id : ids) {
                if (world_->IsSparseId(id)) {
                    return true;
                }
            }
            return false;
        };
        std::vector<ComponentTypeId> r, o, e;
        detail::ResolveIds<WorldT, RequiredList>::Into(world_, r);
        detail::ResolveIds<WorldT, OptionalList>::Into(world_, o);
        detail::ResolveIds<WorldT, ExcludedList>::Into(world_, e);
        return has_sparse(r) || has_sparse(o) || has_sparse(e);
    }

    // Visits every matching (archetype, row). Sparse == false takes the dense-only
    // fast path with no per-entity sparse checks.
    template<bool Sparse, typename Visitor>
    void Visit(Visitor&& visit) const {
        std::vector<ComponentTypeId> req_ids, opt_ids, exc_ids;
        detail::ResolveIds<WorldT, RequiredList>::Into(world_, req_ids);
        detail::ResolveIds<WorldT, OptionalList>::Into(world_, opt_ids);
        detail::ResolveIds<WorldT, ExcludedList>::Into(world_, exc_ids);

        std::vector<ComponentTypeId> dense_req, sparse_req, dense_exc, sparse_exc;
        detail::PartitionIds(world_, req_ids, dense_req, sparse_req);
        detail::PartitionIds(world_, exc_ids, dense_exc, sparse_exc);

        for (const auto& aptr : world_->Archetypes()) {
            Archetype& a = *aptr;
            if (!detail::ArchetypeContainsAll(a, dense_req)) {
                continue;
            }
            if (detail::ArchetypeContainsAny(a, dense_exc)) {
                continue;
            }
            const std::size_t n = a.RowCount();
            for (std::size_t row = 0; row < n; ++row) {
                const EntityId idx = a.entities[row];
                if constexpr (Sparse) {
                    if (!detail::AllSparsePresent(world_, sparse_req, idx)) {
                        continue;
                    }
                    if (detail::AnySparsePresent(world_, sparse_exc, idx)) {
                        continue;
                    }
                }
                const Entity e = world_->GetEntity(idx);
                if constexpr (!std::is_same_v<PredicateT, detail::EmptyPredicate>) {
                    if (!detail::InvokeArchetype<Sparse>(predicate_, e, world_, a, row, RequiredList{}, OptionalList{})) {
                        continue;
                    }
                }
                visit(e, a, row);
            }
        }
    }

    // Iterates all matching entities; callback receives component references
    // (optional components as pointers), optionally preceded by the Entity handle.
    template<typename F>
    void ForEach(F&& func) const {
        const bool sparse = QueryHasSparse();
        auto body = [&](Entity e, Archetype& a, std::size_t row) {
            if (sparse) {
                detail::InvokeArchetype<true>(func, e, world_, a, row, RequiredList{}, OptionalList{});
            } else {
                detail::InvokeArchetype<false>(func, e, world_, a, row, RequiredList{}, OptionalList{});
            }
        };
        if (sparse) {
            Visit<true>(body);
        } else {
            Visit<false>(body);
        }
    }

    template<typename F>
    void Each(F&& func) const {
        ForEach(std::forward<F>(func));
    }

    // Parallel scalar iteration (chunked across the pool); supports mixed
    // dense + sparse queries.
    template<typename F>
    void ForEachParallel(ThreadPool& pool, F&& func) const {
        const bool sparse = QueryHasSparse();

        std::vector<ComponentTypeId> req_ids, opt_ids, exc_ids;
        detail::ResolveIds<WorldT, RequiredList>::Into(world_, req_ids);
        detail::ResolveIds<WorldT, OptionalList>::Into(world_, opt_ids);
        detail::ResolveIds<WorldT, ExcludedList>::Into(world_, exc_ids);

        std::vector<ComponentTypeId> dense_req, sparse_req, dense_exc, sparse_exc;
        detail::PartitionIds(world_, req_ids, dense_req, sparse_req);
        detail::PartitionIds(world_, exc_ids, dense_exc, sparse_exc);

        auto run = [&](const Archetype& a) {
            const std::size_t n = a.RowCount();
            detail::ParallelFor(pool, n, [&](std::size_t begin, std::size_t end) {
                for (std::size_t row = begin; row < end; ++row) {
                    const EntityId idx = a.entities[row];
                    if (sparse) {
                        if (!detail::AllSparsePresent(world_, sparse_req, idx)) continue;
                        if (detail::AnySparsePresent(world_, sparse_exc, idx)) continue;
                    }
                    const Entity e = world_->GetEntity(idx);
                    if constexpr (!std::is_same_v<PredicateT, detail::EmptyPredicate>) {
                        bool keep;
                        if (sparse) {
                            keep = detail::InvokeArchetype<true>(predicate_, e, world_, const_cast<Archetype&>(a), row, RequiredList{}, OptionalList{});
                        } else {
                            keep = detail::InvokeArchetype<false>(predicate_, e, world_, const_cast<Archetype&>(a), row, RequiredList{}, OptionalList{});
                        }
                        if (!keep) continue;
                    }
                    if (sparse) {
                        detail::InvokeArchetype<true>(func, e, world_, const_cast<Archetype&>(a), row, RequiredList{}, OptionalList{});
                    } else {
                        detail::InvokeArchetype<false>(func, e, world_, const_cast<Archetype&>(a), row, RequiredList{}, OptionalList{});
                    }
                }
            });
        };

        for (const auto& aptr : world_->Archetypes()) {
            const Archetype& a = *aptr;
            if (!detail::ArchetypeContainsAll(a, dense_req)) continue;
            if (detail::ArchetypeContainsAny(a, dense_exc)) continue;
            run(a);
        }
    }

    // Dense-only SoA batch iteration. Throws when a queried component is sparse.
    template<typename F>
    void ForEachBatch(F&& func, std::size_t batch_size = (std::numeric_limits<std::size_t>::max)()) const {
        RequireAllDense();
        std::vector<ComponentTypeId> req_ids, exc_ids;
        detail::ResolveIds<WorldT, RequiredList>::Into(world_, req_ids);
        detail::ResolveIds<WorldT, ExcludedList>::Into(world_, exc_ids);

        for (const auto& aptr : world_->Archetypes()) {
            Archetype& a = *aptr;
            if (!detail::ArchetypeContainsAll(a, req_ids)) continue;
            if (detail::ArchetypeContainsAny(a, exc_ids)) continue;
            const std::size_t n = a.RowCount();
            for (std::size_t base = 0; base < n; base += batch_size) {
                const std::size_t count = (std::min)(batch_size, n - base);
                detail::InvokeBatch(func, a, base, count, RequiredList{}, OptionalList{});
            }
        }
    }

    template<typename F>
    void ForEachBatchParallel(ThreadPool& pool, F&& func, std::size_t batch_size = 256) const {
        RequireAllDense();
        std::vector<ComponentTypeId> req_ids, exc_ids;
        detail::ResolveIds<WorldT, RequiredList>::Into(world_, req_ids);
        detail::ResolveIds<WorldT, ExcludedList>::Into(world_, exc_ids);

        for (const auto& aptr : world_->Archetypes()) {
            Archetype& a = *aptr;
            if (!detail::ArchetypeContainsAll(a, req_ids)) continue;
            if (detail::ArchetypeContainsAny(a, exc_ids)) continue;
            const std::size_t n = a.RowCount();
            const std::size_t num_batches = (n + batch_size - 1) / batch_size;
            detail::ParallelFor(pool, num_batches, [&](std::size_t b0, std::size_t b1) {
                for (std::size_t bi = b0; bi < b1; ++bi) {
                    const std::size_t base = bi * batch_size;
                    const std::size_t count = (std::min)(batch_size, n - base);
                    detail::InvokeBatch(func, a, base, count, RequiredList{}, OptionalList{});
                }
            });
        }
    }

    std::size_t Count() const {
        std::size_t count = 0;
        if (QueryHasSparse()) {
            Visit<true>([&](Entity, Archetype&, std::size_t) { ++count; });
        } else {
            Visit<false>([&](Entity, Archetype&, std::size_t) { ++count; });
        }
        return count;
    }

private:
    void RequireAllDense() const {
        bool ok = true;
        auto check = [&](ComponentTypeId id) { if (world_->IsSparseId(id)) ok = false; };
        {
            std::vector<ComponentTypeId> ids;
            detail::ResolveIds<WorldT, RequiredList>::Into(world_, ids);
            for (auto id : ids) check(id);
        }
        {
            std::vector<ComponentTypeId> ids;
            detail::ResolveIds<WorldT, OptionalList>::Into(world_, ids);
            for (auto id : ids) check(id);
        }
        {
            std::vector<ComponentTypeId> ids;
            detail::ResolveIds<WorldT, ExcludedList>::Into(world_, ids);
            for (auto id : ids) check(id);
        }
        if (!ok) {
            throw EkitException(
                "ekit: ForEachBatch requires every queried component (required, optional "
                "and excluded) to be dense (archetype SoA). Access sparse components inside "
                "the callback via world.TryGet/Get instead, or use ForEach for sparse filters.");
        }
    }

    Query(WorldT& world, PredicateT pred)
        : world_(&world), predicate_(std::move(pred)) {}

    template<typename, typename, typename, typename, typename>
    friend class Query;

    WorldT* world_;
    mutable PredicateT predicate_;
};

} // namespace ekit
