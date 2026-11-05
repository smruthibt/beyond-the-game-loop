// The run coordinator: out of band, identical for every strategy, and never
// carrying gameplay state. It checks that every expected endpoint agrees on
// options, schema, input traces and level before warm-up, releases each phase,
// and timestamps the measurement: from the common MEASURE release to the last
// endpoint's DONE, which that endpoint sends only after it has applied the
// final update of every stream it consumes.
#include "Endpoint.hpp"

#include <cstdio>
#include <map>
#include <sstream>
#include <thread>

namespace rb {

using namespace engine;

int RunCoordinator(const Options &o) {
    std::vector<std::string> names;
    if (o.Cs()) {
        names.push_back("server");
        for (int i = 0; i < o.players; ++i) names.push_back("client" + std::to_string(i));
    } else {
        for (int i = 0; i < o.players; ++i) names.push_back("peer" + std::to_string(i));
    }

    net::Socket rep(net::SocketType::Reply);
    net::Socket pub(net::SocketType::Publish);
    rep.SetReceiveTimeoutMs(2);
    pub.SetSendQueue(10000);
    if (!rep.Bind(Bind(CoordinatorReqPort(o))) || !pub.Bind(Bind(CoordinatorPubPort(o)))) {
        std::printf("RESULT {\"run\":\"%s\",\"role\":\"coordinator\",\"status\":\"failed\",\"error\":\"cannot bind\"}\n",
                    JsonEscape(o.run).c_str());
        return 3;
    }

    struct State {
        std::string ready;
        bool warmed = false;
        int64_t doneNs = 0;
        bool bye = false;
    };
    std::map<std::string, State> state;
    for (const std::string &n : names) state[n];

    enum class Phase { Setup, Warmup, Measure, Finish } phase = Phase::Setup;
    const int64_t start = NowNs();
    int64_t phaseStart = start, lastBeacon = 0, warmRelease = 0, measureRelease = 0, finishRelease = 0;
    std::string failure;
    std::vector<std::string> log;

    auto allOf = [&](auto pred) {
        for (const std::string &n : names) if (!pred(state[n])) return false;
        return true;
    };
    auto release = [&](const char *p) {
        const int64_t now = NowNs();
        pub.Send(std::string("R_") + p + " " + std::to_string(now));
        return now;
    };

    std::string request;
    while (failure.empty()) {
        const int64_t now = NowNs();
        if (phase == Phase::Setup && now - lastBeacon > 5000000) {
            pub.Send("R_B");
            lastBeacon = now;
        }
        // Phase deadlines.
        const int64_t limit = phase == Phase::Setup ? (90LL + o.delayStartMs / 1000) * 1000000000LL
                              : phase == Phase::Warmup ? 300LL * 1000000000LL
                              : phase == Phase::Measure ? static_cast<int64_t>(o.timeoutS) * 1000000000LL
                                                        : 60LL * 1000000000LL;
        if (now - phaseStart > limit) {
            const char *names4[] = {"setup", "warm-up", "measure", "finish"};
            if (phase == Phase::Finish) break;   // results are already printed by then; a missing BYE is noted
            failure = std::string("timed out in ") + names4[static_cast<int>(phase)];
            break;
        }

        if (!rep.Receive(request)) continue;
        const int64_t received = NowNs();
        std::istringstream in(request);
        std::string verb, who;
        in >> verb >> who;
        auto it = state.find(who);
        if (it == state.end()) {
            rep.Send("ABORT unknown endpoint " + who);
            log.push_back("unexpected endpoint " + who);
            continue;
        }
        State &s = it->second;
        std::string rest;
        std::getline(in, rest);
        if (verb == "READY") {
            if (phase != Phase::Setup) {
                rep.Send("ABORT late READY");
                failure = who + " became ready after the warm-up release (late join is not allowed)";
                break;
            }
            s.ready = rest;
            rep.Send("OK " + std::to_string(NowNs()));
        } else if (verb == "WARMED") {
            s.warmed = true;
            rep.Send("OK " + std::to_string(NowNs()));
        } else if (verb == "DONE") {
            s.doneNs = received;
            rep.Send("OK " + std::to_string(NowNs()));
        } else if (verb == "BYE") {
            s.bye = true;
            rep.Send("OK " + std::to_string(NowNs()));
        } else if (verb == "FAIL") {
            rep.Send("OK " + std::to_string(NowNs()));
            failure = who + " failed:" + rest;
            break;
        } else {
            rep.Send("ABORT unknown request");
            continue;
        }

        if (phase == Phase::Setup && allOf([](const State &x) { return !x.ready.empty(); })) {
            // Every endpoint must agree on options, schema, traces and level,
            // and must have passed the initial-state check.
            const std::string &first = state[names.front()].ready;
            std::istringstream a(first);
            std::string optsHash, schema, trace, level;
            int initial = 0;
            a >> optsHash >> schema >> trace >> level >> initial;
            for (const std::string &n : names) {
                std::istringstream b(state[n].ready);
                std::string o2, s2, t2, l2;
                int i2 = 0;
                b >> o2 >> s2 >> t2 >> l2 >> i2;
                if (o2 != optsHash) failure = n + " has different options";
                else if (s2 != schema) failure = n + " has a different component schema";
                else if (t2 != trace) failure = n + " has different input traces";
                else if (l2 != level) failure = n + " has a different level";
                else if (i2 != 1) failure = n + " failed the initial-state check";
                if (!failure.empty()) break;
            }
            if (!failure.empty()) break;
            warmRelease = release("WARMUP");
            phase = Phase::Warmup;
            phaseStart = warmRelease;
        } else if (phase == Phase::Warmup && allOf([](const State &x) { return x.warmed; })) {
            measureRelease = release("MEASURE");
            phase = Phase::Measure;
            phaseStart = measureRelease;
        } else if (phase == Phase::Measure && allOf([](const State &x) { return x.doneNs > 0; })) {
            finishRelease = release("FINISH");
            phase = Phase::Finish;
            phaseStart = finishRelease;
        } else if (phase == Phase::Finish && allOf([](const State &x) { return x.bye; })) {
            break;
        }
    }

    if (!failure.empty()) {
        for (int i = 0; i < 5; ++i) {
            pub.Send("R_ABORT " + failure);
            // Keep answering, so blocked reports learn of the abort.
            const int64_t until = NowNs() + 20000000;
            while (NowNs() < until) {
                if (rep.Receive(request)) rep.Send("ABORT " + failure);
            }
        }
    }

    int64_t lastDone = 0;
    std::string done = "{";
    for (const std::string &n : names) {
        lastDone = std::max(lastDone, state[n].doneNs);
        done += (done.size() > 1 ? ",\"" : "\"") + n + "\":" + std::to_string(state[n].doneNs > 0 ? state[n].doneNs - measureRelease : -1);
    }
    done += "}";
    std::string byes = "[";
    for (const std::string &n : names) {
        if (!state[n].bye) byes += (byes.size() > 1 ? ",\"" : "\"") + n + "\"";
    }
    byes += "]";
    Json j;
    j.Str("run", o.run)
        .Str("campaign", o.campaign)
        .Str("role", "coordinator")
        .Str("name", "coordinator")
        .Str("options", o.Describe())
        .Str("status", failure.empty() ? "ok" : "failed")
        .Str("error", failure)
        .Int("endpoints", static_cast<long long>(names.size()))
        .Int("setup_ns", warmRelease > 0 ? warmRelease - start : 0)
        .Int("warmup_ns", measureRelease > 0 ? measureRelease - warmRelease : 0)
        .Int("measure_release", measureRelease)
        .Int("end_to_end_ns", failure.empty() && lastDone > 0 ? lastDone - measureRelease : 0)
        .Raw("done_after_release_ns", done)
        .Raw("missing_bye", byes);
    std::printf("RESULT %s\n", j.Text().c_str());
    std::fflush(stdout);
    return failure.empty() ? 0 : 3;
}

} // namespace rb
