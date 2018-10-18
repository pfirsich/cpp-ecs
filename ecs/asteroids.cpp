#define _USE_MATH_DEFINES
#include <cmath>
#include <vector>
#include <random>
#include <iostream>

#include <SFML/Window.hpp>
#include <SFML/Graphics.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

#include "ecs.hpp"

float randf(float min = 0.f, float max = 1.f) {
    static std::default_random_engine engine;
    static std::uniform_real_distribution<float> dist(0.f, 1.f);
    return dist(engine) * (max - min) + min;
}

constexpr float bool2Float(bool v, float min = 0.f, float max = 1.f) {
    return v ? max : min;
}

struct BaseController {
    virtual ~BaseController() = default;
    virtual float thrust() const = 0; // [0, 1]
    virtual float steer() const = 0; // [-1, 1]
    virtual bool shoot() const = 0;
};

struct KeyboardController : public BaseController {
    ~KeyboardController() = default;

    float thrust() const override {
        return bool2Float(sf::Keyboard::isKeyPressed(sf::Keyboard::Up));
    }

    float steer() const override {
        return bool2Float(sf::Keyboard::isKeyPressed(sf::Keyboard::Right))
            - bool2Float(sf::Keyboard::isKeyPressed(sf::Keyboard::Left));
    }

    bool shoot() const override {
        return sf::Keyboard::isKeyPressed(sf::Keyboard::Space);
    }
};

struct CTransform {
    glm::vec2 position, scale;
    float angle;
    CTransform(float x, float y, float angle = 0.f) : position(x, y), angle(angle), scale(1.f, 1.f) {}
};

struct CVelocity {
    glm::vec2 value;
    CVelocity(float x = 0.f, float y = 0.f) : value(x, y)  {}
};

struct CFriction {
    float value;
    CFriction(float friction) : value(friction) {}
};

struct CMaxSpeed {
    float value;
    CMaxSpeed(float maxSpeed) : value(maxSpeed) {}
};

template <typename DrawableType>
struct CRender {
    DrawableType drawable;
    template <typename... Args> CRender(Args... args) : drawable(std::forward<Args>(args)...) {}
};

struct CController {
    std::unique_ptr<BaseController> controller;
    CController(std::unique_ptr<BaseController> controller) : controller(std::move(controller)) {}
};

struct CFlight {
    float rotationSpeed, acceleration;
    CFlight(float rotationSpeed, float acceleration) : rotationSpeed(rotationSpeed), acceleration(acceleration) {}
};

void flightSystem(float dt, const CController& controller, const CFlight& flight, CTransform& transform, CVelocity& velocity) {
    const auto ctrl = controller.controller.get();

    transform.angle += ctrl->steer() * flight.rotationSpeed * dt;

    const auto shipDir = glm::vec2(std::cos(transform.angle), std::sin(transform.angle));
    velocity.value += shipDir * ctrl->thrust() * flight.acceleration * dt;
}

void maxSpeedSystem(CVelocity& velocity, const CMaxSpeed& maxSpeed) {
    const auto shipSpeed = glm::length(velocity.value);
    if(shipSpeed > maxSpeed.value) velocity.value *= maxSpeed.value / shipSpeed;
}

void frictionSystem(float dt, CVelocity& velocity, const CFriction& friction) {
    velocity.value -= velocity.value * friction.value * dt;
}

void physicsIntegrationSystem(float dt, const glm::vec2& winSize, CTransform& transform, const CVelocity& velocity) {
    transform.position += velocity.value * dt;
    if(transform.position.x < 0) transform.position.x += winSize.x;
    if(transform.position.y < 0) transform.position.y += winSize.y;
    if(transform.position.x > winSize.x) transform.position.x -= winSize.x;
    if(transform.position.y > winSize.y) transform.position.y -= winSize.y;
}

template <typename DrawableType>
void renderSystem(sf::RenderWindow& window, const glm::vec2& winSize, CRender<DrawableType>& render, const CTransform& transform) {
    DrawableType& drawable = render.drawable;

    drawable.setPosition(transform.position.x, transform.position.y);
    drawable.setRotation(transform.angle / glm::pi<float>() * 180.f + 90.f);

    window.draw(drawable);

    auto drawOffset = [&window, &drawable](float x, float y) {
        window.draw(drawable, sf::Transform().translate(x, y));
    };

    const auto bBox = drawable.getGlobalBounds();
    const auto topLeft = glm::vec2(bBox.left, bBox.top);
    const auto botRight = glm::vec2(bBox.left + bBox.width, bBox.top + bBox.height);
    const auto topRight = glm::vec2(botRight.x, topLeft.y);
    const auto botLeft = glm::vec2(topLeft.x, botRight.y);
    if(topLeft.x < 0) drawOffset(winSize.x, 0.f);
    if(topLeft.y < 0) drawOffset(0.f, winSize.y);
    if(botRight.x > winSize.x) drawOffset(-winSize.x, 0.f);
    if(botRight.y > winSize.y) drawOffset(0.f, -winSize.y);
    // diagonals
    if(topLeft.x < 0 && topLeft.y < 0) drawOffset(winSize.x, winSize.y);
    if(topLeft.x < 0 && botLeft.y > winSize.y) drawOffset(winSize.x, -winSize.y);
    if(botRight.x > winSize.x && topRight.y < 0) drawOffset(-winSize.x, winSize.y);
    if(botRight.x > winSize.x && botRight.y > winSize.y) drawOffset(-winSize.x, -winSize.y);
}

const auto shipSize = 25.f;
const auto shipAccel = 100.f;
const auto shipRotSpeed = glm::pi<float>() * 0.6f;
const auto shipMaxSpeed = 500.f;
const auto shipFriction = 0.4f;

int main(int argc, char** argv) {
    sf::ContextSettings settings;
    settings.antialiasingLevel = 8;

    const auto winSize = sf::Vector2u(1366, 768);
    const auto winSizef = glm::vec2(winSize.x, winSize.y);
    sf::RenderWindow window(sf::VideoMode(winSize.x, winSize.y), "Asteroids", sf::Style::Default, settings);

    ecs::World world;

    auto ship = world.createEntity();
    ship.add<CTransform>(winSize.x/2.f, winSize.y/2.f);
    ship.add<CVelocity>(0.f, 0.f);
    ship.add<CMaxSpeed>(shipMaxSpeed);
    ship.add<CFriction>(shipFriction);
    ship.add<CRender<sf::CircleShape>>(shipSize, 3).drawable.setOrigin(shipSize, shipSize);
    ship.add<CController>(std::make_unique<KeyboardController>());
    ship.add<CFlight>(shipRotSpeed, shipAccel);

    for(int i = 0; i < 4; ++i) {
        auto asteroid = world.createEntity();
        asteroid.add<CTransform>(randf(0.f, winSizef.x), randf(0.f, winSizef.y), randf(0.f, 2.f * glm::pi<float>()));
        const auto angle = randf(0.f, 2.f * glm::pi<float>());
        const auto speed = randf(150.f, 300.f);
        asteroid.add<CVelocity>(speed * std::cos(angle), speed * std::sin(angle));
        const auto size = randf(10.f, 120.f);
        asteroid.add<CRender<sf::CircleShape>>(size, 9).drawable.setOrigin(size, size);
    }

    world.flush();

    sf::Clock clock, dtClock;
    auto fpsCounter = 0;
    auto lastFpsUpdate = clock.getElapsedTime();
    while(window.isOpen()) {
        sf::Event event;
        while (window.pollEvent(event)) {
            if (event.type == sf::Event::Closed) window.close();
        }

        // update
        const auto dt = dtClock.restart().asSeconds();

        world.tickSystem<const CController, const CFlight, CTransform, CVelocity>(false, false, flightSystem, dt);
        world.tickSystem<CVelocity, const CMaxSpeed>(false, true, maxSpeedSystem);
        world.tickSystem<CVelocity, const CFriction>(false, true, frictionSystem, dt);
        world.tickSystem<CTransform, const CVelocity>(false, true, physicsIntegrationSystem, dt, winSizef);

        // draw
        window.clear(sf::Color::Black);
        world.tickSystem<CRender<sf::CircleShape>, const CTransform>(false, false,
            renderSystem<sf::CircleShape>, window, winSizef);
        window.display();

        world.finishTick();

        // count fps and update window title
        fpsCounter++;
        const auto fpsUpdateFreq = 1.f;
        if((clock.getElapsedTime() - lastFpsUpdate).asSeconds() > fpsUpdateFreq) {
            const auto fps = fpsCounter / fpsUpdateFreq;
            window.setTitle("Android - FPS: " + std::to_string(fps));
            lastFpsUpdate = clock.getElapsedTime();
            fpsCounter = 0;
        }
    }

    return 0;
}
