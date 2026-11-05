// rerun_bench: every process of a benchmark run.
//
//   rerun_bench coordinator|server|client|peer|oracle [--flag value]...
//
// See tools/campaign.py, which launches and validates runs, and METHODOLOGY.md.
#include "Endpoint.hpp"

#include <cstdio>

int main(int argc, char **argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    rb::Options o;
    std::string error;
    if (!rb::ParseOptions(argc, argv, o, error)) {
        std::fprintf(stderr, "rerun_bench: %s\n", error.c_str());
        return 2;
    }
    if (o.role == "coordinator") return rb::RunCoordinator(o);
    if (o.role == "server") return rb::RunServer(o);
    if (o.role == "client") return rb::RunClient(o);
    if (o.role == "peer") return rb::RunPeer(o);
    return rb::RunOracle(o);
}
