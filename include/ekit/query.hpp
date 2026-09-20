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
// sets). Queries with required sparse components iterate the smallest required
// sparse pool; other queries iterate matching archetypes. Bindings are cached
// on the query object and refreshed when storage metadata changes.
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

// Cache stable storage objects and column indices, never relocatable column data.
template<typename T>
struct ComponentBinding {
    ComponentStorage<T>* sparse = nullptr;
    std::vector<std::ptrdiff_t> columns;

    template<typename WorldT>
    void Bind(WorldT* world) {
        const auto id = world->template GetComponentTypeId<T>();
        sparse = world->IsSparseId(id) ? &world->template GetSparseStorage<T>() : nullptr;
        columns.clear();
        for (const auto& a : world->Archetypes()) columns.push_back(a->ColumnIndex(id));
    }

    T* Get(EntityId index, Archetype& a, std::size_t aid, std::size_t row) const {
        if (sparse) return sparse->TryGet(index);
        const auto col = columns[aid];
        return col < 0 ? nullptr : &a.Column<T>(static_cast<std::size_t>(col))[row];
    }
};

template<typename List> struct BoundComponents;
template<typename... Ts>
struct BoundComponents<TypeList<Ts...>> {
    std::tuple<ComponentBinding<Ts>...> bindings;

    template<typename WorldT>
    void Bind(WorldT* world) {
        std::apply([&](auto&... b) { (b.Bind(world), ...); }, bindings);
    }
    auto Get(EntityId index, Archetype& a, std::size_t aid, std::size_t row) const {
        return std::apply([&](const auto&... b) {
            return std::tuple<Ts*...>{b.Get(index, a, aid, row)...};
        }, bindings);
    }
    // Resolve column bases per execution/archetype, never across structural changes.
    auto DenseColumns(Archetype& a, std::size_t aid) const {
        return std::apply([&](const auto&... b) {
            return std::tuple<Ts*...>{
                (b.columns[aid] < 0 ? nullptr : a.Column<Ts>(static_cast<std::size_t>(b.columns[aid])))...};
        }, bindings);
    }

    template<bool Optional>
    static auto DenseAt(const std::tuple<Ts*...>& bases, std::size_t row) {
        return std::apply([&](auto*... p) {
            if constexpr (Optional) return std::tuple<Ts*...>{(p ? p + row : nullptr)...};
            else return std::tuple<Ts*...>{(p + row)...};
        }, bases);
    }

    template<typename Driver>
    auto SparseAt(EntityId index, std::size_t row) const {
        return std::apply([&](const auto&... b) {
            return std::tuple<Ts*...>{SparsePointer<Ts, Driver>(b, index, row)...};
        }, bindings);
    }

    template<typename T, typename Driver>
    static T* SparsePointer(const ComponentBinding<T>& b, EntityId index, std::size_t row) {
        if constexpr (std::is_same_v<T, Driver>) return &b.sparse->ComponentAt(row);
        else return b.sparse->TryGet(index);
    }

    template<typename F>
    void DispatchSmallest(F&& fn) const {
        const auto* driver = Smallest();
        bool dispatched = false;
        std::apply([&](const auto&... b) {
            auto dispatch = [&](const auto& binding) {
                if (!dispatched && binding.sparse == driver) {
                    dispatched = true;
                    fn(*binding.sparse);
                }
            };
            (dispatch(b), ...);
        }, bindings);
    }

    const IComponentStorage* Smallest() const {
        const IComponentStorage* result = nullptr;
        std::apply([&](const auto&... b) {
            auto consider = [&](const auto& binding) {
                if (binding.sparse && (!result || binding.sparse->Size() < result->Size()))
                    result = binding.sparse;
            };
            (consider(b), ...);
        }, bindings);
        return result;
    }
};

template<typename F, typename... Reqs, typename... Opts>
decltype(auto) InvokeBound(F& f, Entity e, const std::tuple<Reqs*...>& req,
                           const std::tuple<Opts*...>& opt) {
    return std::apply([&](auto*... r) -> decltype(auto) {
        return std::apply([&](auto*... o) -> decltype(auto) {
            if constexpr (std::is_invocable_v<F&, Entity, Reqs&..., Opts*...>) {
                return f(e, *r..., o...);
            } else if constexpr (std::is_invocable_v<F&, Reqs&..., Opts*...>) {
                return f(*r..., o...);
            } else {
                static_assert(AlwaysFalse<F>::value,
                    "ekit: expected callable (Entity, T&..., U*...) or (T&..., U*...).");
            }
        }, opt);
    }, req);
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

    bool QueryHasSparse() const {
        Prepare();
        return has_sparse_;
    }

    // Candidates before dense/sparse filters and Where. Read current pool sizes
    // even when bindings are cached.
    std::size_t CandidateCount() const {
        Prepare();
        if (const auto* driver = required_.Smallest()) return driver->Size();
        std::size_t count = 0;
        for (std::size_t aid = 0; aid < matches_.size(); ++aid)
            if (matches_[aid]) count += world_->Archetypes()[aid]->RowCount();
        return count;
    }

    template<bool Sparse, typename Visitor>
    void Visit(Visitor&& visit) const {
        Execute(nullptr, [&](Entity e, const auto&, const auto&) {
            const auto index = e.GetIndex();
            visit(e, *world_->Archetypes()[world_->EntityArchetype(index)], world_->EntityRow(index));
        });
    }

    template<typename F>
    void ForEach(F&& func) const {
        Execute(nullptr, [&](Entity e, const auto& req, const auto& opt) {
            detail::InvokeBound(func, e, req, opt);
        });
    }

    template<typename F>
    void Each(F&& func) const { ForEach(std::forward<F>(func)); }

    template<typename F>
    void ForEachParallel(ThreadPool& pool, F&& func) const {
        Execute(&pool, [&](Entity e, const auto& req, const auto& opt) {
            detail::InvokeBound(func, e, req, opt);
        });
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
        Execute(nullptr, [&](Entity, const auto&, const auto&) { ++count; });
        return count;
    }

private:
    void Prepare() const {
        if (cache_version_ == world_->StorageVersion()) return;
        std::vector<ComponentTypeId> req, opt, exc, dense_req, sparse_req, dense_exc, sparse_exc;
        detail::ResolveIds<WorldT, RequiredList>::Into(world_, req);
        detail::ResolveIds<WorldT, OptionalList>::Into(world_, opt);
        detail::ResolveIds<WorldT, ExcludedList>::Into(world_, exc);
        detail::PartitionIds(world_, req, dense_req, sparse_req);
        detail::PartitionIds(world_, exc, dense_exc, sparse_exc);
        has_sparse_ = !sparse_req.empty() || !sparse_exc.empty();
        all_sparse_ = dense_req.empty() && dense_exc.empty();
        for (auto id : opt) {
            has_sparse_ = has_sparse_ || world_->IsSparseId(id);
            all_sparse_ = all_sparse_ && world_->IsSparseId(id);
        }
        required_.Bind(world_);
        optional_.Bind(world_);
        excluded_.Bind(world_);
        matches_.clear();
        for (const auto& a : world_->Archetypes()) {
            matches_.push_back(detail::ArchetypeContainsAll(*a, dense_req) &&
                               !detail::ArchetypeContainsAny(*a, dense_exc));
        }
        cache_version_ = world_->StorageVersion();
    }

    template<typename Required, typename Optional, typename Visitor>
    void Emit(EntityId index, const Required& req, const Optional& opt, Visitor& visit) const {
        const auto e = world_->GetEntity(index);
        if constexpr (!std::is_same_v<PredicateT, detail::EmptyPredicate>) {
            if (!detail::InvokeBound(predicate_, e, req, opt)) return;
        }
        visit(e, req, opt);
    }

    template<typename Visitor>
    void Execute(ThreadPool* pool, Visitor&& visit) const {
        Prepare();
        auto run = [&](std::size_t count, auto&& body) {
            if (pool) {
                detail::ParallelFor(*pool, count, [&](std::size_t begin, std::size_t end) {
                    for (auto i = begin; i < end; ++i) body(i);
                });
            } else {
                for (std::size_t i = 0; i < count; ++i) body(i);
            }
        };
        if (!has_sparse_) {
            // Dense membership is already known from the archetype signature.
            for (std::size_t aid = 0; aid < matches_.size(); ++aid) {
                if (!matches_[aid]) continue;
                auto& a = *world_->Archetypes()[aid];
                const auto req_bases = required_.DenseColumns(a, aid);
                const auto opt_bases = optional_.DenseColumns(a, aid);
                run(a.RowCount(), [&](std::size_t row) {
                    auto req = required_.template DenseAt<false>(req_bases, row);
                    auto opt = optional_.template DenseAt<true>(opt_bases, row);
                    Emit(a.entities[row], req, opt, visit);
                });
            }
            return;
        }
        if (all_sparse_) {
            // Dispatch once by driver type: its own component needs no sparse lookup.
            required_.DispatchSmallest([&]<typename T>(ComponentStorage<T>& driver) {
                const auto& entities = driver.Entities();
                run(entities.size(), [&](std::size_t row) {
                    const auto index = entities[row];
                    auto req = required_.template SparseAt<T>(index, row);
                    if (!std::apply([](auto*... p) { return ((p != nullptr) && ...); }, req)) return;
                    auto exc = excluded_.template SparseAt<T>(index, row);
                    if (std::apply([](auto*... p) { return ((p != nullptr) || ...); }, exc)) return;
                    auto opt = optional_.template SparseAt<T>(index, row);
                    Emit(index, req, opt, visit);
                });
            });
            return;
        }
        auto row_visit = [&](EntityId index, std::size_t aid, std::size_t row) {
            if (!matches_[aid]) return;
            auto& a = *world_->Archetypes()[aid];
            auto req = required_.Get(index, a, aid, row);
            if (!std::apply([](auto*... p) { return ((p != nullptr) && ...); }, req)) return;
            auto exc = excluded_.Get(index, a, aid, row);
            if (std::apply([](auto*... p) { return ((p != nullptr) || ...); }, exc)) return;
            auto opt = optional_.Get(index, a, aid, row);
            Emit(index, req, opt, visit);
        };
        if (const auto* driver = required_.Smallest()) {
            const auto& entities = driver->Entities();
            run(entities.size(), [&](std::size_t i) {
                const auto index = entities[i];
                row_visit(index, world_->EntityArchetype(index), world_->EntityRow(index));
            });
        } else {
            for (std::size_t aid = 0; aid < matches_.size(); ++aid) {
                if (!matches_[aid]) continue;
                auto& a = *world_->Archetypes()[aid];
                run(a.RowCount(), [&](std::size_t row) { row_visit(a.entities[row], aid, row); });
            }
        }
    }

    mutable std::size_t cache_version_ = (std::numeric_limits<std::size_t>::max)();
    mutable bool has_sparse_ = false;
    mutable bool all_sparse_ = false;
    mutable detail::BoundComponents<RequiredList> required_;
    mutable detail::BoundComponents<OptionalList> optional_;
    mutable detail::BoundComponents<ExcludedList> excluded_;
    mutable std::vector<bool> matches_;

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
