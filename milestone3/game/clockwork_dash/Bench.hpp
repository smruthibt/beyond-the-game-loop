#pragma once

#include "Game.hpp"

namespace clockwork {

// Headless endpoints for the Part 1C experiments (Bench.cpp, run as clockwork_dash bench-server/bench-client).
struct BenchSettings {
    int port = DEFAULT_PORT;
    int clients = 2;              // the server waits for this many before timing
    int moving = 0;               // moving platforms added to the course
    int statics = 0;              // static ledges added to the course
    long long iterations = 10000; // timed server iterations
    long long warmup = 500;       // untimed ones first: longer than a 3 s countdown at 120 Hz
};

int BenchServer(const BenchSettings &settings, const engine::NetOptions &options);
int BenchClient(const BenchSettings &settings, const engine::NetOptions &options);

} // namespace clockwork
