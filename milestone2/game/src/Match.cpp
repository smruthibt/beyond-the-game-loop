#include "Match.hpp"
#include <algorithm>
#include <limits>
#include <map>
#include <sstream>
#include <tuple>

namespace towerfall {
int64_t WallMicros() { return engine::Timeline::RealTimeNow(); }
const char *PhaseLabel(Phase p) {
  switch (p) {
  case Phase::Lobby:
    return "GATHER YOUR ARCHERS";
  case Phase::Countdown:
    return "DRAW YOUR BOW";
  case Phase::Playing:
    return "LAST ARCHER STANDING";
  case Phase::Confirming:
    return "CONFIRMING ROUND";
  case Phase::Results:
    return "ROUND COMPLETE";
  default:
    return "A CHAMPION RISES";
  }
}
std::string Encode(const Record &r) {
  std::ostringstream o;
  o << "TF1 " << int(r.type) << ' ' << r.author << ' ' << r.round << ' '
    << r.attempt << ' ' << r.serial << ' ' << r.at << ' ' << r.tick << ' '
    << r.number << ' ' << r.input.buttons << ' ' << r.input.aim << ' '
    << r.input.speed << ' ' << r.input.paused << ' ' << r.hash;
  return o.str();
}
bool Decode(const std::string &s, Record &r) {
  if (s.size() > 512)
    return false;
  std::istringstream in(s);
  std::string tag, extra;
  int type;
  if (!(in >> tag >> type >> r.author >> r.round >> r.attempt >> r.serial >>
        r.at >> r.tick >> r.number >> r.input.buttons >> r.input.aim >>
        r.input.speed >> r.input.paused >> r.hash) ||
      tag != "TF1" || type < 0 || type > 7 || (in >> extra))
    return false;
  r.type = RecordType(type);
  return true;
}
bool Journal::Insert(const Record &r, int count) {
  if (r.author < 0 || r.author >= count || r.round < 0 || r.round > 9999 ||
      r.attempt < 0 || r.attempt > 127 || r.serial < 1 || r.at < 1 ||
      r.tick < 0 || r.number < 0 || r.number > 5000 ||
      records.size() >= 50000)
    return false;
  if (int(r.type) < 0 || int(r.type) > 7 || r.input.buttons < 0 ||
      r.input.buttons > 63 || r.input.aim < 0 || r.input.aim > 7 ||
      (r.input.speed != 1 && r.input.speed != 2 && r.input.speed != 4))
    return false;
  if (r.type == RecordType::Command && (r.number == 0 || r.tick == 0))
    return false;
  if (!identities.insert({r.author, r.serial}).second)
    return false;
  records.push_back(r);
  return true;
}
bool Match::Has(const Journal &j, RecordType type, int author, int round,
                int attempt) {
  for (const auto &r : j.Records())
    if (r.type == type && r.author == author && r.round == round &&
        r.attempt == attempt)
      return true;
  return false;
}
void Match::Update(const Journal &journal, int64_t now) {
  if (served) {
    UpdateServed(journal, now);
    return;
  }
  auto ordered = journal.Records();
  std::sort(
      ordered.begin(), ordered.end(), [](const Record &a, const Record &b) {
        return std::tie(a.author, a.serial) < std::tie(b.author, b.serial);
      });
  if (!simulation) {
    view.ready = 0;
    int64_t latest = 0;
    for (int i = 0; i < count; ++i)
      for (const auto &r : ordered)
        if (r.type == RecordType::Ready && r.author == i && r.number == count) {
          ++view.ready;
          latest = std::max(latest, r.at);
          break;
        }
    view.world = NewRound(0, count);
    if (view.ready != count)
      return;
    view.start = latest + 1800000;
    simulation = std::make_unique<Simulation>(0, count);
  }
  // Reconstruct old committed rounds too: a late observer needs no privileged
  // host.
  for (int advance = 0; advance < 128; ++advance) {
    for (const auto &r : ordered)
      if (r.type == RecordType::Command && r.round == view.round &&
          loaded.insert({r.author, r.number}).second)
        simulation->Add({r.author, r.number, r.tick, r.input});
    int target = int(std::max<int64_t>(0, (now - view.start) / 10000));
    bool closing = false;
    std::array<const Record *, MaxPlayers> seals{};
    for (const auto &r : ordered)
      if (r.round == view.round && r.attempt == view.attempt) {
        if (r.type == RecordType::Close)
          closing = true;
        if (r.type == RecordType::Seal && !seals[r.author])
          seals[r.author] = &r;
      }
    view.seals = 0;
    view.votes = 0;
    view.prefixComplete = false;
    view.freezeTick = 0;
    view.resultHash = 0;
    if (closing) {
      for (int i = 0; i < count; ++i)
        if (seals[i]) {
          ++view.seals;
          view.freezeTick = std::max(view.freezeTick, seals[i]->tick);
        }
      if (view.seals == count) {
        bool complete = true;
        for (int i = 0; i < count; ++i) {
          std::set<int> numbers;
          for (const auto &r : ordered)
            if (r.type == RecordType::Command && r.round == view.round &&
                r.author == i && r.number <= seals[i]->number)
              numbers.insert(r.number);
          if (int(numbers.size()) != seals[i]->number)
            complete = false;
          // A valid producer cannot create commands beyond its sealed prefix
          // until this closure attempt is agreed to be a false alarm.
          for (const auto &r : ordered)
            if (r.type == RecordType::Command && r.round == view.round &&
                r.author == i && r.number <= seals[i]->number &&
                r.tick > view.freezeTick)
              complete = false;
        }
        view.prefixComplete = complete;
        if (complete) {
          target = view.freezeTick;
        }
      }
    }
    simulation->Seek(target);
    view.world = simulation->State();
    view.rollbacks = simulation->Rollbacks();
    view.phase = now < view.start ? Phase::Countdown : Phase::Playing;
    if (closing)
      view.phase = Phase::Confirming;
    if (view.prefixComplete) {
      view.resultHash = WorldHash(view.world);
      int64_t latestVote = 0;
      for (int i = 0; i < count; ++i)
        for (const auto &r : ordered)
          if (r.type == RecordType::Vote && r.round == view.round &&
              r.attempt == view.attempt && r.author == i &&
              r.tick == view.freezeTick && r.hash == view.resultHash) {
            ++view.votes;
            latestVote = std::max(latestVote, r.at);
            break;
          }
      if (view.votes == count) {
        if (view.world.result == NoResult) {
          ++view.attempt;
          continue;
        } // replay disproved an early prediction
        // One round decides the game: whoever wins it wins, and a round with
        // no winner ends the game as a draw.
        view.phase = Phase::Complete;
        view.transition = latestVote;
        if (!awarded) {
          if (view.world.result >= 0)
            ++view.scores[view.world.result];
          awarded = true;
          ++view.commits;
        }
        return;
      }
    }
    return;
  }
}
std::vector<Record> Match::ActionsFor(int author, const Journal &journal,
                                      int64_t now) const {
  std::vector<Record> out;
  if (author < 0 || author >= count)
    return out;
  Record base;
  base.author = author;
  base.round = view.round;
  base.attempt = view.attempt;
  base.at = now;
  if (view.phase == Phase::Playing && view.world.result != NoResult &&
      !Has(journal, RecordType::Close, author, view.round, view.attempt)) {
    auto r = base;
    r.type = RecordType::Close;
    r.tick = view.world.tick;
    out.push_back(r);
  }
  if (view.phase == Phase::Confirming &&
      !Has(journal, RecordType::Seal, author, view.round, view.attempt)) {
    auto r = base;
    r.type = RecordType::Seal;
    r.tick = std::max(0, int((now - view.start) / 10000));
    for (const auto &c : journal.Records())
      if (c.type == RecordType::Command && c.round == view.round &&
          c.author == author) {
        r.number = std::max(r.number, c.number);
        r.tick = std::max(r.tick, c.tick);
      }
    out.push_back(r);
  }
  if (view.phase == Phase::Confirming && view.prefixComplete) {
    bool voted = false;
    for (const auto &r : journal.Records())
      if (r.type == RecordType::Vote && r.author == author &&
          r.round == view.round && r.attempt == view.attempt &&
          r.hash == view.resultHash && r.tick == view.freezeTick)
        voted = true;
    if (!voted) {
      auto r = base;
      r.type = RecordType::Vote;
      r.tick = view.freezeTick;
      r.hash = view.resultHash;
      out.push_back(r);
    }
  }
  return out;
}
namespace {
// The server's Join and Leave records, in the order it wrote them.
std::vector<const Record *> RosterRecords(const Journal &journal) {
  std::vector<const Record *> out;
  for (const auto &r : journal.Records())
    if (r.type == RecordType::Join || r.type == RecordType::Leave)
      out.push_back(&r);
  std::sort(out.begin(), out.end(), [](const Record *a, const Record *b) {
    return a->serial < b->serial;
  });
  return out;
}
int Apply(int mask, const Record &r) {
  return r.type == RecordType::Join ? mask | 1 << r.author
                                    : mask & ~(1 << r.author);
}
// The archers present as `round` starts: every change made before it, plus
// the ones made for its start.
int StartingRoster(const std::vector<const Record *> &roster, int round) {
  int mask = 0;
  for (auto *r : roster)
    if (r->round < round || (r->round == round && r->tick <= 1))
      mask = Apply(mask, *r);
  return mask;
}
} // namespace
void Match::UpdateServed(const Journal &journal, int64_t now) {
  const auto roster = RosterRecords(journal);
  std::map<int, const Record *> commits;
  for (const auto &r : journal.Records())
    if (r.type == RecordType::Commit) {
      auto &c = commits[r.round];
      if (!c || r.serial < c->serial)
        c = &r;
    }
  // Walk the decided rounds to the one in progress. Every time here comes
  // from a record, so all processes - and a late observer - agree on it.
  MatchView next;
  int64_t scheduled = std::numeric_limits<int64_t>::min();
  const Record *commit = nullptr;
  bool waiting = false;
  // Each round is a whole game; `round` numbers the games of this session.
  for (int round = 0;; ++round) {
    next.round = round;
    next.scores.fill(0);
    int carried = 0;
    for (auto *r : roster)
      if (r->round < round)
        carried = Apply(carried, *r);
    // A round that would start empty waits for an archer, then counts down
    // from the arrival that filled it.
    constexpr int64_t never = std::numeric_limits<int64_t>::min();
    int starting = carried;
    int64_t arrived = carried ? never : 0;
    for (auto *r : roster)
      if (r->round == round && r->tick <= 1) {
        const int before = starting;
        starting = Apply(starting, *r);
        if (!before && starting)
          arrived = r->at;
      }
    if (!starting) {
      waiting = true;
      break;
    }
    next.start =
        std::max(scheduled, arrived == never ? never : arrived + 1800000);
    auto found = commits.find(round);
    if (found == commits.end())
      break;
    commit = found->second;
    ++next.commits;
    const int winner = commit->number - 1;
    if (winner >= 0 && winner < count)
      next.scores[winner] = 1;
    // The winner - or the draw - is shown for five seconds, then a new game
    // begins with whoever is connected.
    next.transition = commit->at + 5000000;
    if (now < next.transition) {
      next.phase = Phase::Complete;
      break;
    }
    scheduled = next.transition + 1200000;
    commit = nullptr;
  }
  if (waiting) {
    next.phase = Phase::Lobby;
    next.world = NewRound(next.round, count, 0);
    view = next;
    simulation.reset();
    return;
  }
  const int round = next.round;
  const int starting = StartingRoster(roster, round);
  if (!simulation || simulationRound != round ||
      simulationRoster != starting) {
    simulation = std::make_unique<Simulation>(round, count, starting);
    simulationRound = round;
    simulationRoster = starting;
    loaded.clear();
    rosterLoaded.clear();
  }
  for (auto *r : roster)
    if (r->round == round && r->tick > 1 &&
        rosterLoaded.insert(r->serial).second)
      simulation->Add(RosterChange{r->author, r->tick,
                                   r->type == RecordType::Join, r->serial});
  for (const auto &r : journal.Records())
    if (r.type == RecordType::Command && r.round == round &&
        loaded.insert({r.author, r.number}).second)
      simulation->Add({r.author, r.number, r.tick, r.input});
  simulation->Seek(commit ? commit->tick
                          : int(std::max<int64_t>(
                                (now - next.start) / 10000, 0)));
  next.world = simulation->State();
  next.rollbacks = simulation->Rollbacks();
  for (int i = 0; i < count; ++i)
    next.ready += next.world.archers[i].present;
  if (commit) {
    // The server's decision stands, even where this replay differs.
    next.world.result = commit->number - 1;
    next.world.resultTick = commit->tick;
    next.resultHash = commit->hash;
  } else {
    next.phase = now < next.start                   ? Phase::Countdown
                 : next.world.result != NoResult ? Phase::Confirming
                                                 : Phase::Playing;
  }
  view = next;
}
std::pair<int, int> Match::Stamp(int64_t now) const {
  switch (view.phase) {
  case Phase::Lobby:
  case Phase::Countdown:
    return {view.round, 1};
  case Phase::Playing:
    return {view.round,
            int(std::max<int64_t>((now - view.start) / 10000 + 2, 2))};
  default:
    return {view.round + 1, 1};
  }
}
bool Match::CommitDue(int64_t now, Record &commit) const {
  // A short grace period lets any command stamped before the result reach
  // the server and be replayed before the decision is final.
  if (view.phase != Phase::Confirming ||
      now < view.start + int64_t(view.world.resultTick) * 10000 + 300000)
    return false;
  commit = Record{};
  commit.type = RecordType::Commit;
  commit.round = view.round;
  commit.tick = view.world.resultTick;
  commit.number = view.world.result + 1;
  commit.hash = WorldHash(view.world);
  return true;
}
World Match::Replay(const Journal &journal, int count, bool served, int round,
                    int tick) {
  std::unique_ptr<Simulation> replay;
  if (served) {
    const auto roster = RosterRecords(journal);
    replay = std::make_unique<Simulation>(round, count,
                                          StartingRoster(roster, round));
    for (auto *r : roster)
      if (r->round == round && r->tick > 1 && r->tick <= tick)
        replay->Add(RosterChange{r->author, r->tick,
                                 r->type == RecordType::Join, r->serial});
  } else {
    replay = std::make_unique<Simulation>(round, count);
  }
  for (const auto &r : journal.Records())
    if (r.type == RecordType::Command && r.round == round && r.tick <= tick)
      replay->Add({r.author, r.number, r.tick, r.input});
  replay->Seek(tick);
  return replay->State();
}
} // namespace towerfall
