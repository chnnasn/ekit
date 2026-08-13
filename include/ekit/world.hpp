#pragma once
// ekit - world.hpp
//
// World - the central ECS container.
//
//   ekit::World world;
//   world.RegisterComponent<Position>();       // dense  (archetype, SoA)
//   world.RegisterSparseComponent<Velocity>(); // sparse (random access / churn)
//
//   Entity e = world.Create("player");
//   world.Add<Position>(e, 0.f, 0.f);
//   world.Add<Velocity>(e, 1.f, 0.f);
//
//   world.Query<Position, Velocity>()
//        .ForEach([](Entity e, Position& p, Velocity& v) { ... });
//
// Dense components live in archetypes (SoA columns -> fast iteration / batch /
// SIMD); sparse components live in per-type sparse sets (fast random access and
// cheap add/remove). Queries transparently mix the two.
//
// Events:
//   world.Subscribe<CollisionEvent>(handler);   // returns EventSubscription
//   world.Emit<CollisionEvent>(a, b);

#include "core.hpp"
#include "entity.hpp"
#include "component.hpp"
#include "query.hpp"

#include <any>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ekit {

// Storage backend for a component type.
enum class StorageKind { Dense, Sparse };

class World;

// ---------------------------------------------------------------------------
// Event subscriptions
// ---------------------------------------------------------------------------

class EventSubscription {
public:
    EventSubscription() = default;
    void Unsubscribe();
    bool IsValid() const {
        return world_ != nullptr && index_ != detail::kNpos;
    }
private:
    friend class World;
    EventSubscription(World* world, std::size_t event_id, std::size_t index)
        : world_(world), event_id_(event_id), index_(index) {}
    World* world_ = nullptr;
    std::size_t event_id_ = 0;
    std::size_t index_ = detail::kNpos;
};

namespace detail {
struct HandlerSlot {
    std::function<void(const void*)> fn;
    bool alive = true;
};
template<typename T, typename = void>
struct IsSystemLike : std::false_type {};
template<typename T>
struct IsSystemLike<T, std::void_t<decltype(std::declval<T&>().Execute(std::declval<World&>()))>>
    : std::true_type {};
} // namespace detail

// ---------------------------------------------------------------------------
// World
// ---------------------------------------------------------------------------

class World {
    friend class EventSubscription;

public:
    World() = default;
    World(const World&) = delete;
    World& operator=(const World&) = delete;
    World(World&&) = delete;
    World& operator=(World&&) = delete;

    // =====================================================================
    // Component registration (explicit, mandatory)
    // =====================================================================

    template<typename T>
    ComponentTypeId RegisterComponent() {
        return RegisterComponentImpl<T>(StorageKind::Dense);
    }

    // Registers T in a per-type sparse set instead of the archetype storage.
    template<typename T>
    ComponentTypeId RegisterSparseComponent() {
        return RegisterComponentImpl<T>(StorageKind::Sparse);
    }

    template<typename... Ts>
    void RegisterComponents() {
        (RegisterComponent<Ts>(), ...);
    }

    template<typename T>
    ComponentTypeId GetComponentTypeId() const {
        static_assert(IsComponent<T>::value,
                      "ekit: T is not declared as a component. Add 'EKIT_COMPONENT(T)'.");
        const auto id = ComponentTypeIdOf<T>;
        if (id == kInvalidComponentTypeId || static_cast<std::size_t>(id) >= component_infos_.size() ||
            component_infos_[id].size == 0) {
            throw EkitException("ekit: component '" + std::string(ComponentNameOf<T>()) +
                                "' is not registered in this World. Call world.RegisterComponent<" +
                                ComponentNameOf<T>() + ">() first.");
        }
        return id;
    }

    template<typename T>
    bool IsComponentRegistered() const {
        const auto id = ComponentTypeIdOf<T>;
        return id != kInvalidComponentTypeId && static_cast<std::size_t>(id) < component_infos_.size() &&
               component_infos_[id].size != 0;
    }

    // Whether T is stored in a sparse set (true) or an archetype (false).
    template<typename T>
    bool IsSparseComponent() const {
        const auto id = ComponentTypeIdOf<T>;
        return id != kInvalidComponentTypeId && static_cast<std::size_t>(id) < component_kinds_.size() &&
               component_kinds_[id] == StorageKind::Sparse;
    }

    std::size_t GetRegisteredComponentCount() const {
        return storage_count_;
    }

    const ComponentInfo& GetComponentInfo(ComponentTypeId id) const {
        return component_infos_[id];
    }

    // =====================================================================
    // Entity creation / destruction
    // =====================================================================

    Entity Create() {
        EntityId index;
        EntityGeneration generation;
        if (!free_list_.empty()) {
            index = free_list_.back();
            free_list_.pop_back();
            generation = entities_[index].GetGeneration();
            if (generation == 0) {
                index = static_cast<EntityId>(entities_.size());
                generation = 1;
                entities_.emplace_back(index, generation);
                alive_.push_back(1);
                entity_archetype_.push_back(0);
                entity_row_.push_back(0);
            } else {
                entities_[index] = Entity(index, generation);
                alive_[index] = 1;
            }
        } else {
            index = static_cast<EntityId>(entities_.size());
            generation = 1;
            entities_.emplace_back(index, generation);
            alive_.push_back(1);
            entity_archetype_.push_back(0);
            entity_row_.push_back(0);
        }
        ++alive_count_;
        PlaceInArchetype(index, EmptyArchetypeId());
        return Entity(index, generation);
    }

    Entity Create(std::string_view name) {
        Entity e = Create();
        SetName(e, name);
        return e;
    }

    void Destroy(Entity e) {
        if (!IsAlive(e)) {
            return;
        }
        const auto index = e.GetIndex();

        if (auto it = entity_to_name_.find(index); it != entity_to_name_.end()) {
            name_to_entity_.erase(it->second);
            entity_to_name_.erase(it);
        }

        EraseFromArchetype(entity_archetype_[index], entity_row_[index]);
        for (auto& storage : sparse_storages_) {
            if (storage) {
                storage->TryRemove(index);
            }
        }

        const auto generation = static_cast<EntityGeneration>(e.GetGeneration() + 1);
        entities_[index] = Entity(index, generation);
        alive_[index] = 0;
        if (generation != 0) {
            free_list_.push_back(index);
        }
        --alive_count_;
    }

    bool IsAlive(Entity e) const {
        return e.IsValid() && e.GetIndex() < entities_.size() && alive_[e.GetIndex()] != 0 &&
               entities_[e.GetIndex()] == e;
    }

    std::size_t GetAliveEntityCount() const {
        return alive_count_;
    }

    Entity GetEntity(EntityId index) const {
        return index < entities_.size() && alive_[index] != 0 ? entities_[index] : Entity::Null;
    }

    template<typename F>
    void ForEachEntity(F&& f) const {
        for (std::size_t i = 1; i < entities_.size(); ++i) {
            if (alive_[i] != 0) {
                f(entities_[i]);
            }
        }
    }

    // =====================================================================
    // Named entities
    // =====================================================================

    void SetName(Entity e, std::string_view name) {
        if (!IsAlive(e)) {
            throw EkitException("ekit: cannot name a dead entity.");
        }
        const auto index = e.GetIndex();
        if (auto it = entity_to_name_.find(index); it != entity_to_name_.end()) {
            name_to_entity_.erase(it->second);
        }
        const std::string key(name);
        if (auto it = name_to_entity_.find(key); it != name_to_entity_.end() && it->second != e) {
            entity_to_name_.erase(it->second.GetIndex());
        }
        name_to_entity_[key] = e;
        entity_to_name_[index] = key;
    }

    const std::string& GetName(Entity e) const {
        static const std::string kEmpty;
        if (auto it = entity_to_name_.find(e.GetIndex()); it != entity_to_name_.end()) {
            return it->second;
        }
        return kEmpty;
    }

    Entity Find(std::string_view name) const {
        if (auto it = name_to_entity_.find(std::string(name)); it != name_to_entity_.end()) {
            return it->second;
        }
        return Entity::Null;
    }

    // =====================================================================
    // Component access
    // =====================================================================

    template<typename T, typename... Args>
    T& Add(Entity e, Args&&... args) {
        RequireAlive(e);
        const ComponentTypeId id = GetComponentTypeId<T>();
        if (component_kinds_[id] == StorageKind::Sparse) {
            return GetSparseStorage<T>().Emplace(e.GetIndex(), std::forward<Args>(args)...);
        }
        return AddDense<T>(e, id, std::forward<Args>(args)...);
    }

    template<typename T, typename... Args>
    T& Emplace(Entity e, Args&&... args) {
        return Add<T>(e, std::forward<Args>(args)...);
    }

    template<typename T, typename... Args>
    T& Set(Entity e, Args&&... args) {
        RequireAlive(e);
        const ComponentTypeId id = GetComponentTypeId<T>();
        if (component_kinds_[id] == StorageKind::Sparse) {
            auto& storage = GetSparseStorage<T>();
            if (T* existing = storage.TryGet(e.GetIndex())) {
                *existing = T{std::forward<Args>(args)...};
                return *existing;
            }
            return storage.Emplace(e.GetIndex(), std::forward<Args>(args)...);
        }
        Archetype& a = *archetypes_[entity_archetype_[e.GetIndex()]];
        const std::size_t row = entity_row_[e.GetIndex()];
        const std::ptrdiff_t col = a.ColumnIndex(id);
        if (col >= 0) {
            T value{std::forward<Args>(args)...};
            a.Write<T>(static_cast<std::size_t>(col), row, value);
            return a.Column<T>(static_cast<std::size_t>(col))[row];
        }
        return AddDense<T>(e, id, std::forward<Args>(args)...);
    }

    template<typename T>
    bool Has(Entity e) const {
        if (!IsAlive(e)) {
            return false;
        }
        const ComponentTypeId id = GetComponentTypeId<T>();
        if (component_kinds_[id] == StorageKind::Sparse) {
            return GetSparseStorage<T>().Contains(e.GetIndex());
        }
        return archetypes_[entity_archetype_[e.GetIndex()]]->ColumnIndex(id) >= 0;
    }

    template<typename T>
    T& Get(Entity e) {
        RequireAlive(e);
        const ComponentTypeId id = GetComponentTypeId<T>();
        if (component_kinds_[id] == StorageKind::Sparse) {
            return GetSparseStorage<T>().Get(e.GetIndex());
        }
        Archetype& a = *archetypes_[entity_archetype_[e.GetIndex()]];
        const std::ptrdiff_t col = a.ColumnIndex(id);
        if (col < 0) {
            throw EkitException("ekit: component is not present on this entity.");
        }
        return a.Column<T>(static_cast<std::size_t>(col))[entity_row_[e.GetIndex()]];
    }

    template<typename T>
    const T& Get(Entity e) const {
        RequireAlive(e);
        const ComponentTypeId id = GetComponentTypeId<T>();
        if (component_kinds_[id] == StorageKind::Sparse) {
            return GetSparseStorage<T>().Get(e.GetIndex());
        }
        const Archetype& a = *archetypes_[entity_archetype_[e.GetIndex()]];
        const std::ptrdiff_t col = a.ColumnIndex(id);
        if (col < 0) {
            throw EkitException("ekit: component is not present on this entity.");
        }
        return a.Column<T>(static_cast<std::size_t>(col))[entity_row_[e.GetIndex()]];
    }

    template<typename T>
    T* TryGet(Entity e) {
        if (!IsAlive(e)) {
            return nullptr;
        }
        const ComponentTypeId id = GetComponentTypeId<T>();
        if (component_kinds_[id] == StorageKind::Sparse) {
            return GetSparseStorage<T>().TryGet(e.GetIndex());
        }
        Archetype& a = *archetypes_[entity_archetype_[e.GetIndex()]];
        const std::ptrdiff_t col = a.ColumnIndex(id);
        if (col < 0) {
            return nullptr;
        }
        return &a.Column<T>(static_cast<std::size_t>(col))[entity_row_[e.GetIndex()]];
    }

    template<typename T>
    const T* TryGet(Entity e) const {
        if (!IsAlive(e)) {
            return nullptr;
        }
        const ComponentTypeId id = GetComponentTypeId<T>();
        if (component_kinds_[id] == StorageKind::Sparse) {
            return GetSparseStorage<T>().TryGet(e.GetIndex());
        }
        const Archetype& a = *archetypes_[entity_archetype_[e.GetIndex()]];
        const std::ptrdiff_t col = a.ColumnIndex(id);
        if (col < 0) {
            return nullptr;
        }
        return &a.Column<T>(static_cast<std::size_t>(col))[entity_row_[e.GetIndex()]];
    }

    template<typename T>
    bool Remove(Entity e) {
        if (!IsAlive(e)) {
            return false;
        }
        const ComponentTypeId id = GetComponentTypeId<T>();
        if (component_kinds_[id] == StorageKind::Sparse) {
            return GetSparseStorage<T>().TryRemove(e.GetIndex());
        }
        return RemoveDense(e, id);
    }

    template<typename T, typename F>
    void Patch(Entity e, F&& fn) {
        static_assert(std::is_invocable_v<F&, T&>,
                      "ekit: Patch requires a callable accepting (T&).");
        fn(Get<T>(e));
    }

    template<typename T>
    void ClearComponent() {
        if (!IsComponentRegistered<T>()) {
            return;
        }
        const ComponentTypeId id = GetComponentTypeId<T>();
        if (component_kinds_[id] == StorageKind::Sparse) {
            GetSparseStorage<T>().Clear();
            return;
        }
        std::vector<EntityId> affected;
        for (std::size_t aid = 0; aid < archetypes_.size(); ++aid) {
            if (archetypes_[aid]->ColumnIndex(id) < 0) {
                continue;
            }
            for (EntityId idx : archetypes_[aid]->entities) {
                affected.push_back(idx);
            }
        }
        for (EntityId idx : affected) {
            RemoveDense(Entity(idx, entities_[idx].GetGeneration()), id);
        }
    }

    void ClearAll() {
        for (auto& a : archetypes_) {
            a->Clear();
        }
        for (auto& s : sparse_storages_) {
            if (s) {
                s->Clear();
            }
        }
        free_list_.clear();
        entities_.assign(1, Entity::Null);
        alive_.assign(1, 0);
        entity_archetype_.assign(1, 0);
        entity_row_.assign(1, 0);
        alive_count_ = 0;
        name_to_entity_.clear();
        entity_to_name_.clear();
    }

    // =====================================================================
    // Queries
    // =====================================================================

    template<typename... Ts>
    auto Query() -> ekit::Query<World, detail::EmptyPredicate, TypeList<Ts...>,
                                TypeList<>, TypeList<>> {
        return ekit::Query<World, detail::EmptyPredicate, TypeList<Ts...>, TypeList<>,
                           TypeList<>>(*this);
    }

    // =====================================================================
    // C#-style shortcuts
    // ---------------------------------------------------------------------
    // The fluent Query remains available for the full LINQ experience:
    //   world.Query<Position, Velocity>().With<Tag>().ForEach(...)
    // The shortcuts below cover the 90% case with a single call:
    //   world.ForEach<Position, Velocity>(...)
    //   world.Count<Position, Velocity>()
    // =====================================================================

    // Iterates every entity that has all of Ts..., passing component references
    // (optional Entity handle first). Equivalent to Query<Ts...>().ForEach(fn).
    template<typename... Ts, typename F>
    void ForEach(F&& func) {
        Query<Ts...>().ForEach(std::forward<F>(func));
    }

    // Parallel scalar iteration (chunked across the pool).
    template<typename... Ts, typename F>
    void ForEachParallel(ThreadPool& pool, F&& func) {
        Query<Ts...>().ForEachParallel(pool, std::forward<F>(func));
    }

    // Dense-only SoA batch iteration. Ts... must all be dense components.
    template<typename... Ts, typename F>
    void ForEachBatch(F&& func,
                      std::size_t batch_size = (std::numeric_limits<std::size_t>::max)()) {
        Query<Ts...>().ForEachBatch(std::forward<F>(func), batch_size);
    }

    // Parallel dense-only SoA batch iteration.
    template<typename... Ts, typename F>
    void ForEachBatchParallel(ThreadPool& pool, F&& func, std::size_t batch_size = 256) {
        Query<Ts...>().ForEachBatchParallel(pool, std::forward<F>(func), batch_size);
    }

    // Number of entities that have all of Ts....
    template<typename... Ts>
    std::size_t Count() {
        return Query<Ts...>().Count();
    }

    // =====================================================================
    // Events
    // =====================================================================

    template<typename T, typename F>
    EventSubscription Subscribe(F&& handler) {
        static_assert(std::is_invocable_v<F&, const T&>,
                      "ekit: event handlers must be callable with (const T&).");
        auto& slots = EventSlots<T>();
        slots.push_back(detail::HandlerSlot{
            [h = std::forward<F>(handler)](const void* data) { h(*static_cast<const T*>(data)); },
            true});
        return EventSubscription(this, detail::TypeIdOf<T>(), slots.size() - 1);
    }

    template<typename T, typename... Args>
    void Emit(Args&&... args) {
        T event(std::forward<Args>(args)...);
        EmitInternal(event);
    }

    template<typename T>
    void Emit(const T& event) {
        EmitInternal(event);
    }

    // =====================================================================
    // Systems
    // =====================================================================

    template<typename T>
    void RunSystem(T&& system) {
        static_assert(detail::IsSystemLike<std::decay_t<T>>::value,
                      "ekit: RunSystem requires a system object with an Execute(World&) method.");
        system.Execute(*this);
    }

    // =====================================================================
    // Storage access (queries)
    // =====================================================================

    const std::vector<std::unique_ptr<Archetype>>& Archetypes() const {
        return archetypes_;
    }

    // Sparse storages indexed by ComponentTypeId (null for dense / unregistered).
    const std::vector<std::unique_ptr<IComponentStorage>>& SparseStorages() const {
        return sparse_storages_;
    }

    // Whether a given component id uses sparse storage.
    bool IsSparseId(ComponentTypeId id) const {
        return id < component_kinds_.size() && component_kinds_[id] == StorageKind::Sparse;
    }

    // Typed access to a sparse component storage (used by queries).
    template<typename T>
    ComponentStorage<T>& GetSparseStorage() {
        const auto id = GetComponentTypeId<T>();
        return *static_cast<ComponentStorage<T>*>(sparse_storages_[id].get());
    }

    template<typename T>
    const ComponentStorage<T>& GetSparseStorage() const {
        const auto id = GetComponentTypeId<T>();
        return *static_cast<const ComponentStorage<T>*>(sparse_storages_[id].get());
    }

private:
    template<typename T>
    ComponentTypeId RegisterComponentImpl(StorageKind kind) {
        static_assert(IsComponent<T>::value,
                      "ekit: T is not declared as a component. Add 'EKIT_COMPONENT(T)' after "
                      "declaring T, or specialize ekit::IsComponent<T>.");
        static_assert(std::is_trivially_copyable_v<T>,
                      "ekit: component types must be trivially copyable (POD-like).");
        static_assert(std::is_default_constructible_v<T>,
                      "ekit: component types must be default constructible.");
        static_assert(alignof(T) <= alignof(std::max_align_t),
                      "ekit: over-aligned components are not supported.");

        auto& id = ComponentTypeIdOf<T>;
        if (id == kInvalidComponentTypeId) {
            id = detail::NextComponentTypeId().fetch_add(1);
        }
        if (static_cast<std::size_t>(id) >= component_infos_.size()) {
            component_infos_.resize(static_cast<std::size_t>(id) + 1);
            component_kinds_.resize(static_cast<std::size_t>(id) + 1);
        }
        if (component_infos_[id].size == 0) {
            component_infos_[id] = ComponentInfo{sizeof(T), alignof(T), ComponentNameOf<T>()};
            component_kinds_[id] = kind;
            ++storage_count_;
        }
        if (kind == StorageKind::Sparse) {
            if (static_cast<std::size_t>(id) >= sparse_storages_.size()) {
                sparse_storages_.resize(static_cast<std::size_t>(id) + 1);
            }
            if (!sparse_storages_[id]) {
                sparse_storages_[id] = std::make_unique<ComponentStorage<T>>();
            }
        }
        return id;
    }

    // Add a DENSE component (moves the entity between archetypes).
    template<typename T, typename... Args>
    T& AddDense(Entity e, ComponentTypeId id, Args&&... args) {
        const EntityId index = e.GetIndex();
        const std::size_t from_aid = entity_archetype_[index];
        const std::size_t from_row = entity_row_[index];
        Archetype& from = *archetypes_[from_aid];
        if (from.ColumnIndex(id) >= 0) {
            throw EkitException("ekit: component already present on this entity.");
        }

        std::vector<ComponentTypeId> target_types = from.types;
        target_types.push_back(id);
        std::sort(target_types.begin(), target_types.end());
        const std::size_t to_aid = GetOrCreateArchetype(target_types);
        Archetype& to = *archetypes_[to_aid];

        to.PushRow(index);
        const std::size_t to_row = to.RowCount() - 1;
        for (std::size_t k = 0; k < from.types.size(); ++k) {
            const ComponentTypeId cid = from.types[k];
            const std::ptrdiff_t to_k = to.ColumnIndex(cid);
            std::memcpy(to.columns[to_k].data() + to_row * from.sizes[k],
                        from.columns[k].data() + from_row * from.sizes[k], from.sizes[k]);
        }
        T value{std::forward<Args>(args)...};
        const std::ptrdiff_t new_k = to.ColumnIndex(id);
        to.Write<T>(static_cast<std::size_t>(new_k), to_row, value);

        EraseFromArchetype(from_aid, from_row);
        entity_archetype_[index] = to_aid;
        entity_row_[index] = to_row;
        return to.Column<T>(static_cast<std::size_t>(new_k))[to_row];
    }

    bool RemoveDense(Entity e, ComponentTypeId id) {
        const EntityId index = e.GetIndex();
        const std::size_t from_aid = entity_archetype_[index];
        const std::size_t from_row = entity_row_[index];
        Archetype& from = *archetypes_[from_aid];
        if (from.ColumnIndex(id) < 0) {
            return false;
        }

        std::vector<ComponentTypeId> target_types;
        target_types.reserve(from.types.size());
        for (ComponentTypeId t : from.types) {
            if (t != id) {
                target_types.push_back(t);
            }
        }
        const std::size_t to_aid = GetOrCreateArchetype(target_types);
        Archetype& to = *archetypes_[to_aid];

        to.PushRow(index);
        const std::size_t to_row = to.RowCount() - 1;
        for (std::size_t k = 0; k < from.types.size(); ++k) {
            if (from.types[k] == id) {
                continue;
            }
            const std::ptrdiff_t to_k = to.ColumnIndex(from.types[k]);
            std::memcpy(to.columns[to_k].data() + to_row * from.sizes[k],
                        from.columns[k].data() + from_row * from.sizes[k], from.sizes[k]);
        }

        EraseFromArchetype(from_aid, from_row);
        entity_archetype_[index] = to_aid;
        entity_row_[index] = to_row;
        return true;
    }

    std::size_t EmptyArchetypeId() {
        return GetOrCreateArchetype({});
    }

    std::size_t GetOrCreateArchetype(const std::vector<ComponentTypeId>& types) {
        auto it = archetype_index_.find(types);
        if (it != archetype_index_.end()) {
            return it->second;
        }
        auto a = std::make_unique<Archetype>();
        a->types = types;
        a->BuildColumnLookup();
        a->sizes.reserve(types.size());
        for (ComponentTypeId id : types) {
            a->sizes.push_back(component_infos_[id].size);
        }
        a->columns.resize(types.size());
        const std::size_t id = archetypes_.size();
        archetypes_.push_back(std::move(a));
        archetype_index_.emplace(types, id);
        return id;
    }

    void PlaceInArchetype(EntityId index, std::size_t aid) {
        Archetype& a = *archetypes_[aid];
        a.PushRow(index);
        entity_archetype_[index] = aid;
        entity_row_[index] = a.RowCount() - 1;
    }

    void EraseFromArchetype(std::size_t aid, std::size_t row) {
        Archetype& a = *archetypes_[aid];
        a.RemoveRow(row);
        if (row < a.RowCount()) {
            const EntityId moved = a.entities[row];
            entity_archetype_[moved] = aid;
            entity_row_[moved] = row;
        }
    }

    void RequireAlive(Entity e) const {
        if (!IsAlive(e)) {
            throw EkitException("ekit: invalid or dead entity passed to World operation.");
        }
    }

    void Unsubscribe(std::size_t event_id, std::size_t index) {
        auto it = event_sinks_.find(event_id);
        if (it == event_sinks_.end()) {
            return;
        }
        auto& slots = std::any_cast<std::vector<detail::HandlerSlot>&>(it->second);
        if (index < slots.size()) {
            slots[index].alive = false;
        }
    }

    template<typename T>
    std::vector<detail::HandlerSlot>& EventSlots() {
        const std::size_t id = detail::TypeIdOf<T>();
        auto [it, inserted] = event_sinks_.emplace(id, std::vector<detail::HandlerSlot>{});
        (void)inserted;
        return std::any_cast<std::vector<detail::HandlerSlot>&>(it->second);
    }

    template<typename T>
    void EmitInternal(const T& event) {
        auto it = event_sinks_.find(detail::TypeIdOf<T>());
        if (it == event_sinks_.end()) {
            return;
        }
        auto& slots = std::any_cast<std::vector<detail::HandlerSlot>&>(it->second);
        for (std::size_t i = 0; i < slots.size(); ++i) {
            if (slots[i].alive) {
                slots[i].fn(&event);
            }
        }
    }

    std::vector<ComponentInfo> component_infos_{ComponentInfo{}};
    std::vector<StorageKind> component_kinds_{StorageKind::Dense};
    std::size_t storage_count_ = 0;

    std::vector<std::unique_ptr<Archetype>> archetypes_;
    std::map<std::vector<ComponentTypeId>, std::size_t> archetype_index_;
    std::vector<std::unique_ptr<IComponentStorage>> sparse_storages_;

    std::vector<Entity> entities_{Entity::Null};
    std::vector<std::uint8_t> alive_{0};
    std::vector<EntityId> free_list_;
    std::size_t alive_count_ = 0;

    std::vector<std::size_t> entity_archetype_{0};
    std::vector<std::size_t> entity_row_{0};

    std::unordered_map<std::string, Entity> name_to_entity_;
    std::unordered_map<EntityId, std::string> entity_to_name_;

    std::unordered_map<std::size_t, std::any> event_sinks_;
};

inline void EventSubscription::Unsubscribe() {
    if (world_) {
        world_->Unsubscribe(event_id_, index_);
    }
    world_ = nullptr;
    index_ = detail::kNpos;
}

} // namespace ekit
