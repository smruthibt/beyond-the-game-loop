#include "Bench.hpp"
#include "Game.hpp"
#include "PeerRace.hpp"
#include "engine/Engine.hpp"
#include "engine/Input.hpp"

#include <SDL3/SDL_main.h>
#include <SDL3_image/SDL_image.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace engine;
namespace {
const char *Value(int argc,char **argv,const char *flag,const char *fallback) {
    for(int i=1;i+1<argc;++i) if(std::strcmp(argv[i],flag)==0) return argv[i+1];
    return fallback;
}
bool Flag(int argc,char **argv,const char *flag) {
    for(int i=1;i<argc;++i) if(std::strcmp(argv[i],flag)==0) return true;
    return false;
}
int Number(int argc,char **argv,const char *flag,int fallback) {
    const auto s=Value(argc,argv,flag,nullptr); return s?std::atoi(s):fallback;
}
void Usage() {
    std::puts("Clockwork Dash\n"
              "  clockwork_dash [practice]       play a solo practice run\n"
              "  clockwork_dash server           host the shared race (headless)\n"
              "  clockwork_dash client           join a race on this computer\n"
              "  clockwork_dash peer             play a serverless P2P race (up to four peers)\n"
              "  --id N                          peer slot 0..3; omitted = first free slot\n"
              "  clockwork_dash bench-server     timed, uncapped race server (Part 1C benchmark)\n"
              "  clockwork_dash bench-client     scripted runner with no window, for bench-server\n"
              "  --clients C --moving M --static S --iterations N --warmup W   benchmark settings\n"
              "  --port-base N                   default 8650\n"
              "  --encoding text|binary          default binary\n"
              "  --delta on|off                  default off\n"
              "  --width W --height H --x X --y Y window layout\n"
              "  --seconds N --screenshot PATH   bounded run / PNG capture\n"
              "Controls: A/D or arrows to run, Space/W/Up to jump, R for a new run, Esc to exit.\n"
              "Multiplayer uses local 0MQ: CS sends inputs to a server; P2P peers exchange owned state.");
}

int ServerMain(int port,const NetOptions &options,int seconds) {
    Engine app;
    if(!app.InitHeadless()) return 1;
    SceneServer scene(app,clockwork::ServerConfig(port),options);
    clockwork::BuildCourse(scene.GetWorld());
    double elapsed=0;
    SceneServer::Callbacks callbacks;
    callbacks.onJoin=[](World &world,int id) { clockwork::AddPlayer(world,id); };
    callbacks.step=[&](World &world,float dt) {
        clockwork::Step(world,dt,elapsed);
        if(seconds>0&&elapsed>=seconds) scene.Stop();
    };
    std::printf("Clockwork Dash server / port %d / %s\n",port,options.Describe().c_str());
    const bool ok=scene.Run(callbacks);
    app.Shutdown();
    return ok?0:1;
}

int Play(int argc,char **argv,int port,const NetOptions &options,bool practice,bool p2p) {
    Engine app;
    SceneClient net(clockwork::ClientConfig(port),options);
    std::unique_ptr<clockwork::PeerRace> peer;
    if(p2p) {
        peer=std::make_unique<clockwork::PeerRace>(port,options);
        const int requested=Number(argc,argv,"--id",-1);
        if(requested < -1 || requested > 3) { std::fprintf(stderr,"Choose a peer id from 0 to 3.\n"); return 2; }
        if(!peer->Join(requested)) {
            std::fprintf(stderr,"Could not start Clockwork Dash peer: %s\n",peer->LastError().c_str());
            return 1;
        }
        std::printf("Clockwork Dash peer %d / port base %d / %s\n",peer->Id(),port,options.Describe().c_str());
    }
    if(!practice&&!p2p&&!net.Join()) {
        std::fprintf(stderr,"Could not join Clockwork Dash: %s\nStart ./build/clockwork_dash server first.\n",net.LastError().c_str());
        return 1;
    }
    const int owner=practice?0:p2p?peer->Id():net.Id();
    const std::string title="Clockwork Dash | "+(practice?std::string("Practice"):(p2p?"P2P Player ":"CS Player ")+std::to_string(owner+1));
    if(!app.Init(title.c_str(),Number(argc,argv,"--width",1280),Number(argc,argv,"--height",720))) return 1;
    if(Flag(argc,argv,"--x")) SDL_SetWindowPosition(app.GetWindow(),Number(argc,argv,"--x",0),Number(argc,argv,"--y",0));
    SDL_SetRenderLogicalPresentation(app.GetRenderer(),1600,900,SDL_LOGICAL_PRESENTATION_LETTERBOX);
    app.SetTargetFrameRate(120);
    World solo;
    World &world=practice?solo:p2p?peer->GetWorld():net.GetWorld();
    if(practice) { clockwork::BuildCourse(world); clockwork::AddPlayer(world,owner); }
    auto &mutex=app.GetWorldMutex();
    auto &cameraObject=world.Create("camera");
    const ObjectId cameraId=cameraObject.Id();
    auto &camera=cameraObject.Add<Camera>();
    camera.view={1600,900}; camera.boundsMin={0,0}; camera.boundsMax={clockwork::LEVEL_WIDTH,clockwork::LEVEL_HEIGHT};
    systems::AddScrollBoundaries(world,cameraId,560);
    double seconds=0;
    float respawnFlash=0;
    int lastDeaths=0;
    bool shotTaken=false,shotFailed=false;
    const char *screenshot=Value(argc,argv,"--screenshot",nullptr);
    const int stopAfter=Number(argc,argv,"--seconds",0);
    const auto start=std::chrono::steady_clock::now();
    auto wallSeconds=[&]() { return std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count(); };
    app.Run([&](float dt) {
        if(Input::IsKeyJustPressed(SDL_SCANCODE_ESCAPE)) app.Stop();
        std::lock_guard<std::mutex> lock(mutex);
        if(auto *me=clockwork::Player(world,owner)) {
            auto &input=*me->Get<InputState>();
            systems::ReadKeyboard(input);
            if(Input::IsKeyPressed(SDL_SCANCODE_R)) input.buttons|=clockwork::RESTART;
            if(!practice&&!p2p) net.StampLocal();
        }
        if(practice) clockwork::Step(world,dt,seconds);
        else if(p2p) { peer->Step(dt); seconds=peer->Seconds(); }
        else seconds+=dt;
        if(auto *me=clockwork::Player(world,owner)) {
            const int deaths=me->Get<Respawn>()->deaths;
            if(deaths>lastDeaths) respawnFlash=1;
            lastDeaths=deaths;
            auto &cam=*world.Find(cameraId)->Get<Camera>();
            cam.target=me->Id(); systems::UpdateCameras(world);
        }
        respawnFlash=std::max(0.0f,respawnFlash-dt);
        if(stopAfter>0&&wallSeconds()>=stopAfter) app.Stop();
    },[&]() {
        std::lock_guard<std::mutex> lock(mutex);
        const auto &cam=*world.Find(cameraId)->Get<Camera>();
        clockwork::Draw(world,app.GetRenderer(),cam,owner,practice||(p2p?peer->PeersHeard()>0:net.GetClient().Connected()),practice,seconds,respawnFlash,p2p);
        if(screenshot&&!shotTaken&&wallSeconds()>= (stopAfter>0?std::max(0.1,stopAfter-0.1):3.5)) {
            SDL_Surface *surface=SDL_RenderReadPixels(app.GetRenderer(),nullptr);
            shotFailed=!surface||!IMG_SavePNG(surface,screenshot);
            if(surface) SDL_DestroySurface(surface);
            if(shotFailed) std::fprintf(stderr,"Screenshot failed: %s\n",SDL_GetError());
            else std::printf("Saved %s\n",screenshot);
            shotTaken=true;
        }
    },practice?std::function<void(float)>{}:std::function<void(float)>{[&](float dt) { if(p2p) peer->Tick(dt,mutex); else net.Tick(dt,mutex); }});
    if(!practice&&!p2p) net.Leave();
    app.Shutdown();
    return shotFailed?1:0;
}
}

int main(int argc,char **argv) {
    setvbuf(stdout,nullptr,_IOLBF,0);
    if(Flag(argc,argv,"--help")) { Usage(); return 0; }
    const std::string mode=argc>1?argv[1]:"practice";
    const bool bench=mode=="bench-server"||mode=="bench-client";
    const bool p2p=mode=="peer";
    if(mode!="practice"&&mode!="server"&&mode!="client"&&!p2p&&!bench) { Usage(); return 2; }
    clockwork::RegisterComponents();
    NetOptions options=clockwork::NetworkOptions();
    if(p2p) { options.topology=Topology::PeerToPeer; options.upstream=Upstream::State; }
    std::string error;
    if(!NetOptions::Parse(argc,argv,options,&error)) { std::fprintf(stderr,"%s\n",error.c_str()); return 2; }
    if(options.topology!=(p2p?Topology::PeerToPeer:Topology::ClientServer)||options.upstream!=(p2p?Upstream::State:Upstream::Input)) {
        std::fprintf(stderr,"Use peer for P2P state mode, or server/client for CS input mode.\n"); return 2;
    }
    const int port=Number(argc,argv,"--port-base",clockwork::DEFAULT_PORT);
    if(port<1024||port>64000) { std::fprintf(stderr,"Choose a port base between 1024 and 64000.\n"); return 2; }
    if(bench) {
        clockwork::BenchSettings settings; settings.port=port;
        settings.clients=Number(argc,argv,"--clients",settings.clients);
        settings.moving=Number(argc,argv,"--moving",settings.moving);
        settings.statics=Number(argc,argv,"--static",settings.statics);
        settings.iterations=Number(argc,argv,"--iterations",static_cast<int>(settings.iterations));
        settings.warmup=Number(argc,argv,"--warmup",static_cast<int>(settings.warmup));
        return mode=="bench-server"?clockwork::BenchServer(settings,options):clockwork::BenchClient(settings,options);
    }
    return mode=="server"?ServerMain(port,options,Number(argc,argv,"--seconds",0)):Play(argc,argv,port,options,mode=="practice",p2p);
}
