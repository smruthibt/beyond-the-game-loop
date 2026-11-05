#include "Game.hpp"
#include "engine/Engine.hpp"
#include "engine/Network.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

using namespace engine;
using namespace clockwork;
namespace {
int failures=0;
void Check(bool ok,const char *name) { std::printf("%s %s\n",ok?"PASS":"FAIL",name); if(!ok) ++failures; }
bool Wait(const std::function<bool()> &condition,int ms=3000) {
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(ms);
    do { if(condition()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(8)); } while(std::chrono::steady_clock::now()<deadline);
    return condition();
}
struct ClientRunner {
    SceneClient scene;
    std::mutex mutex;
    std::atomic<bool> running{false};
    std::atomic<uint32_t> buttons{0};
    std::thread thread;
    ClientRunner(int port,const NetOptions &options):scene(ClientConfig(port),options) {}
    bool Start() {
        if(!scene.Join()) return false;
        running=true;
        thread=std::thread([&]() {
            auto last=std::chrono::steady_clock::now();
            while(running) {
                const auto now=std::chrono::steady_clock::now();
                const float dt=std::chrono::duration<float>(now-last).count(); last=now;
                { std::lock_guard<std::mutex> lock(mutex); if(auto *p=scene.MyPlayer()) { p->Get<InputState>()->buttons=buttons; scene.StampLocal(); } }
                scene.Tick(dt,mutex);
                std::this_thread::sleep_for(std::chrono::milliseconds(4));
            }
        });
        return true;
    }
    void Stop(bool leave=true) { running=false; if(thread.joinable()) thread.join(); if(leave) scene.Leave(); }
    ~ClientRunner() { Stop(); }
    bool Read(const std::function<bool(World &)> &predicate) { std::lock_guard<std::mutex> lock(mutex); return predicate(scene.GetWorld()); }
};
int Players(World &w) { int count=0; w.Each<Racer>([&](GameObject &,Racer &) { ++count; }); return count; }

void Round(int port,NetOptions options) {
    std::printf("\nNETWORK %s\n",options.Describe().c_str());
    Engine app;
    if(!app.InitHeadless()) { Check(false,"headless engine starts"); return; }
    auto config=ServerConfig(port); config.clientTimeoutMs=700;
    SceneServer server(app,config,options); BuildCourse(server.GetWorld());
    double seconds=0;
    SceneServer::Callbacks cb;
    cb.onJoin=[](World &w,int id) { AddPlayer(w,id).Get<Racer>()->countdown=0; };
    cb.step=[&](World &w,float dt) { Step(w,dt,seconds); };
    std::atomic<bool> serverOk{false};
    std::thread host([&]() { serverOk=server.Run(cb); });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    {
        std::vector<std::unique_ptr<ClientRunner>> clients;
        bool joined=true;
        for(int i=0;i<4;++i) { clients.push_back(std::make_unique<ClientRunner>(port,options)); joined=clients.back()->Start()&&joined; }
        Check(joined,"four clients join the real 0MQ server");
        if(joined) {
            Check(Wait([&]() {
                for(auto &c:clients) if(!c->Read([](World &w) { return Players(w)==4; })) return false;
                return true;
            }),"all four endpoints receive every player");
            const int id=clients[0]->scene.Id();
            clients[0]->buttons=InputState::Right;
            Check(Wait([&]() { return clients[3]->Read([&](World &w) { auto *p=Player(w,id); return p&&p->Get<Transform>()->Left()>180; }); }),"keyboard input moves the player on a different client");
            clients[0]->buttons=0;
            // Let the last input reach the server before testing a stationary respawn.
            Wait([&]() { std::lock_guard<std::mutex> l(app.GetWorldMutex()); return Player(server.GetWorld(),id)->Get<InputState>()->buttons==0; });
            {
                // Stand under the leftmost ledge; a straight-up jump there bumps the runner's head.
                std::lock_guard<std::mutex> lock(app.GetWorldMutex());
                float left=0,centre=0;
                server.GetWorld().Each<Impact,Transform>([&](GameObject &,Impact &,Transform &t) {
                    if(centre==0||t.Left()<left) { left=t.Left(); centre=t.Center().x; }
                });
                auto *p=Player(server.GetWorld(),id);
                p->Get<Transform>()->position={centre-22,FLOOR_Y-60};
                p->Get<RigidBody>()->velocity={0,0};
            }
            clients[0]->buttons=InputState::Jump;
            std::vector<bool> saw(clients.size(),false); // the push back is brief, so remember each sighting
            Check(Wait([&]() {
                bool all=true;
                for(size_t i=0;i<clients.size();++i) {
                    saw[i]=saw[i]||clients[i]->Read([&](World &w) {
                        auto *p=Player(w,id);
                        return p&&p->Get<Respawn>()->deaths==0&&p->Get<Knockback>()->remaining>0&&p->Get<RigidBody>()->velocity.y>0;
                    });
                    all=all&&saw[i];
                }
                return all;
            }),"all clients see a head bump shove the runner back down");
            clients[0]->buttons=0;
            Wait([&]() { std::lock_guard<std::mutex> l(app.GetWorldMutex()); return Player(server.GetWorld(),id)->Get<InputState>()->buttons==0; });
            {
                std::lock_guard<std::mutex> lock(app.GetWorldMutex());
                auto *p=Player(server.GetWorld(),id);
                p->Get<Racer>()->checkpoint=1; p->Get<Respawn>()->spawnIndex=101;
                p->Get<Transform>()->position={1700,1000};
            }
            Check(Wait([&]() {
                for(auto &c:clients) if(!c->Read([&](World &w) {
                    auto *p=Player(w,id); if(!p||p->Get<Respawn>()->deaths!=1||p->Get<Racer>()->checkpoint!=1) return false;
                    return std::abs(p->Get<Transform>()->Left()-2070)<1;
                })) return false;
                return true;
            }),"all clients see the same checkpoint respawn after a fall");
            {
                std::lock_guard<std::mutex> lock(app.GetWorldMutex());
                auto *p=Player(server.GetWorld(),id); p->Get<Racer>()->protection=0;
                server.GetWorld().Each<Hazard,Transform>([&](GameObject &,Hazard &h,Transform &t) {
                    if(h.type==Hazard::Type::Saw&&t.Left()<1000) p->Get<Transform>()->position=t.position;
                });
            }
            Check(Wait([&]() {
                for(auto &c:clients) if(!c->Read([&](World &w) {
                    auto *p=Player(w,id); return p&&p->Get<Respawn>()->deaths==2&&std::abs(p->Get<Transform>()->Left()-2070)<1;
                })) return false;
                return true;
            }),"saw death and respawn agree on all clients");
            Check(clients[1]->Read([&](World &w) { auto *p=Player(w,clients[1]->scene.Id()); return p&&p->Get<Respawn>()->deaths==0&&p->Get<Racer>()->elapsed>0; }),"another player keeps racing without losing a life");
            clients[3]->Stop();
            Check(Wait([&]() { return clients[0]->Read([](World &w) { return Players(w)==3; }); }),"graceful disconnect removes the departed player");
            clients[2]->Stop(false);
            Check(Wait([&]() { return clients[0]->Read([](World &w) { return Players(w)==2; }); }),"silent disconnect is removed by engine timeout");
            const int survivor=clients[1]->scene.Id(); clients[1]->buttons=InputState::Right;
            Check(Wait([&]() { return clients[0]->Read([&](World &w) { auto *p=Player(w,survivor); return p&&p->Get<Transform>()->Left()>230; }); }),"remaining clients continue after both disconnects");
        }
    }
    server.Stop(); host.join();
    Check(serverOk,"server shuts down cleanly");
    app.Shutdown();
}
}
int main() {
    setvbuf(stdout,nullptr,_IOLBF,0);
    {
        net::Socket probe(net::SocketType::Publish);
        if(!probe.Bind("tcp://127.0.0.1:*")) {
            const std::string error=probe.LastError();
            if(error.find("Operation not permitted")!=std::string::npos||error.find("Permission denied")!=std::string::npos) {
                std::printf("SKIP: this environment prohibits local TCP sockets: %s\n",error.c_str());
                return 77;
            }
            std::fprintf(stderr,"Socket preflight failed: %s\n",error.c_str());
            return 1;
        }
    }
    RegisterComponents();
    int port=9150;
    for(auto encoding:{Encoding::Text,Encoding::Binary}) for(bool delta:{false,true}) {
        auto options=NetworkOptions(); options.encoding=encoding; options.delta=delta;
        Round(port,options); port+=200;
    }
    return failures?1:0;
}
