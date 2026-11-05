#include "Match.hpp"
#include "engine/Physics.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <tuple>

using namespace towerfall;
void Check(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
// Walks `owner` from its spawn on the centre platform to that platform's
// right end, beside the diamond's platform, which then carries it. Returns
// the commands, so the same walk can be replayed through a Match.
std::vector<Command> DiamondRoute(int owner, int count, int roster) {
  Simulation walk(0, count, roster);
  std::vector<Command> route;
  auto control = [&](int buttons) {
    Command c{owner, int(route.size()) + 1, walk.State().tick + 1,
              {buttons, 0, 2, false}};
    Check(walk.Add(c), "diamond route input");
    route.push_back(c);
  };
  control(Right);
  for (int n = 0; n < 200 && walk.State().archers[owner].x < 1150; ++n)
    walk.Seek(walk.State().tick + 1);
  Check(walk.State().archers[owner].x >= 1150, "reach the diamond platform");
  control(0);
  return route;
}
int main() {
  try {
    engine::Physics::SetGravity(2000);
    std::atomic<int64_t> counter{0};
    engine::Timeline root(counter, 1), child(&root, 10);
    counter = 20;
    Check(root.GetTime() == 20 && child.GetTime() == 2,
          "counter and arbitrary child anchor");
    child.SetTic(5);
    Check(child.GetTime() == 4, "adjustable tic preserves elapsed time");
    child.SetScale(.5);
    counter = 30;
    Check(child.GetTime() == 5, "child scale composes with its anchor");
    root.Pause();
    counter = 100;
    Check(child.GetTime() == 5, "parent pause freezes child");
    root.Unpause();
    counter = 110;
    Check(child.GetTime() == 6, "anchored clock resumes without catch-up");
    Check(std::abs(SegmentHit(0, 5, 100, 5, {20, 0, 1, 10}) - .2f) < .0001f,
          "swept hit crosses thin obstacle");
    Check(SegmentHit(0, 20, 100, 20, {20, 0, 1, 10}) > 1, "swept miss");
    // Follow real routes from the spawn rather than injecting positions: step
    // off the left ledge, jump back from the floor, then reach the upper ledge.
    // The same route must work at all personal time scales.
    for (int speed : {1, 2, 4}) {
      Simulation platforms(0, 3);
      int number = 0;
      auto control = [&](int buttons) {
        Check(platforms.Add({0,
                             ++number,
                             platforms.State().tick + 1,
                             {buttons, 0, speed, false}}),
              "platform route input");
      };
      auto until = [&](auto condition, int maxTicks, const char *message) {
        for (int n = 0; n < maxTicks; ++n) {
          platforms.Seek(platforms.State().tick + 1);
          if (condition(platforms.State().archers[0]))
            return;
        }
        throw std::runtime_error(std::string(message) + " (speed " +
                                 std::to_string(speed * .5) + ")");
      };
      control(Left);
      until([](const Archer &p) { return p.x < 140; }, 150, "walk off ledge");
      control(0);
      until(
          [](const Archer &p) {
            return p.grounded && p.riding == 0 && std::abs(p.vx) < 1;
          },
          200, "settle on floor");
      control(Right | Jump);
      // Stop horizontal input beneath the broad target ledge, making this a
      // reachable jump rather than a running leap past its far edge.
      until([](const Archer &p) { return p.x >= 270; }, 200,
            "approach lower ledge");
      control(0);
      until(
          [](const Archer &p) {
            return p.grounded && p.riding == 3 && std::abs(p.vx) < 1;
          },
          220, "land on lower ledge from floor");
      control(Right);
      until([](const Archer &p) { return p.x >= 390; }, 150,
            "walk beneath upper ledge");
      control(0);
      until(
          [](const Archer &p) {
            return p.grounded && p.riding == 3 && std::abs(p.vx) < 1;
          },
          100, "settle before upper ledge jump");
      control(Jump);
      until([](const Archer &p) { return p.grounded && p.riding == 6; }, 260,
            "jump from lower to upper ledge");
      const auto ledge = SurfaceAt(Arena()[6], platforms.State().tick);
      Check(std::abs(platforms.State().archers[0].y + ArcherHeight - ledge.y) <
                .01,
            "upper ledge landing resolves to its surface");
      // A full jump from the upper ledge must stay in arrow range below the
      // HUD, and it is tall enough to meet the ceiling.
      control(0);
      until([](const Archer &) { return true; }, 1, "release jump");
      control(Jump);
      float highest = platforms.State().archers[0].y;
      until(
          [&](const Archer &p) {
            highest = std::min(highest, p.y);
            return p.grounded && p.riding == 6;
          },
          260, "land after upper ledge jump");
      Check(highest >= Ceiling, "jump stays below the HUD ceiling");
      Check(highest == Ceiling, "upper ledge jump reaches the ceiling");
    }
    std::vector<Command> commands{{0, 1, 1, {Right, 0, 2, false}},
                                  {0, 2, 45, {Jump | Right, 0, 2, false}},
                                  {1, 1, 6, {Left | Shoot, 4, 1, false}},
                                  {2, 1, 2, {Left | Jump, 4, 4, false}},
                                  {0, 3, 72, {Shoot, 0, 2, true}},
                                  {0, 4, 99, {Left | Shoot, 4, 2, false}}};
    Simulation reference(0, 3), late(0, 3);
    for (auto c : commands)
      Check(reference.Add(c), "insert command");
    reference.Seek(180);
    late.Seek(180);
    std::reverse(commands.begin(), commands.end());
    for (auto c : commands) {
      Check(late.Add(c), "late insert");
      late.Seek(180);
    }
    Check(WorldHash(reference.State()) == WorldHash(late.State()),
          "late and reordered commands replay identically");
    Check(late.Rollbacks() > 0, "late commands cause replay");
    Check(!late.Add(commands.front()), "duplicate command rejected");
    auto hash = WorldHash(late.State());
    late.Seek(50);
    late.Seek(180);
    Check(hash == WorldHash(late.State()), "historical seek preserves result");
    Simulation time(0, 3);
    time.Add({0, 1, 1, {Right, 0, 1, false}});
    time.Add({1, 1, 1, {Left, 4, 2, false}});
    time.Add({2, 1, 1, {0, 0, 4, false}});
    time.Seek(40);
    Check(time.State().archers[0].personalUs == 200000,
          "half speed personal clock");
    Check(time.State().archers[1].personalUs == 400000,
          "normal personal clock");
    Check(time.State().archers[2].personalUs == 800000,
          "double personal clock");
    auto position = time.State().archers[0];
    time.Add({0, 2, 41, {Right, 0, 1, true}});
    time.Seek(80);
    Check(time.State().archers[0].x == position.x &&
              time.State().archers[0].y == position.y,
          "paused archer stays fixed");
    Check(time.State().archers[0].personalUs == position.personalUs,
          "paused clock stays fixed");
    Check(time.State().archers[1].personalUs == 800000,
          "other clock continues");
    time.Add({0, 3, 81, {Right, 0, 1, false}});
    time.Seek(81);
    Check(time.State().archers[0].personalUs == position.personalUs + 5000,
          "resume has no accumulated time jump");
    Simulation combat(0, 2);
    Simulation practice(0, 3);
    int botCommands[3]{};
    for (int tick = 1; tick <= PracticeWarmupTicks; ++tick) {
      if (tick % 5 == 1)
        for (int id = 1; id < 3; ++id)
          practice.Add({id, ++botCommands[id], tick,
                        BotInput(practice.State(), id, false, true)});
      practice.Seek(tick);
    }
    Check(practice.State().shots == 0 && practice.State().archers[0].alive,
          "idle practice player survives eight-second no-fire warm-up");
    auto trainingWorld = NewRound(0, 2);
    trainingWorld.archers[1].x = 600;
    trainingWorld.tick = PracticeWarmupTicks + 30;
    Check(!(BotInput(trainingWorld, 1, false, true).buttons & Shoot),
          "practice rival gives an aiming warning before firing");
    trainingWorld.tick = PracticeWarmupTicks + 60;
    Check(BotInput(trainingWorld, 1, false, true).buttons & Shoot,
          "practice rival can fire after the warning");
    trainingWorld.tick += 40;
    Check(!(BotInput(trainingWorld, 1, false, true).buttons & Shoot) &&
              (BotInput(trainingWorld, 1).buttons & Shoot),
          "practice spaces shots while competitive bot behavior is preserved");
    combat.Add({0, 1, 1, {Shoot, 0, 2, false}});
    combat.Seek(120);
    Check(combat.State().shots == 3, "only three arrows available");
    Check(combat.State().hits == 1 && !combat.State().archers[1].alive &&
              combat.State().result == NoResult,
          "arrow hit eliminates enemy; the survivor must still claim the "
          "diamond");
    // Upward shots miss opponents and cannot be recovered. Holding the bow
    // spaces arrows by the short refire delay, not a multi-second cooldown.
    for (int speed : {1, 2, 4}) {
      Simulation volley(0, 2);
      volley.Add({0, 1, 1, {Shoot, 6, speed, false}});
      volley.Seek(2);
      Check(volley.State().shots == 1, "held bow does not fire every step");
      volley.Seek(200);
      Check(volley.State().shots == 3 && volley.State().archers[0].ammo == 0,
            "all three arrows fire within two seconds");
    }
    Simulation pausedShot(0, 2);
    pausedShot.Add({0, 1, 1, {Shoot, 6, 2, true}});
    pausedShot.Seek(100);
    Check(pausedShot.State().shots == 0, "paused archer cannot fire");
    // The lower archers shoot each other while archer 2 waits at the diamond.
    // The survivor wins only when active; a paused last survivor is a draw.
    const auto route = DiamondRoute(2, 3, 7);
    for (bool paused : {false, true}) {
      Simulation waiting(0, 3);
      for (auto c : route)
        waiting.Add(c);
      waiting.Add({2, int(route.size()) + 1, 101, {0, 0, 2, paused}});
      waiting.Add({0, 1, 101, {Shoot, 0, 2, false}});
      waiting.Add({1, 1, 101, {Shoot, 4, 2, false}});
      waiting.Seek(700);
      const auto &w = waiting.State();
      Check(w.archers[2].alive && !w.archers[0].alive && !w.archers[1].alive,
            "mutual arrow hits eliminate both rivals");
      Check(w.result == (paused ? Draw : 2),
            paused ? "paused last survivor cannot win"
                   : "active last survivor claims the diamond");
    }
    for (bool paused : {false, true}) {
      Simulation idle(0, 2);
      idle.Add({0, 1, 1, {0, 0, 2, paused}});
      idle.Seek(13000);
      Check(idle.State().tick == 13000 && idle.State().result == NoResult &&
                idle.State().archers[0].alive && idle.State().archers[1].alive,
            "idle and paused archers survive beyond the old flood and tick limit");
      Check(idle.Add({0, 2, 13001, {Shoot, 0, 2, false}}),
            "commands remain accepted in a long round");
      idle.Seek(13200);
      Check(!idle.State().archers[1].alive && idle.State().hits == 1,
            "combat continues after two minutes");
    }
    {
      Simulation lone(0, 4, 1 << 3);
      for (auto c : DiamondRoute(3, 4, 1 << 3))
        lone.Add(c);
      lone.Seek(700);
      Check(lone.State().result == 3, "a lone archer wins by claiming the diamond");
    }
    {
      // With a rival alive, standing at the diamond wins nothing.
      const int pairRoster = 1 << 3 | 1 << 0;
      Simulation pair(0, 4, pairRoster);
      for (auto c : DiamondRoute(3, 4, pairRoster))
        pair.Add(c);
      pair.Seek(700);
      Check(pair.State().result == NoResult,
            "the diamond is only for the last archer standing");
      Check(pair.Add(RosterChange{0, 701, false, 1}), "roster change");
      pair.Seek(1400);
      Check(!pair.State().archers[0].present && pair.State().result == 3,
            "a rival who leaves leaves the diamond to the last archer");
    }
    {
      Simulation joined(0, 4, 1 << 3), late(0, 4, 1 << 3);
      joined.Seek(13000);
      Check(joined.State().archers[3].alive && !joined.State().archers[1].present,
            "a lone archer can wait for a late arrival without a hazard");
      joined.Add(RosterChange{1, 13001, true, 1});
      joined.Seek(13001);
      const auto spawn = NewRound(0, 4).archers[1];
      Check(joined.State().archers[1].present &&
                joined.State().archers[1].alive &&
                joined.State().archers[1].x == spawn.x &&
                joined.State().archers[1].ammo == 3,
            "a newcomer spawns at its slot's start");
      joined.Seek(17000);
      Check(joined.State().archers[1].alive && joined.State().archers[3].alive,
            "both archers survive well beyond the old flood deadline");
      late.Seek(17000);
      late.Add(RosterChange{1, 13001, true, 1});
      late.Seek(17000);
      Check(WorldHash(late.State()) == WorldHash(joined.State()),
            "a late roster change replays identically");
    }
    Simulation recovery(0, 2);
    recovery.Add({0, 1, 1, {Shoot, 2, 2, false}});
    recovery.Add({0, 2, 2, {0, 2, 2, false}});
    recovery.Seek(25);
    Check(recovery.State().shots == 1 &&
              recovery.State().archers[0].ammo == 3 &&
              recovery.State().arrows.empty(),
          "stuck arrow is collected once and restores ammunition");
    Journal journal;
    int64_t serial = 1, now = 1000000;
    auto add = [&](Record r) {
      r.serial = serial++;
      r.at = now;
      Check(journal.Insert(r, 3), "journal insertion");
    };
    for (int i = 0; i < 3; ++i) {
      Record r;
      r.author = i;
      r.number = 3;
      add(r);
    }
    // Archer 2 walks to the diamond; its rivals later shoot each other.
    Check(route.back().tick <= 100, "the walk fits inside the sealed prefix");
    auto walkToDiamond = [&](int round) {
      for (const auto &c : route) {
        Record r;
        r.type = RecordType::Command;
        r.author = 2;
        r.round = round;
        r.number = c.number;
        r.tick = c.tick;
        r.input = c.input;
        add(r);
      }
    };
    walkToDiamond(0);
    Match match(3);
    match.Update(journal, now);
    now = match.View().start + 1000000;
    Record close;
    close.type = RecordType::Close;
    close.tick = 100;
    add(close);
    for (int i = 0; i < 3; ++i) {
      Record seal;
      seal.type = RecordType::Seal;
      seal.author = i;
      seal.tick = 100;
      seal.number = i == 0 ? 1 : i == 2 ? int(route.size()) : 0;
      add(seal);
    }
    match.Update(journal, now);
    Check(match.View().phase == Phase::Confirming &&
              !match.View().prefixComplete,
          "missing command blocks round vote");
    Record command;
    command.type = RecordType::Command;
    command.number = 1;
    command.tick = 1;
    add(command);
    match.Update(journal, now);
    Check(match.View().prefixComplete, "complete command prefix permits vote");
    auto agreedHash = match.View().resultHash;
    for (int i = 0; i < 3; ++i) {
      Record vote;
      vote.type = RecordType::Vote;
      vote.author = i;
      vote.tick = 100;
      vote.hash = agreedHash + (i == 2);
      add(vote);
    }
    match.Update(journal, now);
    Check(match.View().votes == 2 && match.View().commits == 0,
          "wrong hash cannot commit");
    Record vote;
    vote.type = RecordType::Vote;
    vote.author = 2;
    vote.tick = 100;
    vote.hash = agreedHash;
    add(vote);
    match.Update(journal, now);
    Check(match.View().attempt == 1 && match.View().phase == Phase::Playing,
          "disproved predicted result resumes play");
    for (int i = 0; i < 2; ++i) {
      Record shot;
      shot.type = RecordType::Command;
      shot.author = i;
      shot.number = i == 0 ? 2 : 1;
      shot.tick = 101;
      shot.input = {Shoot, i == 0 ? 0 : 4, 2, false};
      add(shot);
    }
    now = match.View().start + 7000000;
    match.Update(journal, now);
    for (int pass = 0; pass < 5; ++pass) {
      for (int i = 0; i < 3; ++i)
        for (auto r : match.ActionsFor(i, journal, now))
          add(r);
      match.Update(journal, now);
    }
    Check(match.View().commits == 1 && match.View().scores[2] == 1 &&
              match.View().phase == Phase::Complete,
          "the one agreed round decides the game");
    now += 60000000;
    match.Update(journal, now);
    Check(match.View().scores[2] == 1 && match.View().round == 0 &&
              match.View().phase == Phase::Complete,
          "a decided game stays decided: no further rounds");
    Match observer(3);
    observer.Update(journal, now);
    Check(observer.View().scores == match.View().scores &&
              observer.View().resultHash == match.View().resultHash &&
              observer.View().phase == Phase::Complete,
          "late observer reconstructs the decided game");
    {
      // A server's match: archers come and go, and the server decides rounds.
      Journal served;
      Match host(4, true);
      int64_t order = 0, clock = 5000000;
      auto write = [&](Record r) {
        r.serial = ++order;
        r.at = clock;
        Check(served.Insert(r, 4), "server record");
        host.Update(served, clock);
      };
      auto roster = [&](int archer, bool joins) {
        host.Update(served, clock);
        Record r;
        r.type = joins ? RecordType::Join : RecordType::Leave;
        r.author = archer;
        std::tie(r.round, r.tick) = host.Stamp(clock);
        write(r);
      };
      auto commitFor = [&](int round, int winner) {
        Record c;
        c.type = RecordType::Commit;
        c.round = round;
        c.tick = 10;
        c.number = winner + 1;
        write(c);
      };
      host.Update(served, clock);
      Check(host.View().phase == Phase::Lobby, "an empty server waits");
      roster(3, true);
      Check(host.View().phase == Phase::Countdown &&
                host.View().start == clock + 1800000 &&
                host.View().world.archers[3].present &&
                !host.View().world.archers[0].present,
            "the first archer starts a countdown on its own");
      clock = host.View().start + 5000000;
      host.Update(served, clock);
      Check(host.View().phase == Phase::Playing, "a lone archer plays");
      roster(0, true);
      clock += 100000;
      host.Update(served, clock);
      Check(host.View().world.archers[0].present &&
                host.View().world.archers[0].alive,
            "a client that starts mid-round gets an archer at once");
      roster(0, false);
      clock += 100000;
      host.Update(served, clock);
      Check(!host.View().world.archers[0].present,
            "a client that quits takes its archer with it");
      for (const auto &c : DiamondRoute(3, 4, 1 << 3)) {
        Record r;
        r.type = RecordType::Command;
        r.author = 3;
        r.number = c.number;
        r.tick = c.tick;
        r.input = c.input;
        write(r);
      }
      Check(host.View().phase == Phase::Confirming &&
                host.View().world.result == 3,
            "a late walk replays into a claimed diamond");
      Record commit;
      Check(host.CommitDue(clock, commit) && commit.number == 4,
            "the server commits the claimed diamond after a grace period");
      write(commit);
      Check(host.View().phase == Phase::Complete &&
                host.View().scores[3] == 1 && host.View().commits == 1,
            "the committed round decides the game for its winner");
      Check(host.Stamp(clock) == std::make_pair(1, 1),
            "a change after the result waits for the next game");
      Match watcher(4, true);
      watcher.Update(served, clock);
      Check(watcher.View().scores == host.View().scores &&
                watcher.View().phase == Phase::Complete &&
                watcher.View().resultHash == commit.hash,
            "a late observer reconstructs the server's game");
      roster(0, true);
      clock = host.View().transition - 1;
      host.Update(served, clock);
      Check(host.View().phase == Phase::Complete,
            "the winner is shown for five seconds");
      clock = host.View().transition + 1;
      host.Update(served, clock);
      Check(host.View().round == 1 && host.View().phase == Phase::Countdown &&
                host.View().scores == std::array<int, MaxPlayers>{} &&
                host.View().world.archers[3].present &&
                host.View().world.archers[0].present,
            "a new game starts with everyone connected");
      clock = host.View().start + 100000;
      host.Update(served, clock);
      commitFor(1, -1);
      Check(host.View().phase == Phase::Complete &&
                host.View().world.result == Draw &&
                host.View().scores == std::array<int, MaxPlayers>{},
            "a round with no winner ends the game as a draw");
      clock = host.View().transition + 1;
      host.Update(served, clock);
      roster(0, false);
      roster(3, false);
      Check(host.View().phase == Phase::Lobby,
            "a game left empty waits for an archer");
      clock += 30000000;
      roster(2, true);
      Check(host.View().phase == Phase::Countdown &&
                host.View().start == clock + 1800000,
            "the next archer's arrival starts the countdown");
      Match lateWatcher(4, true);
      lateWatcher.Update(served, clock);
      Check(lateWatcher.View().round == host.View().round &&
                lateWatcher.View().start == host.View().start &&
                WorldHash(lateWatcher.View().world) ==
                    WorldHash(host.View().world),
            "a late observer reconstructs the whole session");
    }
    Record parsed;
    Check(Decode(Encode(vote), parsed) && parsed.hash == vote.hash,
          "record round trip");
    Check(!Decode(Encode(vote) + " junk", parsed), "trailing data rejected");
    std::cout << "Towerfall rules: all checks passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "FAIL: " << e.what() << '\n';
    return 1;
  }
}
