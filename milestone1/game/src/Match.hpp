#pragma once
#include "Simulation.hpp"
#include <set>
#include <string>

namespace towerfall {
// Join, Leave and Commit are written only by a server (client/server and
// hybrid). Join and Leave name the archer in `author` and take effect at
// `round`/`tick`. A Commit is the server's decision for `round`: the result
// frozen at `tick`, the winner as `number` - 1 (0 is a draw), and that
// world's hash.
enum class RecordType { Ready, Command, Close, Seal, Vote, Join, Leave, Commit };
struct Record {
  RecordType type = RecordType::Ready;
  int author = 0, round = 0, attempt = 0;
  int64_t serial = 0, at = 0;
  int tick = 0, number = 0;
  Input input;
  uint64_t hash = 0;
};
std::string Encode(const Record &);
bool Decode(const std::string &, Record &);
class Journal {
public:
  bool Insert(const Record &, int players);
  const std::vector<Record> &Records() const { return records; }
  size_t Size() const { return records.size(); }

private:
  std::vector<Record> records;
  std::set<std::pair<int, int64_t>> identities;
};
enum class Phase { Lobby, Countdown, Playing, Confirming, Results, Complete };
struct MatchView {
  Phase phase = Phase::Lobby;
  World world;
  std::array<int, MaxPlayers> scores{};
  int ready = 0, round = 0, attempt = 0, seals = 0, votes = 0, freezeTick = 0;
  int rollbacks = 0, commits = 0;
  int64_t start = 0, transition = 0;
  uint64_t resultHash = 0;
  bool prefixComplete = false;
};
class Match {
public:
  // `served`: a server decides who is playing and each round's result, so
  // archers can join and leave at any time. Otherwise the roster is fixed at
  // `players` and results are agreed by seals and votes.
  explicit Match(int players, bool served = false)
      : count(players), served(served) {}
  void Update(const Journal &, int64_t now);
  const MatchView &View() const { return view; }
  // Served: where a roster change made at `now` takes effect, as
  // {round, tick}. Before a round begins it is part of that round's starting
  // line-up; during play it lands just ahead of the present; once the round
  // is decided it waits for the next one.
  std::pair<int, int> Stamp(int64_t now) const;
  // Served: the Commit the server should write now, if the predicted result
  // has stood long enough for late commands to have arrived.
  bool CommitDue(int64_t now, Record &commit) const;
  // Rebuilds one round from the journal and runs it to `tick` - the same
  // replay the match itself uses, for comparing history across processes.
  static World Replay(const Journal &, int count, bool served, int round,
                      int tick);
  // These records are generated only on the owning networking thread.
  std::vector<Record> ActionsFor(int author, const Journal &,
                                 int64_t now) const;
  static bool Has(const Journal &, RecordType, int author, int round,
                  int attempt);

private:
  void UpdateServed(const Journal &, int64_t now);
  int count;
  bool served;
  MatchView view;
  int simulationRound = -1, simulationRoster = -1;
  std::set<int64_t> rosterLoaded;
  std::unique_ptr<Simulation> simulation;
  std::set<std::pair<int, int>> loaded;
  bool awarded = false;
};
int64_t WallMicros();
const char *PhaseLabel(Phase);
} // namespace towerfall
