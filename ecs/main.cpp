#include <iostream>

#include "ecs.hpp"

struct Position {
    float x, y;

    Position() : x(), y() {}
    Position(float x, float y) : x(x), y(y) {}
};

struct Velocity {
    float x, y;

    Velocity() : x(), y() {}
    Velocity(float x, float y) : x(x), y(y) {}
};

void physicsSystem(float dt, Position& p, const Velocity& v) {
    p.x += v.x * dt;
    p.y += v.y * dt;
}

void tickPhysicsSystem(ecs::World& world, float dt) {
    for (auto e : world.entitiesWith<Position, Velocity>()) {
        auto& p = e.get<Position>();
        const auto& v = e.get<Velocity>();
        p.x += v.x * dt;
        p.y += v.y * dt;
    }
}

int main(int argc, char** argv) {
    ecs::World world;
    auto e = world.entity();
    e.add<Position>(0.0f, 0.0f);
    e.add<Velocity>(0.0f, 0.0f);
    //tickPhysicsSystem(world, 1.0f);
    //world.tickSystem<Position, const Velocity, float, void(float, Position&, const Velocity&)>(physicsSystem, 1.0f);
    world.tickSystem<Position, const Velocity>(physicsSystem, 1.0f);
}