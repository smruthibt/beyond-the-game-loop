#include "Presentation.hpp"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace towerfall {
namespace {
constexpr SDL_Color Night{12, 16, 29, 255}, Paper{240, 233, 214, 255},
    Gold{221, 182, 100, 255}, Quiet{151, 157, 169, 255};
constexpr SDL_Color Team[]{{75, 207, 181, 255},
                           {236, 102, 122, 255},
                           {110, 169, 242, 255},
                           {240, 188, 71, 255}};
constexpr float AimX[]{1, .7071f, 0, -.7071f, -1, -.7071f, 0, .7071f};
constexpr float AimY[]{0, .7071f, 1, .7071f, 0, -.7071f, -1, -.7071f};
std::string Fixed(double v, int digits = 1) {
  std::ostringstream out;
  out << std::fixed << std::setprecision(digits) << v;
  return out.str();
}
} // namespace
Painter::Painter(SDL_Renderer *r) : renderer(r) {
  SDL_SetRenderLogicalPresentation(r, Width, Height,
                                   SDL_LOGICAL_PRESENTATION_LETTERBOX);
  SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
}
void Painter::Fill(float x, float y, float w, float h, SDL_Color c) {
  SDL_SetRenderDrawColor(renderer, c.r, c.g, c.b, c.a);
  SDL_FRect r{x, y, w, h};
  SDL_RenderFillRect(renderer, &r);
}
void Painter::Stroke(float x, float y, float bx, float by, SDL_Color c) {
  SDL_SetRenderDrawColor(renderer, c.r, c.g, c.b, c.a);
  SDL_RenderLine(renderer, x, y, bx, by);
}
void Painter::Label(float x, float y, const std::string &s, float scale,
                    SDL_Color c) {
  SDL_SetRenderScale(renderer, scale, scale);
  SDL_SetRenderDrawColor(renderer, c.r, c.g, c.b, c.a);
  SDL_RenderDebugText(renderer, x / scale, y / scale, s.c_str());
  SDL_SetRenderScale(renderer, 1, 1);
}
void Painter::Circle(float x, float y, float radius, SDL_Color c) {
  for (int row = -int(radius); row <= int(radius); ++row) {
    float half = std::sqrt(std::max(0.f, radius * radius - row * row));
    Fill(x - half, y + row, half * 2 + 1, 1, c);
  }
}
void Painter::Triangle(float x, float y, float bx, float by, float cx, float cy,
                       SDL_Color color) {
  SDL_FColor c{color.r / 255.f, color.g / 255.f, color.b / 255.f,
               color.a / 255.f};
  SDL_Vertex v[]{
      {{x, y}, c, {0, 0}}, {{bx, by}, c, {0, 0}}, {{cx, cy}, c, {0, 0}}};
  SDL_RenderGeometry(renderer, nullptr, v, 3, nullptr, 0);
}
void Painter::Torch(float x, float y, double t) {
  Circle(x, y, 43, {247, 167, 58, 8});
  Circle(x, y, 25, {255, 167, 58, 14});
  Fill(x - 5, y + 8, 10, 32, {103, 70, 54, 255});
  float flicker = float(std::sin(t * 15 + x)) * 4;
  Triangle(x - 11, y + 10, x + 12, y + 10, x + flicker, y - 26,
           {229, 101, 45, 255});
  Triangle(x - 6, y + 10, x + 7, y + 10, x - flicker * .4f, y - 13,
           {255, 209, 112, 255});
  for (int n = 0; n < 3; ++n) {
    float k = float(std::fmod(t * 25 + n * 18, 65));
    Fill(x + std::sin(k * .14f + n) * 8, y - k, 3, 3,
         {255, 193, 102, Uint8(180 - k * 2)});
  }
}
void Painter::Backdrop(double time, float moonX, float moonY) {
  // A window that isn't 16:9 (fullscreen on a 16:10 Mac) is letterboxed, and
  // the bars show the clear colour. The engine's default is blue, so clear
  // the whole window to black before painting the 16:9 scene.
  SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
  SDL_RenderClear(renderer);
  Fill(0, 0, Width, Height, Night);
  for (int y = 0; y < Height; y += 4) {
    float k = float(y) / Height;
    Fill(0, float(y), Width, 5,
         {Uint8(12 + k * 12), Uint8(16 + k * 16), Uint8(29 + k * 20), 255});
  }
  Circle(moonX, moonY, 156, {97, 113, 137, 8});
  Circle(moonX, moonY, 116, {180, 192, 193, 15});
  Circle(moonX, moonY, 84, {206, 214, 204, 255});
  Circle(moonX - 31, moonY - 26, 79, {31, 41, 59, 255});
  for (int n = 0; n < 80; ++n) {
    int x = (n * 293 + 43) % Width, y = 145 + (n * 73) % 420;
    Fill(float(x), float(y), n % 5 == 0 ? 3 : 2, 2, {163, 186, 205, 110});
  }
  // Original stone arches and suspended banners, built from vector primitives.
  for (int n = 0; n < 6; ++n) {
    float x = 126 + n * 327.f;
    Fill(x, 280, 75, 725, {30, 36, 49, 255});
    Fill(x - 8, 280, 91, 22, {43, 48, 59, 255});
    Fill(x - 11, 981, 97, 40, {45, 49, 58, 255});
    Triangle(x + 30, 281, x + 147, 175, x + 260, 281, {32, 39, 53, 255});
    Triangle(x + 67, 281, x + 147, 215, x + 224, 281, {15, 23, 39, 255});
    for (int j = 0; j < 17; ++j) {
      float y = 316 + j * 39.f;
      Stroke(x, y, x + 75, y, {49, 53, 66, 255});
      Stroke(x + (j % 2 ? 25 : 51), y, x + (j % 2 ? 25 : 51), y + 39,
             {49, 53, 66, 255});
    }
    SDL_Color cloth{36, 67, 71, 255};
    Fill(x + 110, 270, 67, 190, cloth);
    Triangle(x + 110, 460, x + 177, 460, x + 144, 493, cloth);
    Fill(x + 135, 293, 17, 124, {154, 127, 82, 150});
    Triangle(x + 122, 327, x + 165, 327, x + 144, 349, Gold);
  }
  for (int n = 0; n < 3; ++n) {
    float x = 340 + n * 625.f;
    Torch(x, 570, time);
  }
  for (int n = 0; n < 18; ++n) {
    float x = float(std::fmod(n * 113 + time * (3 + n % 3), Width));
    float y = 650 + std::sin(float(time * .22 + n)) * 160;
    Fill(x, y, 3, 3, {186, 174, 126, 65});
  }
}
void Painter::Stone(Rect r, int id, bool moving) {
  Fill(r.x - 4, r.y - 3, r.w + 8, r.h + 7, {7, 11, 21, 170});
  Fill(r.x, r.y, r.w, r.h, {67, 69, 78, 255});
  Fill(r.x, r.y, r.w, 5, moving ? Gold : SDL_Color{143, 139, 131, 255});
  Fill(r.x, r.y + 5, r.w, 4, {92, 92, 98, 255});
  for (int row = 0; row < int(r.h) / 23 + 1; ++row) {
    float y = r.y + 11 + row * 23;
    Stroke(r.x, y, r.x + r.w, y, {38, 43, 55, 255});
    for (int col = 0; col < int(r.w) / 48; ++col) {
      float x = r.x + col * 48 + (row % 2) * 24;
      Stroke(x, y, x, std::min(y + 23, r.y + r.h), {38, 43, 55, 255});
    }
  }
  if (moving) {
    Fill(r.x + 8, r.y + 10, 7, 7, Gold);
    Fill(r.x + r.w - 15, r.y + 10, 7, 7, Gold);
  } else if (r.w > 100) {
    for (int n = 0; n < int(r.w) / 75; ++n)
      Fill(r.x + 20 + n * 75, r.y - 3, 18, 3, {69, 109, 92, 255});
  }
  (void)id;
}
void Painter::ArcherSprite(const Archer &p, int id, bool local) {
  auto color = Team[id];
  float x = p.x, y = p.y;
  float phase = float(p.personalUs / 1000000.0);
  if (!p.alive) {
    Fill(x + 5, y + 42, 27, 7, {67, 59, 68, 255});
    Stroke(x + 8, y + 37, x + 27, y + 53, Quiet);
    Stroke(x + 27, y + 37, x + 8, y + 53, Quiet);
    return;
  }
  float stride =
      p.grounded ? std::sin(phase * 22) * std::min(6.f, std::abs(p.vx) * .02f)
                 : 4;
  float sway = std::sin(phase * 9) * 4;
  Triangle(
      x + 4, y + 18, x + 32, y + 18, x + 18 - p.facing * 18 + sway, y + 51,
      {Uint8(color.r * .5), Uint8(color.g * .5), Uint8(color.b * .5), 255});
  Fill(x + 8, y + 44, 8, 12 + stride, {49, 35, 36, 255});
  Fill(x + 23, y + 44, 8, 12 - stride, {49, 35, 36, 255});
  Fill(x + 6, y + 53 + stride, 12, 5, {111, 78, 54, 255});
  Fill(x + 21, y + 53 - stride, 12, 5, {111, 78, 54, 255});
  Fill(x + 6, y + 22, 26, 25, color);
  Fill(x + 5, y + 40, 29, 5, {93, 63, 43, 255});
  Fill(x + 17, y + 40, 6, 6, Gold);
  Fill(x + 6, y + 5, 26, 21, color);
  Triangle(x + 2, y + 8, x + 36, y + 8, x + 15 - p.facing * 6, y - 7, color);
  Fill(x + (p.facing > 0 ? 15 : 5), y + 11, 17, 12, {207, 164, 124, 255});
  Fill(x + (p.facing > 0 ? 25 : 8), y + 14, 4, 3, Night);
  Fill(x + (p.facing > 0 ? 0 : 31), y + 19, 6, 22, {117, 77, 51, 255});
  for (int a = 0; a < p.ammo; ++a)
    Stroke(x + (p.facing > 0 ? 0 : 35) + a * 2, y + 22,
           x + (p.facing > 0 ? -4 : 39) + a * 2, y + 8, Gold);
  int aim = p.input.aim;
  float ux = AimX[aim], uy = AimY[aim], cx = x + 18 + ux * 27,
        cy = y + 25 + uy * 27;
  Stroke(x + 18, y + 26, cx, cy, {207, 164, 124, 255});
  Stroke(cx - uy * 17, cy + ux * 17, cx + ux * 10, cy + uy * 10, Gold);
  Stroke(cx + ux * 10, cy + uy * 10, cx + uy * 17, cy - ux * 17, Gold);
  Stroke(cx - uy * 17, cy + ux * 17, cx + uy * 17, cy - ux * 17, Paper);
  if (p.input.paused) {
    Circle(x + 18, y + 26, 40, {color.r, color.g, color.b, 25});
    Label(x + 3, y - 34, "II", 2, color);
  } else if (local) {
    Triangle(x + 10, y - 26, x + 26, y - 26, x + 18, y - 16, Paper);
    // Reload meter, shown only while the bow recovers. It fills as the
    // archer's own time runs down.
    auto meter = [&](float top, float wait, float full, SDL_Color c) {
      if (wait <= 0)
        return;
      Fill(x - 4, top, 44, 4, {8, 11, 20, 210});
      Fill(x - 4, top, 44 * (1 - wait / full), 4, c);
    };
    if (p.ammo > 0)
      meter(y - 34, p.shootWait, ShootDelay, Gold);
  }
  if (local && p.ammo > 0) {
    for (int k = 1; k <= 3; ++k)
      Fill(cx + ux * (30 + k * 15), cy + uy * (30 + k * 15), 3, 3,
           {color.r, color.g, color.b, Uint8(160 - k * 35)});
  }
}
void Painter::ArrowSprite(const Arrow &a) {
  float length = std::hypot(a.vx, a.vy);
  float ux = length ? a.vx / length : 1, uy = length ? a.vy / length : 0;
  if (a.state == ArrowState::Flying) {
    for (int i = 1; i < 4; ++i)
      Stroke(a.x - ux * i * 18, a.y - uy * i * 18, a.x - ux * (i + 1) * 18,
             a.y - uy * (i + 1) * 18,
             {Team[a.owner].r, Team[a.owner].g, Team[a.owner].b,
              Uint8(100 - i * 23)});
  }
  Stroke(a.x - ux * 24, a.y - uy * 24, a.x, a.y, Paper);
  Triangle(a.x + ux * 5, a.y + uy * 5, a.x - ux * 5 - uy * 4,
           a.y - uy * 5 + ux * 4, a.x - ux * 5 + uy * 4, a.y - uy * 5 - ux * 4,
           Gold);
  Stroke(a.x - ux * 19, a.y - uy * 19, a.x - ux * 27 - uy * 5,
         a.y - uy * 27 + ux * 5, Team[a.owner]);
}
void Painter::Game(const NetworkFrame &f, const ScreenState &screen,
                   Mode mode) {
  const auto &m = f.match;
  const auto &w = m.world;
  // Solo runs the whole arena on the engine's game time, so the scenery does
  // too: a pause leaves nothing on screen moving. Networked, the arena is
  // shared and keeps real time.
  const double sceneTime =
      mode == Mode::Solo ? screen.personalSeconds : screen.realSeconds;
  Backdrop(sceneTime);
  for (const auto &s : Arena()) {
    Rect r = SurfaceAt(s, w.tick);
    if (s.axis == 2) {
      Stroke(r.x + 18, 190, r.x + 18, r.y, {95, 86, 75, 140});
      Stroke(r.x + r.w - 18, 190, r.x + r.w - 18, r.y, {95, 86, 75, 140});
    }
    Stone(r, s.id, s.axis != 0);
  }
  // The diamond rides its platform. It glows while it can be claimed - by
  // the last archer alive - and is gone once claimed.
  if (w.result < 0) {
    Rect d = DiamondAt(w.tick);
    float cx = d.x + d.w / 2, cy = d.y + d.h / 2;
    bool open = w.result == NoResult && LastStanding(w) >= 0;
    SDL_Color gem = open ? SDL_Color{126, 232, 255, 255}
                         : SDL_Color{76, 126, 150, 255};
    if (open)
      Circle(cx, cy, 34 + std::sin(float(sceneTime * 6)) * 4,
             {126, 232, 255, 45});
    Triangle(d.x, cy, cx, d.y, d.x + d.w, cy, gem);
    Triangle(d.x, cy, cx, d.y + d.h, d.x + d.w, cy,
             {Uint8(gem.r * .7), Uint8(gem.g * .7), Uint8(gem.b * .8), 255});
    Triangle(cx - 5, cy - 3, cx, d.y + 5, cx + 1, cy - 3, Paper);
  }
  for (const auto &a : w.arrows)
    ArrowSprite(a);
  for (int i = 0; i < w.count; ++i)
    if (w.archers[i].present)
      ArcherSprite(w.archers[i], i, i == f.id);
  if (mode == Mode::Solo && m.phase == Phase::Playing) {
    for (int i = 1; i < w.count; ++i) {
      const auto &p = w.archers[i];
      int phase = PracticeShotPhase(w.tick, i);
      if (p.alive && p.ammo > 0 && phase >= 0 && phase < 60)
        Label(p.x + 9, p.y - 39, "!", 3, Gold);
    }
    if (f.id == 0 && !w.archers[0].alive) {
      Fill(600, 460, 720, 128, {10, 15, 27, 238});
      Label(646, 486, "TRY AGAIN / UP TO RESTART", 2.7f, Paper);
      Label(646, 539, "Watch for !, then jump away from the shot.", 1.7f, Gold);
    }
  }
  // SDL's debug font is 8 px square at scale 1.
  auto labelWidth = [](const std::string &s, float scale) {
    return float(s.size()) * 8 * scale;
  };
  auto centered = [&](float y, const std::string &s, float scale,
                      SDL_Color c) {
    Label((Width - labelWidth(s, scale)) / 2, y, s, scale, c);
  };
  // Header: title and phase on the left, scoreboard in the middle, mode on the
  // right. It all ends above Ceiling, so none of it hides the arena.
  Fill(55, 18, 1810, 106, {10, 14, 24, 240});
  Fill(55, 18, 1810, 3, Gold);
  Label(80, 36, "TOWERFALL", 3, Paper);
  Label(80, 72, PhaseLabel(m.phase), 1.8f, Gold);
  Label(80, 98, "MOONSTONE COURT",
        1.4f, Quiet);
  const bool spectating = f.id < 0 || f.id >= w.count;
  const std::string modeText = ModeLabel(mode);
  const std::string subText = spectating    ? "SPECTATING"
                              : screen.demo ? "TIME DEMONSTRATION"
                                            : "ONE ROUND DECIDES";
  Label(1840 - labelWidth(modeText, 1.8f), 40, modeText, 1.8f, Paper);
  Label(1840 - labelWidth(subText, 1.5f), 74, subText, 1.5f,
        screen.demo && !spectating ? Team[0] : Gold);
  const float left = 420, right = 1580;
  float span = std::min(340.f, (right - left) / std::max(1, w.count));
  float start = left + (right - left - span * w.count) / 2;
  for (int i = 0; i < w.count; ++i) {
    float x = start + i * span, cw = span - 12;
    auto &p = w.archers[i];
    if (!p.present) {
      Fill(x, 28, cw, 86, {12, 16, 26, 200});
      Fill(x, 28, 5, 86, {Team[i].r, Team[i].g, Team[i].b, 90});
      Label(x + 17, 38, "P" + std::to_string(i + 1), 2, Quiet);
      Label(x + 17, 94, "OPEN SLOT", 1.6f, Quiet);
      continue;
    }
    Fill(x, 28, cw, 86,
         i == f.id ? SDL_Color{27, 36, 54, 245} : SDL_Color{16, 22, 34, 235});
    Fill(x, 28, 5, 86, Team[i]);
    Label(x + 17, 38,
          "P" + std::to_string(i + 1) + (i == f.id ? " / YOU" : ""), 2,
          Team[i]);
    // A full slot is a gold arrow on a lit ground; an empty one is a hollow
    // outline, visible on both the plain and the highlighted card.
    for (int a = 0; a < 5; ++a) {
      float ax = x + 17 + a * 20;
      if (a < p.ammo) {
        Fill(ax, 62, 15, 20, {74, 60, 34, 255});
        Fill(ax + 7, 67, 2, 12, Gold);
        Triangle(ax + 3, 69, ax + 13, 69, ax + 8, 64, Gold);
      } else {
        Fill(ax, 62, 15, 20, {78, 84, 100, 255});
        Fill(ax + 2, 64, 11, 16, {12, 16, 26, 255});
      }
    }
    if (!p.alive)
      Fill(x, 28, cw, 86, {8, 11, 20, 130});
    // Solo pauses and scales the whole game; networked, each archer's own.
    const bool paused = mode == Mode::Solo ? screen.paused : p.input.paused;
    const double speed =
        mode == Mode::Solo ? screen.speed : p.input.speed * .5;
    const bool won = m.phase == Phase::Complete && w.result == i;
    Label(x + 17, 94,
          won        ? "WINNER"
          : !p.alive ? "ELIMINATED"
          : paused   ? "PAUSED"
                     : Fixed(speed) + "X SPEED",
          1.6f,
          won            ? Gold
          : !p.alive     ? Quiet
          : paused       ? Gold
          : speed == 1.0 ? Quiet
                         : Paper);
  }
  if (m.phase == Phase::Lobby) {
    Fill(475, 420, 970, 235, {10, 15, 27, 245});
    Label(535, 460, "ARCHERS, TAKE YOUR PLACES", 3, Paper);
    if (Served(mode)) {
      Label(535, 523, "WAITING FOR AN ARCHER", 2.8f, Gold);
      Label(535, 581, "PLAY STARTS AS SOON AS ONE JOINS", 1.8f, Quiet);
    } else {
      Label(535, 523,
            std::to_string(m.ready) + " / " + std::to_string(w.count) +
                " READY",
            2.8f, Gold);
      Label(535, 581, "UP TO READY   /   UP + DOWN FOR OPTIONS", 1.8f, Quiet);
    }
  }
  // Messages during a round sit in a slim banner at the top of the arena,
  // clear of the platforms where arrows may still be flying.
  auto banner = [&](float height, SDL_Color accent) {
    Fill(460, 140, 1000, height, {10, 15, 27, 230});
    Fill(460, 140, 1000, 3, accent);
  };
  if (mode == Mode::Solo && screen.paused && m.phase != Phase::Lobby &&
      m.phase != Phase::Complete) {
    banner(76, Gold);
    centered(154, "GAME PAUSED", 2.5f, Gold);
    centered(190, "PLATFORMS, ARROWS AND RIVALS WAIT   /   P TO RESUME", 1.5f,
             Paper);
  } else if (m.phase == Phase::Countdown) {
    int n = int(std::ceil((m.start - f.now) / 1000000.0));
    banner(56, Gold);
    centered(158,
             "THE TRIAL BEGINS IN " + std::to_string(std::max(1, n)),
             3, Gold);
  } else if (m.phase == Phase::Playing && LastStanding(w) >= 0) {
    banner(56, {126, 232, 255, 255});
    centered(158,
             Slots(w.entered) >= 2 ? "LAST ARCHER STANDING: CLAIM THE DIAMOND"
                                   : "CLAIM THE DIAMOND TO WIN",
             2.2f, {126, 232, 255, 255});
  } else if (m.phase == Phase::Confirming) {
    banner(76, Gold);
    centered(154, "DECIDING THE WINNER", 2.5f, Gold);
    centered(190,
             screen.telemetry ? std::to_string(m.seals) + " INPUT SEALS / " +
                                    std::to_string(m.votes) + " MATCHING VOTES"
                              : "SETTLING THE FINAL SHOT...",
             1.5f, Quiet);
  } else if (m.phase == Phase::Complete) {
    // One round decides the game, so the finale may take the middle of the
    // screen. A draw with someone still standing means the last archer was
    // paused.
    const int winner = w.result;
    bool pausedSurvivor = false;
    for (int i = 0; i < w.count; ++i)
      pausedSurvivor = pausedSurvivor || w.archers[i].alive;
    Fill(495, 357, 930, 251, {10, 15, 27, 244});
    Label(555, 395,
          winner >= 0 ? "PLAYER " + std::to_string(winner + 1) +
                            " TAKES THE CROWN"
                      : std::string("NO ONE TAKES THE CROWN"),
          2.9f, winner >= 0 ? Team[winner] : Gold);
    Label(555, 467,
          winner >= 0      ? "THE DIAMOND IS CLAIMED"
          : pausedSurvivor ? "A PAUSED ARCHER CANNOT WIN / DRAW"
                           : "NO ARCHER LEFT STANDING / DRAW",
          1.8f, Paper);
    Label(555, 532,
          mode == Mode::Solo ? "UP / NEW PRACTICE MATCH   UP + DOWN / OPTIONS"
          : Served(mode)     ? "A NEW GAME BEGINS SHORTLY"
                             : "UP + DOWN / OPTIONS   EXIT TO START A NEW SESSION",
          1.6f, Quiet);
  }
  Fill(55, 1010, 1810, 51, {10, 14, 24, 240});
  Label(78, 1028,
        "LEFT / RIGHT MOVE   SPACE JUMP   UP / DOWN AIM   ENTER SHOOT",
        1.6f,
        Paper);
  const std::string hint = "P PAUSE   SHIFT + 1 / 2 / 3 SPEED   UP + DOWN OPTIONS";
  Label(1840 - labelWidth(hint, 1.35f), 1028, hint, 1.35f,
        screen.demo ? Team[0] : Gold);
  if (screen.telemetry) {
    Fill(119, 245, 400, 190, {9, 14, 25, 240});
    Label(140, 265, "ENGINE / NETWORK", 1.9f, Gold);
    Label(140, 302,
          "STATE TX " + std::to_string(f.tx) + "/S  RX " + std::to_string(f.rx),
          1.5f, Paper);
    Label(140, 330,
          "GAME " + Fixed(screen.personalSeconds, 2) + "  WORLD " +
              Fixed(w.tick * .01, 2),
          1.3f, Quiet);
    Label(140, 358,
          "LOOP " + std::to_string(screen.loops) + "  WORKER " +
              std::to_string(f.workerPasses),
          1.25f, Quiet);
    Label(140, 385,
          "REPLAYS " + std::to_string(m.rollbacks) + "  LOG " +
              std::to_string(f.records),
          1.25f, Quiet);
    Label(140, 411, "COMMITTED ROUNDS " + std::to_string(m.commits), 1.1f,
          Team[0]);
  }
  if (!f.message.empty()) {
    Fill(210, 908, 1500, 56, {91, 31, 42, 245});
    Label(233, 929, f.message.substr(0, 110), 1.6f, Paper);
  }
  if (screen.help) {
    Fill(375, 280, 1170, 620, {8, 13, 24, 248});
    Label(425, 323, "THE ARCHER'S FIELD GUIDE", 3.8f, Gold);
    const char *lines[]{
        "THREE ARROWS. A SINGLE HIT ELIMINATES YOU.",
        "Missed arrows stick in stone. Walk over them to recover ammo.",
        "The gold bar above your archer shows the bow reloading.",
        "LEFT/RIGHT move and turn. SPACE jumps, including wall jumps.",
        "UP/DOWN tilt your bow. ENTER shoots wherever it points.",
        "Walk off ledges to descend. UP + DOWN together opens options.",
        "One round: outlast your rivals, then claim the diamond to win.",
        "There is no time limit. Keep fighting until one archer remains.",
        "P pauses. SHIFT + 1 / 2 / 3 set 0.5x / 1x / 2x speed.",
        mode == Mode::Solo
            ? "Pause and speed apply to the whole arena in practice."
            : "Pause and speed affect only your archer; the arena continues.",
        "UP retries after a practice defeat. ESC returns to options."};
    for (int n = 0; n < 11; ++n)
      Label(430, 405 + n * 39.f, lines[n], n == 0 ? 1.9f : 1.65f,
            n == 0 ? Paper : Quiet);
  }
  if (mode == Mode::Solo && !screen.help && m.phase == Phase::Playing) {
    Fill(575, 963, 770, 40, {10, 15, 27, 220});
    std::string tip =
        w.tick < PracticeWarmupTicks
            ? "WARM-UP " +
                  std::to_string((PracticeWarmupTicks - w.tick + Hz - 1) / Hz) +
                  "s / MOVE, JUMP AND TRY YOUR BOW"
            : "! = RIVAL AIMING / JUMP OR STEP OUT OF ITS LINE";
    Label(600, 976, tip, 1.65f, Gold);
  }
  if (screen.options) {
    Fill(430, 245, 1060, 700, {8, 13, 24, 248});
    Label(490, 279, "OPTIONS", 4, Gold);
    Label(490, 335, "UP / DOWN CHOOSE   ENTER SELECT   ESC BACK", 1.8f, Quiet);
    const std::string entries[]{
        "BACK TO GAME",
        mode == Mode::Solo
            ? (screen.paused ? "RESUME GAME" : "PAUSE GAME")
            : (screen.paused ? "RESUME MY ARCHER" : "PAUSE MY ARCHER"),
        (mode == Mode::Solo ? "GAME SPEED / " : "PERSONAL SPEED / ") +
            Fixed(screen.speed) + "X",
        mode == Mode::Solo ? "RESTART PRACTICE"
                           : "RESTART PRACTICE / SOLO ONLY",
        screen.telemetry ? "HIDE STATS" : "SHOW STATS",
        "HOW TO PLAY",
        "TOGGLE FULLSCREEN",
        "SAVE SCREENSHOT",
        "EXIT GAME"};
    for (int i = 0; i < 9; ++i) {
      float y = 382 + i * 49.f;
      if (i == screen.option)
        Fill(477, y - 8, 966, 43, {54, 49, 46, 255});
      Label(498, y, entries[i], 2, i == screen.option ? Paper : Quiet);
    }
    Label(490, 884, "The shared arena keeps moving while options are open.",
          1.65f, Gold);
  }
  if (screen.quitPrompt) {
    // Drops in from above the screen, easing out over a fifth of a second,
    // while everything behind it dims.
    float t = float(
        std::clamp((screen.realSeconds - screen.quitOpened) / .2, 0., 1.));
    t = 1 - (1 - t) * (1 - t) * (1 - t);
    Fill(0, 0, Width, Height, {4, 6, 12, Uint8(150 * t)});
    const float h = 260, top = -h + (400 + h) * t;
    Fill(510, top, 900, h, {8, 13, 24, 250});
    Fill(510, top + h - 3, 900, 3, Gold);
    centered(top + 40, "DO YOU WANT TO QUIT?", 3, Gold);
    const std::string choices[]{"NO, KEEP PLAYING", "YES, QUIT"};
    for (int i = 0; i < 2; ++i) {
      const bool on = i == screen.quitChoice;
      float x = 570 + i * 400.f, y = top + 115;
      Fill(x, y, 380, 60,
           on ? SDL_Color{54, 49, 46, 255} : SDL_Color{14, 19, 31, 235});
      if (on)
        Fill(x, y, 380, 3, Gold);
      Label(x + (380 - labelWidth(choices[i], 2)) / 2, y + 22, choices[i], 2,
            on ? Paper : Quiet);
    }
    centered(top + 210, "LEFT / RIGHT CHOOSE   ENTER CONFIRM   ESC CANCEL",
             1.5f, Quiet);
  }
}
bool Painter::Screenshot(const std::string &path) {
  auto *s = SDL_RenderReadPixels(renderer, nullptr);
  if (!s)
    return false;
  bool ok = SDL_SaveBMP(s, path.c_str());
  SDL_DestroySurface(s);
  return ok;
}
void Audio::Open() {
  if (!SDL_InitSubSystem(SDL_INIT_AUDIO))
    return;
  SDL_AudioSpec spec{SDL_AUDIO_F32, 1, 24000};
  stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec,
                                     nullptr, nullptr);
  if (stream)
    SDL_ResumeAudioStreamDevice(stream);
}
void Audio::Note(float frequency, float length, float volume) {
  if (!stream)
    return;
  std::vector<float> data(size_t(24000 * length));
  for (size_t i = 0; i < data.size(); ++i) {
    float t = float(i) / 24000, fade = 1 - float(i) / data.size();
    data[i] = volume * fade * fade *
              (std::sin(t * frequency * 6.2831853f) +
               .25f * std::sin(t * frequency * 12.56637f));
  }
  SDL_PutAudioStreamData(stream, data.data(), int(data.size() * sizeof(float)));
}
void Audio::Close() {
  if (stream) {
    SDL_DestroyAudioStream(stream);
    stream = nullptr;
  }
}
} // namespace towerfall
