#pragma once
// ekit - component.hpp
//
// Components are plain POD structs that must be explicitly declared with the
// EKIT_COMPONENT(T) macro (or an explicit specialization of ekit::IsComponent)
// and explicitly registered at runtime with world.RegisterComponent<T>().
//
// ComponentStorage<T> is a sparse-set based storage: a dense, cache-friendly
// array of components plus a sparse index from entity -> dense position.
// Removal is swap-and-pop (O(1), order not preserved).

#include "core.hpp"
#include "entity.hpp"

#include <algorithm>
#include <memory>
#include <typeindex>
#include <vector>

namespace ekit {

// ---------------------------------------------------------------------------
// Component declaration
// ---------------------------------------------------------------------------

// Trait that marks a type as an ekit component. Specialized by EKIT_COMPONENT.
template<typename T>
struct IsComponent : std::false_type {};

namespace detail {

template<typename T, typename = void>
struct ComponentNameImpl {
    static const char* Get() noexcept {
        return typeid(T).name();
    }
};

template<typename T>
struct ComponentNameImpl<T, std::void_t<decltype(T::GetComponentName())>> {
    static constexpr const char* Get() noexcept {
        return T::GetComponentName();
    }
};

} // namespace detail

// Human-readable name of a component. Prefer EKIT_COMPONENT(T) so the name is
// exactly the spelling used in source code (great for editor integration).
template<typename T>
constexpr const char* ComponentNameOf() noexcept {
    return detail::ComponentNameImpl<T>::Get();
}

// Runtime id assigned by world.RegisterComponent<T>(). 0 == not registered.
template<typename T>
inline ComponentTypeId ComponentTypeIdOf = kInvalidComponentTypeId;

// Declares T as an ekit component. Place after the struct definition, at
// namespace scope:
//
//   struct Position { float x = 0.f, y = 0.f; };
//   EKIT_COMPONENT(Position);
//
// This produces clear, readable compile errors when T is used without being
// declared (instead of a template error storm).
#define EKIT_COMPONENT(Type)                                                                          \
    template<> struct ::ekit::IsComponent<Type> : std::true_type {};                                 \
    template<> struct ::ekit::detail::ComponentNameImpl<Type, void> {                                \
        static constexpr const char* Get() noexcept { return #Type; }                                \
    }

// ---------------------------------------------------------------------------
// Component storage
// ---------------------------------------------------------------------------

// Type-erased base of every component storage. The World stores these in a
// vector indexed by ComponentTypeId.
class IComponentStorage {
public:
    virtual ~IComponentStorage() = default;

    // Number of entities that currently own this component.
    virtual std::size_t Size() const = 0;

    // Remove the component of the entity with the given index, if present.
    virtual bool TryRemove(EntityId index) = 0;

    // Remove all instances of this component.
    virtual void Clear() = 0;

    virtual std::type_index GetTypeIndex() const = 0;
    virtual const char* GetTypeName() const = 0;
};

template<typename T>
class ComponentStorage final : public IComponentStorage {
public:
    std::size_t Size() const override {
        return components_.size();
    }

    bool Contains(EntityId index) const {
        return index < sparse_.size() && sparse_[index] != 0;
    }

    T* TryGet(EntityId index) {
        if (!Contains(index)) {
            return nullptr;
        }
        return &components_[static_cast<std::size_t>(sparse_[index]) - 1];
    }

    const T* TryGet(EntityId index) const {
        if (!Contains(index)) {
            return nullptr;
        }
        return &components_[static_cast<std::size_t>(sparse_[index]) - 1];
    }

    // Returns a reference to the component, throws if not present.
    T& Get(EntityId index) {
        T* result = TryGet(index);
        if (result == nullptr) {
            throw EkitException("ekit: component is not present on this entity.");
        }
        return *result;
    }

    const T& Get(EntityId index) const {
        const T* result = TryGet(index);
        if (result == nullptr) {
            throw EkitException("ekit: component is not present on this entity.");
        }
        return *result;
    }

    // Constructs the component in place. Throws if the entity already owns it.
    template<typename... Args>
    T& Emplace(EntityId index, Args&&... args) {
        EnsureSparse(index);
        if (sparse_[index] != 0) {
            throw EkitException("ekit: component already present on this entity.");
        }
        const std::size_t dense_index = components_.size();
        components_.emplace_back(std::forward<Args>(args)...);
        entities_.push_back(index);
        sparse_[index] = static_cast<std::uint32_t>(dense_index) + 1;
        return components_.back();
    }

    // Swap-and-pop removal. O(1); does not preserve iteration order.
    bool TryRemove(EntityId index) override {
        if (!Contains(index)) {
            return false;
        }
        const std::size_t dense_index = static_cast<std::size_t>(sparse_[index]) - 1;
        const std::size_t last = components_.size() - 1;
        if (dense_index != last) {
            components_[dense_index] = std::move(components_[last]);
            entities_[dense_index] = entities_[last];
            sparse_[entities_[dense_index]] = static_cast<std::uint32_t>(dense_index) + 1;
        }
        components_.pop_back();
        entities_.pop_back();
        sparse_[index] = 0;
        return true;
    }

    void Clear() override {
        components_.clear();
        entities_.clear();
        std::fill(sparse_.begin(), sparse_.end(), 0);
    }

    // Entity index of the dense element at the given position.
    EntityId EntityAt(std::size_t dense_index) const {
        return entities_[dense_index];
    }

    T& ComponentAt(std::size_t dense_index) {
        return components_[dense_index];
    }

    const T& ComponentAt(std::size_t dense_index) const {
        return components_[dense_index];
    }

    void Reserve(std::size_t capacity) {
        components_.reserve(capacity);
    }

    std::type_index GetTypeIndex() const override {
        return typeid(T);
    }

    const char* GetTypeName() const override {
        return ComponentNameOf<T>();
    }

private:
    void EnsureSparse(EntityId index) {
        if (static_cast<std::size_t>(index) >= sparse_.size()) {
            sparse_.resize(static_cast<std::size_t>(index) + 1, 0);
        }
    }

    // Dense storage (cache-friendly iteration).
    std::vector<T> components_;
    std::vector<EntityId> entities_;

    // Sparse index: entity index -> dense index + 1 (0 == absent).
    std::vector<std::uint32_t> sparse_;
};

} // namespace ekit

