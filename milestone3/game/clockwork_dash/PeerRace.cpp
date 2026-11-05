#include "PeerRace.hpp"

#include <algorithm>
#include <sstream>

namespace clockwork {
using namespace engine;
namespace {
ScenePeer::Config Config(int port) {
    ScenePeer::Config config;
    config.peer.portBase = port + 200;
    config.peer.maxPeers = 4;
    config.peer.gossipMs = 100;
    return config;
}
NetOptions PeerOptions(NetOptions options) {
    options.topology = Topology::PeerToPeer;
    options.upstream = Upstream::State;
    return options;
}
}

PeerCourse::PeerCourse(World &w, int id, std::function<void(const std::string &)> emit)
    : world(w), owner(id), append(std::move(emit)) {}

CourseEvents PeerCourse::Hooks() {
    CourseEvents hooks;
    hooks.bump = [this](GameObject &ledge) {
        append("bump " + std::to_string(ledge.Id()) + " " + std::to_string(Timeline::RealTimeNow()));
    };
    hooks.drink = [this](GameObject &ledge, GameObject &player) {
        if (!requested.insert(ledge.Id()).second) return;
        const auto &session = *player.Get<PeerSession>();
        std::ostringstream out;
        out << "take " << ledge.Id() << ' ' << session.session << ' ' << session.run << ' '
            << player.Get<Respawn>()->deaths << ' ' << Timeline::RealTimeNow();
        append(out.str());
    };
    hooks.stomp = [this](GameObject &, GameObject &victim) {
        const auto *session = victim.Get<PeerSession>();
        if (!session || !session->boostToken || victim.Get<Boost>()->remaining <= 0) return;
        std::ostringstream out;
        out << "stomp " << OwnerOf(victim) << ' ' << session->session << ' ' << session->run << ' '
            << session->boostToken;
        append(out.str());
    };
    return hooks;
}

void PeerCourse::Reconcile(const std::vector<Peer::Event> &events, int64_t now) {
    std::map<ObjectId, Claim> claims;
    for (const auto &event : events) {
        std::istringstream in(event.payload);
        std::string kind;
        ObjectId id = NO_OBJECT;
        in >> kind;
        if (kind == "bump") {
            int64_t time = 0;
            if (!(in >> id >> time)) continue;
            auto *ledge = world.Find(id);
            if (!ledge || !ledge->Has<Impact>()) continue;
            if (auto *potion = ledge->Get<PotionPlatform>()) potion->opened = true;
            if (seenBumps.emplace(event.owner, event.seq).second) {
                const float age = std::max(0.0, (now - time) / 1000000.0);
                auto &amount = ledge->Get<Impact>()->amount;
                amount = std::max(amount, std::max(0.0f, 1 - 4 * age));
            }
        } else if (kind == "take") {
            Claim claim; claim.owner = event.owner; claim.seq = event.seq;
            if (!(in >> id >> claim.session >> claim.run >> claim.deaths >> claim.time)) continue;
            auto *ledge = world.Find(id);
            if (!ledge || !ledge->Has<PotionPlatform>()) continue;
            const auto found = claims.find(id);
            if (found == claims.end() || claim.Order() < found->second.Order()) claims[id] = claim;
        }
    }

    auto *me = Player(world, owner);
    auto *session = me ? me->Get<PeerSession>() : nullptr;
    // Correct a speculative award if an earlier competing claim arrives later.
    // Ordering uses the machine's monotonic clock, then owner and event sequence.
    for (const auto &[id, old] : winners) {
        const auto next = claims.find(id);
        if (session && old.owner == owner && next != claims.end() && old.Order() != next->second.Order()
            && session->boostToken == old.seq) RemoveBoost(*me);
    }
    std::vector<Claim> localAwards;
    for (const auto &[id, claim] : claims) {
        auto &potion = *world.Find(id)->Get<PotionPlatform>();
        potion.opened = potion.taken = true;
        if (claim.owner == owner && awards.emplace(claim.owner, claim.seq).second) localAwards.push_back(claim);
    }
    std::sort(localAwards.begin(), localAwards.end(), [](const Claim &a, const Claim &b) { return a.Order() < b.Order(); });
    for (const auto &claim : localAwards) {
        if (!session || session->session != claim.session || session->run != claim.run
            || me->Get<Respawn>()->deaths != claim.deaths || me->Get<Racer>()->finished
            || (session->boostToken && session->boostToken > claim.seq)) continue;
        const float left = BOOST_SECONDS - std::max(0.0, (now - claim.time) / 1000000.0);
        if (left > 0) { GiveBoost(*me, left); session->boostToken = claim.seq; }
    }
    winners = std::move(claims);

    // Revisit stomp events after awards, including ones delivered before the
    // corresponding potion claim. Tokens prevent a stale stomp ending a new boost.
    for (const auto &event : events) {
        std::istringstream in(event.payload);
        std::string kind;
        int target = -1, run = 0;
        int64_t process = 0, token = 0;
        if (!(in >> kind) || kind != "stomp" || !(in >> target >> process >> run >> token)) continue;
        if (session && target == owner && process == session->session && run == session->run
            && token != 0 && token == session->boostToken) {
            RemoveBoost(*me);
            me->Get<Boost>()->stomped = 0.6f;
        }
    }
}

PeerRace::PeerRace(int port, const NetOptions &options) : scene(Config(port), PeerOptions(options)) {
    BuildCourse(scene.GetWorld());
}

bool PeerRace::Join(int requested) {
    if (scene.Claim(requested) < 0) return false;
    scene.GetPeer().EnableSharedClock();
    AddPlayer(scene.GetWorld(), Id()).Add<PeerSession>().session = scene.GetPeer().FirstSeq();
    course = std::make_unique<PeerCourse>(scene.GetWorld(), Id(), [this](const std::string &event) {
        scene.GetPeer().AppendEvent(event);
    });
    hooks = course->Hooks();
    return true;
}

void PeerRace::Sync() {
    if (scene.GetPeer().TakeEventsDirty()) course->Reconcile(scene.GetPeer().Events(), Timeline::RealTimeNow());
}

void PeerRace::Step(float dt) {
    Sync();
    // Personal timers advance by dt; shared movers and hazards use the mesh clock.
    double seconds = Seconds() - dt;
    clockwork::Step(scene.GetWorld(), dt, seconds, OwnedBy(Id()), hooks);
    Sync();
    scene.StampLocal();
}
} // namespace clockwork
