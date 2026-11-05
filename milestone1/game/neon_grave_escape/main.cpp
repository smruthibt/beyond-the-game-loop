#include "engine/Collision.hpp"
#include "engine/Engine.hpp"
#include "engine/Entity.hpp"
#include "engine/Input.hpp"
#include "engine/Physics.hpp"
#include "engine/Scaling.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>

namespace {

constexpr float PI = 3.14159265358979323846f;
constexpr float GAME_DESIGN_WIDTH =
    static_cast<float>(engine::DEFAULT_WINDOW_WIDTH);
constexpr float GAME_DESIGN_HEIGHT =
    static_cast<float>(engine::DEFAULT_WINDOW_HEIGHT);
constexpr float GRAVITY = 2625.0f;
constexpr float MOVE_ACCELERATION = 2250.0f;
constexpr float AIR_ACCELERATION = 1380.0f;
constexpr float MOVE_DECELERATION = 2850.0f;
constexpr float MAX_MOVE_SPEED = 495.0f;
constexpr float JUMP_SPEED = 1155.0f;
constexpr float MAX_FALL_SPEED = 1440.0f;
constexpr float ENEMY_PATROL_SPEED = 138.0f;
constexpr float ENEMY_SPEED_PER_RUNE = 15.0f;
constexpr float INVULNERABILITY_SECONDS = 0.95f;
constexpr float STOMP_BOUNCE_SPEED = 780.0f;
constexpr float JUMP_BUFFER_SECONDS = 0.12f;
constexpr std::size_t PLATFORM_COUNT = 14;
constexpr std::size_t HAZARD_COUNT = 12;
constexpr std::size_t RUNE_COUNT = 6;
constexpr std::size_t DISAPPEARING_PLATFORM_INDEX = 8;
constexpr std::size_t PATROL_PLATFORM_INDEX = 6;
constexpr float SENTINEL_BASE_X = 4380.0f;
constexpr int MAX_DEATHS = 10;
constexpr float RESPAWN_COUNTDOWN_SECONDS = 3.0f;

struct Firefly {
    engine::Vec2 origin;
    float phase = 0.0f;
    float speed = 1.0f;
    float radius = 10.0f;
};

struct TrailParticle {
    engine::Vec2 position;
    engine::Vec2 velocity;
    SDL_Color color = {91, 242, 224, 255};
    float life = 0.0f;
    float maxLife = 0.0f;
    float size = 3.0f;
    bool active = false;
};

void FillCircle(SDL_Renderer *renderer, float centerX, float centerY,
                float radius, SDL_Color color) {
    if (radius <= 0.0f) {
        return;
    }
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    const int extent = static_cast<int>(std::ceil(radius));
    for (int y = -extent; y <= extent; ++y) {
        const float inside = radius * radius - static_cast<float>(y * y);
        if (inside < 0.0f) {
            continue;
        }
        const float halfWidth = std::sqrt(inside);
        SDL_RenderLine(renderer, centerX - halfWidth, centerY + static_cast<float>(y),
                       centerX + halfWidth, centerY + static_cast<float>(y));
    }
}

void FillTriangle(SDL_Renderer *renderer, const SDL_FPoint &a,
                  const SDL_FPoint &b, const SDL_FPoint &c, SDL_FColor color) {
    const std::array<SDL_Vertex, 3> vertices = {{
        {a, color, {0.0f, 0.0f}},
        {b, color, {0.0f, 0.0f}},
        {c, color, {0.0f, 0.0f}}
    }};
    constexpr std::array<int, 3> indices = {{0, 1, 2}};
    SDL_RenderGeometry(renderer, nullptr, vertices.data(),
                       static_cast<int>(vertices.size()), indices.data(),
                       static_cast<int>(indices.size()));
}

void RequestQuit() {
    SDL_Event quitEvent{};
    quitEvent.type = SDL_EVENT_QUIT;
    SDL_PushEvent(&quitEvent);
}

bool SetWindowSize(engine::Engine &app, int width, int height) {
    return SDL_SetWindowSize(app.GetWindow(), width, height);
}

class NeonGroveEscapeGame {
public:
    explicit NeonGroveEscapeGame(engine::Engine &engine)
        : app(engine), viewportWidth(GAME_DESIGN_WIDTH),
          worldWidth(GAME_DESIGN_WIDTH * 3.125f),
          worldHeight(GAME_DESIGN_HEIGHT) {
        SDL_SetRenderDrawBlendMode(app.GetRenderer(), SDL_BLENDMODE_BLEND);
        engine::Physics::SetGravity(GRAVITY);
        BuildRequiredEntities();
        BuildAtmosphere();
        RestartRun();
    }

    void Update(float dt) {
        ambientTime += dt;
        UpdateFirefliesAndParticles(dt);
        collisionFlash = std::max(0.0f, collisionFlash - dt * 2.8f);
        jumpPulse = std::max(0.0f, jumpPulse - dt * 3.5f);
        landingPulse = std::max(0.0f, landingPulse - dt * 4.5f);
        invulnerability = std::max(0.0f, invulnerability - dt);
        scalingNotice = std::max(0.0f, scalingNotice - dt);
        lockedNotice = std::max(0.0f, lockedNotice - dt);
        jumpBuffer = std::max(0.0f, jumpBuffer - dt);

        if (engine::Input::IsKeyJustPressed(SDL_SCANCODE_ESCAPE)) {
            RequestQuit();
        }
        if (engine::Input::IsKeyJustPressed(SDL_SCANCODE_T)) {
            ToggleScalingDemo();
        }
        if (engine::Input::IsKeyJustPressed(SDL_SCANCODE_R)) {
            RestartRun();
            return;
        }

        if (gameOver) {
            if (engine::Input::IsKeyJustPressed(SDL_SCANCODE_SPACE)) {
                RestartRun();
            }
            return;
        }
        if (awaitingDeathContinue) {
            if (engine::Input::IsKeyJustPressed(SDL_SCANCODE_SPACE)) {
                StartRespawnCountdown();
            }
            return;
        }
        if (respawnCountdown > 0.0f) {
            UpdateRespawnCountdown(dt);
            return;
        }
        if (escaped) {
            return;
        }

        runTime += dt;
        UpdateDisappearingPlatform();
        UpdatePatrolEnemy(dt);
        UpdateSentinelEnemy(dt);

        if (escapeSequence > 0.0f) {
            UpdateEscapeSequence(dt);
            UpdatePlayerAppearance();
            UpdateCamera(dt);
            return;
        }

        const engine::Vec2 previousPosition = player.position;
        ApplyPlayerInput(dt);
        const float velocityBeforePhysics = player.velocity.y;
        engine::Physics::Update(player, dt);
        player.velocity.y = std::min(player.velocity.y, MAX_FALL_SPEED);

        grounded = false;
        ResolvePlatformCollision(previousPosition, velocityBeforePhysics);
        if (ResolveHazardCollisions() || ResolveWorldBounds()) {
            return;
        }
        ResolveEnemyCollision(previousPosition);
        CollectRunes();
        ResolveExitCollision();
        UpdatePlayerAppearance();
        EmitMovementTrail(dt);
        UpdateCamera(dt);
    }

    void Render() const {
        SDL_Renderer *renderer = app.GetRenderer();
        const float scale = GetWorldScale();
        DrawTwilightGradient(renderer);
        DrawSpores(renderer, scale);
        DrawMoon(renderer, scale);
        DrawCavernLayers(renderer, scale);
        DrawFungalRuins(renderer, scale);
        DrawFireflies(renderer, scale);
        DrawExitPortal(renderer, scale);
        DrawGroundPlatform(renderer, scale);
        DrawHazards(renderer, scale);
        DrawRunes(renderer, scale);
        DrawCheckpoint(renderer, scale);
        DrawTrail(renderer, scale);
        DrawPatrolEnemy(renderer, scale);
        DrawSentinelEnemy(renderer, scale);
        DrawPlayer(renderer, scale);
        DrawHud(renderer);
    }

    bool RunLogicVerification() {
        bool passed = true;
        const auto check = [&](bool condition, const char *message) {
            if (!condition) {
                std::cerr << "FAILED: " << message << '\n';
                passed = false;
            }
        };

        check(viewportWidth == static_cast<float>(engine::DEFAULT_WINDOW_WIDTH) &&
                  worldHeight == static_cast<float>(engine::DEFAULT_WINDOW_HEIGHT),
              "gameplay uses the same 1920x1080 coordinate system as the engine window");
        check(worldWidth == 6000.0f,
              "the scrolling level preserves its multi-screen width in Full HD units");

        patrolEnemy.position.x = patrolLeftEndpoint;
        patrolDirection = 1.0f;
        float previousDirection = patrolDirection;
        int reversals = 0;
        for (int frame = 0; frame < 1800; ++frame) {
            ambientTime += 1.0f / 120.0f;
            UpdatePatrolEnemy(1.0f / 120.0f);
            if (patrolDirection != previousDirection) {
                ++reversals;
                previousDirection = patrolDirection;
            }
        }
        check(reversals >= 4,
              "patrol enemy repeatedly reverses at both fixed endpoints");
        check(patrolEnemy.position.x >= patrolLeftEndpoint &&
                  patrolEnemy.position.x <= patrolRightEndpoint,
              "patrol enemy remains inside its predefined path");

        sentinelEnemy.position.y = 555.0f;
        sentinelDirection = 1.0f;
        previousDirection = sentinelDirection;
        reversals = 0;
        for (int frame = 0; frame < 500; ++frame) {
            ambientTime += 1.0f / 120.0f;
            UpdateSentinelEnemy(1.0f / 120.0f);
            if (sentinelDirection != previousDirection) {
                ++reversals;
                previousDirection = sentinelDirection;
            }
        }
        check(reversals >= 2,
              "vertical sentinel repeatedly reverses on its fixed path");

        float largestGap = routePlatforms.front().position.x -
                           (groundPlatform.position.x + groundPlatform.size.x);
        for (std::size_t i = 1; i < routePlatforms.size(); ++i) {
            largestGap = std::max(
                largestGap,
                routePlatforms[i].position.x -
                    (routePlatforms[i - 1].position.x +
                     routePlatforms[i - 1].size.x));
        }
        check(largestGap <= 75.0f,
              "every required jump has a fair horizontal gap");
        check(routePlatforms.size() >= 12 &&
                  exitPortal.position.x > viewportWidth * 2.5f,
              "the continuous route spans multiple screens and many jumps");
        const float jumpRise = JUMP_SPEED * JUMP_SPEED / (2.0f * GRAVITY);
        check(jumpRise > 240.0f,
              "the grounded jump arc comfortably exceeds the largest platform rise");

        RestartRun();
        player.position.y = groundPlatform.position.y - player.size.y - 27.0f;
        player.velocity.y = 270.0f;
        grounded = false;
        for (int frame = 0; frame < 120 && !grounded; ++frame) {
            const engine::Vec2 previousPosition = player.position;
            const float previousVerticalVelocity = player.velocity.y;
            engine::Physics::Update(player, 1.0f / 120.0f);
            player.velocity.y = std::min(player.velocity.y, MAX_FALL_SPEED);
            grounded = false;
            ResolvePlatformCollision(previousPosition, previousVerticalVelocity);
        }
        check(grounded, "gravity-driven player lands and becomes grounded");
        check(std::fabs(player.position.y -
                        (groundPlatform.position.y - player.size.y)) < 0.01f &&
                  player.velocity.y == 0.0f,
              "platform penetration is corrected and vertical speed is cleared");

        const engine::Vec2 beforeStomp = {
            patrolEnemy.position.x,
            patrolEnemy.position.y - player.size.y - 3.0f
        };
        player.position = {
            patrolEnemy.position.x,
            patrolEnemy.position.y - player.size.y + 3.0f
        };
        player.velocity = {0.0f, 270.0f};
        invulnerability = 0.0f;
        ResolveEnemyCollision(beforeStomp);
        check(!patrolEnemy.active && deaths == 0 && player.active &&
                  player.velocity.y < 0.0f,
              "landing on a monster defeats it and bounces the player");

        player.position = sentinelEnemy.position;
        player.velocity = {0.0f, 0.0f};
        ResolveEnemyCollision(player.position);
        check(deaths == 1 && awaitingDeathContinue && !player.active,
              "side or underside monster contact consumes a life");
        ResolveEnemyCollision(player.position);
        check(deaths == 1,
              "one monster contact cannot consume multiple lives");

        runTime = 3.0f;
        UpdateDisappearingPlatform();
        check(routePlatforms[DISAPPEARING_PLATFORM_INDEX].active &&
                  disappearingWarning,
              "disappearing platform visibly warns before vanishing");
        runTime = 3.8f;
        UpdateDisappearingPlatform();
        check(!routePlatforms[DISAPPEARING_PLATFORM_INDEX].active,
              "timed platform becomes non-colliding during its vanished phase");
        runTime = 5.1f;
        UpdateDisappearingPlatform();
        check(routePlatforms[DISAPPEARING_PLATFORM_INDEX].active,
              "disappearing platform returns on its bounded cycle");

        RestartRun();
        player.position = runes[0].position;
        CollectRunes();
        const engine::Vec2 earnedCheckpoint = checkpointPosition;
        player.position = hazards[0].position;
        const bool hitHazard = ResolveHazardCollisions();
        check(hitHazard && deaths == 1 && awaitingDeathContinue &&
                  !player.active,
              "spikes record a lost life and pause for player confirmation");
        StartRespawnCountdown();
        check(respawnCountdown == RESPAWN_COUNTDOWN_SECONDS,
              "pressing continue starts a three-second respawn countdown");
        UpdateRespawnCountdown(1.0f);
        UpdateRespawnCountdown(1.0f);
        UpdateRespawnCountdown(1.0f);
        check(player.active && respawnCountdown == 0.0f &&
                  std::fabs(player.position.x - earnedCheckpoint.x) < 0.01f &&
                  std::fabs(player.position.y - earnedCheckpoint.y) < 0.01f,
              "the countdown respawns at the earned checkpoint");

        RestartRun();
        for (engine::Entity &rune : runes) {
            player.position = rune.position;
            CollectRunes();
        }
        check(runeCount == static_cast<int>(RUNE_COUNT),
              "all rune entities can be collected across the full level");
        runTime = 12.0f;
        player.position = exitPortal.position;
        ResolveExitCollision();
        check(escapeSequence > 0.0f,
              "collecting every rune unlocks the gate escape sequence");
        for (int frame = 0; frame < 150 && !escaped; ++frame) {
            UpdateEscapeSequence(1.0f / 120.0f);
        }
        check(escaped && bestTime > 0.0f,
              "the final gate sequence completes and records a best time");

        RestartRun();
        player.position.x = exitPortal.position.x;
        UpdateCamera(0.0f, true);
        const float gateScreenX = ScreenX(exitPortal.position.x, 1.0f);
        check(cameraX > viewportWidth && gateScreenX >= 0.0f &&
                  gateScreenX <= viewportWidth,
              "the camera follows the player and reveals the far end of the level");

        RestartRun();
        player.position = {worldWidth * 0.5f, worldHeight + 45.0f};
        const bool fellOut = ResolveWorldBounds();
        check(fellOut && deaths == 1 && awaitingDeathContinue,
              "falling below the world records one lost life");
        for (int death = 1; death < MAX_DEATHS; ++death) {
            RespawnPlayer();
            player.position.y = worldHeight + 45.0f;
            ResolveWorldBounds();
        }
        check(deaths == MAX_DEATHS && gameOver && !awaitingDeathContinue,
              "losing the tenth life ends the run");

        if (passed) {
            std::cout << "All Neon Grove gameplay checks passed.\n";
        }
        return passed;
    }

private:
    std::uint32_t NextRandom() {
        randomState = randomState * 1664525u + 1013904223u;
        return randomState;
    }

    float Random01() {
        return static_cast<float>((NextRandom() >> 8u) & 0x00ffffffu) /
               16777215.0f;
    }

    float RandomRange(float minimum, float maximum) {
        return minimum + (maximum - minimum) * Random01();
    }

    void BuildRequiredEntities() {
        // Required static entity: visible, collidable, stationary, no gravity.
        groundPlatform.position = {90.0f, worldHeight - 202.5f};
        groundPlatform.size = {420.0f, 123.0f};
        groundPlatform.velocity = {0.0f, 0.0f};
        groundPlatform.color = {55, 62, 85, 255};
        groundPlatform.affectedByGravity = false;
        groundPlatform.active = true;

        // Required controllable entity: physics-owned vertical motion.
        player.size = {57.0f, 81.0f};
        player.color = {91, 242, 224, 255};
        player.affectedByGravity = true;
        player.active = true;

        // Required kinematic entity: explicit fixed-endpoint patrol path.
        patrolEnemy.size = {72.0f, 84.0f};
        patrolEnemy.color = {231, 67, 157, 255};
        patrolEnemy.velocity = {0.0f, 0.0f};
        patrolEnemy.affectedByGravity = false;
        patrolEnemy.active = true;

        const std::array<SDL_FRect, PLATFORM_COUNT> route = {{
            {577.5f, 780.0f, 307.5f, 63.0f},
            {960.0f, 667.5f, 285.0f, 57.0f},
            {1312.5f, 772.5f, 277.5f, 63.0f},
            {1650.0f, 645.0f, 240.0f, 66.0f},
            {1965.0f, 750.0f, 315.0f, 63.0f},
            {2355.0f, 615.0f, 270.0f, 60.0f},
            {2700.0f, 802.5f, 345.0f, 69.0f},
            {3120.0f, 682.5f, 285.0f, 60.0f},
            {3480.0f, 547.5f, 277.5f, 57.0f},
            {3832.5f, 712.5f, 322.5f, 63.0f},
            {4230.0f, 615.0f, 300.0f, 60.0f},
            {4605.0f, 780.0f, 345.0f, 66.0f},
            {5025.0f, 652.5f, 285.0f, 60.0f},
            {5385.0f, 765.0f, 345.0f, 66.0f}
        }};
        for (std::size_t i = 0; i < routePlatforms.size(); ++i) {
            routePlatforms[i].position = {route[i].x, route[i].y};
            routePlatforms[i].size = {route[i].w, route[i].h};
            routePlatforms[i].velocity = {0.0f, 0.0f};
            routePlatforms[i].color = {55, 62, 85, 255};
            routePlatforms[i].affectedByGravity = false;
            routePlatforms[i].active = true;
        }

        patrolLeftEndpoint = routePlatforms[PATROL_PLATFORM_INDEX].position.x + 30.0f;
        patrolRightEndpoint = routePlatforms[PATROL_PLATFORM_INDEX].position.x +
                              routePlatforms[PATROL_PLATFORM_INDEX].size.x -
                              patrolEnemy.size.x - 18.0f;
        patrolEnemy.position = {
            patrolLeftEndpoint,
            routePlatforms[PATROL_PLATFORM_INDEX].position.y -
                patrolEnemy.size.y - 6.0f
        };

        sentinelEnemy.size = {36.0f, 51.0f};
        sentinelEnemy.position = {SENTINEL_BASE_X, 540.0f};
        sentinelEnemy.color = {255, 101, 114, 255};
        sentinelEnemy.affectedByGravity = false;
        sentinelEnemy.active = true;

        exitPortal.size = {72.0f, 138.0f};
        exitPortal.position = {
            routePlatforms.back().position.x + routePlatforms.back().size.x - 94.5f,
            routePlatforms.back().position.y - exitPortal.size.y
        };
        exitPortal.velocity = {0.0f, 0.0f};
        exitPortal.color = {106, 76, 174, 255};
        exitPortal.affectedByGravity = false;
        exitPortal.active = true;

        spawnPosition = {
            groundPlatform.position.x + 87.0f,
            groundPlatform.position.y - player.size.y
        };
        checkpointPosition = spawnPosition;

        const std::array<SDL_FRect, HAZARD_COUNT> spikeBounds = {{
            {510.0f, 918.0f, 67.5f, 51.0f},
            {885.0f, 877.5f, 75.0f, 57.0f},
            {1245.0f, 877.5f, 67.5f, 57.0f},
            {1590.0f, 855.0f, 60.0f, 57.0f},
            {1890.0f, 855.0f, 75.0f, 57.0f},
            {2280.0f, 855.0f, 75.0f, 57.0f},
            {2625.0f, 877.5f, 75.0f, 57.0f},
            {3045.0f, 877.5f, 75.0f, 57.0f},
            {3405.0f, 855.0f, 75.0f, 57.0f},
            {3757.5f, 855.0f, 75.0f, 57.0f},
            {4530.0f, 877.5f, 75.0f, 57.0f},
            {4950.0f, 855.0f, 75.0f, 57.0f}
        }};
        for (std::size_t i = 0; i < hazards.size(); ++i) {
            hazards[i].position = {spikeBounds[i].x, spikeBounds[i].y};
            hazards[i].size = {spikeBounds[i].w, spikeBounds[i].h};
            hazards[i].color = {238, 63, 130, 255};
            hazards[i].affectedByGravity = false;
            hazards[i].active = true;
        }

        const std::array<engine::Vec2, RUNE_COUNT> runePositions = {{
            {705.0f, 708.0f}, {1527.0f, 700.5f}, {2475.0f, 543.0f},
            {3255.0f, 610.5f}, {4350.0f, 543.0f}, {5145.0f, 580.5f}
        }};
        for (std::size_t i = 0; i < runes.size(); ++i) {
            runes[i].position = runePositions[i];
            runes[i].size = {33.0f, 42.0f};
            runes[i].color = {255, 216, 89, 255};
            runes[i].affectedByGravity = false;
            runes[i].active = true;
        }
    }

    void BuildAtmosphere() {
        for (SDL_FPoint &star : stars) {
            star.x = RandomRange(18.0f, worldWidth - 18.0f);
            star.y = RandomRange(22.0f, worldHeight * 0.52f);
        }
        for (Firefly &firefly : fireflies) {
            firefly.origin = {
                RandomRange(70.0f, worldWidth - 70.0f),
                RandomRange(worldHeight * 0.38f, groundPlatform.position.y - 18.0f)
            };
            firefly.phase = RandomRange(0.0f, 2.0f * PI);
            firefly.speed = RandomRange(0.45f, 1.15f);
            firefly.radius = RandomRange(9.0f, 22.5f);
        }
    }

    void RestartRun() {
        runeCount = 0;
        deaths = 0;
        runTime = 0.0f;
        escapeSequence = 0.0f;
        escaped = false;
        awaitingDeathContinue = false;
        respawnCountdown = 0.0f;
        gameOver = false;
        checkpointPosition = spawnPosition;
        checkpointIndex = 0;
        patrolDirection = 1.0f;
        sentinelDirection = 1.0f;
        patrolEnemy.active = true;
        sentinelEnemy.active = true;
        patrolEnemy.position.x = patrolLeftEndpoint;
        sentinelEnemy.position = {SENTINEL_BASE_X, 555.0f};
        routePlatforms[DISAPPEARING_PLATFORM_INDEX].active = true;
        routePlatforms[DISAPPEARING_PLATFORM_INDEX].color = {55, 62, 85, 255};
        disappearingWarning = false;
        lockedNotice = 0.0f;
        for (engine::Entity &rune : runes) {
            rune.active = true;
        }
        RespawnPlayer(false);
    }

    void RespawnPlayer(bool afterDeath = true) {
        player.position = checkpointPosition;
        player.velocity = {0.0f, 0.0f};
        player.color = {91, 242, 224, 255};
        player.active = true;
        grounded = true;
        escaped = false;
        awaitingDeathContinue = false;
        respawnCountdown = 0.0f;
        invulnerability = afterDeath ? INVULNERABILITY_SECONDS : 0.0f;
        collisionFlash = afterDeath ? 0.25f : 0.0f;
        jumpPulse = 0.0f;
        landingPulse = 0.0f;
        jumpBuffer = 0.0f;
        trailTimer = 0.0f;
        for (TrailParticle &particle : trail) {
            particle.active = false;
        }
        UpdateCamera(0.0f, true);
    }

    void LoseLife() {
        ++deaths;
        player.velocity = {0.0f, 0.0f};
        player.active = false;
        grounded = false;
        jumpBuffer = 0.0f;
        trailTimer = 0.0f;
        EmitBurst(18, {255, 91, 174, 255}, 270.0f);

        if (deaths >= MAX_DEATHS) {
            gameOver = true;
            awaitingDeathContinue = false;
            respawnCountdown = 0.0f;
        } else {
            awaitingDeathContinue = true;
        }
    }

    void StartRespawnCountdown() {
        awaitingDeathContinue = false;
        respawnCountdown = RESPAWN_COUNTDOWN_SECONDS;
    }

    void UpdateRespawnCountdown(float dt) {
        respawnCountdown = std::max(0.0f, respawnCountdown - dt);
        if (respawnCountdown <= 0.0f) {
            RespawnPlayer();
        }
    }

    void ApplyPlayerInput(float dt) {
        float direction = 0.0f;
        if (engine::Input::IsKeyPressed(SDL_SCANCODE_A) ||
            engine::Input::IsKeyPressed(SDL_SCANCODE_LEFT)) {
            direction -= 1.0f;
        }
        if (engine::Input::IsKeyPressed(SDL_SCANCODE_D) ||
            engine::Input::IsKeyPressed(SDL_SCANCODE_RIGHT)) {
            direction += 1.0f;
        }

        if (direction != 0.0f) {
            const float acceleration = grounded ? MOVE_ACCELERATION : AIR_ACCELERATION;
            player.velocity.x += direction * acceleration * dt;
        } else {
            const float deceleration = MOVE_DECELERATION * dt;
            if (player.velocity.x > 0.0f) {
                player.velocity.x = std::max(0.0f, player.velocity.x - deceleration);
            } else if (player.velocity.x < 0.0f) {
                player.velocity.x = std::min(0.0f, player.velocity.x + deceleration);
            }
        }
        player.velocity.x = std::clamp(player.velocity.x,
                                       -MAX_MOVE_SPEED, MAX_MOVE_SPEED);

        if (engine::Input::IsKeyJustPressed(SDL_SCANCODE_SPACE)) {
            jumpBuffer = JUMP_BUFFER_SECONDS;
        }
        if (grounded && jumpBuffer > 0.0f) {
            player.velocity.y = -JUMP_SPEED;
            grounded = false;
            jumpBuffer = 0.0f;
            jumpPulse = 1.0f;
            EmitBurst(9, {102, 255, 232, 255}, 142.5f);
        }
    }

    void UpdatePatrolEnemy(float dt) {
        if (!patrolEnemy.active) {
            return;
        }
        const float speed = ENEMY_PATROL_SPEED +
                            ENEMY_SPEED_PER_RUNE * static_cast<float>(runeCount);
        patrolEnemy.position.x += patrolDirection * speed * dt;
        if (patrolEnemy.position.x >= patrolRightEndpoint) {
            patrolEnemy.position.x = patrolRightEndpoint;
            patrolDirection = -1.0f;
        } else if (patrolEnemy.position.x <= patrolLeftEndpoint) {
            patrolEnemy.position.x = patrolLeftEndpoint;
            patrolDirection = 1.0f;
        }

        const float hover = std::sin(ambientTime * 2.6f) * 6.0f;
        patrolEnemy.position.y = routePlatforms[PATROL_PLATFORM_INDEX].position.y -
                                 patrolEnemy.size.y - 9.0f + hover;
    }

    void UpdateSentinelEnemy(float dt) {
        if (!sentinelEnemy.active) {
            return;
        }
        constexpr float upperEndpoint = 517.5f;
        constexpr float lowerEndpoint = 757.5f;
        const float speed = 108.0f + 18.0f * static_cast<float>(runeCount);
        sentinelEnemy.position.y += sentinelDirection * speed * dt;
        if (sentinelEnemy.position.y >= lowerEndpoint) {
            sentinelEnemy.position.y = lowerEndpoint;
            sentinelDirection = -1.0f;
        } else if (sentinelEnemy.position.y <= upperEndpoint) {
            sentinelEnemy.position.y = upperEndpoint;
            sentinelDirection = 1.0f;
        }
        sentinelEnemy.position.x = SENTINEL_BASE_X +
                                   std::sin(ambientTime * 2.0f) * 4.5f;
    }

    void UpdateDisappearingPlatform() {
        constexpr float cycleLength = 5.0f;
        constexpr float warningStart = 2.8f;
        constexpr float vanishStart = 3.6f;
        const float phase = std::fmod(runTime, cycleLength);
        disappearingWarning = phase >= warningStart && phase < vanishStart;
        engine::Entity &platform = routePlatforms[DISAPPEARING_PLATFORM_INDEX];
        platform.active = phase < vanishStart;
        if (disappearingWarning) {
            const bool bright = std::fmod(phase - warningStart, 0.20f) < 0.10f;
            platform.color = bright
                ? SDL_Color{230, 89, 178, 255}
                : SDL_Color{91, 65, 116, 255};
        } else {
            platform.color = {55, 62, 85, 255};
        }
    }

    void ResolvePlatformCollision(const engine::Vec2 &previousPosition,
                                  float previousVerticalVelocity) {
        ResolveSolidPlatform(groundPlatform, previousPosition,
                             previousVerticalVelocity);
        for (const engine::Entity &platform : routePlatforms) {
            if (platform.active) {
                ResolveSolidPlatform(platform, previousPosition,
                                     previousVerticalVelocity);
            }
        }
    }

    void ResolveSolidPlatform(const engine::Entity &platform,
                              const engine::Vec2 &previousPosition,
                              float previousVerticalVelocity) {
        if (!engine::Collision::CheckCollision(player, platform)) {
            return;
        }

        const float platformLeft = platform.position.x;
        const float platformRight = platformLeft + platform.size.x;
        const float platformTop = platform.position.y;
        const float platformBottom = platformTop + platform.size.y;
        const float previousLeft = previousPosition.x;
        const float previousRight = previousPosition.x + player.size.x;
        const float previousTop = previousPosition.y;
        const float previousBottom = previousPosition.y + player.size.y;

        if (previousBottom <= platformTop + 4.5f && player.velocity.y >= 0.0f) {
            const float landingSpeed = std::max(previousVerticalVelocity,
                                                player.velocity.y);
            player.position.y = platformTop - player.size.y;
            player.velocity.y = 0.0f;
            grounded = true;
            if (landingSpeed > 390.0f) {
                landingPulse = 1.0f;
                EmitBurst(7, {87, 238, 204, 220}, 105.0f);
            }
            return;
        }
        if (previousTop >= platformBottom - 4.5f && player.velocity.y < 0.0f) {
            player.position.y = platformBottom;
            player.velocity.y = 0.0f;
            return;
        }
        if (previousRight <= platformLeft + 4.5f && player.velocity.x > 0.0f) {
            player.position.x = platformLeft - player.size.x;
            player.velocity.x = 0.0f;
            return;
        }
        if (previousLeft >= platformRight - 4.5f && player.velocity.x < 0.0f) {
            player.position.x = platformRight;
            player.velocity.x = 0.0f;
            return;
        }

        const SDL_FRect playerBounds = player.GetBounds();
        const SDL_FRect platformBounds = platform.GetBounds();
        const float pushUp = playerBounds.y + playerBounds.h - platformBounds.y;
        const float pushDown = platformBounds.y + platformBounds.h - playerBounds.y;
        const float pushLeft = playerBounds.x + playerBounds.w - platformBounds.x;
        const float pushRight = platformBounds.x + platformBounds.w - playerBounds.x;
        const float minimum = std::min({pushUp, pushDown, pushLeft, pushRight});
        if (minimum == pushUp) {
            player.position.y -= pushUp;
            player.velocity.y = 0.0f;
            grounded = true;
        } else if (minimum == pushDown) {
            player.position.y += pushDown;
            player.velocity.y = 0.0f;
        } else if (minimum == pushLeft) {
            player.position.x -= pushLeft;
            player.velocity.x = 0.0f;
        } else {
            player.position.x += pushRight;
            player.velocity.x = 0.0f;
        }
    }

    void ResolveEnemyCollision(const engine::Vec2 &previousPosition) {
        if (invulnerability > 0.0f) {
            return;
        }
        if (engine::Collision::CheckCollision(player, patrolEnemy)) {
            ResolveMonsterContact(patrolEnemy, previousPosition);
        } else if (engine::Collision::CheckCollision(player, sentinelEnemy)) {
            ResolveMonsterContact(sentinelEnemy, previousPosition);
        }
    }

    void ResolveMonsterContact(engine::Entity &monster,
                               const engine::Vec2 &previousPosition) {
        constexpr float stompTolerance = 6.0f;
        const float previousBottom = previousPosition.y + player.size.y;
        const bool descending = player.velocity.y >= 0.0f;
        const bool approachedFromAbove =
            previousBottom <= monster.position.y + stompTolerance;

        if (descending && approachedFromAbove) {
            player.position.y = monster.position.y - player.size.y;
            player.velocity.y = -STOMP_BOUNCE_SPEED;
            grounded = false;
            jumpPulse = 1.0f;
            monster.active = false;
            EmitBurst(20, {255, 203, 72, 255}, 285.0f);
            return;
        }

        LoseLife();
    }

    void ResolveExitCollision() {
        if (!engine::Collision::CheckCollision(player, exitPortal)) {
            return;
        }
        if (runeCount == static_cast<int>(RUNE_COUNT)) {
            escapeSequence = 0.001f;
            player.velocity = {0.0f, 0.0f};
            player.color = {154, 255, 209, 255};
            EmitBurst(24, {130, 255, 212, 255}, 315.0f);
        } else {
            player.position.x = exitPortal.position.x - player.size.x - 4.5f;
            player.velocity.x = -360.0f;
            lockedNotice = 1.2f;
            EmitBurst(8, {255, 91, 174, 255}, 157.5f);
        }
    }

    void CollectRunes() {
        for (std::size_t i = 0; i < runes.size(); ++i) {
            engine::Entity &rune = runes[i];
            if (!rune.active || !engine::Collision::CheckCollision(player, rune)) {
                continue;
            }
            rune.active = false;
            ++runeCount;
            EmitBurst(15, {255, 221, 92, 255}, 232.5f);
            if (i == 0) {
                checkpointPosition = {
                    routePlatforms[0].position.x + 27.0f,
                    routePlatforms[0].position.y - player.size.y
                };
                checkpointIndex = 1;
            } else if (i == 2) {
                checkpointPosition = {
                    routePlatforms[5].position.x + 18.0f,
                    routePlatforms[5].position.y - player.size.y
                };
                checkpointIndex = 2;
            } else if (i == 4) {
                checkpointPosition = {
                    routePlatforms[10].position.x + 18.0f,
                    routePlatforms[10].position.y - player.size.y
                };
                checkpointIndex = 3;
            }
        }
    }

    bool ResolveHazardCollisions() {
        for (const engine::Entity &hazard : hazards) {
            if (engine::Collision::CheckCollision(player, hazard)) {
                LoseLife();
                return true;
            }
        }
        return false;
    }

    void UpdateEscapeSequence(float dt) {
        escapeSequence += dt;
        const engine::Vec2 target = {
            exitPortal.position.x + (exitPortal.size.x - player.size.x) * 0.5f,
            exitPortal.position.y + (exitPortal.size.y - player.size.y) * 0.5f
        };
        const float approach = std::min(1.0f, dt * 5.5f);
        player.position.x += (target.x - player.position.x) * approach;
        player.position.y += (target.y - player.position.y) * approach;
        if (escapeSequence >= 1.15f) {
            escaped = true;
            player.active = false;
            if (bestTime <= 0.0f || runTime < bestTime) {
                bestTime = runTime;
            }
        }
    }

    void ResolveHorizontalWorldBounds() {
        if (player.position.x < 0.0f) {
            player.position.x = 0.0f;
            player.velocity.x = std::max(0.0f, player.velocity.x);
        }
        if (player.position.x + player.size.x > worldWidth) {
            player.position.x = worldWidth - player.size.x;
            player.velocity.x = std::min(0.0f, player.velocity.x);
        }
    }

    bool ResolveWorldBounds() {
        ResolveHorizontalWorldBounds();
        if (player.position.y < 0.0f) {
            player.position.y = 0.0f;
            player.velocity.y = std::max(0.0f, player.velocity.y);
        }
        if (player.position.y > worldHeight + 37.5f) {
            LoseLife();
            return true;
        }
        return false;
    }

    void UpdatePlayerAppearance() {
        if (collisionFlash > 0.0f) {
            const float amount = std::clamp(collisionFlash, 0.0f, 1.0f);
            player.color = {
                static_cast<Uint8>(91.0f + 164.0f * amount),
                static_cast<Uint8>(242.0f - 72.0f * amount),
                static_cast<Uint8>(224.0f - 44.0f * amount),
                255
            };
        } else {
            player.color = {91, 242, 224, 255};
        }
    }

    void ToggleScalingDemo() {
        engine::Scaling::Toggle();
        if (!scalingDemoResized) {
            const int width = std::max(760, app.GetBaseWidth() * 17 / 20);
            const int height = std::max(440, app.GetBaseHeight() * 17 / 20);
            scalingDemoResized = SetWindowSize(app, width, height);
        }
        scalingNotice = 1.8f;
    }

    void UpdateFirefliesAndParticles(float dt) {
        for (TrailParticle &particle : trail) {
            if (!particle.active) {
                continue;
            }
            particle.life -= dt;
            if (particle.life <= 0.0f) {
                particle.active = false;
                continue;
            }
            particle.position.x += particle.velocity.x * dt;
            particle.position.y += particle.velocity.y * dt;
            particle.velocity.y -= 18.0f * dt;
        }
    }

    void EmitMovementTrail(float dt) {
        if (!grounded || std::fabs(player.velocity.x) < 52.5f) {
            trailTimer = 0.0f;
            return;
        }
        trailTimer -= dt;
        if (trailTimer <= 0.0f) {
            const engine::Vec2 origin = {
                player.position.x + player.size.x * 0.5f -
                    (player.velocity.x > 0.0f ? player.size.x * 0.45f
                                             : -player.size.x * 0.45f),
                player.position.y + player.size.y - 7.5f
            };
            EmitParticle(origin,
                         {-player.velocity.x * 0.12f + RandomRange(-24.0f, 24.0f),
                          RandomRange(-57.0f, -18.0f)},
                         RandomRange(0.28f, 0.48f), RandomRange(3.0f, 6.75f),
                         {91, 242, 224, 255});
            trailTimer = 0.045f;
        }
    }

    void EmitBurst(int count, SDL_Color color, float speed) {
        const engine::Vec2 center = {
            player.position.x + player.size.x * 0.5f,
            player.position.y + player.size.y * 0.72f
        };
        for (int i = 0; i < count; ++i) {
            const float angle = RandomRange(0.0f, 2.0f * PI);
            const float particleSpeed = RandomRange(speed * 0.35f, speed);
            EmitParticle(center,
                         {std::cos(angle) * particleSpeed,
                          std::sin(angle) * particleSpeed},
                         RandomRange(0.32f, 0.72f), RandomRange(2.0f, 5.0f),
                         color);
        }
    }

    void EmitParticle(const engine::Vec2 &position, const engine::Vec2 &velocity,
                      float life, float size, SDL_Color color) {
        TrailParticle &particle = trail[nextTrailParticle];
        nextTrailParticle = (nextTrailParticle + 1) % trail.size();
        particle.position = position;
        particle.velocity = velocity;
        particle.color = color;
        particle.life = life;
        particle.maxLife = life;
        particle.size = size;
        particle.active = true;
    }

    void UpdateCamera(float dt, bool snap = false) {
        int windowWidth = 0;
        int windowHeight = 0;
        app.GetWindowSize(&windowWidth, &windowHeight);
        const float scale = std::max(0.001f, GetWorldScale());
        const float visibleWorldWidth = static_cast<float>(windowWidth) / scale;
        const float target = std::clamp(
            player.position.x + player.size.x * 0.5f - visibleWorldWidth * 0.42f,
            0.0f, std::max(0.0f, worldWidth - visibleWorldWidth));
        if (snap) {
            cameraX = target;
            return;
        }
        const float approach = std::min(1.0f, dt * 7.0f);
        cameraX += (target - cameraX) * approach;
    }

    float ScreenX(float worldX, float scale) const {
        return (worldX - cameraX) * scale;
    }

    float GetWorldScale() const {
        return app.GetScale();
    }

    void DrawTwilightGradient(SDL_Renderer *renderer) const {
        int width = 0;
        int height = 0;
        app.GetWindowSize(&width, &height);
        const SDL_FColor top = {0.008f, 0.020f, 0.043f, 1.0f};
        const SDL_FColor bottom = {0.028f, 0.115f, 0.135f, 1.0f};
        const std::array<SDL_Vertex, 4> vertices = {{
            {{0.0f, 0.0f}, top, {0.0f, 0.0f}},
            {{static_cast<float>(width), 0.0f}, top, {1.0f, 0.0f}},
            {{static_cast<float>(width), static_cast<float>(height)}, bottom,
             {1.0f, 1.0f}},
            {{0.0f, static_cast<float>(height)}, bottom, {0.0f, 1.0f}}
        }};
        constexpr std::array<int, 6> indices = {{0, 1, 2, 0, 2, 3}};
        SDL_RenderGeometry(renderer, nullptr, vertices.data(),
                           static_cast<int>(vertices.size()), indices.data(),
                           static_cast<int>(indices.size()));
    }

    void DrawSpores(SDL_Renderer *renderer, float scale) const {
        for (std::size_t i = 0; i < stars.size(); ++i) {
            const float shimmer = 0.55f + 0.45f *
                                  std::sin(ambientTime * 1.15f +
                                           static_cast<float>(i));
            const float drift = std::fmod(stars[i].y + ambientTime *
                (2.0f + static_cast<float>(i % 5)), worldHeight * 0.58f);
            const float sway = std::sin(ambientTime * 0.45f +
                                        static_cast<float>(i) * 0.7f) * 4.0f;
            const float size = i % 11 == 0 ? 2.2f : 1.1f;
            FillCircle(renderer, ScreenX(stars[i].x + sway, scale), drift * scale,
                       size * scale,
                       {190, 236, 239,
                        static_cast<Uint8>(65.0f + 125.0f * shimmer)});
        }
    }

    void DrawMoon(SDL_Renderer *renderer, float scale) const {
        const float x = (viewportWidth * 0.78f - cameraX * 0.035f) * scale;
        const float y = worldHeight * 0.20f * scale;
        FillCircle(renderer, x, y, 130.0f * scale, {105, 214, 224, 10});
        FillCircle(renderer, x, y, 101.0f * scale, {144, 220, 227, 19});
        FillCircle(renderer, x, y, 76.0f * scale, {218, 237, 235, 245});
        FillCircle(renderer, x - 19.0f * scale, y - 12.0f * scale,
                   13.0f * scale, {162, 199, 207, 90});
        FillCircle(renderer, x + 22.0f * scale, y + 17.0f * scale,
                   9.0f * scale, {162, 199, 207, 75});
    }

    void DrawCavernLayers(SDL_Renderer *renderer, float scale) const {
        const float farShift = -std::fmod(cameraX * 0.12f, viewportWidth);
        const SDL_FColor ceilingColor = {0.018f, 0.046f, 0.070f, 1.0f};
        const std::array<float, 9> ceilingSpikes = {{
            -40, 105, 245, 410, 575, 735, 900, 1080, 1260
        }};
        for (int repeat = -1; repeat <= 1; ++repeat) {
            for (std::size_t i = 0; i < ceilingSpikes.size(); ++i) {
                const float x = ceilingSpikes[i] + farShift +
                                static_cast<float>(repeat) * viewportWidth;
                const float width = 72.0f + static_cast<float>((i * 19) % 58);
                const float length = 80.0f + static_cast<float>((i * 43) % 145);
                FillTriangle(renderer,
                             {(x - width) * scale, 0.0f},
                             {x * scale, length * scale},
                             {(x + width) * scale, 0.0f}, ceilingColor);
            }
        }

        const float nearShift = -std::fmod(cameraX * 0.24f, viewportWidth);
        const float cavernBase = worldHeight * 0.75f;
        const SDL_FColor spireColor = {0.025f, 0.075f, 0.090f, 1.0f};
        const std::array<float, 8> spires = {{
            20, 180, 350, 530, 705, 885, 1060, 1240
        }};
        for (int repeat = -1; repeat <= 1; ++repeat) {
            for (std::size_t i = 0; i < spires.size(); ++i) {
                const float x = spires[i] + nearShift +
                                static_cast<float>(repeat) * viewportWidth;
                const float width = 105.0f + static_cast<float>((i * 13) % 55);
                const float height = 135.0f + static_cast<float>((i * 37) % 135);
                FillTriangle(renderer,
                             {(x - width) * scale, cavernBase * scale},
                             {x * scale, (cavernBase - height) * scale},
                             {(x + width) * scale, cavernBase * scale}, spireColor);
            }
        }

        SDL_SetRenderDrawColor(renderer, 129, 213, 213, 10);
        const SDL_FRect highMist = {0.0f, 310.0f * scale,
                                    viewportWidth * scale, 42.0f * scale};
        const SDL_FRect lowMist = {0.0f, 455.0f * scale,
                                   viewportWidth * scale, 58.0f * scale};
        SDL_RenderFillRect(renderer, &highMist);
        SDL_RenderFillRect(renderer, &lowMist);
    }

    void DrawFungalRuins(SDL_Renderer *renderer, float scale) const {
        // These silhouettes sit on the same ground as the gameplay geometry,
        // so keep them in world space. Applying the player's parallax offset
        // here made the trees and ruins slide whenever the player moved.
        const std::array<float, 10> stalkPositions = {{
            28, 150, 280, 430, 565, 750, 895, 1015, 1150, 1270
        }};
        for (float region = 0.0f; region < worldWidth; region += viewportWidth) {
            for (std::size_t i = 0; i < stalkPositions.size(); ++i) {
                const float x = ScreenX(region + stalkPositions[i], scale);
                const float baseY = groundPlatform.position.y + 12.0f;
                const float height = 105.0f + static_cast<float>((i * 29) % 88);
                SDL_SetRenderDrawColor(renderer, 17, 55, 62, 255);
                const SDL_FRect stalk = {x - 5.0f * scale,
                                         (baseY - height) * scale,
                                         10.0f * scale, height * scale};
                SDL_RenderFillRect(renderer, &stalk);
                FillCircle(renderer, x, (baseY - height) * scale,
                           30.0f * scale, {33, 83, 88, 255});
                FillCircle(renderer, x - 23.0f * scale,
                           (baseY - height + 10.0f) * scale,
                           18.0f * scale, {24, 68, 76, 255});
                FillCircle(renderer, x + 23.0f * scale,
                           (baseY - height + 9.0f) * scale,
                           19.0f * scale, {26, 72, 78, 255});
            }
        }

        SDL_SetRenderDrawColor(renderer, 28, 54, 68, 255);
        const std::array<float, 5> pillarX = {{78, 405, 690, 980, 1210}};
        for (float region = 0.0f; region < worldWidth; region += viewportWidth) {
            for (std::size_t i = 0; i < pillarX.size(); ++i) {
                const float x = ScreenX(region + pillarX[i], scale);
                const float height = 78.0f + static_cast<float>(i % 3) * 35.0f;
                const SDL_FRect pillar = {x - 14.0f * scale,
                                          (groundPlatform.position.y - height) * scale,
                                          28.0f * scale, height * scale};
                SDL_RenderFillRect(renderer, &pillar);
                SDL_SetRenderDrawColor(renderer, 57, 86, 98, 255);
                const SDL_FRect cap = {
                    x - 21.0f * scale,
                    (groundPlatform.position.y - height - 8.0f) * scale,
                    42.0f * scale, 9.0f * scale
                };
                SDL_RenderFillRect(renderer, &cap);
                FillTriangle(renderer,
                             {x - 17.0f * scale,
                              (groundPlatform.position.y - height - 8.0f) * scale},
                             {x,
                              (groundPlatform.position.y - height - 31.0f) * scale},
                             {x + 17.0f * scale,
                              (groundPlatform.position.y - height - 8.0f) * scale},
                             {0.11f, 0.21f, 0.25f, 1.0f});
                SDL_SetRenderDrawColor(renderer, 28, 54, 68, 255);
            }
        }

        SDL_SetRenderDrawColor(renderer, 80, 132, 137, 105);
        const std::array<float, 7> vineX = {{95, 260, 505, 665, 845, 1080, 1240}};
        for (float region = 0.0f; region < worldWidth; region += viewportWidth) {
            for (std::size_t i = 0; i < vineX.size(); ++i) {
                const float x = ScreenX(region + vineX[i], scale);
                const float length =
                    (62.0f + static_cast<float>((i * 31) % 105)) * scale;
                SDL_RenderLine(renderer, x, 0.0f, x - 9.0f * scale, length);
                SDL_RenderLine(renderer, x + 4.0f * scale, 0.0f,
                               x + 13.0f * scale, length * 0.72f);
            }
        }
    }

    void DrawFireflies(SDL_Renderer *renderer, float scale) const {
        for (const Firefly &firefly : fireflies) {
            const float x = ScreenX(
                firefly.origin.x +
                    std::sin(ambientTime * firefly.speed + firefly.phase) *
                        firefly.radius,
                scale);
            const float y = (firefly.origin.y +
                             std::cos(ambientTime * firefly.speed * 0.73f +
                                      firefly.phase) * firefly.radius * 0.55f) * scale;
            const float pulse = 0.65f + 0.35f *
                                std::sin(ambientTime * 2.4f + firefly.phase);
            FillCircle(renderer, x, y, 7.0f * scale,
                       {96, 255, 194, static_cast<Uint8>(18.0f * pulse)});
            FillCircle(renderer, x, y, 2.1f * scale,
                       {159, 255, 205, static_cast<Uint8>(170.0f + 75.0f * pulse)});
        }
    }

    void DrawHazards(SDL_Renderer *renderer, float scale) const {
        for (const engine::Entity &hazard : hazards) {
            const float x = ScreenX(hazard.position.x, scale);
            const float y = hazard.position.y * scale;
            const float width = hazard.size.x * scale;
            const float height = hazard.size.y * scale;
            SDL_SetRenderDrawColor(renderer, 255, 55, 160, 28);
            const SDL_FRect dangerGlow = {
                x - 5.0f * scale, y - 6.0f * scale,
                width + 10.0f * scale, height + 10.0f * scale
            };
            SDL_RenderFillRect(renderer, &dangerGlow);
            SDL_SetRenderDrawColor(renderer, 74, 21, 69, 180);
            const SDL_FRect dangerBase = {x, y, width, height};
            SDL_RenderFillRect(renderer, &dangerBase);
            constexpr int spikeCount = 4;
            for (int spike = 0; spike < spikeCount; ++spike) {
                const float left = x + width * static_cast<float>(spike) /
                                           static_cast<float>(spikeCount);
                const float right = x + width * static_cast<float>(spike + 1) /
                                            static_cast<float>(spikeCount);
                FillTriangle(renderer,
                             {left, y + height},
                             {(left + right) * 0.5f, y},
                             {right, y + height},
                             {0.95f, 0.20f, 0.53f, 1.0f});
            }
        }
    }

    void DrawRunes(SDL_Renderer *renderer, float scale) const {
        for (std::size_t i = 0; i < runes.size(); ++i) {
            const engine::Entity &rune = runes[i];
            if (!rune.active) {
                continue;
            }
            const float bob = std::sin(ambientTime * 3.0f +
                                       static_cast<float>(i) * 1.7f) * 4.0f;
            const float centerX = ScreenX(
                rune.position.x + rune.size.x * 0.5f, scale);
            const float centerY = (rune.position.y + rune.size.y * 0.5f + bob) * scale;
            FillCircle(renderer, centerX, centerY, 24.0f * scale,
                       {255, 203, 72, 22});
            FillTriangle(renderer,
                         {centerX, centerY - 15.0f * scale},
                         {centerX + 11.0f * scale, centerY},
                         {centerX, centerY + 15.0f * scale},
                         {1.0f, 0.67f, 0.20f, 1.0f});
            FillTriangle(renderer,
                         {centerX, centerY - 15.0f * scale},
                         {centerX, centerY + 15.0f * scale},
                         {centerX - 11.0f * scale, centerY},
                         {1.0f, 0.93f, 0.42f, 1.0f});
        }
    }

    void DrawCheckpoint(SDL_Renderer *renderer, float scale) const {
        if (checkpointIndex <= 0) {
            return;
        }
        const float centerX = ScreenX(
            checkpointPosition.x + player.size.x * 0.5f, scale);
        const float baseY = (checkpointPosition.y + player.size.y) * scale;
        FillCircle(renderer, centerX, baseY - 12.0f * scale,
                   15.0f * scale, {74, 255, 214, 28});
        FillTriangle(renderer,
                     {centerX, baseY - 30.0f * scale},
                     {centerX + 9.0f * scale, baseY - 9.0f * scale},
                     {centerX - 9.0f * scale, baseY - 9.0f * scale},
                     {0.29f, 1.0f, 0.82f, 0.82f});
    }

    void DrawGroundPlatform(SDL_Renderer *renderer, float scale) const {
        DrawStonePlatform(renderer, scale, groundPlatform, true);
        for (std::size_t i = 0; i < routePlatforms.size(); ++i) {
            DrawStonePlatform(renderer, scale, routePlatforms[i],
                              i != DISAPPEARING_PLATFORM_INDEX);
        }
    }

    void DrawStonePlatform(SDL_Renderer *renderer, float scale,
                           const engine::Entity &platform, bool mossy) const {
        const float x = ScreenX(platform.position.x, scale);
        const float y = platform.position.y * scale;
        const float width = platform.size.x * scale;
        const float height = platform.size.y * scale;

        if (!platform.active) {
            SDL_SetRenderDrawColor(renderer, 229, 73, 184, 45);
            const SDL_FRect echo = {x, y, width, height};
            SDL_RenderRect(renderer, &echo);
            return;
        }

        SDL_SetRenderDrawColor(renderer, 37, 255, 194, 15);
        const SDL_FRect glowOuter = {x - 8.0f * scale, y - 9.0f * scale,
                                     width + 16.0f * scale, 22.0f * scale};
        SDL_RenderFillRect(renderer, &glowOuter);
        SDL_SetRenderDrawColor(renderer, 66, 255, 204, 42);
        const SDL_FRect glowInner = {x - 3.0f * scale, y - 4.0f * scale,
                                     width + 6.0f * scale, 10.0f * scale};
        SDL_RenderFillRect(renderer, &glowInner);

        engine::Entity platformVisual = platform;
        platformVisual.position.x -= cameraX;
        platformVisual.Draw(renderer, scale);
        SDL_SetRenderDrawColor(renderer, 86, 240, 183, 255);
        SDL_RenderLine(renderer, x, y, x + width, y);

        SDL_SetRenderDrawColor(renderer, 75, 83, 108, 255);
        const float brickHeight = std::max(16.0f, platform.size.y * 0.42f) * scale;
        for (int row = 1; row <= 2 && brickHeight * row < height; ++row) {
            SDL_RenderLine(renderer, x, y + brickHeight * row,
                           x + width, y + brickHeight * row);
        }
        const int columns = std::max(2, static_cast<int>(platform.size.x / 70.0f));
        for (int column = 1; column < columns; ++column) {
            const float offset = (column % 2 == 0 ? 12.0f : -8.0f) * scale;
            const float lineX = x + width * static_cast<float>(column) /
                                      static_cast<float>(columns) + offset;
            SDL_RenderLine(renderer, lineX, y + 3.0f * scale,
                           lineX, y + height - 3.0f * scale);
        }

        if (!mossy) {
            return;
        }
        SDL_SetRenderDrawColor(renderer, 42, 139, 102, 255);
        const std::array<float, 7> moss = {{0.04f, 0.16f, 0.31f, 0.47f,
                                            0.63f, 0.79f, 0.91f}};
        for (std::size_t i = 0; i < moss.size(); ++i) {
            const float desiredWidth =
                (24.0f + static_cast<float>(i % 3) * 10.0f) * scale;
            const float mossWidth = std::min(desiredWidth,
                                             width * (1.0f - moss[i]));
            const SDL_FRect patch = {x + width * moss[i], y - 3.0f * scale,
                                     mossWidth, 7.0f * scale};
            SDL_RenderFillRect(renderer, &patch);
        }
    }

    void DrawExitPortal(SDL_Renderer *renderer, float scale) const {
        const bool unlocked = runeCount == static_cast<int>(RUNE_COUNT);
        const float centerX = ScreenX(
            exitPortal.position.x + exitPortal.size.x * 0.5f, scale);
        const float centerY = (exitPortal.position.y + exitPortal.size.y * 0.48f) * scale;
        FillCircle(renderer, centerX, centerY, 58.0f * scale,
                   unlocked ? SDL_Color{66, 255, 208, 28}
                            : SDL_Color{118, 87, 255, 18});
        FillCircle(renderer, centerX, centerY, 43.0f * scale,
                   unlocked ? SDL_Color{96, 255, 208, 42}
                            : SDL_Color{255, 69, 166, 18});
        engine::Entity portalVisual = exitPortal;
        portalVisual.position.x -= cameraX;
        portalVisual.color = unlocked ? SDL_Color{57, 208, 172, 255}
                                      : SDL_Color{106, 76, 174, 255};
        portalVisual.Draw(renderer, scale);
        const SDL_FRect inner = {
            ScreenX(exitPortal.position.x + 8.0f, scale),
            (exitPortal.position.y + 8.0f) * scale,
            (exitPortal.size.x - 16.0f) * scale,
            (exitPortal.size.y - 8.0f) * scale
        };
        SDL_SetRenderDrawColor(renderer, 19, 22, 54, 255);
        SDL_RenderFillRect(renderer, &inner);
        SDL_SetRenderDrawColor(renderer,
                               unlocked ? 92 : 238,
                               unlocked ? 255 : 73,
                               unlocked ? 213 : 165, 230);
        SDL_RenderLine(renderer, inner.x, inner.y,
                       inner.x + inner.w, inner.y);
        if (!unlocked) {
            SDL_RenderLine(renderer, inner.x, inner.y,
                           inner.x + inner.w, inner.y + inner.h);
            SDL_RenderLine(renderer, inner.x + inner.w, inner.y,
                           inner.x, inner.y + inner.h);
        }
    }

    void DrawTrail(SDL_Renderer *renderer, float scale) const {
        for (const TrailParticle &particle : trail) {
            if (!particle.active || particle.maxLife <= 0.0f) {
                continue;
            }
            const float ratio = std::clamp(particle.life / particle.maxLife,
                                           0.0f, 1.0f);
            SDL_SetRenderDrawColor(renderer, particle.color.r, particle.color.g,
                                   particle.color.b,
                                   static_cast<Uint8>(190.0f * ratio));
            const SDL_FRect point = {
                ScreenX(particle.position.x - particle.size * 0.5f, scale),
                (particle.position.y - particle.size * 0.5f) * scale,
                particle.size * scale, particle.size * scale
            };
            SDL_RenderFillRect(renderer, &point);
        }
    }

    void DrawPlayer(SDL_Renderer *renderer, float scale) const {
        if (!player.active) {
            return;
        }
        const float centerX = ScreenX(
            player.position.x + player.size.x * 0.5f, scale);
        const float centerY = (player.position.y + player.size.y * 0.5f) * scale;
        const float pulse = 1.0f + jumpPulse * 0.11f - landingPulse * 0.08f;
        const float auraRadius = (37.0f + 7.0f *
                                  std::sin(ambientTime * 4.0f)) * scale * pulse;
        FillCircle(renderer, centerX, centerY, auraRadius,
                   {128, 255, 238, 19});
        FillCircle(renderer, centerX, centerY, auraRadius * 0.70f,
                   {172, 255, 244, 34});
        engine::Entity playerVisual = player;
        playerVisual.position.x -= cameraX;
        playerVisual.Draw(renderer, scale);

        const float faceY = (player.position.y + 19.0f) * scale;
        const SDL_Color bodyColor = player.color;
        FillCircle(renderer, centerX, faceY, 18.0f * scale, bodyColor);
        FillCircle(renderer, centerX, (player.position.y + 39.0f) * scale,
                   15.0f * scale, bodyColor);

        SDL_SetRenderDrawColor(renderer, 68, 190, 182, 255);
        SDL_RenderLine(renderer, centerX, (player.position.y + 4.0f) * scale,
                       centerX - 4.0f * scale,
                       (player.position.y - 3.0f) * scale);
        FillCircle(renderer, centerX - 4.0f * scale,
                   (player.position.y - 4.0f) * scale,
                   2.4f * scale, {189, 255, 235, 255});

        const float eyeY = (player.position.y + 18.0f) * scale;
        for (float direction : {-1.0f, 1.0f}) {
            const float eyeX = centerX + direction * 7.0f * scale;
            FillCircle(renderer, eyeX, eyeY, 5.4f * scale,
                       {226, 255, 250, 255});
            FillCircle(renderer, eyeX, eyeY + 0.8f * scale,
                       3.1f * scale, {8, 54, 67, 255});
            FillCircle(renderer, eyeX - 1.1f * scale,
                       eyeY - 1.0f * scale, 1.1f * scale,
                       {255, 255, 255, 255});
        }
        FillCircle(renderer, centerX - 13.0f * scale,
                   (player.position.y + 27.0f) * scale,
                   2.3f * scale, {255, 153, 185, 155});
        FillCircle(renderer, centerX + 13.0f * scale,
                   (player.position.y + 27.0f) * scale,
                   2.3f * scale, {255, 153, 185, 155});
        SDL_SetRenderDrawColor(renderer, 21, 96, 98, 230);
        SDL_RenderLine(renderer, centerX - 2.0f * scale,
                       (player.position.y + 28.5f) * scale,
                       centerX + 2.0f * scale,
                       (player.position.y + 28.5f) * scale);

        FillCircle(renderer, ScreenX(player.position.x + 10.0f, scale),
                   (player.position.y + player.size.y - 1.0f) * scale,
                   5.0f * scale, {47, 190, 178, 255});
        FillCircle(renderer,
                   ScreenX(player.position.x + player.size.x - 10.0f, scale),
                   (player.position.y + player.size.y - 1.0f) * scale,
                   5.0f * scale, {47, 190, 178, 255});
    }

    void DrawPatrolEnemy(SDL_Renderer *renderer, float scale) const {
        if (!patrolEnemy.active) {
            return;
        }
        const float centerX = ScreenX(
            patrolEnemy.position.x + patrolEnemy.size.x * 0.5f, scale);
        const float centerY = (patrolEnemy.position.y + patrolEnemy.size.y * 0.5f) * scale;
        FillCircle(renderer, centerX, centerY, 54.0f * scale,
                   {255, 22, 91, 18});
        FillCircle(renderer, centerX, centerY, 38.0f * scale,
                   {255, 68, 36, 30});

        const float top = patrolEnemy.position.y * scale;
        const float left = ScreenX(patrolEnemy.position.x, scale);
        const float right = ScreenX(
            patrolEnemy.position.x + patrolEnemy.size.x, scale);
        FillTriangle(renderer,
                     {left + 3.0f * scale, top + 8.0f * scale},
                     {left + 10.0f * scale, top - 15.0f * scale},
                     {left + 19.0f * scale, top + 5.0f * scale},
                     {1.0f, 0.18f, 0.34f, 1.0f});
        FillTriangle(renderer,
                     {right - 19.0f * scale, top + 5.0f * scale},
                     {right - 9.0f * scale, top - 16.0f * scale},
                     {right - 3.0f * scale, top + 8.0f * scale},
                     {1.0f, 0.18f, 0.34f, 1.0f});
        engine::Entity patrolVisual = patrolEnemy;
        patrolVisual.position.x -= cameraX;
        patrolVisual.Draw(renderer, scale);

        SDL_SetRenderDrawColor(renderer, 91, 9, 64, 255);
        const SDL_FRect faceShadow = {
            left + 5.0f * scale, top + 8.0f * scale,
            (patrolEnemy.size.x - 10.0f) * scale, 31.0f * scale
        };
        SDL_RenderFillRect(renderer, &faceShadow);
        FillTriangle(renderer,
                     {left + 8.0f * scale, top + 15.0f * scale},
                     {left + 20.0f * scale, top + 19.0f * scale},
                     {left + 9.0f * scale, top + 22.0f * scale},
                     {1.0f, 0.69f, 0.16f, 1.0f});
        FillTriangle(renderer,
                     {right - 8.0f * scale, top + 15.0f * scale},
                     {right - 20.0f * scale, top + 19.0f * scale},
                     {right - 9.0f * scale, top + 22.0f * scale},
                     {1.0f, 0.69f, 0.16f, 1.0f});

        SDL_SetRenderDrawColor(renderer, 24, 3, 29, 255);
        const SDL_FRect mouth = {
            centerX - 12.0f * scale, top + 29.0f * scale,
            24.0f * scale, 8.0f * scale
        };
        SDL_RenderFillRect(renderer, &mouth);
        for (int tooth = 0; tooth < 3; ++tooth) {
            const float toothX = centerX + (static_cast<float>(tooth) - 1.0f) *
                                          7.0f * scale;
            FillTriangle(renderer,
                         {toothX - 2.5f * scale, mouth.y},
                         {toothX + 2.5f * scale, mouth.y},
                         {toothX, mouth.y + 5.0f * scale},
                         {1.0f, 0.87f, 0.66f, 1.0f});
        }

        SDL_SetRenderDrawColor(renderer, 255, 104, 93, 230);
        const float bottom = (patrolEnemy.position.y + patrolEnemy.size.y) * scale;
        for (int i = 0; i < 4; ++i) {
            const float x = ScreenX(
                patrolEnemy.position.x + 6.0f + i * 12.0f, scale);
            FillTriangle(renderer,
                         {x - 4.0f * scale, bottom - 1.0f * scale},
                         {x + 7.0f * scale, bottom - 1.0f * scale},
                         {x + 2.0f * scale, bottom + 11.0f * scale},
                         {1.0f, 0.29f, 0.31f, 0.92f});
        }
    }

    void DrawSentinelEnemy(SDL_Renderer *renderer, float scale) const {
        if (!sentinelEnemy.active) {
            return;
        }
        const float centerX =
            ScreenX(sentinelEnemy.position.x + sentinelEnemy.size.x * 0.5f,
                    scale);
        const float centerY =
            (sentinelEnemy.position.y + sentinelEnemy.size.y * 0.5f) * scale;
        FillCircle(renderer, centerX, centerY, 39.0f * scale,
                   {255, 29, 73, 22});
        FillCircle(renderer, centerX, centerY, 29.0f * scale,
                   {255, 91, 25, 31});
        const float left = ScreenX(sentinelEnemy.position.x, scale);
        const float right = ScreenX(
            sentinelEnemy.position.x + sentinelEnemy.size.x, scale);
        const float top = sentinelEnemy.position.y * scale;
        const float bottom = (sentinelEnemy.position.y + sentinelEnemy.size.y) * scale;
        FillTriangle(renderer, {left, top + 10.0f * scale},
                     {left - 10.0f * scale, centerY},
                     {left, bottom - 7.0f * scale},
                     {1.0f, 0.19f, 0.27f, 1.0f});
        FillTriangle(renderer, {right, top + 10.0f * scale},
                     {right + 10.0f * scale, centerY},
                     {right, bottom - 7.0f * scale},
                     {1.0f, 0.19f, 0.27f, 1.0f});
        FillTriangle(renderer, {left + 4.0f * scale, top},
                     {centerX, top - 12.0f * scale},
                     {right - 4.0f * scale, top},
                     {1.0f, 0.19f, 0.27f, 1.0f});
        FillTriangle(renderer, {left + 4.0f * scale, bottom},
                     {centerX, bottom + 12.0f * scale},
                     {right - 4.0f * scale, bottom},
                     {1.0f, 0.19f, 0.27f, 1.0f});
        engine::Entity sentinelVisual = sentinelEnemy;
        sentinelVisual.position.x -= cameraX;
        sentinelVisual.Draw(renderer, scale);
        SDL_SetRenderDrawColor(renderer, 255, 226, 108, 255);
        const SDL_FRect eye = {
            centerX - 5.0f * scale, centerY - 4.0f * scale,
            10.0f * scale, 7.0f * scale
        };
        SDL_RenderFillRect(renderer, &eye);
        SDL_SetRenderDrawColor(renderer, 92, 20, 74, 255);
        SDL_RenderLine(renderer, centerX, eye.y,
                       centerX, eye.y + eye.h);
    }

    void DrawHud(SDL_Renderer *renderer) const {
        int width = 0;
        int height = 0;
        app.GetWindowSize(&width, &height);
        SDL_SetRenderDrawColor(renderer, 5, 10, 29, 205);
        const SDL_FRect panel = {14.0f, 14.0f,
                                 std::min(885.0f, static_cast<float>(width) - 28.0f),
                                 101.0f};
        SDL_RenderFillRect(renderer, &panel);
        SDL_SetRenderDrawColor(renderer, 74, 241, 213, 190);
        SDL_RenderRect(renderer, &panel);

        SDL_SetRenderDrawColor(renderer, 139, 255, 226, 255);
        SDL_RenderDebugTextFormat(renderer, 27.0f, 25.0f,
                                  "NEON GROVE ESCAPE   RUN: %05.1fs   LIVES: %d/%d",
                                  runTime, MAX_DEATHS - deaths, MAX_DEATHS);
        SDL_SetRenderDrawColor(renderer, 209, 222, 255, 255);
        SDL_RenderDebugText(renderer, 27.0f, 43.0f,
                            "MOVE: A/D OR LEFT/RIGHT   JUMP: SPACE   RESTART: R   EXIT: ESC");
        SDL_SetRenderDrawColor(renderer, 255, 218, 92, 255);
        SDL_RenderDebugTextFormat(renderer, 27.0f, 61.0f,
                                  "RUNES: %d/6   GATE: %s   CHECKPOINT: %d/3",
                                  runeCount,
                                  runeCount == static_cast<int>(RUNE_COUNT)
                                      ? "OPEN" : "LOCKED",
                                  checkpointIndex);
        SDL_SetRenderDrawColor(renderer, 255, 176, 225, 255);
        SDL_RenderDebugTextFormat(renderer, 27.0f, 79.0f,
                                  "SCALING: %s - PRESS T TO TOGGLE",
                                  engine::Scaling::GetModeName());
        SDL_SetRenderDrawColor(renderer, 167, 191, 233, 255);
        SDL_RenderDebugText(renderer, 27.0f, 97.0f,
                            "KEEP MOVING RIGHT. COLLECT 6 RUNES AND REACH THE MOON GATE.");

        if (scalingNotice > 0.0f) {
            SDL_SetRenderDrawColor(renderer, 94, 255, 213, 255);
            SDL_RenderDebugText(renderer, width * 0.5f - 76.0f,
                                112.0f, "SCALING MODE CHANGED");
        }
        if (lockedNotice > 0.0f) {
            SDL_SetRenderDrawColor(renderer, 255, 112, 176, 255);
            SDL_RenderDebugText(renderer, width * 0.5f - 96.0f,
                                132.0f, "THE MOON GATE NEEDS ALL 6 RUNES");
        }
        if (gameOver || awaitingDeathContinue || respawnCountdown > 0.0f) {
            SDL_SetRenderDrawColor(renderer, 2, 5, 15, 178);
            const SDL_FRect shade = {
                0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)
            };
            SDL_RenderFillRect(renderer, &shade);

            const SDL_FRect lifePanel = {
                width * 0.5f - 190.0f, height * 0.5f - 68.0f,
                380.0f, 136.0f
            };
            SDL_SetRenderDrawColor(renderer, 7, 13, 35, 245);
            SDL_RenderFillRect(renderer, &lifePanel);
            SDL_SetRenderDrawColor(renderer,
                                   gameOver ? 255 : 105,
                                   gameOver ? 83 : 255,
                                   gameOver ? 145 : 214, 255);
            SDL_RenderRect(renderer, &lifePanel);

            if (gameOver) {
                SDL_SetRenderDrawColor(renderer, 255, 91, 151, 255);
                SDL_RenderDebugText(renderer, width * 0.5f - 36.0f,
                                    height * 0.5f - 38.0f, "GAME OVER");
                SDL_SetRenderDrawColor(renderer, 229, 218, 242, 255);
                SDL_RenderDebugText(renderer, width * 0.5f - 68.0f,
                                    height * 0.5f - 10.0f,
                                    "ALL 10 LIVES WERE LOST");
                SDL_SetRenderDrawColor(renderer, 255, 218, 92, 255);
                SDL_RenderDebugText(renderer, width * 0.5f - 108.0f,
                                    height * 0.5f + 25.0f,
                                    "PRESS SPACE TO START A NEW RUN");
            } else if (awaitingDeathContinue) {
                SDL_SetRenderDrawColor(renderer, 255, 112, 176, 255);
                SDL_RenderDebugText(renderer, width * 0.5f - 76.0f,
                                    height * 0.5f - 38.0f,
                                    "ONE LIFE WAS LOST");
                SDL_SetRenderDrawColor(renderer, 218, 229, 255, 255);
                SDL_RenderDebugTextFormat(renderer, width * 0.5f - 72.0f,
                                          height * 0.5f - 10.0f,
                                          "LIVES REMAINING: %d",
                                          MAX_DEATHS - deaths);
                SDL_SetRenderDrawColor(renderer, 126, 255, 207, 255);
                SDL_RenderDebugText(renderer, width * 0.5f - 92.0f,
                                    height * 0.5f + 25.0f,
                                    "PRESS SPACE TO CONTINUE");
            } else {
                const int seconds = std::clamp(
                    static_cast<int>(std::ceil(respawnCountdown)), 1, 3);
                SDL_SetRenderDrawColor(renderer, 126, 255, 207, 255);
                SDL_RenderDebugText(renderer, width * 0.5f - 36.0f,
                                    height * 0.5f - 30.0f, "GET READY");
                SDL_SetRenderDrawColor(renderer, 255, 218, 92, 255);
                SDL_RenderDebugTextFormat(renderer, width * 0.5f - 64.0f,
                                          height * 0.5f + 8.0f,
                                          "RESUMING IN %d...", seconds);
            }
        }
        if (escaped) {
            SDL_SetRenderDrawColor(renderer, 5, 10, 29, 225);
            const SDL_FRect successPanel = {
                width * 0.5f - 170.0f, height * 0.5f - 42.0f,
                340.0f, 102.0f
            };
            SDL_RenderFillRect(renderer, &successPanel);
            SDL_SetRenderDrawColor(renderer, 126, 255, 207, 255);
            SDL_RenderRect(renderer, &successPanel);
            SDL_RenderDebugText(renderer, width * 0.5f - 55.0f,
                                height * 0.5f - 22.0f, "GROVE ESCAPED!");
            SDL_SetRenderDrawColor(renderer, 218, 229, 255, 255);
            SDL_RenderDebugTextFormat(renderer, width * 0.5f - 74.0f,
                                      height * 0.5f,
                                      "TIME %05.1fs   BEST %05.1fs",
                                      runTime, bestTime);
            SDL_RenderDebugText(renderer, width * 0.5f - 72.0f,
                                height * 0.5f + 22.0f, "PRESS R TO PLAY AGAIN");
        }
    }

    engine::Engine &app;
    float viewportWidth;
    float worldWidth;
    float worldHeight;
    float cameraX = 0.0f;

    // The names intentionally mirror the three Milestone 1 entity roles.
    engine::Entity groundPlatform;
    engine::Entity player;
    engine::Entity patrolEnemy;
    engine::Entity sentinelEnemy;
    engine::Entity exitPortal;
    std::array<engine::Entity, PLATFORM_COUNT> routePlatforms{};
    std::array<engine::Entity, HAZARD_COUNT> hazards{};
    std::array<engine::Entity, RUNE_COUNT> runes{};

    engine::Vec2 spawnPosition;
    engine::Vec2 checkpointPosition;
    float patrolLeftEndpoint = 0.0f;
    float patrolRightEndpoint = 0.0f;
    float patrolDirection = 1.0f;
    float sentinelDirection = 1.0f;
    float ambientTime = 0.0f;
    float runTime = 0.0f;
    float bestTime = 0.0f;
    float escapeSequence = 0.0f;
    float invulnerability = 0.0f;
    float collisionFlash = 0.0f;
    float jumpPulse = 0.0f;
    float landingPulse = 0.0f;
    float jumpBuffer = 0.0f;
    float trailTimer = 0.0f;
    float scalingNotice = 0.0f;
    float lockedNotice = 0.0f;
    float respawnCountdown = 0.0f;
    int runeCount = 0;
    int deaths = 0;
    int checkpointIndex = 0;
    bool grounded = true;
    bool escaped = false;
    bool awaitingDeathContinue = false;
    bool gameOver = false;
    bool disappearingWarning = false;
    bool scalingDemoResized = false;

    std::array<SDL_FPoint, 96> stars{};
    std::array<Firefly, 26> fireflies{};
    std::array<TrailParticle, 72> trail{};
    std::size_t nextTrailParticle = 0;
    std::uint32_t randomState = 0x4e454f4eu;
};

} // namespace

int main(int argc, char **argv) {
    bool smokeTest = false;
    bool logicTest = false;
    bool captureFrame = false;
    bool capturePixelResized = false;
    bool captureProportional = false;
    for (int i = 1; i < argc; ++i) {
        if (SDL_strcmp(argv[i], "--smoke-test") == 0) {
            smokeTest = true;
        } else if (SDL_strcmp(argv[i], "--logic-test") == 0) {
            logicTest = true;
        } else if (SDL_strcmp(argv[i], "--capture-frame") == 0) {
            captureFrame = true;
        } else if (SDL_strcmp(argv[i], "--capture-pixel-resized") == 0) {
            captureFrame = true;
            capturePixelResized = true;
        } else if (SDL_strcmp(argv[i], "--capture-proportional") == 0) {
            captureFrame = true;
            captureProportional = true;
        }
    }

    engine::Engine app;
    if (!app.Init("CSC 581 - Neon Grove Escape")) {
        return 1;
    }

    NeonGroveEscapeGame game(app);
    if (logicTest) {
        const bool passed = game.RunLogicVerification();
        app.Shutdown();
        return passed ? 0 : 1;
    }
    if (capturePixelResized || captureProportional) {
        SetWindowSize(app, 1024, 576);
    }
    if (captureProportional) {
        engine::Scaling::Toggle();
    }

    int verificationFrames = 0;
    bool frameCaptured = false;
    app.Run(
        [&](float dt) {
            game.Update(dt);
            if ((smokeTest || captureFrame) && ++verificationFrames >= 3) {
                RequestQuit();
            }
        },
        [&]() {
            game.Render();
            if (captureFrame && !frameCaptured && verificationFrames >= 2) {
                SDL_Surface *surface = SDL_RenderReadPixels(app.GetRenderer(), nullptr);
                if (surface) {
                    const char *path = captureProportional
                        ? "/tmp/neon_grove_escape_proportional.bmp"
                        : capturePixelResized
                            ? "/tmp/neon_grove_escape_pixel_resized.bmp"
                            : "/tmp/neon_grove_escape.bmp";
                    SDL_SaveBMP(surface, path);
                    SDL_DestroySurface(surface);
                }
                frameCaptured = true;
            }
        });

    app.Shutdown();
    return 0;
}
