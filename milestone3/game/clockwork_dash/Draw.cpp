#include "Game.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace clockwork {
using namespace engine;
namespace {
// Night shift: midnight sky, gunmetal machinery, neon trim. Cool tones are safe
// footing, gold is machinery that moves, hot pink is always lethal.
constexpr SDL_Color INK{10,12,28,255};
constexpr SDL_Color STEEL{36,40,80,255};
constexpr SDL_Color PANEL{13,15,34,255};
constexpr SDL_Color LINE{48,54,100,255};
constexpr SDL_Color TEXT{232,238,255,255};
constexpr SDL_Color MUTED{126,138,190,255};
constexpr SDL_Color EDGE{150,172,255,255};
constexpr SDL_Color CYAN{72,232,220,255};
constexpr SDL_Color GOLD{255,190,80,255};
constexpr SDL_Color PINK{255,64,129,255};
constexpr SDL_Color GREEN{96,230,150,255};
constexpr SDL_Color DIAMOND{170,235,255,255}; // the potion and the boost it gives
constexpr float TAU=PathMover::TWO_PI;
SDL_Color Alpha(SDL_Color c,float a) { c.a=static_cast<Uint8>(std::clamp(a,0.0f,255.0f)); return c; }
SDL_FColor ToF(SDL_Color c) { return {c.r/255.0f,c.g/255.0f,c.b/255.0f,c.a/255.0f}; }
int Mod(int a,int m) { return (a%m+m)%m; }
void Color(SDL_Renderer *r, SDL_Color c) { SDL_SetRenderDrawColor(r,c.r,c.g,c.b,c.a); }
void Rect(SDL_Renderer *r,float x,float y,float w,float h,SDL_Color c) {
    Color(r,c); SDL_FRect box{x,y,w,h}; SDL_RenderFillRect(r,&box);
}
void Poly(SDL_Renderer *r,const std::vector<SDL_FPoint> &points,SDL_Color color) {
    const SDL_FColor c=ToF(color);
    std::vector<SDL_Vertex> vertices; std::vector<int> indices;
    for(auto p:points) vertices.push_back({p,c,{0,0}});
    for(size_t i=1;i+1<points.size();++i) { indices.push_back(0); indices.push_back(static_cast<int>(i)); indices.push_back(static_cast<int>(i+1)); }
    SDL_RenderGeometry(r,nullptr,vertices.data(),static_cast<int>(vertices.size()),indices.data(),static_cast<int>(indices.size()));
}
void Gradient(SDL_Renderer *r,float x,float y,float w,float h,SDL_Color top,SDL_Color bottom) {
    const SDL_Vertex v[]={{{x,y},ToF(top),{0,0}},{{x+w,y},ToF(top),{0,0}},{{x+w,y+h},ToF(bottom),{0,0}},{{x,y+h},ToF(bottom),{0,0}}};
    const int i[]={0,1,2,0,2,3};
    SDL_RenderGeometry(r,nullptr,v,4,i,6);
}
void Line(SDL_Renderer *r,float x,float y,float xx,float yy,SDL_Color c,float width=1) {
    const float length=std::hypot(xx-x,yy-y);
    if(length<0.001f) return;
    const float dx=(yy-y)/length*width/2,dy=(xx-x)/length*width/2;
    Poly(r,{{x-dx,y+dy},{xx-dx,yy+dy},{xx+dx,yy-dy},{x+dx,y-dy}},c);
}
void Ellipse(SDL_Renderer *r,float x,float y,float rx,float ry,SDL_Color c) {
    std::vector<SDL_FPoint> p;
    for(int i=0;i<64;++i) p.push_back({x+std::cos(TAU*i/64)*rx,y+std::sin(TAU*i/64)*ry});
    Poly(r,p,c);
}
void Circle(SDL_Renderer *r,float x,float y,float radius,SDL_Color c) { Ellipse(r,x,y,radius,radius,c); }
void Ring(SDL_Renderer *r,float x,float y,float radius,SDL_Color c,float width=1) {
    for(int i=0;i<64;++i) {
        const float a=TAU*i/64,b=TAU*(i+1)/64;
        Line(r,x+std::cos(a)*radius,y+std::sin(a)*radius,x+std::cos(b)*radius,y+std::sin(b)*radius,c,width);
    }
}
void Round(SDL_Renderer *r,float x,float y,float w,float h,float radius,SDL_Color c) {
    radius=std::min(radius,std::min(w,h)/2);
    // One convex polygon avoids alpha accumulating where corner pieces overlap.
    std::vector<SDL_FPoint> points;
    const SDL_FPoint centers[]={{x+w-radius,y+radius},{x+w-radius,y+h-radius},
                               {x+radius,y+h-radius},{x+radius,y+radius}};
    for(int corner=0;corner<4;++corner) for(int step=0;step<=12;++step) {
        const float a=(corner-1)*TAU/4+step*TAU/48;
        points.push_back({centers[corner].x+std::cos(a)*radius,centers[corner].y+std::sin(a)*radius});
    }
    Poly(r,points,c);
}
// Light is added rather than blended, so overlapping glows brighten like neon.
template<class Fn> void Additive(SDL_Renderer *r,Fn draw) {
    SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_ADD); draw(); SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_BLEND);
}
void Glow(SDL_Renderer *r,float x,float y,float radius,SDL_Color c) {
    // Big halos get more, fainter layers so the falloff shows no rings.
    const int layers=std::clamp(static_cast<int>(radius/6),6,24);
    Additive(r,[&] {
        for(int i=0;i<layers;++i) Circle(r,x,y,radius*(1-0.9f*i/layers),Alpha(c,static_cast<float>(c.a)/layers));
    });
}
// Dark glass card; nearly opaque, so the hairline edge drawn beneath barely tints it.
void Panel(SDL_Renderer *r,float x,float y,float w,float h,float radius,SDL_Color edge={96,112,200,70}) {
    Round(r,x+4,y+6,w,h,radius,{0,0,0,80});
    Round(r,x-1,y-1,w+2,h+2,radius+1,edge);
    Round(r,x,y,w,h,radius,Alpha(PANEL,240));
}
void Text(SDL_Renderer *r,float x,float y,const std::string &text,SDL_Color c=TEXT,float scale=1.5f) {
    // Composes with any scale already set, so text inside a scaled drawing scales with it.
    float sx=1,sy=1; SDL_GetRenderScale(r,&sx,&sy);
    Color(r,c); SDL_SetRenderScale(r,sx*scale,sy*scale);
    SDL_RenderDebugText(r,x/scale,y/scale,text.c_str()); SDL_SetRenderScale(r,sx,sy);
}
void Gear(SDL_Renderer *r,float x,float y,float radius,float angle,SDL_Color c,SDL_Color hole,int teeth=14,SDL_Color trim={0,0,0,0}) {
    std::vector<SDL_FPoint> points;
    for(int i=0;i<teeth*4;++i) {
        const float a=angle+TAU*i/(teeth*4);
        const float rr=radius*(i%4==0||i%4==3?0.83f:1.0f);
        points.push_back({x+std::cos(a)*rr,y+std::sin(a)*rr});
    }
    // Fan from the centre keeps the concave teeth correctly filled.
    for(size_t i=0;i<points.size();++i) Poly(r,{{x,y},points[i],points[(i+1)%points.size()]},c);
    Circle(r,x,y,radius*0.63f,hole);
    if(trim.a) Ring(r,x,y,radius*0.71f,trim,1.5f);
    for(int i=0;i<5;++i) {
        const float a=angle+i*TAU/5;
        Line(r,x,y,x+std::cos(a)*radius*0.66f,y+std::sin(a)*radius*0.66f,c,radius*0.13f);
    }
    Circle(r,x,y,radius*0.24f,c); Circle(r,x,y,radius*0.1f,hole);
}
std::string Time(float seconds) {
    char b[40]; const int cs=static_cast<int>(seconds*100);
    std::snprintf(b,sizeof b,"%02d:%02d.%02d",cs/6000,(cs/100)%60,cs%100); return b;
}
void Cloud(SDL_Renderer *r,float x,float y,float scale,SDL_Color c) {
    Ellipse(r,x,y,115*scale,21*scale,c);
    Ellipse(r,x-45*scale,y-12*scale,44*scale,27*scale,c);
    Ellipse(r,x+10*scale,y-25*scale,53*scale,41*scale,c);
    Ellipse(r,x+63*scale,y-9*scale,42*scale,26*scale,c);
}
// Tiles a parallax layer; the world-anchored index keeps per-tile variety from popping.
template<class Fn> void Layer(double scroll,float spacing,int first,int last,Fn draw) {
    const double tile=std::floor(scroll/spacing);
    const float shift=static_cast<float>(scroll-tile*spacing);
    for(int i=first;i<last;++i) draw(i*spacing-shift,i+static_cast<int>(tile));
}
void Background(SDL_Renderer *r,const Camera &cam,double seconds) {
    const SDL_Color zenith{8,10,26,255},mid{22,20,58,255},horizon{66,34,98,255};
    Gradient(r,0,0,1600,460,zenith,mid); Gradient(r,0,460,1600,440,mid,horizon);
    for(int i=0;i<90;++i) {
        const float x=static_cast<float>(std::fmod(i*233.7+12800-cam.position.x*0.02,1600));
        const float y=static_cast<float>(std::fmod(i*61.3+(i*i%97)*5.0,560));
        const float twinkle=0.55f+0.45f*static_cast<float>(std::sin(seconds*(0.8+i%5*0.35)+i));
        const float size=i%7==0?3.0f:2.0f;
        Rect(r,x,y,size,size,Alpha({200,212,255,255},200*twinkle));
    }
    // A pale clock-face moon keeps the clockwork motif in the sky.
    const float mx=1160-cam.position.x*0.04f,my=200;
    Glow(r,mx,my,160,{110,130,255,72});
    Circle(r,mx,my,56,{204,212,246,255});
    const SDL_Color face{160,172,226,255};
    for(int i=0;i<12;++i) {
        const float a=TAU*i/12,inner=i%3?43.0f:38.0f;
        Line(r,mx+std::cos(a)*inner,my+std::sin(a)*inner,mx+std::cos(a)*49,my+std::sin(a)*49,face,i%3?2.0f:3.0f);
    }
    const float hour=static_cast<float>(seconds*0.05)-1.2f,minute=static_cast<float>(seconds*0.6);
    Line(r,mx,my,mx+std::cos(hour)*22,my+std::sin(hour)*22,face,4);
    Line(r,mx,my,mx+std::cos(minute)*33,my+std::sin(minute)*33,face,2.5f);
    Circle(r,mx,my,4,face);
    Layer(cam.position.x*0.10+seconds*3,480,-1,5,[&](float x,int k) {
        Cloud(r,x+50,300+Mod(k,2)*96,0.8f,{120,110,210,22});
        Cloud(r,x+280,500+Mod(k,2)*40,1.2f,{150,90,190,26});
    });
    // Distant clock towers and sky bridges, on their own parallax layer.
    const SDL_Color tower{40,32,86,255},band{52,42,104,255};
    Layer(cam.position.x*0.19,310,-1,8,[&](float x,int k) {
        const float top=390.0f+Mod(k,3)*46;
        Round(r,x,top,112,430,50,tower);
        Rect(r,x-9,top+63,130,8,band);
        Rect(r,x+14,top+158,84,6,band);
        Poly(r,{{x-15,top+12},{x+56,top-55},{x+127,top+12}},tower);
        Line(r,x+56,top-55,x+56,top-87,tower,3);
        const float blink=0.5f+0.5f*static_cast<float>(std::sin(seconds*2.2+k*1.7));
        Glow(r,x+56,top-89,18,Alpha(PINK,110*blink)); Circle(r,x+56,top-89,3,Alpha(PINK,120+135*blink));
        Glow(r,x+56,top+81,52,Alpha(CYAN,40));
        Circle(r,x+56,top+81,29,{24,52,74,255});
        Ring(r,x+56,top+81,25,Alpha(CYAN,190),2);
        Line(r,x+56,top+81,x+56,top+64,{170,250,240,255},3); Line(r,x+56,top+81,x+69,top+90,{170,250,240,255},3);
        for(int j=0;j<3;++j) Round(r,x+20+j*26,top+182,12,72,6,Mod(k*7+j*3,5)<3?SDL_Color{240,160,90,150}:band);
        Rect(r,x+112,top+211,198,13,tower);
        for(int j=0;j<6;++j) Rect(r,x+126+j*34,top+185,3,26,tower);
        Line(r,x+112,top+185,x+310,top+185,tower,3);
    });
    // A closer row of suspended mechanisms, with real cog silhouettes.
    const SDL_Color structure{18,17,44,255},rim{28,26,64,255};
    Layer(cam.position.x*0.37,570,-1,5,[&](float x,int) {
        Rect(r,x+75,552,24,330,structure); Rect(r,x+281,620,22,280,structure);
        Round(r,x+48,524,76,22,6,rim);
        Line(r,x+86,550,x+292,690,structure,8);
        Gear(r,x+87,531,65,static_cast<float>(seconds*0.18),rim,tower,14,Alpha(CYAN,50));
        Gear(r,x+177,572,38,static_cast<float>(-seconds*0.3),{24,23,58,255},tower,12,Alpha(GOLD,45));
        Rect(r,x+50,693,260,8,structure);
    });
    // Low fog banks make gaps read as a drop into the dark.
    Layer(cam.position.x*0.45+seconds*5,390,-1,6,[&](float x,int k) {
        Cloud(r,x,840+Mod(k,2)*25,1.5f,{84,60,140,90});
        Cloud(r,x+40,885+Mod(k,2)*25,1.5f,{30,24,66,240});
    });
    // Sparks drift up out of the foundry.
    Additive(r,[&] {
        for(int i=0;i<28;++i) {
            const float x=static_cast<float>(std::fmod(i*137.0+seconds*(6+i%3)-cam.position.x*0.3+12800,1600));
            const float y=900-static_cast<float>(std::fmod(i*83.0+seconds*(14+i%4*5),620));
            const SDL_Color c=i%3?GOLD:CYAN;
            Circle(r,x,y,5,Alpha(c,28)); Circle(r,x,y,1.6f,Alpha(c,200));
        }
    });
}

void Platform(SDL_Renderer *r,const GameObject &o,const Transform &t,float x,float y) {
    const auto c=o.Get<Renderable>()->color;
    const bool moving=o.Has<PathMover>();
    if(o.Kind()=="ground") {
        // Viaduct supports leave open arches underneath the course.
        const SDL_Color pier{16,17,40,255};
        Rect(r,x,y+14,t.size.x,35,{28,30,62,255});
        Rect(r,x,y+49,t.size.x,12,{20,21,48,255});
        for(float at=0;at<t.size.x;at+=160) {
            const float width=std::min(38.0f,t.size.x-at);
            Rect(r,x+at,y+54,width,160,pier);
            Rect(r,x+at,y+54,6,160,{40,44,90,255});
            Poly(r,{{x+at+width,y+55},{x+at+width+27,y+55},{x+at+width,y+98}},pier);
        }
        Rect(r,x,y,t.size.x,14,{46,50,96,255});
        for(float at=8;at<t.size.x;at+=44) Line(r,x+at,y+5,x+at,y+12,{30,33,70,255},2);
        // A lit edge marks the running surface.
        Additive(r,[&] { Rect(r,x,y-6,t.size.x,8,Alpha(EDGE,18)); Rect(r,x,y-2,t.size.x,4,Alpha(EDGE,30)); });
        Rect(r,x,y,t.size.x,3,EDGE);
        for(float edge:{x,x+t.size.x-26}) {
            Rect(r,edge,y+14,26,24,GOLD);
            for(int j=0;j<2;++j) Line(r,edge+j*13+1,y+15,edge+j*13+10,y+37,INK,5);
        }
        for(float at=65;at<t.size.x;at+=160) { Glow(r,x+at,y+35,12,Alpha(CYAN,90)); Circle(r,x+at,y+35,3,CYAN); }
        return;
    }
    // Gunmetal decks with a neon running edge in the platform's own colour. A ledge
    // struck from below pops up and flashes, like a Mario block.
    const float hit=o.Has<Impact>()?o.Get<Impact>()->amount:0;
    y-=hit*6;
    auto brackets=[&] {
        Poly(r,{{x+16,y+t.size.y},{x+46,y+t.size.y},{x+16,y+t.size.y+20}},INK);
        Poly(r,{{x+t.size.x-16,y+t.size.y},{x+t.size.x-46,y+t.size.y},{x+t.size.x-16,y+t.size.y+20}},INK);
    };
    if(const auto *potion=o.Get<PotionPlatform>(); potion&&!potion->opened) {
        // Copper bricks until bumped open, with the hidden potion's diamond hovering just above.
        const SDL_Color brick{150,84,66,255},lit{196,122,92,255};
        const float row=t.size.y/2;
        Round(r,x-2,y+2,t.size.x+4,t.size.y+5,4,INK);
        Rect(r,x,y,t.size.x,t.size.y,{62,34,40,255});
        for(int j=0;j<2;++j) for(float bx=x-(j?14:0);bx<x+t.size.x;bx+=28) {
            const float left=std::max(bx+1,x+1),right=std::min(bx+27,x+t.size.x-1);
            Rect(r,left,y+j*row+1,right-left,row-2,brick); Rect(r,left,y+j*row+1,right-left,2,lit);
        }
        const float cx=x+t.size.x/2,cy=y-12;
        Glow(r,cx,cy,22,Alpha(DIAMOND,110));
        Poly(r,{{cx,cy-8},{cx+7,cy},{cx,cy+8},{cx-7,cy}},DIAMOND);
        brackets();
        return;
    }
    const SDL_Color trim=moving?GOLD:c;
    Additive(r,[&] {
        Round(r,x-6,y-7,t.size.x+12,16,8,Alpha(trim,26));
        Round(r,x-2,y-3,t.size.x+4,8,4,Alpha(trim,40));
    });
    Round(r,x-2,y+2,t.size.x+4,t.size.y+5,6,INK);
    Round(r,x,y,t.size.x,t.size.y,5,STEEL);
    Rect(r,x+4,y,t.size.x-8,3,trim);
    if(hit>0) Rect(r,x+4,y,t.size.x-8,3,Alpha(TEXT,255*hit));
    Rect(r,x+5,y+9,t.size.x-10,std::max(5.0f,t.size.y-14),{20,22,50,255});
    for(float at=14;at<t.size.x-6;at+=36) Circle(r,x+at,y+15,2.5f,Alpha(trim,220));
    if(moving) {
        Gear(r,x+22,y+t.size.y+9,11,0,GOLD,INK,10);
        Gear(r,x+t.size.x-22,y+t.size.y+9,11,0,GOLD,INK,10);
        for(int i=-1;i<=1;++i) Poly(r,{{x+t.size.x/2+i*14-3,y+10},{x+t.size.x/2+i*14+3,y+15},{x+t.size.x/2+i*14-3,y+20}},GOLD);
    } else {
        brackets();
    }
}

// A diamond-shaped flask bobbing on top of an opened potion platform.
void DrawPotion(SDL_Renderer *r,float x,float y,double seconds) {
    y+=static_cast<float>(std::sin(seconds*3))*2;
    const float cx=x+14,twinkle=0.5f+0.5f*static_cast<float>(std::sin(seconds*6));
    Glow(r,cx,y+20,30,Alpha(DIAMOND,120));
    Rect(r,cx-4,y+3,8,7,{120,140,190,255});
    Round(r,cx-5,y,10,5,2,{170,120,80,255});
    Poly(r,{{cx,y+7},{cx+13,y+20},{cx,y+33},{cx-13,y+20}},INK);
    Poly(r,{{cx,y+10},{cx+10,y+20},{cx,y+30},{cx-10,y+20}},DIAMOND);
    Poly(r,{{cx-3,y+13},{cx+1,y+17},{cx-5,y+20}},TEXT);
    Line(r,cx+12,y+2,cx+12,y+10,Alpha(TEXT,255*twinkle),2); Line(r,cx+8,y+6,cx+16,y+6,Alpha(TEXT,255*twinkle),2);
}

void DrawHazard(SDL_Renderer *r,const Transform &t,const Hazard &h,float x,float y) {
    if(h.type==Hazard::Type::Saw) {
        const float cx=x+t.size.x/2,cy=y+t.size.y/2,rad=t.size.x/2;
        Ellipse(r,cx,y+t.size.y+2,rad*0.9f,5,{0,0,0,90});
        Glow(r,cx,cy,rad+22,Alpha(PINK,110));
        Circle(r,cx,cy,rad*0.75f,INK);
        for(int i=0;i<16;++i) {
            const float a=h.angle+TAU*i/16;
            Poly(r,{{cx+std::cos(a)*rad,cy+std::sin(a)*rad},
                    {cx+std::cos(a-0.22f)*rad*0.63f,cy+std::sin(a-0.22f)*rad*0.63f},
                    {cx+std::cos(a+0.18f)*rad*0.78f,cy+std::sin(a+0.18f)*rad*0.78f}},{214,222,245,255});
        }
        Circle(r,cx,cy,rad*0.59f,{58,62,108,255}); Ring(r,cx,cy,rad*0.51f,Alpha(PINK,200),2);
        Circle(r,cx,cy,rad*0.32f,PINK); Circle(r,cx,cy,rad*0.14f,INK);
        Line(r,cx-3,cy-3,cx+3,cy+3,TEXT,2);
        return;
    }
    const bool active=h.phase==Hazard::Phase::Active,warning=h.phase==Hazard::Phase::Warning;
    const SDL_Color c=active?PINK:warning?GOLD:GREEN;
    Round(r,x-12,y-16,t.size.x+24,23,6,INK);
    Round(r,x-12,y+t.size.y-7,t.size.x+24,14,4,INK);
    Rect(r,x-5,y-12,t.size.x+10,5,{70,78,130,255});
    for(int i=0;i<4;++i) Rect(r,x+i*10+2,y-3,4,10,c);
    if(active) {
        Additive(r,[&] {
            Round(r,x-14,y+7,t.size.x+28,t.size.y-14,14,Alpha(PINK,30));
            Rect(r,x,y+7,t.size.x,t.size.y-14,Alpha(PINK,30));
        });
        auto strands=[&](SDL_Color color,float width) {
            for(int strand=0;strand<3;++strand) {
                float px=x+8+strand*10,py=y+7;
                for(int i=1;i<=12;++i) {
                    const float nx=x+8+strand*10+std::sin(i*2.6f+h.angle*3)*7;
                    const float ny=y+7+(t.size.y-14)*i/12;
                    Line(r,px,py,nx,ny,color,width); px=nx;py=ny;
                }
            }
        };
        Additive(r,[&] { strands(Alpha(PINK,90),8); });
        strands(PINK,3.5f); strands({255,225,240,255},1.4f);
    } else {
        for(float yy=y+15;yy<y+t.size.y-10;yy+=20) Circle(r,x+t.size.x/2,yy,2,Alpha(c,warning?160:90));
    }
    const std::string label=active?"LIVE":warning?"CHARGING":"SAFE";
    const float lw=label.size()*10.0f+16,lx=x+t.size.x/2-lw/2;
    if(active) Glow(r,x+t.size.x/2,y-36,lw*0.8f,Alpha(PINK,70));
    Round(r,lx-1,y-48,lw+2,25,11,c);
    if(!active) Round(r,lx,y-47,lw,23,10,INK);
    Text(r,lx+8,y-41,label,active?INK:c,1.25f);
}

void Robot(SDL_Renderer *r,const GameObject &o,const Racer &race,const Renderable &visual,float x,float y,bool mine) {
    const auto c=visual.color; const auto &body=*o.Get<RigidBody>();
    const float stride=body.grounded&&std::abs(body.velocity.x)>1?std::sin(race.elapsed*18)*5:0;
    const SDL_Color limb{70,78,130,255},shell{214,222,245,255};
    const auto *k=o.Get<Knockback>();
    const bool knocked=k&&k->remaining>0;
    if(knocked) x+=std::sin(k->remaining*160)*2.5f; // rattles after a head bump
    Ellipse(r,x+22,y+62,24,5,{0,0,0,90});
    Glow(r,x+22,y+30,48,Alpha(c,45)); // backlight lifts the runner off the night
    if(const auto *boost=o.Get<Boost>()) {
        if(boost->remaining>0) {
            // Diamond aura and speed streaks. The aura blinks through the last second,
            // like Mario's star running out.
            const bool blink=boost->remaining<1&&std::fmod(boost->remaining*8,1.0f)<0.5f;
            if(!blink) Glow(r,x+22,y+30,58,Alpha(DIAMOND,80));
            if(std::abs(body.velocity.x)>1) {
                const float behind=body.velocity.x>0?-1.0f:1.0f;
                for(int i=0;i<3;++i) Line(r,x+22+behind*28,y+18+i*12,x+22+behind*(52-i*8),y+18+i*12,Alpha(DIAMOND,170),2);
            }
        }
        if(boost->stomped>0) Text(r,x-18,y-52,"STOMPED!",TEXT,1.25f);
    }
    if(knocked) {
        // Sparks off both helmet corners; the label sits beside, since a ledge is overhead.
        for(float sx:{x+5,x+39}) for(int i=0;i<6;++i) {
            const float a=TAU*i/6;
            Line(r,sx+std::cos(a)*3.6f,y+3+std::sin(a)*3.6f,sx+std::cos(a)*9,y+3+std::sin(a)*9,TEXT,2);
        }
        Text(r,x+50,y+8,"BONK!",TEXT,1.25f);
    }
    if(race.protection>0) { Glow(r,x+22,y+30,46,Alpha(CYAN,50)); Ring(r,x+22,y+30,42,Alpha(CYAN,200),2); }
    // Small courier automaton: porcelain casing, enamel helmet, glowing core.
    Line(r,x+14,y+43,x+12-stride,y+56,limb,9); Line(r,x+30,y+43,x+32+stride,y+56,limb,9);
    Round(r,x+4-stride,y+53,17,7,3,c); Round(r,x+24+stride,y+53,17,7,3,c);
    Round(r,x+5,y+22,34,26,8,INK); Round(r,x+8,y+23,28,21,7,shell);
    Glow(r,x+22,y+33,13,Alpha(c,110)); Circle(r,x+22,y+33,6,c); Circle(r,x+22,y+33,2.5f,TEXT);
    Line(r,x+5,y+27,x+1-stride/2,y+40,limb,7); Line(r,x+39,y+27,x+43+stride/2,y+40,limb,7);
    Circle(r,x+1-stride/2,y+41,4,c); Circle(r,x+43+stride/2,y+41,4,c);
    const float tail=visual.flipX?1:-1;
    Poly(r,{{x+22,y+22},{x+22+tail*31,y+17+stride/2},{x+22+tail*25,y+27}},c);
    Round(r,x+4,y,36,26,10,INK); Round(r,x+6,y+1,32,22,9,c);
    Round(r,x+8,y+9,28,12,5,INK);
    const float eye=visual.flipX?12.0f:23.0f;
    Additive(r,[&] { Round(r,x+eye-2,y+10,15,9,4,Alpha(CYAN,90)); });
    Round(r,x+eye,y+12,4,5,1,{200,255,250,255}); Round(r,x+eye+7,y+12,4,5,1,{200,255,250,255});
    Line(r,x+13,y+4,x+25,y+4,{255,255,255,120},2);
    Circle(r,x+3,y+14,4,GOLD); Circle(r,x+41,y+14,4,GOLD);
    const std::string label="P"+std::to_string(OwnerOf(o)+1)+(mine?" / YOU":"");
    const float width=label.size()*8.0f+12;
    Round(r,x+21-width/2,y-31,width+2,21,7,mine?c:LINE);
    Round(r,x+22-width/2,y-30,width,19,6,INK);
    Text(r,x+28-width/2,y-25,label,TEXT,1);
    if(mine) Poly(r,{{x+18,y-9},{x+26,y-9},{x+22,y-5}},c);
}
}

void Draw(World &world,SDL_Renderer *r,const Camera &cam,int owner,bool connected,bool practice,double seconds,float respawnFlash,bool peer) {
    SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_BLEND);
    Background(r,cam,seconds);
    auto *me=Player(world,owner);
    const Racer *race=me?me->Get<Racer>():nullptr;
    const int checkpoint=race?race->checkpoint:0;
    const float signs[]={70,2020,3860,5540};
    const char *names[]={"THE FOUNDRY","GEARWORKS","HIGH VOLTAGE","LAST SHIFT"};
    for(int i=0;i<4;++i) {
        const float x=signs[i]-cam.position.x;
        if(x>-400&&x<1600) {
            const std::string district="DISTRICT 0"+std::to_string(i+1);
            Text(r,x+2,272,district,INK,1.25f); Text(r,x,270,district,MUTED,1.25f);
            Text(r,x+3,296,names[i],INK,2.5f); Text(r,x,293,names[i],TEXT,2.5f);
            Line(r,x,330,x+54,330,CYAN,3);
            Circle(r,x+61,330,2,CYAN);
        }
    }
    world.Each<Transform,PathMover>([&](GameObject &o,Transform &t,PathMover &p) {
        if(o.Has<Hazard>()||p.pattern==PathMover::Pattern::Circular) return;
        const float x=p.origin.x+t.size.x/2-cam.position.x,y=p.origin.y+t.size.y/2;
        Line(r,x,y,x+p.offset.x,y+p.offset.y,Alpha(GOLD,60),2);
        Circle(r,x,y,4,Alpha(GOLD,160)); Circle(r,x+p.offset.x,y+p.offset.y,4,Alpha(GOLD,160));
    });
    world.Each<Transform,Renderable>([&](GameObject &o,Transform &t,Renderable &visual) {
        if(!visual.visible||o.Has<Racer>()) return;
        const float x=t.Left()-cam.position.x,y=t.Top()-cam.position.y;
        if(x+t.size.x<-150||x>1750) return;
        if(const auto *h=o.Get<Hazard>()) { DrawHazard(r,t,*h,x,y); return; }
        if(o.Has<Finish>()) {
            for(float pole:{x,x+t.size.x-8}) {
                Rect(r,pole,y,8,t.size.y,LINE);
                Glow(r,pole+4,y,24,Alpha(GOLD,100)); Circle(r,pole+4,y,8,GOLD);
            }
            for(int xx=0;xx<10;++xx) for(int yy=0;yy<2;++yy)
                Rect(r,x+xx*17,y+10+yy*17,17,17,(xx+yy)%2?INK:TEXT);
            Glow(r,x+85,y-26,90,Alpha(GOLD,50));
            Round(r,x+19,y-40,132,29,8,GOLD); Text(r,x+37,y-33,"FINISH",INK,2);
            return;
        }
        Platform(r,o,t,x,y);
        if(const auto *potion=o.Get<PotionPlatform>(); potion&&potion->opened&&!potion->taken) {
            const Transform area=PotionArea(t);
            DrawPotion(r,area.Left()-cam.position.x,area.Top()-cam.position.y,seconds);
        }
    });
    world.Each<Racer,Transform,Renderable>([&](GameObject &o,Racer &rr,Transform &t,Renderable &visual) {
        const float x=t.Left()-cam.position.x,y=t.Top()-cam.position.y;
        if(x<-80||x>1680) return;
        // A boosted runner's body is larger; the same robot is drawn scaled to fill it.
        const float s=t.size.y/RUNNER_HEIGHT;
        SDL_SetRenderScale(r,s,s);
        Robot(r,o,rr,visual,x/s,y/s,OwnerOf(o)==owner);
        SDL_SetRenderScale(r,1,1);
    });

    // A compact glass instrument panel leaves most of the skyline visible.
    Panel(r,24,24,1552,111,18);
    Gear(r,63,63,22,static_cast<float>(seconds*0.22),CYAN,PANEL,12);
    Text(r,102,42,"CLOCKWORK DASH",TEXT,2.5f);
    Text(r,104,72,"THE SKYLINE CIRCUIT",MUTED,1.25f);
    Line(r,505,44,505,87,LINE);
    const SDL_Color status=connected||practice||peer?CYAN:PINK;
    Glow(r,537,51,11,Alpha(status,120)); Circle(r,537,51,4,status);
    Text(r,551,44,practice?"PRACTICE":peer?(connected?"P2P ONLINE":"P2P SOLO"):connected?"CS ONLINE":"DISCONNECTED",TEXT,1.5f);
    Text(r,537,73,"CHECKPOINT "+std::to_string(checkpoint)+" / 3",MUTED,1.25f);
    for(int i=0;i<3;++i) Circle(r,749+i*16,78,4,i<checkpoint?CYAN:LINE);
    const float boostLeft=me?me->Get<Boost>()->remaining:0;
    Text(r,820,40,"BOOST",MUTED,1.1f);
    Round(r,820,67,100,8,4,LINE);
    if(boostLeft>0) {
        Round(r,820,67,std::max(8.0f,100*boostLeft/BOOST_SECONDS),8,4,DIAMOND);
        char left[16]; std::snprintf(left,sizeof left,"%.1fs",boostLeft);
        Text(r,880,42,left,DIAMOND,1);
    }
    Text(r,967,40,"RUN TIME",MUTED,1.1f); Text(r,967,64,Time(race?race->elapsed:0),TEXT,2.5f);
    Text(r,1308,40,"RESPAWNS",MUTED,1.1f); Text(r,1308,65,me?std::to_string(me->Get<Respawn>()->deaths):"0",TEXT,2.5f);
    Text(r,1460,70,"NO LIMIT",MUTED,1);
    Rect(r,50,111,1500,3,LINE);
    if(me) {
        const float progress=1500*me->Get<Transform>()->Center().x/LEVEL_WIDTH;
        Additive(r,[&] { Rect(r,50,108,progress,9,Alpha(CYAN,40)); });
        Rect(r,50,111,progress,3,CYAN);
    }
    for(float cp:{2020.0f,3860.0f,5540.0f}) Circle(r,50+1500*cp/LEVEL_WIDTH,112,4,MUTED);
    world.Each<Racer,Transform,NetIdentity>([&](GameObject &,Racer &,Transform &t,NetIdentity &n) {
        const float x=50+1500*t.Center().x/LEVEL_WIDTH;
        Circle(r,x,112,n.owner==owner?7:5,TEXT); Circle(r,x,112,n.owner==owner?5:3,PlayerColor(n.owner));
    });

    Panel(r,24,837,1552,43,13);
    const char *keys[][2]={{"A/D","MOVE"},{"SPACE","JUMP"},{"R","RESTART"},{"ESC","EXIT"}};
    float kx=42;
    for(const auto &key:keys) {
        const float kw=std::strlen(key[0])*10.0f+14;
        Round(r,kx,847,kw,23,6,LINE);
        Text(r,kx+7,852,key[0],TEXT,1.25f);
        Text(r,kx+kw+10,852,key[1],MUTED,1.25f);
        kx+=kw+46+std::strlen(key[1])*10.0f;
    }
    std::vector<GameObject *> racers;
    world.Each<Racer>([&](GameObject &p,Racer &) { racers.push_back(&p); });
    std::sort(racers.begin(),racers.end(),[](GameObject *a,GameObject *b) {
        const auto &ra=*a->Get<Racer>(),&rb=*b->Get<Racer>();
        if(ra.finished!=rb.finished) return ra.finished;
        if(ra.finished) return ra.elapsed<rb.elapsed;
        if(ra.checkpoint!=rb.checkpoint) return ra.checkpoint>rb.checkpoint;
        return a->Get<Transform>()->Left()>b->Get<Transform>()->Left();
    });
    float rx=1080;
    for(size_t i=0;i<std::min<size_t>(4,racers.size());++i) {
        const int id=OwnerOf(*racers[i]);
        Circle(r,rx,858,5,PlayerColor(id));
        Text(r,rx+14,852,"P"+std::to_string(id+1)+"  "+(racers[i]->Get<Racer>()->finished?"DONE":"#"+std::to_string(i+1)),TEXT,1);
        rx+=122;
    }
    if(!race) { Panel(r,500,350,600,130,20); Text(r,580,400,"WAITING FOR THE COURSE...",TEXT,2); }
    if(race&&race->countdown>0) {
        Panel(r,580,305,428,190,24);
        Text(r,642,332,"WIND UP. TAKE OFF.",MUTED,2);
        Glow(r,794,421,72,Alpha(CYAN,60)); Ring(r,794,421,44,Alpha(CYAN,220),2);
        Text(r,775,397,std::to_string(static_cast<int>(std::ceil(race->countdown))),TEXT,6);
    }
    if(race&&race->finished) {
        Panel(r,480,300,640,240,24,Alpha(GOLD,120));
        Text(r,572,335,"SHIFT COMPLETE",GOLD,4);
        Text(r,650,395,Time(race->elapsed),CYAN,3);
        Text(r,576,460,"R  RACE AGAIN   /   ESC  EXIT",TEXT,1.5f);
        Text(r,600,500,"OTHER RUNNERS KEEP RACING",MUTED,1.25f);
    }
    if(respawnFlash>0) {
        Panel(r,580,158,440,48,15,Alpha(PINK,140));
        Glow(r,606,182,14,Alpha(PINK,120)); Circle(r,606,182,5,PINK); Text(r,627,174,"BACK TO CHECKPOINT",PINK,2);
    }
}
} // namespace clockwork
