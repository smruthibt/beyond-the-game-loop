#include "Transport.hpp"
#include "engine/Server.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace towerfall {
// Display names for the command-line modes.
const char *ModeLabel(Mode m) {
  switch (m) {
  case Mode::Solo:
    return "SOLO PRACTICE";
  case Mode::Client:
    return "CLIENT / SERVER";
  case Mode::Hybrid:
    return "HYBRID P2P";
  case Mode::Peer:
    return "SERVERLESS P2P";
  default:
    return "DEDICATED SERVER";
  }
}
std::string WriteWorld(const World &w) {
  std::ostringstream o;
  o << std::setprecision(9) << w.tick << ' ' << w.round << ' ' << w.count << ' '
    << w.nextArrow << ' ' << w.result << ' ' << w.resultTick << ' ' << w.shots
    << ' ' << w.hits << ' ' << w.entered;
  for (const auto &p : w.archers)
    o << ' ' << p.present << ' ' << p.x << ' ' << p.y << ' ' << p.vx << ' ' << p.vy << ' ' << p.ammo
      << ' ' << p.facing << ' ' << p.riding << ' ' << p.wall << ' '
      << p.personalUs << ' ' << p.shootWait << ' ' << p.groundGrace << ' '
      << p.jumpBuffer << ' '
      << p.wallGrace << ' ' << p.previousButtons << ' ' << p.kills << ' '
      << p.alive << ' ' << p.grounded << ' ' << p.input.buttons << ' '
      << p.input.aim << ' ' << p.input.speed << ' ' << p.input.paused;
  o << ' ' << w.arrows.size();
  for (const auto &a : w.arrows)
    o << ' ' << a.id << ' ' << a.owner << ' ' << a.born << ' ' << a.platform
      << ' ' << a.x << ' ' << a.y << ' ' << a.vx << ' ' << a.vy << ' '
      << a.attachX << ' ' << a.attachY << ' ' << int(a.state);
  return o.str();
}
bool ReadWorld(const std::string &s, World &w) {
  if (s.size() > 32768)
    return false;
  World n;
  std::istringstream in(s);
  if (!(in >> n.tick >> n.round >> n.count >> n.nextArrow >> n.result >>
        n.resultTick >> n.shots >> n.hits >> n.entered) ||
      n.count < 1 || n.count > 4 || n.tick < 0)
    return false;
  for (auto &p : n.archers) {
    if (!(in >> p.present >> p.x >> p.y >> p.vx >> p.vy >> p.ammo >> p.facing >> p.riding >>
          p.wall >> p.personalUs >> p.shootWait >>
          p.groundGrace >> p.jumpBuffer >> p.wallGrace >> p.previousButtons >>
          p.kills >> p.alive >> p.grounded >> p.input.buttons >> p.input.aim >>
          p.input.speed >> p.input.paused))
      return false;
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.vx) ||
        !std::isfinite(p.vy) || p.input.aim < 0 || p.input.aim > 7 ||
        p.ammo < 0 || p.ammo > 5)
      return false;
  }
  int size;
  if (!(in >> size) || size < 0 || size > 64)
    return false;
  for (int i = 0; i < size; ++i) {
    Arrow a;
    int state;
    if (!(in >> a.id >> a.owner >> a.born >> a.platform >> a.x >> a.y >> a.vx >>
          a.vy >> a.attachX >> a.attachY >> state) ||
        state < 0 || state > 2 || a.owner < 0 || a.owner >= n.count ||
        !std::isfinite(a.x) || !std::isfinite(a.y))
      return false;
    a.state = ArrowState(state);
    n.arrows.push_back(a);
  }
  w = std::move(n);
  return true;
}
namespace {
std::string Packet(const Journal &j, const MatchView &m, int players,
                   bool hybrid, int64_t now, int64_t epoch) {
  std::ostringstream o;
  o << "TFWORLD 1 " << players << ' ' << hybrid << ' ' << now << ' ' << epoch
    << '\n';
  World shared = m.world;
  // Hybrid live player positions travel directly between peers. The server
  // still resolves shared combat, arrows, round transitions and platforms.
  if (hybrid)
    for (auto &p : shared.archers)
      p.x = p.y = p.vx = p.vy = 0;
  o << "SIM " << WriteWorld(shared) << '\n';
  for (const auto &r : j.Records())
    o << Encode(r) << '\n';
  return o.str();
}
// --log output, stamped with the time of day so that lines from different
// processes compare. Each line is written whole and flushed, so the processes
// of a session can share one terminal without breaking each other's lines.
void LogLine(const std::string &who, const char *format, ...) {
  char text[640];
  va_list args;
  va_start(args, format);
  std::vsnprintf(text, sizeof text, format, args);
  va_end(args);
  const auto clock = std::chrono::system_clock::now();
  const std::time_t seconds = std::chrono::system_clock::to_time_t(clock);
  const int millis = int(std::chrono::duration_cast<std::chrono::milliseconds>(
                             clock.time_since_epoch())
                             .count() %
                         1000);
  std::tm local{};
  localtime_r(&seconds, &local);
  char stamp[16];
  std::strftime(stamp, sizeof stamp, "%H:%M:%S", &local);
  char line[720];
  std::snprintf(line, sizeof line, "%s.%03d [%s] %s\n", stamp, millis,
                who.c_str(), text);
  std::fputs(line, stdout);
  std::fflush(stdout);
}
const char *PhaseName(Phase p) {
  switch (p) {
  case Phase::Lobby:
    return "Lobby";
  case Phase::Countdown:
    return "Countdown";
  case Phase::Playing:
    return "Playing";
  case Phase::Confirming:
    return "Confirming";
  case Phase::Results:
    return "Results";
  default:
    return "Complete";
  }
}
std::string Outcome(int result) {
  return result >= 0 ? "P" + std::to_string(result + 1) + " wins"
         : result == Draw ? std::string("draw")
                          : std::string("undecided");
}
// The records a server writes, in the words the logs use.
std::string DescribeServerRecord(const Record &r) {
  char text[200];
  if (r.type == RecordType::Commit)
    std::snprintf(text, sizeof text,
                  "COMMIT round %d at tick %d: %s, hash %llu", r.round,
                  r.tick, Outcome(r.number - 1).c_str(),
                  (unsigned long long)r.hash);
  else
    std::snprintf(text, sizeof text, "%s P%d at round %d tick %d",
                  r.type == RecordType::Join ? "JOIN" : "LEAVE",
                  r.author + 1, r.round, r.tick);
  return text;
}
} // namespace
template <typename... Args>
void Connection::Log(const char *format, Args... args) const {
  if (options.log)
    LogLine(LogName(), format, args...);
}
std::string Connection::Slot(int peerId) const {
  return peerId < options.players ? "P" + std::to_string(peerId + 1)
                                  : "spectator " + std::to_string(peerId);
}
std::string Connection::LogName() const {
  std::string who = options.mode == Mode::Peer     ? "peer"
                    : options.mode == Mode::Hybrid ? "hybrid"
                    : options.mode == Mode::Client ? "client"
                                                   : "solo";
  if (options.spectator)
    return who + " spectator";
  return id >= 0 ? who + " P" + std::to_string(id + 1) : who;
}
// Describes every record that reached the journal since the last call, then
// any phase change, then - once a second - the traffic counted since.
void Connection::LogTraffic() {
  const auto &records = journal.Records();
  for (; logged < records.size(); ++logged) {
    const auto &r = records[logged];
    const std::string who =
        "P" + std::to_string(r.author + 1) + (r.author == id ? " (me)" : "");
    switch (r.type) {
    case RecordType::Command:
      ++logCommands[r.author];
      break;
    case RecordType::Ready:
      Log("%s READY for a %d-archer match", who.c_str(), r.number);
      break;
    case RecordType::Close:
      Log("%s CLOSE round %d attempt %d at tick %d: it predicts a result",
          who.c_str(), r.round, r.attempt, r.tick);
      break;
    case RecordType::Seal:
      Log("%s SEAL round %d attempt %d: commands 1-%d, last tick %d",
          who.c_str(), r.round, r.attempt, r.number, r.tick);
      break;
    case RecordType::Vote:
      Log("%s VOTE round %d attempt %d: tick %d hash %llu", who.c_str(),
          r.round, r.attempt, r.tick, (unsigned long long)r.hash);
      break;
    default:
      Log("server %s", DescribeServerRecord(r).c_str());
    }
  }
  const auto &v = match.View();
  if (logRound < 0 && v.round == 0 && v.phase == logPhase) {
    logRound = 0;
    Log("starts in the %s", PhaseName(v.phase));
  }
  if (v.round != logRound || v.phase != logPhase || v.attempt != logAttempt) {
    if (v.round == logRound && v.attempt > logAttempt)
      Log("replay disproved the predicted result: round %d resumes as "
          "attempt %d",
          v.round, v.attempt);
    Log("phase %s -> %s (round %d)", PhaseName(logPhase), PhaseName(v.phase),
        v.round);
    if (v.phase == Phase::Complete) {
      const std::string by = Served(options.mode)
                                 ? "the server's commit"
                                 : "all " + std::to_string(v.votes) +
                                       " matching votes";
      Log("round %d decided by %s: %s, result hash %llu", v.round, by.c_str(),
          Outcome(v.world.result).c_str(), (unsigned long long)v.resultHash);
    }
    logRound = v.round;
    logPhase = v.phase;
    logAttempt = v.attempt;
  }
  const int64_t real = WallMicros();
  if (real - logSince < 1000000)
    return;
  // Peers send commands and poses to each other directly. With a server,
  // commands go up in state messages and come back in its snapshots; hybrid
  // poses still go directly between peers.
  const int mine = id >= 0 && id < MaxPlayers ? logCommands[id] : 0;
  std::string out, commands, poses;
  if (client)
    out = std::to_string(logStates) + " state msgs to server (" +
          std::to_string(logStateCommands) + " cmds)";
  else if (!options.spectator)
    out = std::to_string(mine) + " cmds";
  if (peer && !options.spectator)
    out += ", " + std::to_string(logPoses) + " poses to peers";
  for (int i = 0; i < options.players; ++i)
    if (i != id) {
      if (logCommands[i])
        commands += (commands.empty() ? " P" : ", P") + std::to_string(i + 1) +
                    " " + std::to_string(logCommands[i]);
      if (logPosesHeard[i])
        poses += (poses.empty() ? " P" : ", P") + std::to_string(i + 1) + " " +
                 std::to_string(logPosesHeard[i]);
    }
  std::string in = client ? std::to_string(logWorlds) + " snapshots; " : "";
  in += "cmds" + (commands.empty() ? std::string(" -") : commands);
  if (peer)
    in += "; poses" + (poses.empty() ? std::string(" -") : poses);
  Log("1s | out: %s | in: %s | journal %zu, rollbacks +%d, tick %d",
      out.empty() ? "-" : out.c_str(), in.c_str(), journal.Size(),
      v.rollbacks - logRollbacks, v.world.tick);
  logSince = real;
  logStates = logStateCommands = logPoses = logWorlds = 0;
  logRollbacks = v.rollbacks;
  logCommands.fill(0);
  logPosesHeard.fill(0);
}
Connection::Connection(engine::Engine &e, Options o)
    : engine(e), options(std::move(o)),
      match(options.players, Served(options.mode)) {
  epoch = now = rateSince = WallMicros();
  serial = now;
}
// The only doors between the two threads, the ThreadExample's networkMutex
// handoff: the engine's background thread (the example's networkingThread)
// owns networking and the simulation, and publishes `output` in PublishView;
// the main thread reads that with Read() and hands input over with Controls(),
// Ready() and RestartPractice(). Each side copies under the engine's world
// mutex and never holds it across other work.
NetworkFrame Connection::Read() {
  std::lock_guard<std::mutex> guard(engine.GetWorldMutex());
  return output;
}
void Connection::Controls(Input i) {
  std::lock_guard<std::mutex> guard(engine.GetWorldMutex());
  requested = i;
}
void Connection::Ready() {
  std::lock_guard<std::mutex> guard(engine.GetWorldMutex());
  readyRequested = true;
}
void Connection::RestartPractice() {
  if (options.mode != Mode::Solo)
    return;
  std::lock_guard<std::mutex> guard(engine.GetWorldMutex());
  restartRequested = true;
}
void Connection::Open() {
  opened = true;
  if (options.mode == Mode::Solo) {
    id = 0;
    soloOrigin = WallMicros() -
                 int64_t(engine.GetGameTime().GetSeconds() * 1000000);
    return;
  }
  if (options.mode == Mode::Client || options.mode == Mode::Hybrid) {
    engine::Client::Config c;
    c.joinPort = options.port;
    c.worldPort = options.port + 1;
    c.worldTopic = "TFWORLD";
    c.sendIntervalSeconds = 1.f / 30;
    c.timeoutMs = 500;
    client = std::make_unique<engine::Client>(c);
    if (!client->Join()) {
      failed = true;
      problem = client->LastError();
      return;
    }
    // The server answers with this process's archer (-1: watching, or all
    // four archers are taken) and a snapshot of the match so far.
    std::string reply;
    int archer = -1;
    const bool synced = client->Request(
        options.spectator ? "SYNC WATCH" : "SYNC PLAY", reply);
    const size_t end = reply.find('\n');
    if (!synced || end == std::string::npos ||
        std::sscanf(reply.c_str(), "ARCHER %d", &archer) != 1) {
      failed = true;
      problem = "The server did not provide a match snapshot.";
      return;
    }
    id = archer;
    if (id < 0)
      options.spectator = true;
    Log("joined the server on port %d: own REQ/REP channel on port %d, %s",
        options.port, client->ChannelPort(),
        id >= 0 ? ("archer P" + std::to_string(id + 1)).c_str() : "watching");
    ReceiveWorld(reply.substr(end + 1));
  }
  if (options.mode == Mode::Peer || options.mode == Mode::Hybrid) {
    engine::Peer::Config c;
    c.portBase = options.port + 30;
    // Hybrid spectators take peer ids after the four archers.
    c.maxPeers = options.mode == Mode::Hybrid ? MaxClients : MaxPlayers;
    c.gossipMs = 100;
    c.topics = {"TFHELLO", "TFPOSE"};
    peer = std::make_unique<engine::Peer>(c);
    int desired = id;
    if (options.mode == Mode::Peer)
      desired = options.id >= 0 ? options.id
                                : (options.spectator ? options.players : -1);
    int claimed = -1;
    if (options.mode == Mode::Hybrid && id < 0)
      for (int k = MaxPlayers; k < c.maxPeers && claimed < 0; ++k)
        claimed = peer->Claim(k);
    else
      claimed = peer->Claim(desired);
    if (claimed < 0) {
      failed = true;
      problem = peer->LastError();
      return;
    }
    if (options.mode == Mode::Peer)
      id = claimed;
    Log("claimed peer slot %d: PUB on port %d, SUB to ports %d-%d (%s)",
        claimed, c.portBase + claimed, c.portBase, c.portBase + c.maxPeers - 1,
        options.mode == Mode::Peer ? "commands, clock and votes; no server"
                                   : "direct player positions");
    peer->ConnectAll();
    if (options.mode == Mode::Peer)
      peer->EnableSharedClock();
  }
  if (id >= options.players)
    options.spectator = true;
}
void Connection::SendRecord(Record r) {
  r.serial = ++serial;
  if (r.author < 0 || r.author >= options.players)
    return;
  if (options.mode == Mode::Solo) {
    journal.Insert(r, options.players);
    return;
  }
  if (options.mode == Mode::Peer) {
    peer->AppendEvent(Encode(r));
    journal.Insert(r, options.players);
    return;
  }
  // With a server, a command goes into this process's own match at once, so
  // the archer responds immediately, and travels in the next scheduled state
  // message. A client at 2x therefore delivers its moves 60 times a second
  // and one at 0.5x 15 times: the message rate follows the game loop's speed.
  if (journal.Insert(r, options.players))
    outbox.push_back(r);
}
std::string Connection::StateMessage() const {
  std::string message = "STATE " + std::to_string(outbox.size());
  for (const auto &r : outbox)
    message += '\n' + Encode(r);
  return message;
}
void Connection::Delivered(const std::string &reply, size_t count) {
  if (reply != "OK") {
    problem = "The server refused this client's commands.";
    failed = true;
    return;
  }
  outbox.erase(outbox.begin(), outbox.begin() + std::min(count, outbox.size()));
}
void Connection::ReceiveWorld(const std::string &wire) {
  if (wire.size() > 8000000)
    return;
  std::istringstream in(wire);
  std::string line, tag;
  int version, count;
  bool hybrid;
  int64_t t, e;
  if (!std::getline(in, line))
    return;
  std::istringstream header(line);
  if (!(header >> tag >> version >> count >> hybrid >> t >> e) ||
      tag != "TFWORLD" || version != 1)
    return;
  if (count != options.players || hybrid != (options.mode == Mode::Hybrid)) {
    failed = true;
    problem = "Server mode or player count differs from this client.";
    return;
  }
  now = t;
  epoch = e;
  lastWorld = WallMicros();
  ++logWorlds;
  while (std::getline(in, line)) {
    if (line.rfind("SIM ", 0) == 0) {
      World w;
      if (ReadWorld(line.substr(4), w)) {
        authoritative = w;
        hasAuthoritative = true;
      }
    } else {
      Record r;
      if (Decode(line, r) && r.author >= 0 && r.author < options.players) {
        journal.Insert(r, options.players);
        heard[r.author] = lastWorld;
      }
    }
  }
}
void Connection::OnPeer(const std::string &topic, std::istringstream &in) {
  if (topic == "TFHELLO") {
    int author, version, players, pure;
    if (in >> author >> version >> players >> pure && author >= 0 &&
        author < MaxPlayers) {
      if (version != 1 || players != options.players ||
          pure != (options.mode == Mode::Peer)) {
        failed = true;
        problem = "Peer mode/player count mismatch.";
        return;
      }
      if (WallMicros() - heard[author] > 2000000)
        Log("HELLO from %s: same protocol, %d players, %s",
            Slot(author).c_str(), players, pure ? "serverless" : "hybrid");
      heard[author] = WallMicros();
    }
  } else if (topic == "TFPOSE") {
    int author, round, tick;
    float x, y;
    if (in >> author >> round >> tick >> x >> y && author >= 0 &&
        author < MaxPlayers && std::isfinite(x) && std::isfinite(y)) {
      heard[author] = WallMicros();
      ++received;
      ++logPosesHeard[author];
      if (round > poses[author].round ||
          (round == poses[author].round && tick >= poses[author].tick))
        poses[author] = {round, tick, x, y};
    }
  }
}
void Connection::ProduceCommands(int64_t time) {
  const auto &v = match.View();
  if (commandRound != v.round) {
    commandRound = v.round;
    commandNumber.fill(0);
    for (auto &p : previous) {
      p.buttons = -1;
    }
  }
  std::array<int, MaxPlayers> lastTick{};
  for (const auto &r : journal.Records())
    if (r.type == RecordType::Command && r.round == v.round) {
      commandNumber[r.author] = std::max(commandNumber[r.author], r.number);
      lastTick[r.author] = std::max(lastTick[r.author], r.tick);
    }
  bool ready;
  Input controls;
  {
    std::lock_guard<std::mutex> guard(engine.GetWorldMutex());
    ready = readyRequested;
    controls = requested;
  }
  int first = options.mode == Mode::Solo ? 0 : id,
      last = options.mode == Mode::Solo ? options.players : id + 1;
  if (options.spectator)
    return;
  const bool served = Served(options.mode);
  for (int owner = first; owner < last; ++owner) {
    // With a server, joining is the Ready: the server adds the archer.
    if (!served && v.phase == Phase::Lobby &&
        (ready || options.bot || owner != id) &&
        !Match::Has(journal, RecordType::Ready, owner, 0, 0)) {
      Record r;
      r.type = RecordType::Ready;
      r.author = owner;
      r.at = time;
      r.number = options.players;
      SendRecord(r);
    }
    if (v.phase != Phase::Countdown && v.phase != Phase::Playing)
      continue;
    if (v.world.result != NoResult ||
        (served && !v.world.archers[owner].present))
      continue;
    Input in = controls;
    if (owner != id)
      in = BotInput(v.world, owner, options.passive, true);
    // A paused archer is frozen: its pause is sent once, and nothing more
    // until it resumes, so a paused client really is silent. The first
    // command of a round still goes out, so a new round knows it is paused.
    if (served && in.paused && previous[owner].paused &&
        previous[owner].buttons != -1)
      continue;
    // Main controls include bot decisions for the local identity so time
    // controls still apply to that identity through the real engine game
    // timeline.
    if (in != previous[owner]) {
      Record r;
      r.type = RecordType::Command;
      r.author = owner;
      r.round = v.round;
      r.at = time;
      r.number = ++commandNumber[owner];
      // Never earlier than this archer's previous command. With a server,
      // `time` can come from a snapshot slightly behind the local clock; a
      // command stamped a tick early would be replayed before the one it
      // replaces, and the older input would win - a paused archer that sent
      // its pause that way kept running everywhere.
      r.tick = std::max({1, int((time - v.start) / 10000) + 2, lastTick[owner]});
      r.input = in;
      SendRecord(r);
      previous[owner] = in;
    }
  }
}
void Connection::Update(float personalDelta) {
  if (!opened)
    Open();
  bool restart = false;
  {
    std::lock_guard<std::mutex> guard(engine.GetWorldMutex());
    restart = restartRequested;
    restartRequested = false;
    if (restart)
      readyRequested = true;
  }
  if (restart && options.mode == Mode::Solo) {
    journal = Journal{};
    match = Match(options.players);
    commandRound = -1;
    probeCommands = -1;
    probeHash = 0;
  }
  ++passes;
  if (failed) {
    PublishView();
    return;
  }
  int64_t real = WallMicros();
  now = real;
  // Solo has no other players to keep in step with, so the engine's game
  // timeline drives the whole arena: pausing it freezes platforms, arrows and
  // rivals, and scaling it slows or speeds all of them. Networked modes keep
  // the shared world on a common clock and scale only the local archer.
  if (options.mode == Mode::Solo)
    now = soloOrigin + int64_t(engine.GetGameTime().GetSeconds() * 1000000);
  if (peer) {
    peer->Tick([&](const std::string &t, std::istringstream &i, int) {
      OnPeer(t, i);
    });
    if (options.mode == Mode::Peer) {
      epoch = peer->Epoch();
      now = epoch + int64_t(peer->SharedSeconds() * 1000000);
      if (epoch != logEpoch) {
        Log("shared clock: epoch %lld, proposed by %s (%d adoption%s so far)",
            (long long)epoch, Slot(peer->EpochProposer()).c_str(),
            peer->EpochAdoptions(), peer->EpochAdoptions() == 1 ? "" : "s");
        logEpoch = epoch;
      }
      if (peer->TakeEventsDirty())
        for (const auto &e : peer->Events()) {
          Record r;
          if (Decode(e.payload, r) && r.author == e.owner)
            journal.Insert(r, options.players);
        }
    }
    if (real - lastHello > 200000) {
      peer->Publish("TFHELLO", std::to_string(id) + " 1 " +
                                   std::to_string(options.players) + " " +
                                   (options.mode == Mode::Peer ? "1" : "0"));
      lastHello = real;
    }
  }
  if (id >= 0)
    heard[id] = real;
  if (options.mode == Mode::Solo)
    for (int i = 0; i < options.players; ++i)
      heard[i] = real;
  Input controls;
  {
    std::lock_guard<std::mutex> guard(engine.GetWorldMutex());
    controls = requested;
  }
  if (client) {
    // Paced by this player's game time; commands stay in the outbox until
    // the server has answered the message that carried them.
    size_t carried = 0;
    bool sentState = client->Tick(
        options.spectator || controls.paused ? 0 : personalDelta,
        [&](const std::string &s) { ReceiveWorld(s); },
        [&] {
          carried = outbox.size();
          ++logStates;
          logStateCommands += int(carried);
          return StateMessage();
        },
        [&](const std::string &reply) {
          batched += int(carried);
          Delivered(reply, carried);
        });
    if (sentState && options.mode == Mode::Client)
      ++sent;
  }
  match.Update(journal, now);
  ProduceCommands(now);
  if (controls.paused != logPaused && !options.spectator) {
    Log(controls.paused
            ? "paused: personal clock stopped; scheduled messages stop"
            : "resumed: personal clock and scheduled messages restart");
    logPaused = controls.paused;
  }
  // A pause stops the schedule, but the pause itself must still reach the
  // server: send what is waiting once, then stay silent until resumed.
  if (client && controls.paused && !outbox.empty() && !failed) {
    std::string reply;
    const size_t carried = outbox.size();
    if (client->Request(StateMessage(), reply)) {
      ++flushes;
      Log("paused: one unscheduled state message carried %zu command%s; "
          "silent until resumed",
          carried, carried == 1 ? "" : "s");
      Delivered(reply, carried);
    } else {
      problem = "The server did not answer; waiting for it.";
      failed = true;
    }
  }
  int first = options.mode == Mode::Solo ? 0 : id,
      last = options.mode == Mode::Solo ? options.players : id + 1;
  if (!options.spectator && !Served(options.mode))
    for (int owner = first; owner < last; ++owner)
      for (auto r : match.ActionsFor(owner, journal, now))
        SendRecord(r);
  if (peer && !options.spectator) {
    sendCredit += personalDelta;
    if (!controls.paused && personalDelta > 0 && sendCredit >= 1.f / 30) {
      sendCredit = std::min(1.f / 30, sendCredit - 1.f / 30);
      const auto &w = match.View().world;
      const auto &p = w.archers[id];
      peer->Publish("TFPOSE",
                    std::to_string(id) + " " + std::to_string(w.round) + " " +
                        std::to_string(w.tick) + " " + std::to_string(p.x) +
                        " " + std::to_string(p.y));
      ++sent;
      ++logPoses;
    }
  }
  if (real - rateSince >= 1000000) {
    rate = int(std::lround(sent * 1000000.0 / (real - rateSince)));
    sent = 0;
    rateSince = real;
  }
  if (options.log)
    LogTraffic();
  PublishView();
}
void Connection::PublishView() {
  NetworkFrame n;
  n.match = match.View();
  n.id = id;
  n.spectating = options.spectator;
  n.tx = rate;
  n.rx = received;
  n.records = int(journal.Size());
  n.workerPasses = passes;
  n.epoch = epoch;
  n.now = now;
  n.batched = batched;
  n.flushes = flushes;
  n.message = problem;
  int64_t time = WallMicros();
  for (int i = 0; i < MaxPlayers; ++i) {
    n.present[i] = time - heard[i] < 2000000;
    if (n.present[i])
      ++n.peers;
  }
  n.connected =
      !failed && (options.mode == Mode::Solo || options.mode == Mode::Peer ||
                  time - lastWorld < 2000000);
  if (client && hasAuthoritative && authoritative.round == n.match.round) {
    // Keep the local predicted archer responsive; all other objects are from
    // the server's authoritative simulation, including platforms via its tick.
    auto predicted = n.match.world.archers;
    auto mine = predicted[std::clamp(id, 0, 3)];
    n.match.world = authoritative;
    if (options.mode == Mode::Hybrid)
      for (int i = 0; i < options.players; ++i) {
        auto &p = n.match.world.archers[i];
        const auto &pose = poses[i];
        p.x = pose.round == n.match.round ? pose.x : predicted[i].x;
        p.y = pose.round == n.match.round ? pose.y : predicted[i].y;
        p.vx = predicted[i].vx;
        p.vy = predicted[i].vy;
      }
    if (!options.spectator && n.match.phase == Phase::Playing)
      n.match.world.archers[id] = mine;
  }
  if (!n.connected && opened && !failed)
    n.message = "Server disconnected. Close and rejoin when it is available.";
  // A historical, common-tick checksum is comparable despite different render
  // times.
  if (n.match.round == 0 && n.match.world.tick >= 100) {
    int count = 0;
    for (const auto &r : journal.Records())
      if ((r.type == RecordType::Command || r.type == RecordType::Join ||
           r.type == RecordType::Leave) &&
          r.round == 0 && r.tick <= 100)
        ++count;
    if (count != probeCommands) {
      probeHash = WorldHash(Match::Replay(journal, options.players,
                                          Served(options.mode), 0, 100));
      probeCommands = count;
    }
    n.probeHash = probeHash;
  }
  std::lock_guard<std::mutex> guard(engine.GetWorldMutex());
  output = std::move(n);
}
void Connection::Shutdown() {
  client.reset();
  peer.reset();
}
int Serve(engine::Engine &engine, const Options &o) {
  if (!engine.InitHeadless())
    return 1;
  // The server owns the roster and the results: archers join and leave with
  // their connections, up to four at once, and any further connection
  // watches. Clients only ever send commands for their own archer.
  Journal journal;
  Match match(MaxPlayers, true);
  std::mutex lock;
  int64_t epoch = WallMicros(), began = epoch, now = epoch, serial = 0;
  std::array<int, MaxClients> archerOf;
  archerOf.fill(-1);
  engine::Server::Config c;
  c.joinPort = o.port;
  c.worldPort = o.port + 1;
  c.clientPortBase = o.port + 10;
  c.maxClients = MaxClients;
  c.logTag = "towerfall";
  engine::Server server(engine, c);
  // The server has its own engine timeline. Client scales never change it.
  const int64_t origin =
      epoch - int64_t(engine.GetGameTime().GetSeconds() * 1000000);
  auto clock = [&] {
    return origin + int64_t(engine.GetGameTime().GetSeconds() * 1000000);
  };
  // --log: each client's state messages and commands this second, counted on
  // that client's own thread. Callers hold `lock`.
  std::array<int, MaxClients> logStates{}, logCommands{};
  int64_t logSince = epoch;
  auto trace = [&](const char *format, auto... args) {
    if (o.log)
      LogLine("server", format, args...);
  };
  // Records the server authors. Callers hold `lock`.
  auto write = [&](Record r) {
    now = clock();
    r.serial = ++serial;
    r.at = now;
    journal.Insert(r, MaxPlayers);
    match.Update(journal, now);
    trace("writes %s", DescribeServerRecord(r).c_str());
  };
  auto rosterChange = [&](int archer, bool joins) {
    now = clock();
    match.Update(journal, now);
    Record r;
    r.type = joins ? RecordType::Join : RecordType::Leave;
    r.author = archer;
    std::tie(r.round, r.tick) = match.Stamp(now);
    write(r);
  };
  engine::Server::Hooks h;
  h.onRequest = [&](int slot, const std::string &request) {
    std::lock_guard<std::mutex> guard(lock);
    if (request == "SYNC PLAY" || request == "SYNC WATCH") {
      int &archer = archerOf[slot];
      if (request == "SYNC PLAY")
        for (int a = 0; a < MaxPlayers && archer < 0; ++a)
          if (std::find(archerOf.begin(), archerOf.end(), a) ==
              archerOf.end()) {
            archer = a;
            rosterChange(a, true);
          }
      trace("slot %d: %s -> %s", slot, request.c_str(),
          archer >= 0 ? ("archer P" + std::to_string(archer + 1)).c_str()
                      : "watching");
      return "ARCHER " + std::to_string(archer) + "\n" +
             Packet(journal, match.View(), MaxPlayers, o.hybridServer, now,
                    epoch);
    }
    if (request.rfind("STATE ", 0) == 0) {
      // A client's state message, at its own game-time rate: every command
      // it has made since the last one, only ever for its own archer. A
      // command resent after a lost reply is already here and is skipped.
      std::istringstream in(request);
      std::string line;
      int count = -1;
      std::getline(in, line);
      if (std::sscanf(line.c_str(), "STATE %d", &count) != 1 || count < 0)
        return std::string("REJECT");
      std::vector<Record> records;
      while (std::getline(in, line)) {
        Record r;
        if (!Decode(line, r) || r.type != RecordType::Command ||
            archerOf[slot] < 0 || r.author != archerOf[slot])
          return std::string("REJECT");
        records.push_back(r);
      }
      if (int(records.size()) != count)
        return std::string("REJECT");
      for (const auto &r : records)
        journal.Insert(r, MaxPlayers);
      ++logStates[slot];
      logCommands[slot] += count;
    }
    return std::string("OK");
  };
  h.onLeave = [&](int slot) {
    std::lock_guard<std::mutex> guard(lock);
    if (archerOf[slot] >= 0) {
      rosterChange(archerOf[slot], false);
      archerOf[slot] = -1;
    }
  };
  h.buildWorld = [&] {
    std::lock_guard<std::mutex> guard(lock);
    return Packet(journal, match.View(), MaxPlayers, o.hybridServer, now,
                  epoch);
  };
  h.step = [&](float) {
    std::lock_guard<std::mutex> guard(lock);
    now = clock();
    match.Update(journal, now);
    Record commit;
    if (match.CommitDue(now, commit))
      write(commit);
    if (o.log && WallMicros() - logSince >= 1000000) {
      std::string slots;
      for (int k = 0; k < MaxClients; ++k)
        if (archerOf[k] >= 0 || logStates[k])
          slots += " | slot " + std::to_string(k) +
                   (archerOf[k] >= 0 ? " P" + std::to_string(archerOf[k] + 1)
                                     : std::string(" no archer")) +
                   ": " + std::to_string(logStates[k]) + " msgs, " +
                   std::to_string(logCommands[k]) + " cmds";
      trace("1s from clients%s | journal %zu | tick %d", slots.c_str(),
          journal.Size(), match.View().world.tick);
      logStates.fill(0);
      logCommands.fill(0);
      logSince = WallMicros();
    }
    if (o.seconds > 0 && WallMicros() - began > o.seconds * 1000000)
      server.Stop();
  };
  bool ok = server.Run(h);
  engine.Shutdown();
  return ok ? 0 : 1;
}
} // namespace towerfall
