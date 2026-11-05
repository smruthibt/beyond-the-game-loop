#include "Simulation.hpp"
#include "engine/Physics.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <tuple>

namespace towerfall {
namespace {
float Quantize(float x) { return std::round(x * 256.f) / 256.f; }
constexpr float DX[]{1,  .70710678f,  0, -.70710678f,
                     -1, -.70710678f, 0, .70710678f};
constexpr float DY[]{0, .70710678f,  1,  .70710678f,
                     0, -.70710678f, -1, -.70710678f};
bool Overlap(Rect a, Rect b) {
  return a.x < b.x + b.w && a.x + a.w > b.x && a.y < b.y + b.h &&
         a.y + a.h > b.y;
}
Rect Body(const Archer &a) { return {a.x, a.y, ArcherWidth, ArcherHeight}; }
} // namespace
std::vector<Surface> Arena() {
  std::vector<Surface> s{{0, {100, 958, 1720, 90}, false},
                         {1, {68, 138, 32, 910}, false},
                         {2, {1820, 138, 32, 910}, false},
                         {3, {210, 754, 350, 30}, true},
                         {4, {1360, 754, 350, 30}, true},
                         {5, {720, 596, 480, 30}, true},
                         {6, {295, 416, 280, 30}, true},
                         {7, {1345, 416, 280, 30}, true},
                         {8, {670, 820, 160, 26}, true, 1, 210, 480, 0},
                         {9, {1120, 610, 150, 26}, true, 2, 180, 600, 150},
                         {10, {940, 738, 40, 220}, false}};
  return s;
}
Rect SurfaceAt(const Surface &s, int tick) {
  Rect r = s.box;
  if (s.axis) {
    int t = (tick + s.offset) % s.period;
    int half = s.period / 2;
    int triangle = t < half ? t : s.period - t;
    float distance = float(s.travel * triangle) / half;
    if (s.axis == 1)
      r.x += distance;
    else
      r.y -= distance;
  }
  return r;
}
int Slots(int mask) {
  int n = 0;
  for (; mask; mask &= mask - 1)
    ++n;
  return n;
}
World NewRound(int round, int count) {
  return NewRound(round, count, (1 << count) - 1);
}
World NewRound(int round, int count, int roster) {
  World w;
  w.round = round;
  w.count = count;
  roster &= (1 << count) - 1;
  constexpr float xs[]{270, 1590, 810, 1050};
  for (int i = 0; i < MaxPlayers; ++i) {
    auto &a = w.archers[i];
    a.x = xs[i];
    a.y = (i < 2 ? 754 : 596) - ArcherHeight;
    a.facing = i % 2 ? -1 : 1;
    a.input.aim = i % 2 ? 4 : 0;
    a.present = a.alive = (roster >> i) & 1;
  }
  w.entered = roster;
  return w;
}
Rect DiamondAt(int tick) {
  Rect r = SurfaceAt(Arena()[DiamondSurface], tick);
  return {r.x + (r.w - DiamondSize) / 2, r.y - DiamondSize - 2, DiamondSize,
          DiamondSize};
}
int LastStanding(const World &w) {
  int last = -1;
  for (int i = 0; i < w.count; ++i)
    if (w.archers[i].alive) {
      if (last >= 0)
        return -1;
      last = i;
    }
  return last;
}
float SegmentHit(float ax, float ay, float bx, float by, Rect r) {
  float enter = 0, exit = 1;
  for (int axis = 0; axis < 2; ++axis) {
    float p = axis ? ay : ax, d = axis ? by - ay : bx - ax,
          lo = axis ? r.y : r.x, hi = lo + (axis ? r.h : r.w);
    if (std::abs(d) < 1e-7f) {
      if (p < lo || p > hi)
        return 2;
      continue;
    }
    float a = (lo - p) / d, b = (hi - p) / d;
    if (a > b)
      std::swap(a, b);
    enter = std::max(enter, a);
    exit = std::min(exit, b);
    if (enter > exit)
      return 2;
  }
  return enter;
}
uint64_t WorldHash(const World &w) {
  uint64_t h = 1469598103934665603ULL;
  auto add = [&](int64_t v) {
    for (int b = 0; b < 8; ++b) {
      h ^= (uint64_t(v) >> (b * 8)) & 255;
      h *= 1099511628211ULL;
    }
  };
  auto number = [&](float f) { add(int64_t(std::llround(f * 65536))); };
  add(w.tick);
  add(w.round);
  add(w.count);
  add(w.nextArrow);
  add(w.result);
  add(w.resultTick);
  add(w.shots);
  add(w.hits);
  add(w.entered);
  for (const auto &a : w.archers) {
    add(a.present);
    number(a.x);
    number(a.y);
    number(a.vx);
    number(a.vy);
    add(a.ammo);
    add(a.facing);
    add(a.riding);
    add(a.wall);
    add(a.personalUs);
    number(a.shootWait);
    number(a.groundGrace);
    number(a.jumpBuffer);
    number(a.wallGrace);
    add(a.previousButtons);
    add(a.kills);
    add(a.alive);
    add(a.grounded);
    add(a.input.buttons);
    add(a.input.aim);
    add(a.input.speed);
    add(a.input.paused);
  }
  for (const auto &a : w.arrows) {
    add(a.id);
    add(a.owner);
    add(a.born);
    add(a.platform);
    number(a.x);
    number(a.y);
    number(a.vx);
    number(a.vy);
    number(a.attachX);
    number(a.attachY);
    add(int(a.state));
  }
  return h;
}
Simulation::Simulation(int round, int count)
    : Simulation(round, count, (1 << count) - 1) {}
Simulation::Simulation(int round, int count, int roster)
    : state(NewRound(round, count, roster)), surfaces(Arena()) {
  Restore(state);
  history.emplace(0, state);
}
void Simulation::Restore(const World &w) {
  state = w;
  simulatedUs = int64_t(w.tick) * 10000;
  for (int i = 0; i < MaxPlayers; ++i) {
    clocks[i] = std::make_unique<engine::Timeline>(simulatedUs, 1);
    lastClock[i] = 0;
  }
}
bool Simulation::Add(Command c) {
  if (c.owner < 0 || c.owner >= state.count || c.number < 1 || c.tick < 1 ||
      c.input.aim < 0 || c.input.aim > 7 ||
      c.input.buttons < 0 || c.input.buttons > 63 ||
      (c.input.speed != 1 && c.input.speed != 2 && c.input.speed != 4))
    return false;
  for (const auto &o : commands)
    if (o.owner == c.owner && o.number == c.number)
      return false;
  commands.push_back(c);
  std::sort(commands.begin(), commands.end(), [](auto &a, auto &b) {
    return std::tie(a.tick, a.owner, a.number) <
           std::tie(b.tick, b.owner, b.number);
  });
  Rewind(c.tick);
  return true;
}
bool Simulation::Add(RosterChange c) {
  if (c.owner < 0 || c.owner >= state.count || c.tick < 1)
    return false;
  for (const auto &o : roster)
    if (o.order == c.order)
      return false;
  roster.push_back(c);
  std::sort(roster.begin(), roster.end(), [](auto &a, auto &b) {
    return std::tie(a.tick, a.order) < std::tie(b.tick, b.order);
  });
  Rewind(c.tick);
  return true;
}
void Simulation::Rewind(int tick) {
  if (tick > state.tick)
    return;
  auto checkpoint = history.lower_bound(tick);
  --checkpoint;
  World restore = checkpoint->second;
  history.erase(std::next(checkpoint), history.end());
  Restore(restore);
  ++rollbacks;
}
void Simulation::Seek(int target) {
  target = std::max(target, 0);
  if (target < state.tick) {
    auto checkpoint = history.upper_bound(target);
    --checkpoint;
    Restore(checkpoint->second);
  }
  while (state.tick < target) {
    Step();
    if (state.tick % 25 == 0)
      history[state.tick] = state;
  }
}
void Simulation::Step() {
  ++state.tick;
  for (const auto &c : commands)
    if (c.tick == state.tick)
      state.archers[c.owner].input = c.input;
  // Counter-rooted personal timelines measure simulated microseconds. This
  // makes replay independent of frame rate while using the engine's actual
  // pause/scale logic.
  for (int i = 0; i < state.count; ++i) {
    auto &clock = *clocks[i];
    clock.SetScale(state.archers[i].input.speed * .5);
    if (state.archers[i].input.paused)
      clock.Pause();
    else
      clock.Unpause();
  }
  simulatedUs += 10000;
  std::array<float, MaxPlayers> delta{};
  for (int i = 0; i < state.count; ++i) {
    int64_t value = clocks[i]->GetTime();
    delta[i] = float(value - lastClock[i]) / 1000000.f;
    lastClock[i] = value;
  }
  if (state.result != NoResult)
    return;
  for (const auto &r : roster)
    if (r.tick == state.tick) {
      auto &p = state.archers[r.owner];
      if (!r.joins) {
        p.present = p.alive = false;
        continue;
      }
      // A newcomer starts exactly as it would at a round start, at its
      // slot's spawn point.
      p = NewRound(state.round, state.count, 1 << r.owner).archers[r.owner];
      state.entered |= 1 << r.owner;
    }
  std::array<Archer, MaxPlayers> before = state.archers;
  std::vector<Rect> boxes, oldBoxes;
  for (const auto &s : surfaces) {
    boxes.push_back(SurfaceAt(s, state.tick));
    oldBoxes.push_back(SurfaceAt(s, state.tick - 1));
  }
  for (int id = 0; id < state.count; ++id) {
    auto &p = state.archers[id];
    const auto in = p.input;
    const float dt = delta[id];
    if (!p.alive || dt == 0)
      continue;
    p.personalUs += int64_t(std::llround(dt * 1000000));
    p.shootWait = std::max(0.f, p.shootWait - dt);
    p.groundGrace = p.grounded ? .10f : std::max(0.f, p.groundGrace - dt);
    p.wallGrace = std::max(0.f, p.wallGrace - dt);
    bool pressedJump = (in.buttons & Jump) && !(p.previousButtons & Jump);
    p.jumpBuffer = pressedJump ? .12f : std::max(0.f, p.jumpBuffer - dt);
    if (p.riding >= 0 && p.riding < int(boxes.size())) {
      p.x += boxes[p.riding].x - oldBoxes[p.riding].x;
      p.y += boxes[p.riding].y - oldBoxes[p.riding].y;
    }
    int horizontal =
        ((in.buttons & Right) ? 1 : 0) - ((in.buttons & Left) ? 1 : 0);
    if (horizontal)
      p.facing = horizontal;
    float target = horizontal * 375.f;
    p.vx += (target - p.vx) * std::min(1.f, dt * 22);
    if (p.jumpBuffer > 0 && p.groundGrace > 0) {
      p.vy = JumpVelocity;
      p.jumpBuffer = 0;
      p.groundGrace = 0;
      p.riding = -1;
      p.grounded = false;
    } else if (p.jumpBuffer > 0 && p.wallGrace > 0) {
      p.vy = JumpVelocity;
      p.vx = p.wall * 620.f;
      p.jumpBuffer = 0;
      p.wallGrace = 0;
    }
    engine::Entity body;
    body.position = {p.x, p.y};
    body.velocity = {p.vx, p.vy};
    body.affectedByGravity = true;
    engine::Physics::Update(body, dt);
    float nx = body.position.x, ny = body.position.y;
    p.vy = body.velocity.y;
    for (size_t k = 0; k < boxes.size(); ++k)
      if (!surfaces[k].oneWay &&
          Overlap({nx, p.y, ArcherWidth, ArcherHeight}, boxes[k])) {
        auto r = boxes[k];
        if (p.vx > 0) {
          nx = r.x - ArcherWidth;
          p.wall = -1;
        } else if (p.vx < 0) {
          nx = r.x + r.w;
          p.wall = 1;
        }
        p.vx = 0;
        p.wallGrace = .10f;
        if (p.vy > 180)
          p.vy = 180;
      }
    p.grounded = false;
    p.riding = -1;
    for (size_t k = 0; k < boxes.size(); ++k) {
      auto r = boxes[k];
      if (nx + ArcherWidth <= r.x || nx >= r.x + r.w)
        continue;
      bool crossed =
          p.y + ArcherHeight <= oldBoxes[k].y + 5 && ny + ArcherHeight >= r.y;
      if (p.vy >= 0 && crossed &&
          (!(in.buttons & Drop) || !surfaces[k].oneWay)) {
        ny = r.y - ArcherHeight;
        p.vy = 0;
        p.grounded = true;
        p.riding = int(k);
      } else if (!surfaces[k].oneWay && p.vy < 0 &&
                 p.y >= oldBoxes[k].y + oldBoxes[k].h - 2 && ny < r.y + r.h) {
        ny = r.y + r.h;
        p.vy = 0;
      }
    }
    if (ny < Ceiling) {
      ny = Ceiling;
      p.vy = std::max(p.vy, 0.f);
    }
    p.x = Quantize(std::clamp(nx, 100.f, 1820.f - ArcherWidth));
    p.y = Quantize(ny);
    p.vx = Quantize(p.vx);
    p.vy = Quantize(p.vy);
    if ((in.buttons & Shoot) && p.shootWait <= 0 && p.ammo > 0) {
      --p.ammo;
      p.shootWait = ShootDelay;
      int d = in.aim;
      Arrow a;
      a.id = state.nextArrow++;
      a.owner = id;
      a.born = state.tick;
      a.x = Quantize(p.x + 18 + DX[d] * 30);
      a.y = Quantize(p.y + 26 + DY[d] * 30);
      a.vx = DX[d] * 1320;
      a.vy = DY[d] * 1320;
      state.arrows.push_back(a);
      ++state.shots;
    }
    p.previousButtons = in.buttons;
  }
  std::array<bool, MaxPlayers> victims{};
  for (auto &a : state.arrows) {
    if (a.state == ArrowState::Consumed)
      continue;
    if (a.state == ArrowState::Stuck) {
      if (a.platform >= 0) {
        a.x = boxes[a.platform].x + a.attachX;
        a.y = boxes[a.platform].y + a.attachY;
      }
      if (state.tick - a.born > 20)
        for (int id = 0; id < state.count; ++id) {
          auto &p = state.archers[id];
          if (p.alive && !p.input.paused && p.ammo < 5 &&
              Overlap({a.x - 15, a.y - 15, 30, 30}, Body(p))) {
            ++p.ammo;
            a.state = ArrowState::Consumed;
            break;
          }
        }
      continue;
    }
    float bx = a.x + a.vx * StepSeconds, by = a.y + a.vy * StepSeconds,
          earliest = 2;
    int surface = -1, victim = -1;
    for (size_t k = 0; k < boxes.size(); ++k) {
      auto r = oldBoxes[k];
      r.x -= 2;
      r.y -= 2;
      r.w += 4;
      r.h += 4;
      float f = SegmentHit(a.x, a.y, bx - (boxes[k].x - oldBoxes[k].x),
                           by - (boxes[k].y - oldBoxes[k].y), r);
      if (f < earliest) {
        earliest = f;
        surface = int(k);
        victim = -1;
      }
    }
    for (int id = 0; id < state.count; ++id) {
      const auto &p = state.archers[id];
      if (!p.alive || (id == a.owner && state.tick - a.born < 12))
        continue;
      auto r = Body(before[id]);
      r.x -= 2;
      r.y -= 2;
      r.w += 4;
      r.h += 4;
      float f = SegmentHit(a.x, a.y, bx - (p.x - before[id].x),
                           by - (p.y - before[id].y), r);
      if (f < earliest) {
        earliest = f;
        victim = id;
        surface = -1;
      }
    }
    if (earliest <= 1) {
      a.x = Quantize(a.x + (bx - a.x) * earliest);
      a.y = Quantize(a.y + (by - a.y) * earliest);
      if (victim >= 0) {
        a.state = ArrowState::Consumed;
        victims[victim] = true;
        ++state.hits;
        if (victim != a.owner)
          ++state.archers[a.owner].kills;
      } else {
        a.state = ArrowState::Stuck;
        a.platform = surface;
        a.attachX = a.x - boxes[surface].x;
        a.attachY = a.y - boxes[surface].y;
      }
    } else {
      a.x = Quantize(bx);
      a.y = Quantize(by);
      if (a.x < 0 || a.x > Width || a.y < Ceiling || a.y > Height)
        a.state = ArrowState::Consumed;
    }
  }
  for (int i = 0; i < state.count; ++i) {
    auto &p = state.archers[i];
    if (victims[i] || p.y > 1050)
      p.alive = false;
  }
  // The last archer alive wins by claiming the diamond. A paused archer is
  // out of play, so in a contest it cannot win by waiting while the others
  // eliminate each other: if the last one standing is paused, it is a draw.
  // Pause is part of each archer's replicated input, so every process
  // reaches the same result.
  const int last = LastStanding(state);
  bool anyAlive = false;
  for (int i = 0; i < state.count; ++i)
    anyAlive = anyAlive || state.archers[i].alive;
  if (!anyAlive && state.entered)
    state.result = Draw;
  else if (last >= 0 && Slots(state.entered) >= 2 &&
           state.archers[last].input.paused)
    state.result = Draw;
  else if (last >= 0 && Overlap(Body(state.archers[last]), DiamondAt(state.tick)))
    state.result = last;
  if (state.result != NoResult)
    state.resultTick = state.tick;
  state.arrows.erase(std::remove_if(state.arrows.begin(), state.arrows.end(),
                                    [](const Arrow &a) {
                                      return a.state == ArrowState::Consumed;
                                    }),
                     state.arrows.end());
}
int PracticeShotPhase(int tick, int id) {
  int elapsed = tick - PracticeWarmupTicks - std::max(0, id - 1) * 80;
  return elapsed < 0 ? -1 : elapsed % 240;
}
Input BotInput(const World &w, int id, bool passive, bool practice) {
  Input in;
  const auto &p = w.archers[id];
  in.aim = p.facing > 0 ? 0 : 4;
  float nearest = 1e9f;
  int target = -1;
  for (int j = 0; j < w.count; ++j)
    if (j != id && w.archers[j].alive) {
      float d = std::hypot(w.archers[j].x - p.x, w.archers[j].y - p.y);
      if (d < nearest) {
        nearest = d;
        target = j;
      }
    }
  float tx = 960, ty = 480;
  if (target >= 0) {
    tx = w.archers[target].x;
    ty = w.archers[target].y;
  }
  if (p.ammo == 0)
    for (const auto &a : w.arrows)
      if (a.state == ArrowState::Stuck) {
        tx = a.x;
        ty = a.y;
        break;
      }
  // Last one standing: go and claim the diamond, lining up closely enough to
  // touch it rather than just getting near.
  float slack = 110;
  if (target < 0 && p.alive && w.result == NoResult) {
    Rect d = DiamondAt(w.tick);
    tx = d.x + (d.w - ArcherWidth) / 2;
    ty = d.y + d.h - ArcherHeight;
    slack = 10;
  }
  if (std::abs(tx - p.x) > slack)
    in.buttons |= tx > p.x ? Right : Left;
  if ((p.grounded && ((w.tick + id * 41) % 90 < 35 || ty < p.y - 80)) ||
      p.wallGrace > 0)
    in.buttons |= Jump;
  if (!p.grounded && ty > p.y + 180)
    in.buttons |= Drop;
  float angle = std::atan2(ty - p.y, tx - p.x);
  in.aim = (int(std::lround(angle / 0.785398163f)) + 8) % 8;
  bool clear = true;
  for (const auto &s : Arena())
    if (SegmentHit(p.x + 18, p.y + 26, tx + 18, ty + 26, SurfaceAt(s, w.tick)) <
        .98f)
      clear = false;
  if (!passive && target >= 0 && nearest < 1200 && clear)
    in.buttons |= Shoot;
  if (practice) {
    // A short firing window allows one arrow, followed by a long recovery.
    // Rivals stop to aim for 0.6 seconds; the same phase drives the visible
    // warning.
    int phase = PracticeShotPhase(w.tick, id);
    if (phase < 60 || phase >= 66 || nearest > 700)
      in.buttons &= ~Shoot;
    if (nearest < 280 || (phase >= 0 && phase < 66))
      in.buttons &= ~(Left | Right | Jump | Drop);
  }
  return in;
}
} // namespace towerfall
