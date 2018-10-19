#include "ecs.hpp"

namespace ecs {

World::EntityIterator& World::EntityIterator::operator++() {
    mEntityIndex++;
    const auto& world = mList->world;
    while (mEntityIndex < world.getEntityCount()
           && (!world.isValid(mEntityIndex)
           || !world.hasComponents(mEntityIndex, mList->mask))) mEntityIndex++;
    if(mEntityIndex >= world.getEntityCount()) {
        mEntityIndex = MAX_INDEX;
    }
    return *this;
}

World::EntityIterator World::EntityIterator::operator++(int) {
    EntityIterator ret(*this);
    operator++();
    return ret;
}

bool World::EntityIterator::operator==(const EntityIterator& other) const {
    return mList == other.mList && mEntityIndex == other.mEntityIndex;
}

bool World::EntityIterator::operator!=(const EntityIterator& other) const {
    return !operator==(other);
}

EntityHandle World::EntityIterator::operator*() const {
    return mList->world.getEntityHandle(mEntityIndex);
}

EntityHandle World::createEntity() {
    std::lock_guard lock(mMutex);
    if(mEntityIdFreeList.empty()) {
        mComponentMasks.push_back(0);
        mEntityValid.push_back(false);
        assert(mComponentMasks.size() == mEntityValid.size());
        return EntityHandle(*this, mComponentMasks.size() - 1);
    } else {
        const auto entityId = mEntityIdFreeList.top();
        mEntityIdFreeList.pop();
        assert(entityId < mComponentMasks.size() && entityId < mEntityValid.size());
        mComponentMasks[entityId] = 0;
        mEntityValid[entityId] = false;
        return EntityHandle(*this, entityId);
    }
}

EntityHandle World::getEntityHandle(EntityId entityId) {
    assert(entityId < mComponentMasks.size()); // entity has existed
    return EntityHandle(*this, entityId);
}

void World::destroyEntity(EntityId entityId) {
    std::lock_guard lock(mMutex);
    assert(mComponentMasks.size() >= entityId); // entity exists
    for(size_t compId = 0; compId < mPools.size(); ++compId) {
        const auto hasComponent = (mComponentMasks[entityId] & (1ull << compId)) > 0;
        if(mPools[compId] && hasComponent) mPools[compId]->remove(entityId);
    }
    mComponentMasks[entityId] = 0;
    mEntityIdFreeList.push(entityId);
}

void World::flush() {
    mEntityValid.assign(mEntityValid.size(), true);
}

void World::flush(EntityId entityId) {
    assert(entityId < mEntityValid.size());
    mEntityValid[entityId] = true;
}

bool World::hasComponents(EntityId entityId, ComponentMask mask) const {
    assert(mComponentMasks.size() > entityId);
    return (mComponentMasks[entityId] & mask) == mask;
}

ComponentMask World::getComponentMask(EntityId entityId) const {
    assert(mComponentMasks.size() > entityId);
    return mComponentMasks[entityId];
}

void World::waitForSystems(ComponentMask readMask, ComponentMask writeMask) {
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

void World::joinSystemThreads() {
    for (auto& system : mRunningSystems) system->thread.join();
    mRunningSystems.clear();
}

// EntityHandle implementation
void EntityHandle::destroy() {
    mWorld.destroyEntity(mId);
    mId = INVALID_ENTITY;
}

} // namespace ecs
