# C++ ECS
Inspired by ideas presented in Allan Deutsch's C++Now 2018 talk [Game Engine API Design](https://www.youtube.com/watch?v=W3ViIBnTTKA), I started writing an ECS for Modern C++. One personal goal was to "modernize" my C++ as an exercise.

## Inspiration
The main idea that sparked my interest was to simplify the interface of System definitions.

Naively one would define a system like this:
(taken from [the talk at ~1:12:00](https://youtu.be/W3ViIBnTTKA?t=4333))
```cpp
class VelocitySystem {
public:
    void Update(float dt) {
        for(auto& e : GetEntitiesWith<Transform, const RigidBody>) {
            const RigidBody& rb = e.Get<const RigidBody>();
            Transform& tf = e.Get<Transform>();
            tf.position += rb.velocity *dt;
        }
    }
};
```
But most of this code is boilerplate and the minimum amount of code that is necessary would look more like this (also taken from the talk):
```cpp
void Update(float dt, Transform& tf, const RigidBody& rb) {
    tf.position += rb.velocity * dt;
}
```
__Which also gives us the opportunity to enable optimizations automatically that process entities in parallel or even use the const qualifiers to deduce which components are accessed read-only, so that the systems can be executed in parallel as well.__

Additional data required for the system to update may be passed via extra arguments, as shown above, but other approaches are possible too. One option would be to introduce singleton components attached to some special entity that hold this data. Another option (the one suggested in the talk) would be make them member variables of a class that contains the system update function:
```cpp
struct ParallelVelocitySystem {
    void Process(Transform& tf, const RigidBody& rb) const {
        tf.position += rb.velocity * Dt;
    }
    float Dt = 1.f / 60.f;
};
```

## Implementation
I learned a lot looking at the source code of [EntityX](https://github.com/alecthomas/entityx), so I think it should be mentioned!

A `World` is an entity container that can hold a number of entities. Most of the functionality of the ECS is implemented somewhere in the `World` class.

The entity itself only consists of a pointer to a `World` instance and an integer id. In fact there is no `Entity` class, but only an `EntityHandle`, since an entity is something abstract that doesn't really occupy any memory itself (because all data is stored in components).

Inside the world for each entity a bit mask is stored that encodes which components are attached to that entity. Currently, this mask is a `uint64_t`, limiting the maximum number of components to 64, but at a price, this could be replaced with a `std::bitset` or some custom-tailored optimized solution (especially since the real maximum number of components will probably not exceed 128 - GCC and Clang support `__int128` and `boost::multiprecision` has `int128_t`!).

#### Example:
```cpp
struct Position {
    float x, y;
    Position(float x, float y) : x(x), y(y) {}
};

struct Velocity {
    float x, y;
    Velocity(float x, float y) : x(x), y(y) {}
};

void physicsSystem(float dt, Position& p, const Velocity& v) {
    p.x += v.x * dt;
    p.y += v.y * dt;
}

int main(int argc, char** argv) {
    ecs::World world;

    auto e = world.createEntity();
    e.add<Position>(0.0f, 0.0f);
    e.add<Velocity>(0.0f, 0.0f);
    world.flush();

    while(true) {
        world.tickSystem<Position, const Velocity>(true, true, physicsSystem, 1.0f);
        world.finishTick();
    }
}
```
Currently the components have to passed to `World::tickSystem` explicitely as template parameters and they can not be deduced from the function that is passed to `World::tickSystem`. I have not yet found a way to make this deduction possible if I want to pass the additional parameters to tickSystem as well.

A read and a write mask are built from the components that are passed as const or non-const template arguments respectively and `World::tickSystem` will wait for systems that write to the components the tick function wants to access until it executes the tick function.

The first two parameters indicate whether the function should be executed asynchronously at all (`false` will execute it in the main thread and `World::tickSystem` will block until the tick function terminates). The second parameter indicates whether the system considers entity interactions, meaning that it accesses multiple entities at once, which would make it unsafe to parallelize the for loop over the entities. If `true` the C++17 execution policy `std::execution::par` is used with `std::for_each` to iterate over the entities in parallel. The third argument is the tick function to be executed for each entity and the remaining arguments are forwarded to the tick function as-is.

`World::finishTick` flushes all newly created entities and joins all system threads.

### Entity Creation & Deletion
Ids of removed entities are saved in a free list and reused, when a new entity is created. Therefore I need to make sure that entities are not processed by systems prematurely. Especially if that behaviour is possibly non-deterministic/pseudo-random - if you are currently iterating entities and adding a new one, entity id reuse may add it into the range that is currently being processed and will therefore process the new entity too, but it may also just add the entity to the end, which is not part of the currently iterated range. My approach was to introduce a bitfield (`std::vector<bool>`) that marks newly created entities as invalid (which will result in them being skipped during iteration). They may be "flushed" (marked as valid) manually via `World::flush` or they will be flushed automatically in `World::finishTick`, which should be called at the end of each tick.

Another problem related to entity creation is that systems executed in parallel might want to create entities at the same time. Currently I am protecting the related data structures with a mutex, but like the components the system accesses, it would be nice to move this information to the type system and somehow encode which systems even create or destroy entities at all. Similarly it would be nice to encode in the types whether systems deal with entity interactions (and therefore access entities that are not the currently processed entity) to decide whether a parallel for loop over the entities can be safe. As stated above, currently both these properties are stated explicitely as boolean parameters to `World::tickSystem` and are therefore a source of possible errors that might be tricky to debug.

### Events
Commonly in ECS based design event systems are employed to have systems interact (which is required in some way for meaningful games). Since systems may be running in parallel, the event system can not be some sort of Signal/Slot implementation in which an emitted event will simply forward a function call to the subscribers. One reason against this is that a system function that accesses some set of components may emit an event that another system subscribed to, that will access a completely different set of components. This will falsify our judgment about which systems can be parallelized safely. Therefore the events would probably have to be  objects that are buffered in a queue, which is processed by the systems.

It is clear that this roughly resembles the workings of the ECS itself with the events being very similar to components and the listeners very similar to systems. This motivates to turn events into components and event handlers into systems. Emitting an event would then consist of adding a special component that contains the event data. To allow multiple events of the same type the component may either contain a list of these event datums or for each event a new entity must be created. Event handlers are just regular systems that operate on the event components and at the end of the tick, all event components (or entities) are removed.

### Component Storage
A major reason for the rising popularity of ECS based design is that it can be implemented in very cache friendly manner, which is highly relevant in a time where main memory access is a significant bottleneck for many applications.

In essence pieces of code (systems) only touch the data they really care about (the components) instead of touching some Entity object, which may contain a lot more data that is entirely irrelevant to them, but still pollutes the cache. This improves how much of the data that gets pulled into cache is actually used. Also components may be stored in entity traversal order, which improves cache friendliness even more.

As stated earlier, an entity is just a pointer to a `World` instance and an integer id. One approach would be to allocate a big array with `MAX_ENTITIES` entries for each component and have the entity id be an index into these arrays. This is very wasteful of memory though (to the point of being completely unreasonable for larger indie games or small-ish AAA games) and may not be cache friendly if there are a lot of holes between each component that is actually in use. It is also horribly expensive to add a new component, even if it might only be used for a single entity.

We might also just have a `std::vector` of components and an index map that maps entity indices to component indices, but, while it is memory efficient, it is potentially *very* unfriendly to the cache, because we might end up hopping around in the component array "randomly".

I think an "AAA-grade" ECS would probably have a notion of components sets/groups (as shown in Allan Deutsch's talk when talking about Blizzard's ECS), that are stored interleaved in memory and in entity traversal order, which should maximize cache friendliness (for those systems that operate on these specific sets) and minimize memory usage. But this is **not easy** to implement and requires thought being put into which components should be grouped together, i.e. which systems are most performance critical. In general I think such an approach is more "high-maintenance" and therefore not a great fit for indie developers, which should try to minimize friction to facilitate easy experimentation.

The approach that I chose for my ECS is one that is also used by EntityX and a variation of the first approach I described with each component having an array that is indexed by the entity id, but instead a "paged array" is used, in which not the whole range of the array is contiguous in memory, but only segments of it. It is essentially an interpolation of a linked list (best memory usage, worst cache friendliness) and an array (worst memory usage, best cache friendliness if most indices are used) as a list of memory blocks that each store a number of elements.

I added a compile-time way to configure the block size via the definition of a static variable inside the component class:
```cpp
struct PositionComponent {
    static const size_t BLOCK_SIZE = 2048; // number of components
    float x, y;
}
```
For components that are used by a almost all entities, a block size close to the maximum number of entities should be chosen (holes should be few, cache misses at block boundaries are minimal). For components that are only used by a very small number of entities, a block size close to 1 should be used (every component access will most likely be a cache miss, but it won't happen a lot, because we don't iterate over many components and we only take up the space we actually need).

This is an insightful (though somewhat broken - images are missing for me) article about data structures for component storage: http://t-machine.org/index.php/2014/03/08/data-structures-for-entity-systems-contiguous-memory/

## Problems / ToDo
I will recap the ones I listed above:

* Deduce components from tick function type
* Encode in the types whether systems create/destroy entities and whether systems consider entity interactions instead of passing two bools to `World::tickSystem`
* Make a proof of concept of the event system as components/systems.

And add:

* Make an actual game with this (very important)
* Creating/joining threads for each call to `World::tickSystem` might be expensive. I should compare it with a thread pool.
* I suspect adding a component/removing in a system that is not part of the function signature will mess up systems that do have it in the function signature, because they might end up running in parallel, even though the first system actually essentially writes to that component, the other one accesses. This might be a big deal. One option is to invalidate components or have a separate component mask that is edited during the tick and only applied at the end (does not work for removal). For component removal this problem could be solved by deferring entity/component to the end of the frame as well.
