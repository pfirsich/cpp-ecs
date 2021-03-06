#pragma once

#include <cassert>
#include <limits>
#include <vector>
#include <type_traits>
#include <thread>
#include <atomic>
#include <algorithm>
#include <execution>
#include <tuple>
#include <bitset>
#include <array>

namespace ecs {

using ComponentMask = uint64_t;
static_assert(std::is_unsigned<ComponentMask>::value, "ComponentMask type must be unsigned");
static const ComponentMask ALL_COMPONENTS = std::numeric_limits<ComponentMask>::max();
static const size_t MAX_COMPONENTS = std::numeric_limits<ComponentMask>::digits;

using EntityId = uint32_t;
static const EntityId INVALID_ENTITY = std::numeric_limits<EntityId>::max();

using IndexType = size_t;
static const IndexType MAX_INDEX = std::numeric_limits<IndexType>::max();


namespace componentId {
    static size_t idCounter = 0;

    template <typename ComponentType>
    size_t get() {
        static auto id = idCounter++;
        assert(id < MAX_COMPONENTS);
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

template <typename ComponentType>
class ComponentPool : public ComponentPoolBase {
public:
    ComponentPool() = default;
    ~ComponentPool();
    ComponentPool(const ComponentPool& other) = delete;
    ComponentPool& operator=(const ComponentPool& other) = delete;

    template<typename... Args>
    ComponentType& add(EntityId entityId, Args... args);

    bool has(EntityId entityId) const;

    ComponentType& get(EntityId entityId);

    void remove(EntityId entityId) override;

    static const size_t DEFAULT_BLOCK_SIZE = 64;

private:
    // https://gist.github.com/pfirsich/72ec22c4407013eccfab3a78f2ac7a23
    template <class T>
    static constexpr size_t getBlockSizeImpl(const T* t, ...) {
        return DEFAULT_BLOCK_SIZE;
    }

    template <class T>
    static constexpr typename std::enable_if<!std::is_void<decltype(T::BLOCK_SIZE)>::value, size_t>::type
        getBlockSizeImpl(const T* t, int) {
        return T::BLOCK_SIZE;
    }

    static const size_t BLOCK_SIZE = getBlockSizeImpl(static_cast<ComponentType*>(nullptr), 0);
    static_assert(BLOCK_SIZE > 0);
    static const size_t COMPONENT_SIZE = sizeof(ComponentType);

    static constexpr auto getIndices(EntityId entityId) {
        return std::pair<size_t, size_t>(entityId / BLOCK_SIZE, entityId % BLOCK_SIZE);
    }

    ComponentType* getPointer(size_t blockIndex, size_t componentIndex) {
        assert(mBlocks[blockIndex].data);
        return reinterpret_cast<ComponentType*>(mBlocks[blockIndex].data) + componentIndex;
    }

    void checkBlockUsage(size_t blockIndex);

    struct Block {
        void* data;
        std::bitset<BLOCK_SIZE> occupied;
        Block() : data(nullptr), occupied() {}
    };
    std::vector<Block> mBlocks;
};

template <typename ComponentType>
ComponentPool<ComponentType>::~ComponentPool() {
    for(auto& block : mBlocks) {
        operator delete(block.data);
        block.data = nullptr;
    }
}

template <typename ComponentType>
template <typename... Args>
ComponentType& ComponentPool<ComponentType>::add(EntityId entityId, Args... args) {
    assert(!has(entityId));
    const auto [blockIndex, componentIndex] = getIndices(entityId);

    if(mBlocks.size() < blockIndex + 1) mBlocks.resize(blockIndex + 1);
    auto& block = mBlocks[blockIndex];
    if(!block.data) block.data = operator new(BLOCK_SIZE * COMPONENT_SIZE);
    block.occupied[componentIndex] = true;
    auto component = new(getPointer(blockIndex, componentIndex)) ComponentType(std::forward<Args>(args)...);

    return *component;
}

template <typename ComponentType>
bool ComponentPool<ComponentType>::has(EntityId entityId) const {
    const auto [blockIndex, componentIndex] = getIndices(entityId);
    return mBlocks.size() > blockIndex && mBlocks[blockIndex].occupied[componentIndex];
}

template <typename ComponentType>
ComponentType& ComponentPool<ComponentType>::get(EntityId entityId) {
    assert(has(entityId));
    const auto [blockIndex, componentIndex] = getIndices(entityId);
    return *getPointer(blockIndex, componentIndex);
}

template <typename ComponentType>
void ComponentPool<ComponentType>::remove(EntityId entityId) {
    assert(has(entityId));
    const auto [blockIndex, componentIndex] = getIndices(entityId);
    auto component = getPointer(blockIndex, componentIndex);
    component->~ComponentType();
    mBlocks[blockIndex].occupied[componentIndex] = false;
    checkBlockUsage(blockIndex);
}

template <typename ComponentType>
void ComponentPool<ComponentType>::checkBlockUsage(size_t blockIndex) {
    auto& block = mBlocks[blockIndex];
    if(block.occupied.none()) { // block is unused
        operator delete(block.data);
        block.data = nullptr;
    }
}


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

        EntityIterator() : mList(nullptr), mEntityIndex(MAX_INDEX) {} // singular iterator
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
            // start at -1 and increment to get an invalid iterator if no entity matches
            // is this hackish?
            return ++EntityIterator(this, -1);
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

    ComponentMask getComponentMask(EntityId entityId) const;

    template <typename ComponentType>
    ComponentType& getComponent(EntityId entityId);

    template <typename ComponentType>
    void removeComponent(EntityId entityId);

    bool isValid(EntityId entityId) const {
        assert(entityId < mEntityValid.size());
        return mEntityValid[entityId];
    }

    template <typename... Components, typename... FuncArgs, typename FuncType>
    void tickSystem(bool async, bool parallelFor, FuncType tickFunc, FuncArgs&&... funcArgs);

    void joinSystemThreads();
    void flush(EntityId entityId);
    void flush(); // flush all

    void finishTick() {
        joinSystemThreads();
        flush();
    }

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

    std::vector<ComponentMask> mComponentMasks;
    std::vector<bool> mEntityValid;
    // the free list is a min heap, so that we try to fill lower indices first
    std::priority_queue<EntityId, std::vector<EntityId>, std::greater<>> mEntityIdFreeList;
    std::vector<std::unique_ptr<RunningSystem>> mRunningSystems;
    std::array<std::unique_ptr<ComponentPoolBase>, MAX_COMPONENTS> mPools;
    mutable std::mutex mMutex;

    template <typename ComponentType>
    ComponentPool<ComponentType>& getPool(bool alloc = true);

    void waitForSystems(ComponentMask readMask, ComponentMask writeMask);
};


class EntityHandle {
public:
    ~EntityHandle() = default;
    EntityHandle(const EntityHandle& other) = default;
    EntityHandle& operator=(const EntityHandle& other) = delete;

    void destroy();

    template <typename ComponentType, typename... Args>
    ComponentType& add(Args&&... args);

    template <typename... Args>
    bool has() const;

    template <typename ComponentType, bool addIfNotPresent = false>
    ComponentType& get();

    template <typename ComponentType>
    void remove();

    // has any components = "exists"
    operator bool() const { return mWorld.getComponentMask(mId) > 0; }

    bool operator==(const EntityHandle& other) { return &mWorld == &other.mWorld && mId == other.mId; }
    bool operator!=(const EntityHandle& other) { return !(*this == other); }

    EntityId getId() const { return mId; }
    World& getWorld() const { return mWorld; }

private:
    World& mWorld;
    EntityId mId;

    EntityHandle(World& world, EntityId id) : mWorld(world), mId(id) {}

    friend EntityHandle World::createEntity();
    friend EntityHandle World::getEntityHandle(EntityId);
};

// Implementation

template <typename ComponentType>
ComponentPool<ComponentType>& World::getPool(bool alloc) {
    const auto compId = componentId::get<ComponentType>();
    assert(compId < mPools.size());
    if(alloc && !mPools[compId]) {
        mPools[compId] = std::make_unique<ComponentPool<ComponentType>>();
    }
    assert(mPools[compId]);
    return *static_cast<ComponentPool<ComponentType>*>(mPools[compId].get());
}

template <typename ComponentType, typename... Args>
ComponentType& World::addComponent(EntityId entityId, Args&&... args) {
    std::lock_guard lock(mMutex);
    assert(mComponentMasks.size() > entityId);
    assert(!hasComponents<ComponentType>(entityId));
    mComponentMasks[entityId] |= componentMask<ComponentType>();
    return getPool<ComponentType>().add(entityId, std::forward<Args>(args)...);
}

template <typename... Args>
bool World::hasComponents(EntityId entityId) const {
    return hasComponents(entityId, componentMask<Args...>());
}

template <typename ComponentType>
ComponentType& World::getComponent(EntityId entityId) {
    assert(hasComponents<ComponentType>(entityId));
    // make getPool not alloc, so we don't have to protect getComponent with the mutex
    // this should never trigger an allocation anyways, since we assert hasComponent above,
    // so this is just an extra safety measure
    return getPool<typename std::remove_const<ComponentType>::type>(false).get(entityId);
}

template <typename ComponentType>
void World::removeComponent(EntityId entityId) {
    std::lock_guard lock(mMutex);
    getPool<ComponentType>().remove(entityId);
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
void World::tickSystem(bool async, bool parallelFor, FuncType tickFunc, FuncArgs&&... funcArgs) {
    static_assert(!(... || std::is_reference<Components>::value), "Component types must not be references");
    static constexpr auto funcValid = std::is_invocable_r<void, FuncType, FuncArgs..., Components&...>::value;
    static constexpr auto funcValidWithEntityHandle = std::is_invocable_r<void, FuncType, EntityHandle, FuncArgs..., Components&...>::value;
    static_assert(funcValid || funcValidWithEntityHandle, "Tick function has invalid signature");

    const auto readMask = constFilteredComponentMask<true, Components...>();
    const auto writeMask = constFilteredComponentMask<false, Components...>();
    assert((readMask | writeMask) == componentMask<Components...>());
    waitForSystems(readMask, writeMask);

    // When you use `if constexpr` in lambdas, MSVC will just roll over dead and do all kinds of crazy things (gcc and clang are fine though)
    // therefore I need to use std::function here. When this is fixed, I can just move the if constexpr into the lambda.
    // It seems a simplified version of this generates the same code on clang and way slower code on gcc (https://godbolt.org/z/QAhhx8)
    // On MSVC I can not measure any performance difference to before adding the `funcValidWithEntityHandle` feature.
    std::function<void(EntityHandle)> tickEntity;
    if constexpr(funcValidWithEntityHandle) {
        tickEntity = [tickFunc, &funcArgs...](EntityHandle e) {
            tickFunc(e, std::forward<FuncArgs>(funcArgs)..., e.get<Components>()...);
        };
    } else {
        tickEntity = [tickFunc, &funcArgs...](EntityHandle e) {
            tickFunc(std::forward<FuncArgs>(funcArgs)..., e.get<Components>()...);
        };
    }

    auto tickAll = [this, parallelFor, tickEntity]() {
        if(parallelFor) {
            forEachEntity<Components...>(tickEntity, std::execution::par);
        } else {
            forEachEntity<Components...>(tickEntity, std::execution::seq);
        }
    };

    if (async) {
        auto system = std::make_unique<RunningSystem>(readMask, writeMask);
        system->thread = std::thread(tickAll);

        // I could make the lambda above a member function of World and do something like the following,
        // but that would only compile in gcc and not msvc, so I use the lambda instead
        //void (World::*threadFunc)(RunningSystem*, FuncType, FuncArgs...) = &systemThreadFunction<Components..., FuncArgs..., FuncType>;
        //system->thread = std::thread(threadFunc, this, system, tickFunc, funcArgs...);

        mRunningSystems.emplace_back(std::move(system));
    } else {
        tickAll();
    }
}

template <typename ComponentType, typename... Args>
ComponentType& EntityHandle::add(Args&&... args) {
    return mWorld.addComponent<ComponentType>(mId, std::forward<Args>(args)...);
}

template <typename... Args>
bool EntityHandle::has() const {
    return mWorld.hasComponents<Args...>(mId);
}

template <typename ComponentType, bool addIfNotPresent>
ComponentType& EntityHandle::get() {
    if constexpr(addIfNotPresent) {
        static_assert(std::is_default_constructible<ComponentType>(), "Component type must be default constructible.");
        if(!mWorld.hasComponents<ComponentType>(mId)) mWorld.addComponent<ComponentType>(mId);
    }
    return mWorld.getComponent<ComponentType>(mId);
}

template <typename ComponentType>
void EntityHandle::remove() {
    mWorld.removeComponent<ComponentType>(mId);
}

} // namespace ecs
