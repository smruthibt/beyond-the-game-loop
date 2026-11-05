#include "Presentation.hpp"
#include "engine/Input.hpp"
#include "engine/Physics.hpp"
#include <algorithm>
#include <cmath>
#include <csignal>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>

namespace towerfall {
namespace {
std::atomic<bool> interrupted{false};
void Signal(int) { interrupted = true; }
bool Press(SDL_Scancode k) { return engine::Input::IsKeyJustPressed(k); }
bool Down(SDL_Scancode k) { return engine::Input::IsKeyPressed(k); }
void Usage() {
  std::cout
      << "Towerfall - original arena archery game for the Team 16 engine\n"
         "  --mode solo|client|hybrid|peer|server\n"
         "  --players 3            Solo/serverless roster, 2-4 archers; solo "
         "fills with AI. With a server, archers come and go (up to 4)\n"
         "  --port 19400           Local TCP base (base,+1,+10..17,+30..37)\n"
         "  --id 0                Claim a particular peer identity\n"
         "  --hybrid              Dedicated server for hybrid clients\n"
         "  --spectator           Observe from an unused slot\n"
         "  --demo                Label the window as a time demonstration "
         "(F2 toggles)\n"
         "  --scale 0.5|1|2        Initial game timeline speed; enables "
         "demo\n"
         "  --bot                 AI controls this archer and readies "
         "automatically\n"
         "  --passive             Bots move without firing (timing test)\n"
         "  --headless            Test client without an SDL window\n"
         "  --seconds 15          Exit after wall time; default unlimited\n"
         "  --pause-at 8          Pause personal time at the given wall "
         "second\n"
         "  --report FILE         Write JSONL telemetry twice per second\n"
         "  --capture FILE.bmp    Capture a rendered frame\n"
         "  --capture-at 6        Wall second to capture\n"
         "  --telemetry           Show the engine/network overlay\n"
         "  --log                 Print network traffic and protocol records "
         "to the terminal\n"
         "Without --mode, solo practice starts directly. See game/README.md.\n";
}
Options Parse(int argc, char **argv) {
  Options o;
  for (int n = 1; n < argc; ++n) {
    std::string arg = argv[n];
    auto value = [&]() {
      if (n + 1 >= argc)
        throw std::runtime_error("Missing value for " + arg);
      return std::string(argv[++n]);
    };
    if (arg == "--help") {
      Usage();
      std::exit(0);
    } else if (arg == "--mode") {
      auto name = value();
      if (name == "solo")
        o.mode = Mode::Solo;
      else if (name == "client")
        o.mode = Mode::Client;
      else if (name == "hybrid")
        o.mode = Mode::Hybrid;
      else if (name == "peer")
        o.mode = Mode::Peer;
      else if (name == "server")
        o.mode = Mode::Server;
      else
        throw std::runtime_error("Unknown mode: " + name);
    } else if (arg == "--players")
      o.players = std::stoi(value());
    else if (arg == "--port")
      o.port = std::stoi(value());
    else if (arg == "--id")
      o.id = std::stoi(value());
    else if (arg == "--seconds")
      o.seconds = std::stod(value());
    else if (arg == "--scale") {
      o.scale = std::stod(value());
      o.demo = true;
    } else if (arg == "--pause-at") {
      o.pauseAt = std::stod(value());
      o.demo = true;
    } else if (arg == "--capture-at")
      o.captureAt = std::stod(value());
    else if (arg == "--report")
      o.report = value();
    else if (arg == "--capture")
      o.capture = value();
    else if (arg == "--demo")
      o.demo = true;
    else if (arg == "--bot")
      o.bot = true;
    else if (arg == "--passive")
      o.passive = true;
    else if (arg == "--headless")
      o.headless = true;
    else if (arg == "--hybrid")
      o.hybridServer = true;
    else if (arg == "--spectator")
      o.spectator = true;
    else if (arg == "--telemetry")
      o.telemetry = true;
    else if (arg == "--log")
      o.log = true;
    else
      throw std::runtime_error("Unknown argument: " + arg);
  }
  if (o.players < 2 || o.players > 4 || o.port < 1024 || o.port > 65000 ||
      o.id < -1 || o.id > 3 || !std::isfinite(o.seconds) || o.seconds < 0 ||
      (o.scale != .5 && o.scale != 1 && o.scale != 2) ||
      !std::isfinite(o.pauseAt) || !std::isfinite(o.captureAt))
    throw std::runtime_error("Invalid roster, port, identity, time, or speed.");
  if (o.spectator && o.mode == Mode::Peer &&
      (o.players == 4 || (o.id >= 0 && o.id < o.players)))
    throw std::runtime_error("Spectators need an unused identity after the "
                             "archer roster (e.g. --players 3 --id 3).");
  return o;
}
} // namespace
int Run(Options o) {
  engine::Engine engine;
  engine::Physics::SetGravity(2000);
  if (o.mode == Mode::Server)
    return Serve(engine, o);
  if (o.headless ? !engine.InitHeadless()
                 : !engine.Init("Towerfall | Moonlit Trials", 1440, 810))
    return 1;
  engine.SetTargetFrameRate(120);
  std::unique_ptr<Painter> painter;
  Audio audio;
  if (!o.headless) {
    painter = std::make_unique<Painter>(engine.GetRenderer());
    audio.Open();
  }
  // With a server the roster is not fixed: any of the four archer slots can
  // fill or empty during a match.
  if (Served(o.mode))
    o.players = MaxPlayers;
  engine.GetGameTime().SetScale(o.scale);
  engine::Timeline animationTime(&engine.GetGameTime(), 1000);
  Connection network(engine, o);
  NetworkFrame frame;
  ScreenState screen;
  screen.demo = o.demo;
  screen.telemetry = o.telemetry;
  std::ofstream report;
  if (!o.report.empty()) {
    report.open(o.report);
    if (!report)
      throw std::runtime_error("Cannot open report file.");
  }
  bool testPaused = false, captured = false, fullscreen = false;
  int previousShots = 0, previousHits = 0, previousRound = -1;
  int keyboardAim = 0, aimRound = -1, aimId = -1;
  // The bow faces the last movement direction and is tilted by UP/DOWN:
  // -1 diagonally up, 0 level, 1 diagonally down.
  int keyboardFacing = 1, aimTilt = 0, tiltBeforeChord = 0;
  auto bowAim = [&] {
    return keyboardFacing > 0 ? (8 + aimTilt) % 8 : 4 - aimTilt;
  };
  bool releaseKeys = false;
  // Default to "no" and wait for held keys, so a held Enter cannot confirm it.
  auto askQuit = [&] {
    screen.quitPrompt = true;
    screen.quitChoice = 0;
    screen.quitOpened = screen.realSeconds;
    releaseKeys = true;
  };
  int64_t began = WallMicros(), lastReport = 0;
  auto update = [&](float) {
    int64_t wall = WallMicros();
    double elapsed = (wall - began) / 1000000.0;
    if (interrupted || (o.seconds > 0 && elapsed >= o.seconds))
      engine.Stop();
    frame = network.Read();
    screen.realSeconds = elapsed;
    screen.personalSeconds = engine.GetGameTime().GetSeconds();
    screen.loops = engine.GetIterationCount();
    if (!o.headless) {
      // Release the chord/selection before handing keys back to gameplay, so
      // the Enter that picked an option does not also fire an arrow.
      bool anyHeld = Down(SDL_SCANCODE_LEFT) || Down(SDL_SCANCODE_RIGHT) ||
                     Down(SDL_SCANCODE_UP) || Down(SDL_SCANCODE_DOWN) ||
                     Down(SDL_SCANCODE_RETURN);
      if (!anyHeld)
        releaseKeys = false;
      if (!Down(SDL_SCANCODE_UP) && !Down(SDL_SCANCODE_DOWN))
        tiltBeforeChord = aimTilt;
      // Escape asks before quitting; inside a menu it closes that menu.
      if (!screen.MenuOpen() && Press(SDL_SCANCODE_ESCAPE))
        askQuit();
      if (!screen.MenuOpen() && !releaseKeys &&
          Down(SDL_SCANCODE_UP) && Down(SDL_SCANCODE_DOWN)) {
        screen.options = true;
        screen.option = 0;
        releaseKeys = true;
        // The first key of the chord already tilted the bow; undo that.
        aimTilt = tiltBeforeChord;
        keyboardAim = bowAim();
      }
      if (screen.help && !screen.quitPrompt && !releaseKeys &&
          Press(SDL_SCANCODE_ESCAPE)) {
        screen.help = false;
        screen.options = true;
        releaseKeys = true;
      }
      if (screen.options && !screen.quitPrompt && !releaseKeys) {
        if (Press(SDL_SCANCODE_UP))
          screen.option = (screen.option + 8) % 9;
        if (Press(SDL_SCANCODE_DOWN))
          screen.option = (screen.option + 1) % 9;
        if (Press(SDL_SCANCODE_ESCAPE)) {
          screen.options = false;
          releaseKeys = true;
        } else if (Press(SDL_SCANCODE_RETURN)) {
          switch (screen.option) {
          case 0:
            screen.options = false;
            break;
          case 1:
            screen.demo = true;
            if (engine.GetGameTime().IsPaused())
              engine.GetGameTime().Unpause();
            else
              engine.GetGameTime().Pause();
            break;
          case 2: {
            screen.demo = true;
            double speed = engine.GetGameTime().GetScale();
            engine.GetGameTime().SetScale(speed == .5  ? 1
                                          : speed == 1 ? 2
                                                       : .5);
            break;
          }
          case 3:
            if (o.mode == Mode::Solo) {
              network.RestartPractice();
              engine.GetGameTime().Unpause();
              previousRound = aimRound = -1;
              screen.options = false;
            }
            break;
          case 4:
            screen.telemetry = !screen.telemetry;
            break;
          case 5:
            screen.help = true;
            screen.options = false;
            break;
          case 6:
            fullscreen = !fullscreen;
            SDL_SetWindowFullscreen(engine.GetWindow(), fullscreen);
            break;
          case 7:
            // Captured after drawing, using the same path as the screenshot
            // shortcut.
            screen.captureRequested = true;
            screen.options = false;
            break;
          case 8:
            // Options stays open underneath, so "no" returns to it.
            askQuit();
            break;
          }
          releaseKeys = true;
        }
      }
      if (screen.quitPrompt && !releaseKeys) {
        if (Press(SDL_SCANCODE_LEFT))
          screen.quitChoice = 0;
        if (Press(SDL_SCANCODE_RIGHT))
          screen.quitChoice = 1;
        if (Press(SDL_SCANCODE_UP) || Press(SDL_SCANCODE_DOWN))
          screen.quitChoice = 1 - screen.quitChoice;
        if (Press(SDL_SCANCODE_ESCAPE)) {
          screen.quitPrompt = false;
          releaseKeys = true;
        } else if (Press(SDL_SCANCODE_RETURN)) {
          if (screen.quitChoice == 1)
            engine.Stop();
          screen.quitPrompt = false;
          releaseKeys = true;
        }
      }
      if (!screen.quitPrompt && Press(SDL_SCANCODE_H))
        screen.help = !screen.help;
      if (Press(SDL_SCANCODE_TAB))
        screen.telemetry = !screen.telemetry;
      if (Press(SDL_SCANCODE_F11)) {
        fullscreen = !fullscreen;
        SDL_SetWindowFullscreen(engine.GetWindow(), fullscreen);
      }
      if (Press(SDL_SCANCODE_F2)) {
        screen.demo = !screen.demo;
        if (!screen.demo) {
          engine.GetGameTime().Unpause();
          engine.GetGameTime().SetScale(1);
        }
      }
      // Time controls are always live: P pauses/resumes the engine's game
      // timeline and Shift + 1 / 2 / 3 set its scale, all through the engine's
      // Input. Speed needs Shift so a stray number key cannot change it.
      if (Press(SDL_SCANCODE_P)) {
        screen.demo = true;
        if (engine.GetGameTime().IsPaused())
          engine.GetGameTime().Unpause();
        else
          engine.GetGameTime().Pause();
      }
      const SDL_Scancode speedKeys[] = {SDL_SCANCODE_1, SDL_SCANCODE_2,
                                        SDL_SCANCODE_3};
      const double speeds[] = {.5, 1, 2};
      const bool shift = Down(SDL_SCANCODE_LSHIFT) || Down(SDL_SCANCODE_RSHIFT);
      for (int k = 0; k < 3; ++k)
        if (shift && Press(speedKeys[k])) {
          screen.demo = true;
          engine.GetGameTime().SetScale(speeds[k]);
        }
      if (!screen.MenuOpen() && !releaseKeys &&
          frame.match.phase == Phase::Lobby && Press(SDL_SCANCODE_UP))
        network.Ready();
      bool retry = frame.id == 0 && (!frame.match.world.archers[0].alive ||
                                     frame.match.phase == Phase::Complete);
      if (o.mode == Mode::Solo &&
          (Press(SDL_SCANCODE_R) ||
           (!screen.MenuOpen() && !releaseKeys && retry &&
            Press(SDL_SCANCODE_UP)))) {
        network.RestartPractice();
        engine.GetGameTime().Unpause();
        previousRound = -1;
        aimRound = -1;
        releaseKeys = true;
      }
    }
    if (o.pauseAt >= 0 && !testPaused && elapsed >= o.pauseAt) {
      engine.GetGameTime().Pause();
      testPaused = true;
    }
    // No archer yet - still connecting - unless this process only watches.
    if (frame.id < 0 && !frame.spectating)
      return;
    auto &w = frame.match.world;
    int id = frame.id;
    Input input;
    input.aim = keyboardAim;
    if (id >= 0 && id < o.players) {
      if (o.bot)
        input = BotInput(w, id, o.passive);
      else if (!o.headless && !screen.MenuOpen() && !releaseKeys) {
        input.buttons = (Down(SDL_SCANCODE_LEFT) ? Left : 0) |
                        (Down(SDL_SCANCODE_RIGHT) ? Right : 0) |
                        (Down(SDL_SCANCODE_SPACE) ? Jump : 0) |
                        (Down(SDL_SCANCODE_RETURN) ? Shoot : 0);
        if (aimRound != w.round || aimId != id) {
          keyboardFacing = w.archers[id].facing;
          aimTilt = 0;
          aimRound = w.round;
          aimId = id;
        }
        // Moving turns the bow; keep that direction after releasing movement.
        // Update locally so turning and firing together uses the new direction.
        bool left = (input.buttons & Left) != 0;
        bool right = (input.buttons & Right) != 0;
        if (left != right)
          keyboardFacing = right ? 1 : -1;
        // UP readies in the lobby, so it only tilts the bow once play begins.
        if (frame.match.phase != Phase::Lobby) {
          if (Press(SDL_SCANCODE_UP))
            aimTilt = std::max(-1, aimTilt - 1);
          if (Press(SDL_SCANCODE_DOWN))
            aimTilt = std::min(1, aimTilt + 1);
        }
        keyboardAim = bowAim();
        input.aim = keyboardAim;
      }
    }
    // Networked, the engine's game time is this player's own clock, carried
    // to every process inside the archer's commands. Solo, it already drives
    // the whole arena (Connection::Update), so the archer's clock stays at 1x
    // rather than being scaled twice.
    if (o.mode != Mode::Solo) {
      input.speed = int(engine.GetGameTime().GetScale() * 2);
      input.paused = engine.GetGameTime().IsPaused();
    }
    screen.paused = engine.GetGameTime().IsPaused();
    screen.speed = engine.GetGameTime().GetScale();
    network.Controls(input);
    screen.aim = input.aim;
    if (w.round != previousRound || w.shots < previousShots ||
        w.hits < previousHits) {
      previousRound = w.round;
      previousShots = previousHits = 0;
    }
    if (w.shots > previousShots) {
      audio.Note(740, .075f);
      previousShots = w.shots;
    }
    if (w.hits > previousHits) {
      audio.Note(135, .18f, .09f);
      previousHits = w.hits;
    }
    if (report && wall - lastReport >= 500000) {
      lastReport = wall;
      const auto &p = w.archers[std::clamp(id, 0, 3)];
      report << std::setprecision(17) << "{\"elapsed\":" << elapsed
             << ",\"id\":" << id << ",\"phase\":" << int(frame.match.phase)
             << ",\"round\":" << w.round << ",\"tick\":" << w.tick
             << ",\"tx\":" << frame.tx << ",\"rx\":" << frame.rx
             << ",\"records\":" << frame.records
             << ",\"personal\":" << screen.personalSeconds
             << ",\"animation\":" << animationTime.GetSeconds()
             << ",\"playerTime\":" << p.personalUs << ",\"x\":" << p.x
             << ",\"y\":" << p.y
             << ",\"alive\":" << (p.alive ? "true" : "false")
             << ",\"workers\":" << frame.workerPasses
             << ",\"loops\":" << screen.loops << ",\"probe\":\""
             << frame.probeHash << "\",\"stateHash\":\"" << WorldHash(w)
             << "\",\"resultHash\":\"" << frame.match.resultHash
             << "\",\"commits\":" << frame.match.commits
             << ",\"replays\":" << frame.match.rollbacks
             << ",\"shots\":" << w.shots << ",\"hits\":" << w.hits
             << ",\"seals\":" << frame.match.seals
             << ",\"votes\":" << frame.match.votes
             << ",\"peers\":" << frame.peers << ",\"epoch\":" << frame.epoch
             << ",\"scores\":[" << frame.match.scores[0] << ','
             << frame.match.scores[1] << ',' << frame.match.scores[2] << ','
             << frame.match.scores[3]
             << "],\"archers\":[" << w.archers[0].present << ','
             << w.archers[1].present << ',' << w.archers[2].present << ','
             << w.archers[3].present << "],\"batched\":" << frame.batched
             << ",\"flushes\":" << frame.flushes
             << ",\"connected\":" << (frame.connected ? "true" : "false")
             << "}\n";
      report.flush();
    }
  };
  auto draw = [&] {
    painter->Game(frame, screen, o.mode);
    if ((!o.capture.empty() && !captured &&
         screen.realSeconds >= o.captureAt) ||
        Press(SDL_SCANCODE_F12) || screen.captureRequested) {
      std::string path =
          o.capture.empty() ? "towerfall-capture.bmp" : o.capture;
      if (!painter->Screenshot(path))
        SDL_Log("Capture failed: %s", SDL_GetError());
      captured = true;
      screen.captureRequested = false;
    }
  };
  // Section 3, on the pattern of the Module 6 ThreadExample's networking
  // thread. This main thread handles input, the time controls, sound and
  // drawing. The engine's background thread runs network.Update: networking,
  // the command journal and the simulation. They share data only through
  // Connection's mutex-guarded getters and setters.
  auto background = [&](float dt) { network.Update(dt); };
  if (o.headless)
    engine.RunHeadless(update, 120, background);
  else
    engine.Run(update, draw, background);
  network.Shutdown();
  audio.Close();
  painter.reset();
  engine.Shutdown();
  return 0;
}
} // namespace towerfall
int main(int argc, char **argv) {
  try {
    std::signal(SIGINT, towerfall::Signal);
    std::signal(SIGTERM, towerfall::Signal);
    auto options = towerfall::Parse(argc, argv);
    return towerfall::Run(options);
  } catch (const std::exception &e) {
    std::cerr << "Towerfall: " << e.what() << '\n';
    return 1;
  }
}
