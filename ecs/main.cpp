#include <iostream>

#include "ecs.hpp"

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
