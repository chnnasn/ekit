#pragma once
// ekit - entity.hpp
//
// Entity - a strong-typed, generation-based entity handle. Unlike a bare
// uint32_t, an Entity carries both an index into the World's entity storage and
// a generation counter, so a handle to a destroyed entity is never silently
// aliased to a newly created one.

#include "core.hpp"

#include <functional>

namespace ekit {

class Entity {
public:
    // The null handle. Equivalent to an invalid / empty entity.
    static const Entity Null;

    constexpr Entity() noexcept = default;

    // Build a handle from raw index and generation. Prefer World::Create().
    constexpr Entity(EntityId index, EntityGeneration generation) noexcept
        : m_value((static_cast<std::uint64_t>(generation) << 32) | index) {}

    // Index of this entity inside the World's entity storage.
    constexpr EntityId GetIndex() const noexcept {
        return static_cast<EntityId>(m_value & 0xFFFFFFFFull);
    }

    // Generation counter of this entity. Bumped on every recycle of the slot.
    constexpr EntityGeneration GetGeneration() const noexcept {
        return static_cast<EntityGeneration>(m_value >> 32);
    }

    // True when this is not the null handle.
    constexpr bool IsValid() const noexcept {
        return m_value != 0;
    }

    // True when this is not the null handle.
    constexpr explicit operator bool() const noexcept {
        return IsValid();
    }

    constexpr bool operator==(const Entity&) const noexcept = default;
    constexpr bool operator!=(const Entity&) const noexcept = default;

    // Total ordering over the packed value. Useful for std::map / std::set.
    constexpr bool operator<(const Entity& other) const noexcept {
        return m_value < other.m_value;
    }

    // Raw packed representation (generation << 32 | index).
    constexpr std::uint64_t RawValue() const noexcept {
        return m_value;
    }

private:
    std::uint64_t m_value = 0;
};

const Entity Entity::Null{};

static_assert(sizeof(Entity) == sizeof(std::uint64_t),
              "ekit::Entity must be exactly 8 bytes (zero overhead).");

} // namespace ekit

namespace std {

template<>
struct hash<ekit::Entity> {
    std::size_t operator()(ekit::Entity e) const noexcept {
        return static_cast<std::size_t>(e.RawValue());
    }
};

} // namespace std


