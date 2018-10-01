#pragma once

#include <cassert>
#include <limits>
#include <vector>
#include <type_traits>
#include <thread>
#include <atomic>
#include <algorithm>
#include <execution>

namespace ecs {

using ComponentMask = uint64_t;
static_assert(std::is_unsigned<ComponentMask>::value, "ComponentMask type must be unsigned");
static const ComponentMask ALL_COMPONENTS = std::numeric_limits<ComponentMask>::max();

using EntityId = uint32_t;

using IndexType = size_t;
static const IndexType MAX_INDEX = std::numeric_limits<IndexType>::max();

namespace componentId {
    static ComponentMask idCounter = 0;

    template <typename ComponentType>
    ComponentMask get() {
        static auto id = idCounter++;
        assert(id <= std::numeric_limits<ComponentMask>::digits);
        return id;
    }
}

template <typename... Args>
ComponentMask componentMask() {
    return (... | (1ull << componentId::get<typename std::remove_const<Args>::type>()));
}

struct ComponentPoolBase {
    virtual ~ComponentPoolBase() = default;
    virtual void remove(EntityId entityId) = 0;
};

template<typename ComponentType>
class ComponentPool : public ComponentPoolBase {
public:
    ComponentPool() : mComponentId(componentId::get<ComponentType>()) {}
    ~ComponentPool() = default;
    ComponentPool(const ComponentPool& other) = delete;
    ComponentPool& operator=(const ComponentPool& other) = delete;

    bool has(EntityId entityId) {
        return mIndexMap.size() > entityId && mIndexMap[entityId] != INVALID_INDEX && mIndexMap[entityId] < mComponents.size();
    }

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
        assert(has(entityId));
        return mComponents[mIndexMap[entityId]];
    }

    void remove(EntityId entityId) override {
        assert(has(entityId));
        mIndexMap[entityId] = INVALID_INDEX;
    }

private:
    ComponentMask mComponentId;
    std::vector<ComponentType> mComponents;
    std::vector<IndexType> mIndexMap;
    static const IndexType INVALID_INDEX = MAX_INDEX;
};

class EntityHandle;

class World {
private:
    struct EntityList;

    class EntityIterator {
    // To be used with std::for_each, this has to be a ForwardIterator: https://en.cppreference.com/w/cpp/named_req/ForwardIterator
    //http://anderberg.me/2016/07/04/c-custom-iterators/
    public:
        // https://en.cppreference.com/w/cpp/iterator/iterator_traits
        using iterator_category = std::forward_iterator_tag;
        using value_type = EntityHandle;
        using pointer = EntityHandle*;
        using reference = EntityHandle&;
        using difference_type = std::ptrdiff_t;

        EntityIterator() : mList(nullptr), mEntityIndex(0) {}
        EntityIterator(const EntityIterator& other) = default;
        EntityIterator& operator=(const EntityIterator& other) = default;

        EntityIterator(EntityList* list, IndexType index) : mList(list), mEntityIndex(index) {}

        EntityIterator& operator++();
        EntityIterator operator++(int);
        bool operator==(const EntityIterator& other) const;
        bool operator!=(const EntityIterator& other) const;
        EntityHandle operator*() const;

    private:
        EntityList* mList;
        IndexType mEntityIndex;
    };

    struct EntityList {
        World& world;
        ComponentMask mask;

        EntityList(World& world, ComponentMask mask) : world(world), mask(mask) {}
        ~EntityList() = default;

        EntityIterator begin() {
            return EntityIterator(this, 0);
        }

        EntityIterator end() {
            return EntityIterator(this, MAX_INDEX);
        }
    };

public:
    World() = default;
    ~World() = default;
    World(const World& other) = default;
    World& operator=(const World& other) = default;

    EntityHandle createEntity();
    EntityHandle getEntityHandle(EntityId entityId);

    void destroyEntity(EntityId entityId);

    template <typename ComponentType, typename... Args>
    ComponentType& addComponent(EntityId entityId, Args&&... args);

    bool hasComponents(EntityId entityId, ComponentMask mask) const;

    template <typename... Args>
    bool hasComponents(EntityId entityId) const;

    template <typename ComponentType>
    ComponentType& getComponent(EntityId entityId);

    template <typename... Components, typename... FuncArgs, typename FuncType>
    void tickSystem(bool async, bool parallelFor, FuncType tickFunc, FuncArgs... funcArgs);

    void finishSystems();

    auto getEntityCount() const { return mComponentMasks.size(); }

    // https://stackoverflow.com/questions/41331215/what-are-the-constraints-on-the-user-using-stls-parallel-algorithms
    template <typename... Components, typename FuncType, typename ExPo>
    void forEachEntity(FuncType func, ExPo executionPolicy = std::execution::seq);

    template <typename... Components>
    EntityList entitiesWith() {
        return EntityList(*this, componentMask<Components...>());
    }

private:
    struct RunningSystem {
        ComponentMask readMask;
        ComponentMask writeMask;
        std::thread thread;
        bool threadJoined;

        RunningSystem(ComponentMask readMask, ComponentMask writeMask) :
            readMask(readMask), writeMask(writeMask), threadJoined(false) {}
    };

    std::vector<std::unique_ptr<ComponentPoolBase>> mPools;
    std::vector<ComponentMask> mComponentMasks;
    std::vector<std::unique_ptr<RunningSystem>> mRunningSystems;

    template <typename ComponentType>
    ComponentPool<ComponentType>& getPool();

    void waitForSystems(ComponentMask readMask, ComponentMask writeMask);
};

class EntityHandle {
public:
    ~EntityHandle() = default;
    EntityHandle(const EntityHandle& other) = default;
    EntityHandle& operator=(const EntityHandle& other) = delete;

    template <typename ComponentType, typename... Args>
    ComponentType& add(Args&&... args);

    template <typename... Args>
    bool has() const;

    template <typename ComponentType>
    ComponentType& get();

    EntityId getId() const { return mId; }
    World& getWorld() const { return mWorld; }

private:
    World& mWorld;
    EntityId mId;

    EntityHandle(World& world, EntityId id) : mWorld(world), mId(id) {}

    friend EntityHandle World::createEntity();
    friend EntityHandle World::getEntityHandle(EntityId);
};

// World implementation
inline World::EntityIterator& World::EntityIterator::operator++() {
    mEntityIndex++;
    while (mEntityIndex < mList->world.getEntityCount() && !mList->world.hasComponents(mEntityIndex, mList->mask)) mEntityIndex++;
    if(mEntityIndex >= mList->world.getEntityCount()) {
        mEntityIndex = MAX_INDEX;
    }
    return *this;
}

inline World::EntityIterator World::EntityIterator::operator++(int) {
    EntityIterator ret(*this);
    operator++();
    return ret;
}

inline bool World::EntityIterator::operator==(const EntityIterator& other) const {
    return mList == other.mList && mEntityIndex == other.mEntityIndex;
}

inline bool World::EntityIterator::operator!=(const EntityIterator& other) const {
    return !operator==(other);
}

inline EntityHandle World::EntityIterator::operator*() const {
    return mList->world.getEntityHandle(mEntityIndex);
}

inline EntityHandle World::createEntity() {
    mComponentMasks.push_back(0);
    return EntityHandle(*this, mComponentMasks.size() - 1);
}

inline EntityHandle World::getEntityHandle(EntityId entityId) {
    assert(mComponentMasks.size() >= entityId); // entity exists
    return EntityHandle(*this, entityId);
}

inline void World::destroyEntity(EntityId entityId) {
    assert(mComponentMasks.size() >= entityId); // entity exists
    mComponentMasks[entityId] = 0;
}

template <typename ComponentType, typename... Args>
ComponentType& World::addComponent(EntityId entityId, Args&&... args) {
    static_assert(std::is_default_constructible<ComponentType>::value, "Component types must be default constructible.");
    assert(mComponentMasks.size() > entityId);
    assert(!hasComponents<ComponentType>(entityId));
    mComponentMasks[entityId] |= componentMask<ComponentType>();
    return getPool<ComponentType>().add(entityId, std::forward<Args>(args)...);
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
    return getPool<typename std::remove_const<ComponentType>::type>().get(entityId);
}

template <bool isConst, typename ComponentType>
ComponentMask _constFilteredComponentMask() {
    if constexpr(std::is_const<ComponentType>::value == isConst) {
        return componentMask<ComponentType>();
    } else {
        return 0;
    }
}

template <bool isConst, typename... Args>
ComponentMask constFilteredComponentMask() {
    return (... | _constFilteredComponentMask<isConst, Args>());
}

template <typename... Components, typename FuncType, typename ExPo>
void World::forEachEntity(FuncType func, ExPo executionPolicy) {
    // EntityHandle has to be passed by value to the invokable, because the EntityHandle returned from the EntityIterator
    // is a temporary, since they are not stored somewhere, but merely handles.
    static_assert(std::is_invocable_r<void, FuncType, EntityHandle>::value);
    auto entityList = entitiesWith<Components...>();
    std::for_each(executionPolicy, entityList.begin(), entityList.end(), func);
}

template <typename... Components, typename... FuncArgs, typename FuncType>
void World::tickSystem(bool async, bool parallelFor, FuncType tickFunc, FuncArgs... funcArgs) {
    static_assert(!(... || std::is_reference<Components>::value), "Component types must not be references");
    static_assert(std::is_same<FuncType, void(*)(FuncArgs..., Components&...)>::value, "Tick function has invalid signature");

    auto readMask = constFilteredComponentMask<true, Components...>();
    auto writeMask = constFilteredComponentMask<false, Components...>();
    assert((readMask | writeMask) == componentMask<Components...>());
    waitForSystems(readMask, writeMask);

    auto tickEntity = [tickFunc, funcArgs...](EntityHandle e) {
        tickFunc(funcArgs..., e.get<Components>()...);
    };

    auto tickAll = [this, parallelFor, tickEntity]() {
        if(parallelFor) {
            forEachEntity<Components...>(tickEntity, std::execution::par);
        } else {
            forEachEntity<Components...>(tickEntity, std::execution::seq);
        }
    };

    if (async) {
        auto system = new RunningSystem(readMask, writeMask);
        system->thread = std::thread(tickAll);

        // I could make the lambda above a member function of World and do something like the following,
        // but that would only compile in gcc and not msvc, so I use the lambda instead
        //void (World::*threadFunc)(RunningSystem*, FuncType, FuncArgs...) = &systemThreadFunction<Components..., FuncArgs..., FuncType>;
        //system->thread = std::thread(threadFunc, this, system, tickFunc, funcArgs...);

        mRunningSystems.emplace_back(system);
    } else {
        tickAll();
    }
}

inline void World::waitForSystems(ComponentMask readMask, ComponentMask writeMask) {
    for (auto& system : mRunningSystems) {
        // if a running system writes to a component we want to read from or write to, wait until it is finished
        if ((system->writeMask & (readMask | writeMask)) > 0) {
            system->thread.join();
            system->threadJoined = true;
        }
    }
    mRunningSystems.erase(
        std::remove_if(mRunningSystems.begin(), mRunningSystems.end(), 
            [](const std::unique_ptr<RunningSystem>& system) {return system->threadJoined; }),
        mRunningSystems.end());
}

inline void World::finishSystems() {
    for (auto& system : mRunningSystems) system->thread.join();
    mRunningSystems.clear();
}

template <typename ComponentType>
ComponentPool<ComponentType>& World::getPool() {
    const auto compId = static_cast<unsigned int>(componentId::get<ComponentType>());
    if (mPools.size() < compId + 1) {
        mPools.resize(compId + 1);
    }
    if(!mPools[compId]) {
        mPools[compId].reset(static_cast<ComponentPoolBase*>(new ComponentPool<ComponentType>()));
    }
    assert(mPools[compId]);
    return *static_cast<ComponentPool<ComponentType>*>(mPools[compId].get());
}

// EntityHandle implementation
template <typename ComponentType, typename... Args>
ComponentType& EntityHandle::add(Args&&... args) {
    return mWorld.addComponent<ComponentType>(mId, std::forward<Args>(args)...);
}

template <typename... Args>
bool EntityHandle::has() const {
    return mWorld.hasComponents<Args...>(mId);
}

template <typename ComponentType>
ComponentType& EntityHandle::get() {
    return mWorld.getComponent<ComponentType>(mId);
}

} // namespace ecs