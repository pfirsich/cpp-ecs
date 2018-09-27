#pragma once

#include <cassert>
#include <limits>
#include <vector>
#include <type_traits>

namespace ecs {

/*
    Problems:
    Entity ID Reuse
    remove entity und components
    updateFunc type validation
    getComponent specialization für const component types hinzufügen
*/

using ComponentMask = uint64_t;
static_assert(std::is_unsigned<ComponentMask>::value, "ComponentMask type must be unsigned");
static const ComponentMask ALL_COMPONENTS = std::numeric_limits<ComponentMask>::max();

using EntityId = uint32_t;

using IndexType = size_t;
static const IndexType MAX_INDEX = std::numeric_limits<IndexType>::max();

class BaseComponent {
protected:
    static ComponentMask idCounter;
};

ComponentMask BaseComponent::idCounter = 0;

template <typename ComponentType>
class Component : BaseComponent {
    static std::vector<ComponentType> pool;

    static ComponentMask getId() {
        static auto id = BaseComponent::idCounter++;
        assert(id <= std::numeric_limits<ComponentMask>::digits);
        return id;
    }
};

template <typename ComponentType>
ComponentMask componentId() {
    return Component<typename std::remove_const<ComponentType>::type>::getId();
}

template <typename ComponentType>
ComponentMask componentMask() {
    return 1 << componentId<ComponentType>();
}

template <typename FirstComponentType, typename... Args>
ComponentMask componentMask() {
    return componentMask<FirstComponentType>() | componentMask<Args...>();
}

template<typename ComponentType>
class ComponentPool {
public:
    ComponentPool() = default;
    ~ComponentPool() = default;
    ComponentPool(const ComponentPool& other) = delete;
    ComponentPool& operator=(const ComponentPool& other) = delete;

    template<typename... Args>
    ComponentType& add(EntityId entityId, Args... args) {
        if(mIndexMap.size() < entityId + 1) {
            mIndexMap.resize(entityId + 1, INVALID_INDEX);
        }
        if(mIndexMap[entityId] == INVALID_INDEX) {
            mIndexMap[entityId] = mComponents.size();
            return mComponents.emplace_back(std::forward<Args>(args)...);
        } else {
            return mComponents[mIndexMap[entityId]];
        }
    }

    ComponentType& get(EntityId entityId) {
        assert(mIndexMap.size() > entityId && mIndexMap[entityId] != INVALID_INDEX && mComponents > mIndexMap[entityId]);
        return mComponents[mIndexMap[entityId]];
    }

private:
    std::vector<ComponentType> mComponents;
    std::vector<IndexType> mIndexMap;
    static const IndexType INVALID_INDEX = MAX_INDEX;
};

class Entity;

class World {
private:
    struct EntityList;

    class EntityIterator {
    public:
        EntityIterator(EntityList& list, IndexType index) : mList(list), mEntityIndex(index) {}

        EntityIterator& operator++();
        bool operator!=(const EntityIterator & other) const;
        Entity operator*() const;
    private:
        EntityList& mList;
        IndexType mEntityIndex;
    };

    struct EntityList {
        World& world;
        ComponentMask mask;

        EntityList(World& world, ComponentMask mask) : world(world), mask(mask) {}
        ~EntityList() = default;

        EntityIterator begin() {
            return EntityIterator(*this, 0);
        }

        EntityIterator end() {
            return EntityIterator(*this, MAX_INDEX);
        }
    };

public:
    World() = default;
    ~World() = default;
    World(const World& other) = default;
    World& operator=(const World& other) = default;

    Entity entity();
    Entity entity(EntityId entityId);

    template <typename ComponentType, typename... Args>
    ComponentType& addComponent(EntityId entityId, Args&&... args);

    bool hasComponents(EntityId entityId, ComponentMask mask) const;

    template <typename... Args>
    bool hasComponents(EntityId entityId) const;

    template <typename ComponentType>
    ComponentType& getComponent(EntityId entityId);

    template <typename ... Types>
    struct TypeTuple;

    template <typename... Components, typename... FuncArgs, typename FuncType>
    void tickSystem(FuncType tickFunc, FuncArgs... funcArgs);

    auto getEntityCount() const { return mComponentMasks.size(); }

    template<typename... Args>
    EntityList entitiesWith() {
        return EntityList(*this, componentMask<Args>());
    }

private:
    template<typename ComponentType> ComponentPool<ComponentType> mPool;
    std::vector<ComponentMask> mComponentMasks;
};

class Entity {
public:
    ~Entity() = default;
    Entity(const Entity& other) = delete;
    Entity& operator=(const Entity& other) = delete;

    template <typename ComponentType, typename... Args>
    ComponentType& add(Args&&... args);

    template <typename... Args>
    bool has() const;

    template <typename ComponentType>
    ComponentType& get();

    EntityId getId() const { return mId; }
    World* getWorld() const { return mWorld; }

private:
    World* mWorld;
    EntityId mId;

    Entity(World* world, EntityId id) : mWorld(world), mId(id) {}

    friend Entity World::entity();
    friend Entity World::entity(EntityId);
};

// World implementation
inline World::EntityIterator& World::EntityIterator::operator++() {
    while (!mList.world.hasComponents(mEntityIndex, mList.mask) && mEntityIndex < mList.world.getEntityCount()) mEntityIndex++;
    if(mEntityIndex >= mList.world.getEntityCount()) {
        mEntityIndex = MAX_INDEX;
    }
    return *this;
}

inline bool World::EntityIterator::operator!=(const EntityIterator& other) const {
    return mEntityIndex != other.mEntityIndex;
}

inline Entity World::EntityIterator::operator*() const {
    return mList.world.entity(mEntityIndex);
}

inline Entity World::entity() {
    mComponentMasks.push_back(0);
    return Entity(this, mComponentMasks.size() - 1);
}

inline Entity World::entity(EntityId entityId) {
    assert(mComponentMasks.size() >= entityId); // entity exists
    return Entity(this, entityId);
}

template <typename ComponentType, typename... Args>
ComponentType& World::addComponent(EntityId entityId, Args&&... args) {
    static_assert(std::is_default_constructible<ComponentType>::value, "Component types must be default constructible.");
    assert(mComponentMasks.size() > entityId);
    assert(!hasComponents<ComponentType>());
    mComponentMasks[entityId] |= componentMask<ComponentType>();
    return mPool<ComponentType>.add(entityId, std::forward<Args>(args)...);
}

inline bool World::hasComponents(EntityId entityId, ComponentMask mask) const {
    assert(mComponentMasks.size() > entityId);
    return (mComponentMasks[entityId] & mask) == mask;
}


template <typename... Args>
bool World::hasComponents(EntityId entityId) const {
    return hasComponents(entityId, componentMask<Args...>());
}

template <typename ComponentType>
ComponentType& World::getComponent(EntityId entityId) {
    assert(hasComponents<ComponentType>(entityId));
    return mPool<ComponentType>.get(entityId);
}

template <typename... Components, typename... FuncArgs, typename FuncType>
void World::tickSystem(FuncType tickFunc, FuncArgs... funcArgs) {
    for (auto e : entitiesWith<Components>()) {
        tickFunc(funcArgs..., e.get<Components>()...);
    }
}

// Entity implementation
template <typename ComponentType, typename... Args>
ComponentType& Entity::add(Args&&... args) {
    return mWorld->addComponent(mId, std::forward<Args>(args)...);
}

template <typename... Args>
bool Entity::has() const {
    return mWorld->hasComponents<Args...>(mId);
}

template <typename ComponentType>
ComponentType& Entity::get() {
    return mWorld->getComponent<ComponentType>(mId);
}

} // namespace ecs