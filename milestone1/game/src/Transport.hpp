#pragma once
#include "Match.hpp"
#include "engine/Client.hpp"
#include "engine/Engine.hpp"
#include "engine/Peer.hpp"
#include <deque>
#include <string>

namespace towerfall {
enum class Mode { Solo, Client, Hybrid, Peer, Server };
// Modes with a server, which admits archers at any time and decides rounds.
inline bool Served(Mode m) { return m == Mode::Client || m == Mode::Hybrid; }
// Server connections: up to MaxPlayers archers, the rest spectators.
constexpr int MaxClients = 8;
struct Options {
  Mode mode = Mode::Solo;
  int players = 3, port = 19400, id = -1;
  bool headless = false, bot = false, passive = false, spectator = false,
       hybridServer = false, demo = false, telemetry = false, log = false;
  double seconds = 0, scale = 1, pauseAt = -1, captureAt = 6;
  std::string report, capture;
};
struct NetworkFrame {
  MatchView match;
  int id = -1, tx = 0, rx = 0, records = 0, peers = 0;
  uint64_t workerPasses = 0, probeHash = 0;
  // With a server: commands delivered inside scheduled state messages, and
  // the unscheduled sends a pause makes (one per pause).
  int batched = 0, flushes = 0;
  // `now` is the match clock the worker last used, so the HUD counts down
  // on the same clock as the match rather than the wall clock.
  int64_t epoch = 0, now = 0;
  std::array<bool, MaxPlayers> present{};
  // Watching rather than playing: `id` is then -1 with a server.
  bool connected = false, spectating = false;
  std::string message;
};
const char *ModeLabel(Mode);
std::string WriteWorld(const World &);
bool ReadWorld(const std::string &, World &);

class Connection {
public:
  Connection(engine::Engine &, Options);
  void Update(float personalDelta);
  NetworkFrame Read();
  void Controls(Input);
  void Ready();
  void RestartPractice();
  void Shutdown();

private:
  void Open();
  void SendRecord(Record);
  void ReceiveWorld(const std::string &);
  void OnPeer(const std::string &, std::istringstream &);
  void PublishView();
  void ProduceCommands(int64_t now);
  // The scheduled state message: every command made since the last one.
  std::string StateMessage() const;
  // The server answered a state message that carried `count` commands.
  void Delivered(const std::string &reply, size_t count);
  // --log: one terminal line per protocol record or phase change, and a
  // traffic summary each second. Worker thread only.
  template <typename... Args> void Log(const char *format, Args... args) const;
  std::string LogName() const;
  // "P2" for an archer's peer id, otherwise a spectator's.
  std::string Slot(int peerId) const;
  void LogTraffic();
  engine::Engine &engine;
  Options options;
  Journal journal;
  Match match;
  // Commands already in this process's match, waiting for the next state
  // message to the server.
  std::vector<Record> outbox;
  int batched = 0, flushes = 0;
  std::unique_ptr<engine::Client> client;
  std::unique_ptr<engine::Peer> peer;
  Input requested;
  bool readyRequested = false, opened = false, failed = false;
  bool restartRequested = false;
  NetworkFrame output;
  World authoritative;
  bool hasAuthoritative = false;
  std::array<Input, MaxPlayers> previous{};
  std::array<int, MaxPlayers> commandNumber{};
  std::array<int64_t, MaxPlayers> heard{};
  struct Pose {
    int round = -1, tick = -1;
    float x = 0, y = 0;
  };
  std::array<Pose, MaxPlayers> poses{};
  int commandRound = -1, id = -1, sent = 0, rate = 0, received = 0;
  int64_t serial = 0, now = 0, epoch = 0, lastWorld = 0, lastHello = 0,
          rateSince = 0;
  // Solo only: maps engine game time onto the match clock (see Update).
  int64_t soloOrigin = 0;
  uint64_t passes = 0;
  uint64_t probeHash = 0;
  int probeCommands = -1;
  float sendCredit = 0;
  std::string problem;
  // --log state: records already described, and this second's counts.
  int64_t logSince = WallMicros(), logEpoch = 0;
  size_t logged = 0;
  int logStates = 0, logStateCommands = 0, logPoses = 0, logWorlds = 0,
      logRollbacks = 0, logRound = -1, logAttempt = 0;
  std::array<int, MaxPlayers> logCommands{}, logPosesHeard{};
  Phase logPhase = Phase::Lobby;
  bool logPaused = false;
};
int Serve(engine::Engine &, const Options &);
} // namespace towerfall
