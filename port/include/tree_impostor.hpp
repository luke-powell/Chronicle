#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>
namespace impostor {
constexpr int Cell=64,Angles=8,Elevations=3,Width=Cell*Angles,Height=Cell*Elevations;
struct Atlas {float centre=0,radius=1;std::vector<uint8_t> pixels;};
// Orthographic captures of the original textured triangles. All views share
// the same physical bounds, so changing view cannot change a tree's scale.
template<class Sources,class Sample> Atlas Bake(const Sources &sources,Sample sample){
    Atlas out;float low=1e30f,high=-1e30f;
    for(auto &s:sources)for(auto &v:s.vertices){low=std::min(low,v.position[1]);high=std::max(high,v.position[1]);}
    out.centre=(low+high)*.5f;
    for(auto &s:sources)for(auto &v:s.vertices)out.radius=std::max(out.radius,std::hypot(std::hypot(v.position[0],v.position[1]-out.centre),v.position[2]));
    out.radius*=1.04f;out.pixels.resize(Width*Height*4);
    for(int elevation=0;elevation<Elevations;++elevation)for(int angle=0;angle<Angles;++angle){
        float yaw=angle*6.28318530718f/Angles,pitch=elevation*1.57079632679f/(Elevations-1);
        std::array<float,3> right{std::cos(yaw),0,-std::sin(yaw)},
            up{-std::sin(yaw)*std::sin(pitch),std::cos(pitch),-std::cos(yaw)*std::sin(pitch)},
            toward{std::sin(yaw)*std::cos(pitch),std::sin(pitch),std::cos(yaw)*std::cos(pitch)};
        std::array<float,Cell*Cell> depth;depth.fill(-1e30f);
        for(auto &s:sources)for(size_t k=0;k+2<s.indices.size();k+=3){
            float x[3]{},y[3]{},z[3]{};
            for(int j=0;j<3;++j){const auto &v=s.vertices[s.indices[k+j]];
                for(int axis=0;axis<3;++axis){float p=v.position[axis]-(axis==1?out.centre:0);x[j]+=p*right[axis];y[j]+=p*up[axis];z[j]+=p*toward[axis];}
                x[j]=(x[j]/out.radius*.5f+.5f)*Cell;y[j]=(.5f-y[j]/out.radius*.5f)*Cell;
            }
            float area=(y[1]-y[2])*(x[0]-x[2])+(x[2]-x[1])*(y[0]-y[2]);if(std::abs(area)<1e-6f)continue;
            int x0=std::clamp(int(std::floor(std::min({x[0],x[1],x[2]}))),0,Cell-1),x1=std::clamp(int(std::ceil(std::max({x[0],x[1],x[2]}))),0,Cell-1);
            int y0=std::clamp(int(std::floor(std::min({y[0],y[1],y[2]}))),0,Cell-1),y1=std::clamp(int(std::ceil(std::max({y[0],y[1],y[2]}))),0,Cell-1);
            for(int py=y0;py<=y1;++py)for(int px=x0;px<=x1;++px){
                float a=((y[1]-y[2])*(px+.5f-x[2])+(x[2]-x[1])*(py+.5f-y[2]))/area;
                float b=((y[2]-y[0])*(px+.5f-x[2])+(x[0]-x[2])*(py+.5f-y[2]))/area,c=1-a-b;
                if(std::min({a,b,c})<0)continue;
                float d=a*z[0]+b*z[1]+c*z[2];if(d<depth[py*Cell+px])continue;
                const auto &v0=s.vertices[s.indices[k]],&v1=s.vertices[s.indices[k+1]],&v2=s.vertices[s.indices[k+2]];
                auto rgba=sample(s.texture,a*v0.uv[0]+b*v1.uv[0]+c*v2.uv[0],a*v0.uv[1]+b*v1.uv[1]+c*v2.uv[1]);
                if(rgba[3]<=32)continue;
                depth[py*Cell+px]=d;size_t dest=((elevation*Cell+py)*Width+angle*Cell+px)*4;
                for(int channel=0;channel<4;++channel)out.pixels[dest+channel]=rgba[channel];
            }
        }
    }
    return out;
}
}
