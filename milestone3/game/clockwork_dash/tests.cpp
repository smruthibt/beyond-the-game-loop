#include "Game.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

using namespace engine;
using namespace clockwork;
namespace {
int failures=0;
void Check(bool ok,const char *name) { std::printf("%s %s\n",ok?"PASS":"FAIL",name); if(!ok) ++failures; }
bool Near(float a,float b) { return std::fabs(a-b)<0.02f; }
void MoveTo(GameObject &p,float x,float y=FLOOR_Y-60) {
    p.Get<Transform>()->position={x,y};
    p.Get<RigidBody>()->velocity={0,0};
    p.Get<RigidBody>()->ground=NO_OBJECT;
}
GameObject *Kind(World &w,const char *kind) {
    GameObject *found=nullptr;
    w.Each<Transform>([&](GameObject &o,Transform &) { if(!found&&o.Kind()==kind) found=&o; });
    return found;
}
void EarnCheckpoint(World &w,GameObject &p,int order) {
    w.Each<Checkpoint,Transform>([&](GameObject &,Checkpoint &cp,Transform &t) { if(cp.order==order) MoveTo(p,t.Left()+40); });
    ResolveCourse(w);
}
// Leaves the runner flush under the first ledge, as physics does after a rising hit.
GameObject &UnderLedge(World &w,GameObject &p) {
    auto &ledge=*Kind(w,"platform"); const auto &lt=*ledge.Get<Transform>();
    MoveTo(p,lt.Center().x-22,lt.Bottom()); p.Get<RigidBody>()->grounded=false;
    return ledge;
}
// Stands the runner at x on a surface whose top is `top`, then jumps holding `dir`
// for `hold` seconds, with real physics. Returns what it lands on.
ObjectId Hop(World &w,GameObject &p,float x,float top,float hold,uint32_t dir=InputState::Right) {
    auto &k=*p.Get<Knockback>(); k.remaining=k.cooldown=0;
    auto &in=*p.Get<InputState>(); auto &body=*p.Get<RigidBody>();
    double clock=0; MoveTo(p,x,top-60); in.buttons=0;
    for(int i=0;i<4;++i) Step(w,1.0f/120,clock);
    for(int i=0;i<240;++i) {
        in.buttons=(i==0?InputState::Jump:0u)|(i<hold*120?dir:0u);
        Step(w,1.0f/120,clock);
        if(i>4&&body.grounded) return body.ground;
    }
    return NO_OBJECT;
}
// Whether a sideways jump from the floor lands on the ledge, taking off 100-200px short
// of its near edge as a player would.
bool LandsOn(World &w,GameObject &p,const GameObject &ledge,uint32_t dir) {
    const auto &lt=*ledge.Get<Transform>();
    for(float gap:{100.0f,150.0f,200.0f}) {
        const float x=dir==InputState::Right?lt.Left()-gap-RUNNER_WIDTH:lt.Right()+gap;
        for(int i=0;i<=18;++i) if(Hop(w,p,x,FLOOR_Y,i/30.0f,dir)==ledge.Id()) return true;
    }
    return false;
}
// Opens a potion platform and stands the runner on it, touching the potion.
void Drink(World &w,GameObject &p,GameObject &platform) {
    platform.Get<PotionPlatform>()->opened=true;
    const auto &lt=*platform.Get<Transform>();
    MoveTo(p,PotionArea(lt).Left(),lt.Top()-60); ResolveCourse(w);
}
std::vector<GameObject *> PotionPlatforms(World &w) {
    std::vector<GameObject *> found; w.Each<PotionPlatform>([&](GameObject &o,PotionPlatform &) { found.push_back(&o); });
    return found;
}
}
int main() {
    RegisterComponents();
    World w; BuildCourse(w);
    auto &a=AddPlayer(w,0),&b=AddPlayer(w,1);
    auto &ra=*a.Get<Racer>(); auto &rb=*b.Get<Racer>();
    ra.countdown=rb.countdown=0;
    int spawns=0,hidden=0,movers=0,patterns=0,platforms=0;
    w.Each<SpawnPoint>([&](GameObject &o,SpawnPoint &) { ++spawns; if(!o.Has<Renderable>()) ++hidden; });
    w.Each<PathMover>([&](GameObject &o,PathMover &p) {
        if(o.Kind()!="mover") return;
        ++movers; patterns|=p.pattern==PathMover::Pattern::Circular?4:p.offset.y!=0?2:1;
    });
    w.Each<Transform>([&](GameObject &o,Transform &) { if(o.Kind()=="platform") ++platforms; });
    Check(spawns==7&&hidden==7,"multiple spawn points are hidden engine objects");
    Check(movers==5&&patterns==7&&platforms>=3,"course has static platforms and horizontal, vertical, circular movers");
    Check(!Kind(w,"death-zone")->Has<Renderable>(),"pit death zone is hidden");

    bool riding=true;
    for(int selected=0;selected<5;++selected) {
        World ride; BuildCourse(ride); auto &rider=AddPlayer(ride,0); rider.Get<Racer>()->countdown=0;
        GameObject *platform=nullptr; int index=0;
        ride.Each<PathMover>([&](GameObject &o,PathMover &) { if(o.Kind()=="mover"&&index++==selected) platform=&o; });
        const auto &pt=*platform->Get<Transform>();
        MoveTo(rider,pt.Left()+60,pt.Top()-60);
        systems::StepPhysics(ride,1.0f/120,PhysicsSettings{},OwnedBy(0));
        double clock=0;
        for(int i=0;i<120;++i) Step(ride,1.0f/120,clock);
        riding=riding&&rider.Get<RigidBody>()->ground==platform->Id()&&
            Near(rider.Get<Transform>()->Left()-pt.Left(),60)&&Near(rider.Get<Transform>()->Bottom(),pt.Top());
    }
    Check(riding,"all five moving platforms carry a standing rider through real physics steps");
    {
        auto &camObject=w.Create("camera"); auto &cam=camObject.Add<Camera>();
        cam.view={1600,900}; cam.boundsMax={LEVEL_WIDTH,LEVEL_HEIGHT}; cam.target=a.Id();
        systems::AddScrollBoundaries(w,camObject.Id(),560);
        MoveTo(a,4000); systems::UpdateCameras(w);
        Check(cam.position.x>2000&&cam.position.x<4000,"engine scroll boundaries move the player's local camera");
        MoveTo(a,90); systems::UpdateCameras(w);
        Check(Near(cam.position.x,0),"camera returns to the start after a teleport");
    }

    EarnCheckpoint(w,a,1);
    Check(ra.checkpoint==1&&a.Get<Respawn>()->spawnIndex==101&&rb.checkpoint==0,"checkpoint belongs only to the player who reached it");
    EarnCheckpoint(w,a,2); EarnCheckpoint(w,a,1);
    Check(ra.checkpoint==2&&a.Get<Respawn>()->spawnIndex==102,"backtracking cannot replace a later checkpoint");
    const auto bPosition=b.Get<Transform>()->position;
    ra.elapsed=42;
    MoveTo(a,3500,1000); a.Get<RigidBody>()->velocity={400,900};
    ResolveCourse(w);
    Check(a.Get<Respawn>()->deaths==1&&Near(a.Get<Transform>()->Left(),3910)&&Near(a.Get<Transform>()->Bottom(),FLOOR_Y),"falling respawns at the latest checkpoint");
    Check(Near(a.Get<RigidBody>()->velocity.x,0)&&Near(a.Get<RigidBody>()->velocity.y,0)&&a.Get<RigidBody>()->ground==NO_OBJECT,"respawn clears velocity and platform attachment");
    Check(b.Get<Respawn>()->deaths==0&&Near(b.Get<Transform>()->Left(),bPosition.x)&&Near(ra.elapsed,42),"death preserves elapsed time and leaves other players alone");

    auto &saw=*Kind(w,"saw");
    ra.protection=0; MoveTo(a,saw.Get<Transform>()->Left()+15,saw.Get<Transform>()->Top());
    ResolveCourse(w);
    Check(a.Get<Respawn>()->deaths==2&&Near(a.Get<Transform>()->Left(),3910),"touching a saw uses the same checkpoint respawn");
    Transform corner(saw.Get<Transform>()->Left()-5,saw.Get<Transform>()->Top()-5,7,7);
    Check(!HitsHazard(corner,*saw.Get<Transform>(),*saw.Get<Hazard>()),"saw collision is circular, not its empty bounding-box corners");
    const auto &electric=*Kind(w,"electric")->Get<Transform>();
    ra.protection=0; MoveTo(a,electric.Left(),electric.Bottom()-60);
    UpdateHazards(w,1); ResolveCourse(w);
    Check(a.Get<Respawn>()->deaths==2,"electric barrier is passable while off");
    UpdateHazards(w,2.5); ResolveCourse(w);
    Check(a.Get<Respawn>()->deaths==2,"electric warning gives a safe advance signal");
    UpdateHazards(w,3); ResolveCourse(w);
    Check(a.Get<Respawn>()->deaths==3,"live electric barrier respawns its touching player");
    ResolveCourse(w);
    Check(a.Get<Respawn>()->deaths==3,"one hit cannot repeatedly kill the player at their safe spawn");

    for(int i=0;i<25;++i) { ra.protection=0; MoveTo(a,3500,1000); ResolveCourse(w); }
    Check(a.Get<Respawn>()->deaths==28&&!ra.finished,"respawns are unlimited");
    MoveTo(a,6200); ResolveCourse(w);
    Check(!ra.finished,"finish requires every checkpoint");
    EarnCheckpoint(w,a,3); MoveTo(a,6200); ResolveCourse(w);
    Check(ra.finished,"all checkpoints plus the finish completes the run");
    double seconds=0; const float finalTime=ra.elapsed,otherTime=rb.elapsed;
    Step(w,0.1f,seconds);
    Check(Near(ra.elapsed,finalTime)&&rb.elapsed>otherTime,"finished timer freezes while other racers continue");
    a.Get<InputState>()->buttons=RESTART; Step(w,0.1f,seconds);
    Check(!ra.finished&&ra.checkpoint==0&&a.Get<Respawn>()->deaths==0&&ra.countdown>2,"R starts only this player's new run");
    Step(w,0.1f,seconds);
    Check(ra.countdown<2.9f,"holding R does not restart every frame");

    {
        // Ledges are solid from below, using real physics steps: a straight-up jump bumps
        // the runner's head and shoves it down; getting on top takes a sideways jump.
        World arena; BuildCourse(arena);
        auto &p=AddPlayer(arena,0),&q=AddPlayer(arena,1);
        auto &rp=*p.Get<Racer>(); rp.countdown=q.Get<Racer>()->countdown=0;
        auto &in=*p.Get<InputState>(); auto &k=*p.Get<Knockback>(); auto &body=*p.Get<RigidBody>();
        auto &ledge=*Kind(arena,"platform"); const auto &lt=*ledge.Get<Transform>();
        double clock=0;
        rp.elapsed=12; rp.checkpoint=1; p.Get<Respawn>()->spawnIndex=101;
        rp.protection=1e6f; // a saw patrols nearby; this block tests ledges only
        MoveTo(p,lt.Center().x-22); in.buttons=0; Step(arena,0.05f,clock);
        const float start=p.Get<Transform>()->Left();
        bool bumped=false,below=true,landed=false,held=true; float shove=0,flash=0;
        for(int i=0;i<120;++i) {
            // Jump straight up, then hold Right from the moment of the bump.
            in.buttons=i==0?InputState::Jump:bumped?InputState::Right:0u;
            const float before=p.Get<Transform>()->Left();
            const bool wasStunned=k.remaining>0;
            Step(arena,1.0f/120,clock);
            if(!bumped&&k.remaining>0) { bumped=true; shove=body.velocity.y; flash=ledge.Get<Impact>()->amount; }
            // The timer ticks before input is read, so only a stun still running after
            // the step was in force for all of it.
            else if(wasStunned&&k.remaining>0) held=held&&Near(p.Get<Transform>()->Left(),before);
            below=below&&p.Get<Transform>()->Top()>=lt.Bottom()-0.01f;
            landed=landed||body.ground==ledge.Id();
        }
        Check(bumped&&shove>0&&flash>0&&below&&!landed,
              "jumping straight up under a ledge bumps the runner's head, shoves it down and pops the ledge");
        Check(held&&p.Get<Transform>()->Left()>start+30,"a head bump briefly ignores input, then control returns");
        Check(p.Get<Respawn>()->deaths==0&&rp.checkpoint==1&&rp.elapsed>12&&q.Get<Knockback>()->remaining==0,
              "a head bump is not a death: checkpoint and timer are kept and other runners are unaffected");
        Check(LandsOn(arena,p,ledge,InputState::Right)&&LandsOn(arena,p,ledge,InputState::Left),
              "a sideways jump lands on a ledge, going right or going left");

        Restart(arena,p); p.Get<Racer>()->countdown=0;
        UnderLedge(arena,p); ResolveCourse(arena);
        const bool bumpedAgain=k.remaining>0;
        MoveTo(p,1500,1000); ResolveCourse(arena);
        Check(bumpedAgain&&p.Get<Respawn>()->deaths==1&&k.remaining==0&&k.cooldown==0,"falling during a head bump still respawns and clears the push back");
        UnderLedge(arena,p); ResolveCourse(arena); Restart(arena,p);
        Check(k.remaining==0&&k.cooldown==0&&body.velocity.y==0,"restart clears the push back and its cooldown");
    }

    {
        // Potion platforms and the boost, with real physics steps.
        World course; BuildCourse(course);
        auto &scout=AddPlayer(course,0); scout.Get<Racer>()->countdown=0; scout.Get<Racer>()->protection=1e6f;
        const auto sealed=PotionPlatforms(course);
        bool reachable=sealed.size()==3;
        for(auto *o:sealed) reachable=reachable&&(LandsOn(course,scout,*o,InputState::Right)||LandsOn(course,scout,*o,InputState::Left));
        Check(reachable,"three potion platforms, each reachable with a jump from the floor");

        World arena; BuildCourse(arena);
        auto &p=AddPlayer(arena,0),&q=AddPlayer(arena,1);
        for(auto *o:{&p,&q}) { o->Get<Racer>()->countdown=0; o->Get<Racer>()->protection=1e6f; } // saws patrol nearby
        auto &in=*p.Get<InputState>(); auto &boost=*p.Get<Boost>(); auto &body=*p.Get<RigidBody>(); auto &pt=*p.Get<Transform>();
        const auto potions=PotionPlatforms(arena);
        const auto &lt=*potions[0]->Get<Transform>(); auto &state=*potions[0]->Get<PotionPlatform>();
        double clock=0; float since=0;
        auto run=[&](uint32_t buttons,float seconds) {
            in.buttons=buttons;
            for(int i=0;i<static_cast<int>(seconds*120+0.5f);++i) { Step(arena,1.0f/120,clock); since+=1.0f/120; }
        };
        MoveTo(p,lt.Center().x-22); run(0,0.05f);
        for(int i=0;i<90&&!state.opened;++i) run(i==0?InputState::Jump:0u,1.0f/120);
        Check(state.opened&&!state.taken&&boost.remaining==0,"bumping a potion platform from below opens it and reveals the potion");
        const Transform area=PotionArea(lt);
        MoveTo(p,area.Left()-50,lt.Top()-60); run(0,0.05f);
        for(int i=0;i<60&&!state.taken;++i) run(InputState::Right,1.0f/120);
        since=0;
        Check(state.taken&&boost.remaining>6.9f&&Near(pt.size.x,RUNNER_WIDTH*1.25f)&&Near(pt.size.y,RUNNER_HEIGHT*1.25f)&&Near(pt.Bottom(),lt.Top()),
              "touching the potion drinks it: the runner grows 1.25x about its feet");
        MoveTo(q,area.Left(),lt.Top()-60); ResolveCourse(arena);
        Check(q.Get<Boost>()->remaining==0,"each potion can be drunk only once");
        MoveTo(p,5600,FLOOR_Y-pt.size.y); run(0,0.05f);
        const float from=pt.Left(); run(InputState::Right,0.5f);
        const float groundSpeed=(pt.Left()-from)/0.5f;
        run(InputState::Right|InputState::Jump,0.1f);
        Check(std::fabs(groundSpeed-600)<5&&!body.grounded&&Near(body.velocity.x,400),
              "a boosted runner runs 1.5x on the ground, but jumps at normal speed");
        run(0,6.85f-since);
        const bool still=boost.remaining>0;
        run(0,0.3f);
        Check(still&&boost.remaining==0&&Near(pt.size.y,RUNNER_HEIGHT)&&Near(pt.Bottom(),FLOOR_Y)&&
              Near(p.Get<PlatformerController>()->moveSpeed,400),
              "the boost lasts 7 seconds, then the runner shrinks back about its feet to normal speed");
        Drink(arena,p,*potions[1]);
        const bool boosted=boost.remaining>0;
        MoveTo(p,1700,1000); ResolveCourse(arena);
        Check(boosted&&boost.remaining==0&&Near(pt.size.y,RUNNER_HEIGHT)&&p.Get<Respawn>()->deaths==1,"dying ends the boost");
        Drink(arena,p,*potions[2]);
        const bool again=boost.remaining>0;
        Restart(arena,p);
        Check(again&&boost.remaining==0&&Near(pt.size.y,RUNNER_HEIGHT),"restarting ends the boost");
    }
    {
        // Stomps: landing on a runner's head bounces the stomper and ends that runner's boost.
        World ring; BuildCourse(ring);
        auto &a=AddPlayer(ring,0),&b=AddPlayer(ring,1);
        for(auto *o:{&a,&b}) { o->Get<Racer>()->countdown=0; o->Get<Racer>()->protection=1e6f; }
        Drink(ring,b,*PotionPlatforms(ring)[0]);
        auto &bt=*b.Get<Transform>(); auto &bb=*b.Get<Boost>(); auto &fall=*a.Get<RigidBody>();
        double clock=0;
        MoveTo(b,1000,FLOOR_Y-bt.size.y); MoveTo(a,1000);
        for(int i=0;i<24;++i) Step(ring,1.0f/120,clock);
        Check(bb.remaining>0&&fall.velocity.y==0,"runners side by side on the ground pass through each other without a stomp");
        auto drop=[&] {
            MoveTo(a,bt.Left()+5,bt.Top()-100);
            for(int i=0;i<60;++i) { Step(ring,1.0f/120,clock); if(fall.velocity.y<0) return true; }
            return false;
        };
        const bool bounced=drop();
        Check(bounced&&bb.remaining==0&&bb.stomped>0&&Near(bt.size.y,RUNNER_HEIGHT)&&b.Get<Respawn>()->deaths==0,
              "landing on a boosted runner's head bounces the stomper off and ends the boost");
        Check(drop()&&b.Get<Respawn>()->deaths==0,"a stomp on a runner without a boost still bounces the stomper");
    }

    // Verify actual gameplay transitions travel through the engine's codecs.
    for(auto encoding:{Encoding::Text,Encoding::Binary}) for(bool delta:{false,true}) {
        World server; BuildCourse(server); auto &p=AddPlayer(server,0); p.Get<Racer>()->countdown=0;
        World client; SnapshotEncoder encoder({encoding,delta}); SnapshotDecoder decoder;
        bool ok=decoder.Apply(client,encoder.Encode(server,1)).ok;
        EarnCheckpoint(server,p,1); MoveTo(p,1500,1000); ResolveCourse(server); UpdateHazards(server,3.5);
        ok=ok&&decoder.Apply(client,encoder.Encode(server,2)).ok;
        auto *copy=client.Find(p.Id());
        ok=ok&&copy&&copy->Get<Respawn>()->deaths==1&&copy->Get<Racer>()->checkpoint==1&&Near(copy->Get<Transform>()->Left(),2070);
        auto *barrier=Kind(client,"electric");
        ok=ok&&barrier&&barrier->Get<Hazard>()->phase==Hazard::Phase::Active;
        UpdateHazards(server,5.5);
        ok=ok&&decoder.Apply(client,encoder.Encode(server,3)).ok;
        ok=ok&&barrier->Get<Hazard>()->phase==Hazard::Phase::Off;
        auto &ledge=UnderLedge(server,p); ResolveCourse(server);
        ok=ok&&decoder.Apply(client,encoder.Encode(server,4)).ok;
        ok=ok&&copy->Get<Knockback>()->remaining>0&&copy->Get<RigidBody>()->velocity.y>0&&
            client.Find(ledge.Id())->Get<Impact>()->amount>0&&copy->Get<Respawn>()->deaths==1;
        double simTime=5.5;
        Step(server,0.3f,simTime);
        ok=ok&&decoder.Apply(client,encoder.Encode(server,5)).ok;
        ok=ok&&copy->Get<Knockback>()->remaining==0&&client.Find(ledge.Id())->Get<Impact>()->amount==0;
        auto &potion=*PotionPlatforms(server)[0];
        Drink(server,p,potion);
        ok=ok&&decoder.Apply(client,encoder.Encode(server,6)).ok;
        const auto *seen=client.Find(potion.Id())->Get<PotionPlatform>();
        ok=ok&&seen->opened&&seen->taken&&copy->Get<Boost>()->remaining>0&&Near(copy->Get<Transform>()->size.y,RUNNER_HEIGHT*1.25f);
        char label[150]; std::snprintf(label,sizeof label,"%s %s snapshots carry checkpoints, deaths, hazards, head bumps, potions and boosts",EncodingName(encoding),delta?"delta":"full");
        Check(ok,label);
    }

    // Four independent endpoint worlds exchange real engine messages without
    // sockets. This remains runnable when an execution sandbox forbids TCP.
    for(auto encoding:{Encoding::Text,Encoding::Binary}) for(bool delta:{false,true}) {
        World server; BuildCourse(server);
        const auto options=[&] { auto o=NetworkOptions(); o.encoding=encoding; o.delta=delta; return o; }();
        SnapshotEncoder down(options.Downstream()); SnapshotDecoder inbox;
        std::vector<std::unique_ptr<World>> clients;
        std::vector<std::unique_ptr<SnapshotEncoder>> inputs;
        std::vector<std::unique_ptr<SnapshotDecoder>> receivers;
        for(int i=0;i<4;++i) {
            AddPlayer(server,i).Get<Racer>()->countdown=0;
            clients.push_back(std::make_unique<World>(1u<<30));
            inputs.push_back(std::make_unique<SnapshotEncoder>(options.Upward()));
            receivers.push_back(std::make_unique<SnapshotDecoder>());
        }
        bool ok=true;
        auto publish=[&](uint32_t tick) {
            const auto message=down.Encode(server,tick);
            for(int i=0;i<4;++i) { ok=receivers[i]->Apply(*clients[i],message).ok&&ok; clients[i]->Flush(); }
        };
        publish(1);
        double clock=0;
        for(uint32_t tick=2;tick<40;++tick) {
            for(int i=0;i<4;++i) {
                Player(*clients[i],i)->Get<InputState>()->buttons=i==0?InputState::Right:0;
                ApplyOptions apply; apply.source=i; apply.onlyOwner=i; apply.createMissing=false;
                ok=inbox.Apply(server,inputs[i]->Encode(*clients[i],tick,OwnedBy(i)),apply).ok&&ok;
            }
            Step(server,1.0f/120,clock); publish(tick);
        }
        ok=ok&&Player(server,0)->Get<Transform>()->Left()>200;
        auto &p=*Player(server,0); p.Get<InputState>()->buttons=0;
        EarnCheckpoint(server,p,1); MoveTo(p,1700,1000); ResolveCourse(server); publish(40);
        for(auto &client:clients) {
            auto *copy=Player(*client,0);
            ok=ok&&copy->Get<Respawn>()->deaths==1&&Near(copy->Get<Transform>()->Left(),2070);
            for(int i=1;i<4;++i) ok=ok&&Player(*client,i)->Get<Respawn>()->deaths==0;
        }
        UnderLedge(server,p); ResolveCourse(server);
        Step(server,0.06f,clock); publish(41);
        for(auto &client:clients) {
            auto *copy=Player(*client,0);
            ok=ok&&copy->Get<Knockback>()->remaining>0&&copy->Get<RigidBody>()->velocity.y>0&&
                Near(copy->Get<Transform>()->Left(),p.Get<Transform>()->Left())&&copy->Get<Respawn>()->deaths==1;
            for(int i=1;i<4;++i) ok=ok&&Player(*client,i)->Get<Knockback>()->remaining==0;
        }
        server.DestroyIf([](const GameObject &o) { return OwnerOf(o)==3; }); server.Flush(); publish(42);
        for(auto &client:clients) ok=ok&&Player(*client,3)==nullptr;
        char label[160]; std::snprintf(label,sizeof label,"four endpoint worlds (%s %s): input, respawn, knockback and removal converge",EncodingName(encoding),delta?"delta":"full");
        Check(ok,label);
    }
    std::printf("%s\n",failures?"FAILED":"ALL GAMEPLAY CHECKS PASSED");
    return failures?1:0;
}
