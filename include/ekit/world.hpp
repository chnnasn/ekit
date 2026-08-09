#pragma once
// ekit - world.hpp
//
// World - the central ECS container.
//
//   ekit::World world;
//   world.RegisterComponent<Position>();
//   world.RegisterComponent<Velocity>();
//
//   Entity e = world.Create("player");
//   world.Add<Position>(e, 0.f, 0.f);
//   world.Add<Velocity>(e, 1.f, 0.f);
//
//   world.Query<Position, Velocity>()
//        .ForEach([](Entity e, Position& p, Velocity& v) { ... });
//
// Events:
//   world.Subscribe<CollisionEvent>(handler);   // returns EventSubscription
//   world.Emit<CollisionEvent>(a, b);
//
// Explicit registration is mandatory: using an undeclared or unregistered
// component produces a clear compile-time or runtime error instead of magic.

#include "core.hpp"
#include "entity.hpp"
#include "component.hpp"
#include "query.hpp"

#include <any>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ekit {

class World;

// ---------------------------------------------------------------------------
// Event subscriptions
// ---------------------------------------------------------------------------

// Handle returned by world.Subscribe<T>(...). Use Unsubscribe() to remove the
// handler. The subscription does NOT auto-unsubscribe on destruction; it is a
// plain ticket you can keep or discard.
class EventSubscription {
public:
    EventSubscription() = default;

    // Removes the handler from the world. Safe to call multiple times.
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

// A single event handler slot. Dead slots keep their index so that
// subscriptions remain stable even when handlers are unsubscribed.
struct HandlerSlot {
    std::function<void(const void*)> fn;
    bool alive = true;
};

// True when T exposes Execute(World&). World is only forward-declared here;
// the trait is instantiated once World is complete.
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

    // Registers T in this world and returns its ComponentTypeId. Idempotent.
    // Unregistered types produce clear compile-time (EKIT_COMPONENT missing) or
    // runtime (RegisterComponent missing) errors.
    template<typename T>
    ComponentTypeId RegisterComponent() {
        static_assert(IsComponent<T>::value,
                      "ekit: T is not declared as a component. Add 'EKIT_COMPONENT(T)' after "
                      "declaring T, or specialize ekit::IsComponent<T>.");
        static_assert(std::is_trivially_copyable_v<T>,
                      "ekit: component types must be trivially copyable (POD-like).");
        static_assert(std::is_default_constructible_v<T>,
                      "ekit: component types must be default constructible.");

        auto& id = ComponentTypeIdOf<T>;
        if (id == kInvalidComponentTypeId) {
            id = detail::NextComponentTypeId().fetch_add(1);
        }
        if (static_cast<std::size_t>(id) >= storages_.size()) {
            storages_.resize(static_cast<std::size_t>(id) + 1);
        }
        if (!storages_[id]) {
            storages_[id] = std::make_unique<ComponentStorage<T>>();
            ++storage_count_;
        }
        return id;
    }

    // Registers several components at once.
    template<typename... Ts>
    void RegisterComponents() {
        (RegisterComponent<Ts>(), ...);
    }

    // Runtime id of a registered component. Throws when not registered.
    template<typename T>
    ComponentTypeId GetComponentTypeId() const {
        static_assert(IsComponent<T>::value,
                      "ekit: T is not declared as a component. Add 'EKIT_COMPONENT(T)'.");
        const auto id = ComponentTypeIdOf<T>;
        if (id == kInvalidComponentTypeId || static_cast<std::size_t>(id) >= storages_.size() ||
            !storages_[id]) {
            throw EkitException("ekit: component '" + std::string(ComponentNameOf<T>()) +
                                "' is not registered in this World. Call world.RegisterComponent<" +
                                ComponentNameOf<T>() + ">() first.");
        }
        return id;
    }

    template<typename T>
    bool IsComponentRegistered() const {
        const auto id = ComponentTypeIdOf<T>;
        return id != kInvalidComponentTypeId && static_cast<std::size_t>(id) < storages_.size() &&
               storages_[id] != nullptr;
    }

    std::size_t GetRegisteredComponentCount() const {
        return storage_count_;
    }

    // =====================================================================
    // Entity creation / destruction
    // =====================================================================

    // Creates a new entity.
    Entity Create() {
        EntityId index;
        EntityGeneration generation;
        if (!free_list_.empty()) {
            index = free_list_.back();
            free_list_.pop_back();
            generation = entities_[index].GetGeneration();
            if (generation == 0) {
                // Generation wrapped around: this slot can never be safely
                // reused, so allocate a fresh one instead.
                index = static_cast<EntityId>(entities_.size());
                generation = 1;
                entities_.emplace_back(index, generation);
            } else {
                entities_[index] = Entity(index, generation);
            }
        } else {
            index = static_cast<EntityId>(entities_.size());
            generation = 1;
            entities_.emplace_back(index, generation);
        }
        ++alive_count_;
        return Entity(index, generation);
    }

    // Creates a new entity with a debug/editor-facing name.
    Entity Create(std::string_view name) {
        Entity e = Create();
        SetName(e, name);
        return e;
    }

    // Destroys an entity and all of its components. Invalid or stale handles
    // are silently ignored (the entity is already gone).
    void Destroy(Entity e) {
        if (!IsAlive(e)) {
            return;
        }
        const auto index = e.GetIndex();

        if (auto it = entity_to_name_.find(index); it != entity_to_name_.end()) {
            name_to_entity_.erase(it->second);
            entity_to_name_.erase(it);
        }

        for (auto& storage : storages_) {
            if (storage) {
                storage->TryRemove(index);
            }
        }

        const auto generation = static_cast<EntityGeneration>(e.GetGeneration() + 1);
        entities_[index] = Entity(index, generation);
        if (generation != 0) {
            free_list_.push_back(index);
        }
        --alive_count_;
    }

    bool IsAlive(Entity e) const {
        return e.IsValid() && e.GetIndex() < entities_.size() && entities_[e.GetIndex()] == e;
    }

    std::size_t GetAliveEntityCount() const {
        return alive_count_;
    }

    // Entity currently stored at the given index (may be dead / recycled).
    Entity GetEntity(EntityId index) const {
        return index < entities_.size() ? entities_[index] : Entity::Null;
    }

    // Iterates all currently alive entities.
    template<typename F>
    void ForEachEntity(F&& f) const {
        for (std::size_t i = 1; i < entities_.size(); ++i) {
            const Entity e = entities_[i];
            if (e.GetGeneration() != 0) {
                f(e);
            }
        }
    }

    // =====================================================================
    // Named entities (editor / debug integration)
    // =====================================================================

    void SetName(Entity e, std::string_view name) {
        if (!IsAlive(e)) {
            throw EkitException("ekit: cannot name a dead entity.");
        }
        const auto index = e.GetIndex();

        // Remove the entity's previous name (if any).
        if (auto it = entity_to_name_.find(index); it != entity_to_name_.end()) {
            name_to_entity_.erase(it->second);
        }

        const std::string key(name);

        // If another entity already owns this name, unname it (last one wins).
        if (auto it = name_to_entity_.find(key); it != name_to_entity_.end() && it->second != e) {
            entity_to_name_.erase(it->second.GetIndex());
        }

        name_to_entity_[key] = e;
        entity_to_name_[index] = key;
    }

    // Name of the entity, or an empty string when unnamed.
    const std::string& GetName(Entity e) const {
        static const std::string kEmpty;
        if (auto it = entity_to_name_.find(e.GetIndex()); it != entity_to_name_.end()) {
            return it->second;
        }
        return kEmpty;
    }

    // Looks up an entity by name. Returns Entity::Null when not found.
    Entity Find(std::string_view name) const {
        if (auto it = name_to_entity_.find(std::string(name)); it != name_to_entity_.end()) {
            return it->second;
        }
        return Entity::Null;
    }

    // =====================================================================
    // Component access
    // =====================================================================

    // Adds a component, constructing it in place. Throws if already present.
    template<typename T, typename... Args>
    T& Add(Entity e, Args&&... args) {
        RequireAlive(e);
        return GetStorage<T>().Emplace(e.GetIndex(), std::forward<Args>(args)...);
    }

    // Alias of Add (constructs in place, throws if already present).
    template<typename T, typename... Args>
    T& Emplace(Entity e, Args&&... args) {
        return Add<T>(e, std::forward<Args>(args)...);
    }

    // Adds the component or replaces its value if it already exists.
    template<typename T, typename... Args>
    T& Set(Entity e, Args&&... args) {
        RequireAlive(e);
        auto& storage = GetStorage<T>();
        if (T* existing = storage.TryGet(e.GetIndex())) {
            *existing = T(std::forward<Args>(args)...);
            return *existing;
        }
        return storage.Emplace(e.GetIndex(), std::forward<Args>(args)...);
    }

    template<typename T>
    bool Has(Entity e) const {
        if (!IsAlive(e)) {
            return false;
        }
        return GetStorage<T>().Contains(e.GetIndex());
    }

    template<typename T>
    T& Get(Entity e) {
        RequireAlive(e);
        return GetStorage<T>().Get(e.GetIndex());
    }

    template<typename T>
    const T& Get(Entity e) const {
        RequireAlive(e);
        return GetStorage<T>().Get(e.GetIndex());
    }

    template<typename T>
    T* TryGet(Entity e) {
        if (!IsAlive(e)) {
            return nullptr;
        }
        return GetStorage<T>().TryGet(e.GetIndex());
    }

    template<typename T>
    const T* TryGet(Entity e) const {
        if (!IsAlive(e)) {
            return nullptr;
        }
        return GetStorage<T>().TryGet(e.GetIndex());
    }

    template<typename T>
    bool Remove(Entity e) {
        if (!IsAlive(e)) {
            return false;
        }
        return GetStorage<T>().TryRemove(e.GetIndex());
    }

    // Applies fn to the component, e.g. world.Patch<Position>(e, [](Position& p) { p.x += 1.f; });
    template<typename T, typename F>
    void Patch(Entity e, F&& fn) {
        static_assert(std::is_invocable_v<F&, T&>,
                      "ekit: Patch requires a callable accepting (T&).");
        fn(Get<T>(e));
    }

    // Removes all instances of T. No-op when T is not registered.
    template<typename T>
    void ClearComponent() {
        if (IsComponentRegistered<T>()) {
            GetStorage<T>().Clear();
        }
    }

    // Destroys every entity and clears every component.
    void ClearAll() {
        for (auto& storage : storages_) {
            if (storage) {
                storage->Clear();
            }
        }
        free_list_.clear();
        entities_.assign(1, Entity::Null);
        alive_count_ = 0;
        name_to_entity_.clear();
        entity_to_name_.clear();
    }

    // =====================================================================
    // Queries
    // =====================================================================

    // Builds a fluent query. Include <ekit/query.hpp> (or <ekit/ekit.hpp>).
    template<typename... Ts>
    auto Query() -> ekit::Query<World, detail::EmptyPredicate, TypeList<Ts...>,
                          TypeList<>, TypeList<>> {
        return ekit::Query<World, detail::EmptyPredicate, TypeList<Ts...>, TypeList<>,
                     TypeList<>>(*this);
    }

    // =====================================================================
    // Events
    // =====================================================================

    // Subscribes a handler for event type T. The handler must be callable with
    // (const T&). Returns a subscription that can be used to unsubscribe.
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

    // Emits an event constructed from args, e.g. world.Emit<HitEvent>(damage, target);
    template<typename T, typename... Args>
    void Emit(Args&&... args) {
        T event(std::forward<Args>(args)...);
        EmitInternal(event);
    }

    // Emits a ready-made event object, e.g. world.Emit(event);
    template<typename T>
    void Emit(const T& event) {
        EmitInternal(event);
    }

    // =====================================================================
    // Systems
    // =====================================================================

    // Runs a single system (a class with Execute(World&)) synchronously.
    template<typename T>
    void RunSystem(T&& system) {
        static_assert(detail::IsSystemLike<std::decay_t<T>>::value,
                      "ekit: RunSystem requires a system object with an Execute(World&) method.");
        system.Execute(*this);
    }

    // =====================================================================
    // Storage access (used by queries and advanced users)
    // =====================================================================

    template<typename T>
    ComponentStorage<T>& GetStorage() {
        static_assert(IsComponent<T>::value,
                      "ekit: T is not declared as a component. Add 'EKIT_COMPONENT(T)'.");
        const auto id = ComponentTypeIdOf<T>;
        if (id == kInvalidComponentTypeId || static_cast<std::size_t>(id) >= storages_.size() ||
            !storages_[id]) {
            throw EkitException("ekit: component '" + std::string(ComponentNameOf<T>()) +
                                "' is not registered in this World. Call world.RegisterComponent<" +
                                ComponentNameOf<T>() + ">() first.");
        }
        return *static_cast<ComponentStorage<T>*>(storages_[id].get());
    }

    template<typename T>
    const ComponentStorage<T>& GetStorage() const {
        static_assert(IsComponent<T>::value,
                      "ekit: T is not declared as a component. Add 'EKIT_COMPONENT(T)'.");
        const auto id = ComponentTypeIdOf<T>;
        if (id == kInvalidComponentTypeId || static_cast<std::size_t>(id) >= storages_.size() ||
            !storages_[id]) {
            throw EkitException("ekit: component '" + std::string(ComponentNameOf<T>()) +
                                "' is not registered in this World. Call world.RegisterComponent<" +
                                ComponentNameOf<T>() + ">() first.");
        }
        return *static_cast<const ComponentStorage<T>*>(storages_[id].get());
    }

private:
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

    // Component storages indexed by ComponentTypeId (id 0 is reserved).
    std::vector<std::unique_ptr<IComponentStorage>> storages_;
    std::size_t storage_count_ = 0;

    // Entity storage: index -> Entity. Slot 0 is reserved for Entity::Null.
    std::vector<Entity> entities_{Entity::Null};
    std::vector<EntityId> free_list_;
    std::size_t alive_count_ = 0;

    std::unordered_map<std::string, Entity> name_to_entity_;
    std::unordered_map<EntityId, std::string> entity_to_name_;

    // Event sinks keyed by detail::TypeIdOf<T>().
    std::unordered_map<std::size_t, std::any> event_sinks_;
};

// ---------------------------------------------------------------------------
// EventSubscription implementation
// ---------------------------------------------------------------------------

inline void EventSubscription::Unsubscribe() {
    if (world_) {
        world_->Unsubscribe(event_id_, index_);
    }
    world_ = nullptr;
    index_ = detail::kNpos;
}


} // namespace ekit






