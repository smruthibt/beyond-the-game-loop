#pragma once
#include "engine/Timeline.hpp"
#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>

namespace towerfall {
constexpr int Width = 1920, Height = 1080, MaxPlayers = 4, Hz = 100;
constexpr float StepSeconds = 1.f / Hz, ArcherWidth = 36, ArcherHeight = 58;
// At gravity 2000, this clears the largest 338 px ledge step even at 2x time
// (20 ms integration). Ground and wall jumps have the same vertical reach.
constexpr float JumpVelocity = -1200.f;
// Top of the playfield, just below the HUD bar. Arrows are discarded above it,
// so archers stop here rather than rising out of reach behind the HUD.
constexpr float Ceiling = 130;
// Seconds of the archer's own time. The HUD draws the reload against it.
constexpr float ShootDelay = .33f;
constexpr int NoResult = -2, Draw = -1;
constexpr int PracticeWarmupTicks = 8 * Hz;
enum Buttons {
  Left = 1,
  Right = 2,
  Jump = 4,
  Shoot = 8,
  Drop = 32
};
struct Input {
  int buttons = 0, aim = 0, speed = 2; // speed / 2: 0.5x, 1x, 2x
  bool paused = false;
  bool operator==(const Input &o) const {
    return buttons == o.buttons && aim == o.aim && speed == o.speed &&
           paused == o.paused;
  }
  bool operator!=(const Input &o) const { return !(*this == o); }
};
struct Rect {
  float x = 0, y = 0, w = 0, h = 0;
};
struct Surface {
  int id = 0;
  Rect box;
  bool oneWay = false;
  int axis = 0, travel = 0, period = 400, offset = 0;
};
struct Archer {
  float x = 0, y = 0, vx = 0, vy = 0;
  int ammo = 3, facing = 1, riding = -1, wall = 0;
  int64_t personalUs = 0;
  float shootWait = 0, groundGrace = 0, jumpBuffer = 0, wallGrace = 0;
  int previousButtons = 0, kills = 0;
  // `present`: someone is playing this archer. An absent slot is never
  // alive, drawn, or counted.
  bool alive = true, grounded = false, present = true;
  Input input;
};
enum class ArrowState { Flying, Stuck, Consumed };
struct Arrow {
  int id = 0, owner = 0, born = 0, platform = -1;
  float x = 0, y = 0, vx = 0, vy = 0, attachX = 0, attachY = 0;
  ArrowState state = ArrowState::Flying;
};
struct World {
  int tick = 0, round = 0, count = 3, nextArrow = 0, result = NoResult,
      resultTick = -1, shots = 0, hits = 0;
  // Bitmask of slots that have had an archer this round; two or more make it
  // a contest.
  int entered = 0;
  std::array<Archer, MaxPlayers> archers{};
  std::vector<Arrow> arrows;
};
struct Command {
  int owner = 0, number = 0, tick = 1;
  Input input;
};
// An archer joining or leaving mid-round. `order` breaks ties within a tick
// and identifies the change, so it can be replayed like a command.
struct RosterChange {
  int owner = 0, tick = 1;
  bool joins = true;
  int64_t order = 0;
};
// The diamond rides this moving platform. Only the last archer alive can
// claim it, and claiming it wins the round - so a lone archer just has to
// reach it, and rivals must first eliminate each other.
constexpr int DiamondSurface = 9;
constexpr float DiamondSize = 28;
std::vector<Surface> Arena();
Rect SurfaceAt(const Surface &, int tick);
World NewRound(int round, int count);
// `roster` is a bitmask of the slots with an archer at the start.
World NewRound(int round, int count, int roster);
Rect DiamondAt(int tick);
// The only archer still alive, or -1 when none or several are.
int LastStanding(const World &);
int Slots(int mask);
uint64_t WorldHash(const World &);
// Fraction of a segment at its first intersection, or > 1 when it misses.
float SegmentHit(float ax, float ay, float bx, float by, Rect bounds);
int PracticeShotPhase(int tick, int id);
Input BotInput(const World &, int id, bool passive = false,
               bool practice = false);

class Simulation {
public:
  Simulation(int round, int count);
  Simulation(int round, int count, int roster);
  bool Add(Command);
  bool Add(RosterChange);
  void Seek(int targetTick);
  const World &State() const { return state; }
  int Rollbacks() const { return rollbacks; }
  const std::vector<Command> &Commands() const { return commands; }

private:
  void Restore(const World &);
  // Rewinds to the last checkpoint before `tick`, so an input that arrives
  // late is replayed in order.
  void Rewind(int tick);
  void Step();
  World state;
  std::vector<Surface> surfaces;
  std::vector<Command> commands;
  std::vector<RosterChange> roster;
  std::map<int, World> history;
  std::atomic<int64_t> simulatedUs{0};
  std::array<std::unique_ptr<engine::Timeline>, MaxPlayers> clocks;
  std::array<int64_t, MaxPlayers> lastClock{};
  int rollbacks = 0;
};
} // namespace towerfall
