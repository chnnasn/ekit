#pragma once
// ekit - component.hpp
//
// Components are plain POD structs that must be explicitly declared with the
// EKIT_COMPONENT(T) macro (or an explicit specialization of ekit::IsComponent)
// and explicitly registered at runtime with world.RegisterComponent<T>().
//
// Storage is archetype-based: all entities that share the exact same component
// set live in one Archetype, and each component is a contiguous SoA column
// aligned by row. This lets queries hand out raw, SIMD-friendly component
// pointers for a whole batch at once (ForEachBatch).

#include "core.hpp"
#include "entity.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <typeindex>
#include <vector>

namespace ekit {

// ---------------------------------------------------------------------------
// Component declaration
// ---------------------------------------------------------------------------

// Trait that marks a type as an ekit component. Set by the EKIT_COMPONENT(T)
// macro (which injects an IsEkitComponent member), or by an explicit
// specialization of IsComponent<T> (e.g. inside namespace ekit for foreign
// types).
template<typename T, typename = void>
struct IsComponent : std::false_type {};

// True when the type carries the marker injected by EKIT_COMPONENT(T).
template<typename T>
struct IsComponent<T, std::void_t<typename T::IsEkitComponent>> : std::true_type {};

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

// Declares T as an ekit component. Place INSIDE the struct body:
//
//   struct Position {
//       float x = 0.f;
//       float y = 0.f;
//       EKIT_COMPONENT(Position);
//   };
//
// The macro only injects a marker member and a name accessor, so the struct
// stays a plain POD. Because it lives inside the class, it works in any
// namespace (no explicit template specialization required).
#define EKIT_COMPONENT(Type)                                                                          \
    using IsEkitComponent = void;                                                                     \
    static constexpr const char* GetComponentName() noexcept { return #Type; }

// ---------------------------------------------------------------------------
// Component metadata (registered at runtime)
// ---------------------------------------------------------------------------

struct ComponentInfo {
    std::size_t size = 0;   // sizeof(T); 0 == unregistered
    std::size_t align = 1;  // alignof(T)
    const char* name = "";  // ComponentNameOf<T>()
};

// ---------------------------------------------------------------------------
// Archetype storage
// ---------------------------------------------------------------------------

// An archetype holds every entity that shares the exact same component set.
// Each component type is a contiguous SoA column, and all columns are aligned
// by row, so a query can hand out raw component pointers for an entire batch
// at once (ForEachBatch). Components are trivially copyable, so rows are moved
// with memcpy and columns are plain aligned byte arrays.
class Archetype {
public:
    // Sorted component ids (ascending) of this archetype.
    std::vector<ComponentTypeId> types;

    // Flat lookup: component id -> column index (-1 == absent).
    std::vector<std::int16_t> col_of_type;

    // Size in bytes of each component type, parallel to `types`.
    std::vector<std::size_t> sizes;

    // Row -> entity index.
    std::vector<EntityId> entities;

    // One byte column per component type; column k holds rows * sizes[k] bytes.
    std::vector<std::vector<std::byte>> columns;

    std::size_t RowCount() const {
        return entities.size();
    }

    // Column index of a component id, or -1 when absent (O(1) flat lookup).
    std::ptrdiff_t ColumnIndex(ComponentTypeId id) const {
        if (static_cast<std::size_t>(id) >= col_of_type.size()) {
            return -1;
        }
        return static_cast<std::ptrdiff_t>(col_of_type[id]);
    }

    // Builds the O(1) type id -> column index lookup after `types` is set.
    void BuildColumnLookup() {
        std::size_t max_id = 0;
        for (ComponentTypeId id : types) {
            max_id = std::max<std::size_t>(max_id, static_cast<std::size_t>(id));
        }
        col_of_type.assign(max_id + 1, -1);
        for (std::size_t k = 0; k < types.size(); ++k) {
            col_of_type[types[k]] = static_cast<std::int16_t>(k);
        }
    }

    // Typed pointer to a component column (aligned SoA array).
    template<typename T>
    T* Column(std::size_t col) {
        return reinterpret_cast<T*>(columns[col].data());
    }

    template<typename T>
    const T* Column(std::size_t col) const {
        return reinterpret_cast<const T*>(columns[col].data());
    }

    // Appends an empty row (component bytes are zero-initialized).
    void PushRow(EntityId entity) {
        entities.push_back(entity);
        for (std::size_t k = 0; k < columns.size(); ++k) {
            columns[k].resize(columns[k].size() + sizes[k]);
        }
    }

    // Copies row `from` into row `to` byte-wise. Both rows must exist.
    void CopyRow(std::size_t from, std::size_t to) {
        for (std::size_t k = 0; k < columns.size(); ++k) {
            std::memcpy(columns[k].data() + to * sizes[k],
                        columns[k].data() + from * sizes[k], sizes[k]);
        }
    }

    // Moves the last row into `row` and pops the last row. The caller must
    // update the moved entity's location afterwards.
    void RemoveRow(std::size_t row) {
        const std::size_t last = RowCount() - 1;
        if (row != last) {
            entities[row] = entities[last];
            CopyRow(last, row);
        }
        entities.pop_back();
        for (std::size_t k = 0; k < columns.size(); ++k) {
            columns[k].resize(columns[k].size() - sizes[k]);
        }
    }

    // Writes one component value into column `col`, row `row`.
    template<typename T>
    void Write(std::size_t col, std::size_t row, const T& value) {
        std::memcpy(columns[col].data() + row * sizeof(T), &value, sizeof(T));
    }

    void Clear() {
        entities.clear();
        for (auto& col : columns) {
            col.clear();
        }
    }
};

// ---------------------------------------------------------------------------
// Sparse storage (alternative backend)
// ---------------------------------------------------------------------------

// Type-erased base of a sparse component storage. Dense components live in
// archetypes (SoA, fast iteration); components the user marks as sparse live
// here (fast random access + cheap structural changes).
class IComponentStorage {
public:
    virtual ~IComponentStorage() = default;
    virtual std::size_t Size() const = 0;
    virtual bool Contains(EntityId index) const = 0;
    virtual bool TryRemove(EntityId index) = 0;
    virtual void Clear() = 0;
    virtual std::type_index GetTypeIndex() const = 0;
    virtual const char* GetTypeName() const = 0;
};

// Sparse-set storage: dense array + entity array + sparse index. Removal is
// swap-and-pop (O(1)); random access is O(1) via the sparse index.
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

    T& Get(EntityId index) {
        T* result = TryGet(index);
        if (result == nullptr) {
            throw EkitException("ekit: sparse component is not present on this entity.");
        }
        return *result;
    }

    const T& Get(EntityId index) const {
        const T* result = TryGet(index);
        if (result == nullptr) {
            throw EkitException("ekit: sparse component is not present on this entity.");
        }
        return *result;
    }

    template<typename... Args>
    T& Emplace(EntityId index, Args&&... args) {
        EnsureSparse(index);
        if (sparse_[index] != 0) {
            throw EkitException("ekit: sparse component already present on this entity.");
        }
        const std::size_t dense_index = components_.size();
        components_.emplace_back(std::forward<Args>(args)...);
        entities_.push_back(index);
        sparse_[index] = static_cast<std::uint32_t>(dense_index) + 1;
        return components_.back();
    }

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

    EntityId EntityAt(std::size_t dense_index) const {
        return entities_[dense_index];
    }

    T& ComponentAt(std::size_t dense_index) {
        return components_[dense_index];
    }

    const T& ComponentAt(std::size_t dense_index) const {
        return components_[dense_index];
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

    std::vector<T> components_;
    std::vector<EntityId> entities_;
    std::vector<std::uint32_t> sparse_;
};

} // namespace ekit
