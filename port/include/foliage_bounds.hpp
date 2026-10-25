#pragma once
#include <algorithm>
#include <array>
#include <cmath>
namespace foliage {
struct Bounds {
    std::array<float,3> lo{1e30f,1e30f,1e30f},hi{-1e30f,-1e30f,-1e30f};
    float height=0;
    void Add(const float *p,float radius,float h) {
        for(int i=0;i<3;++i){lo[i]=std::min(lo[i],p[i]-radius);hi[i]=std::max(hi[i],p[i]+radius);}
        height=std::max(height,h);
    }
    bool Visible(const float *m,float pixelsPerHeight,float minimumPixels=.15f) const {
        float c[3],e[3],clip[4]{},reach[4]{};
        for(int i=0;i<3;++i){c[i]=(lo[i]+hi[i])*.5f;e[i]=(hi[i]-lo[i])*.5f;}
        for(int row=0;row<4;++row){clip[row]=m[12+row];for(int i=0;i<3;++i){clip[row]+=m[i*4+row]*c[i];reach[row]+=std::abs(m[i*4+row])*e[i];}}
        if(clip[3]+reach[3]<=0)return false;
        // Same guard band as terrain: retain geometry for interpolated views.
        for(int axis=0;axis<2;++axis)for(float sign:{-1.f,1.f}){
            float r=0;for(int i=0;i<3;++i)r+=std::abs(1.2f*m[i*4+3]+sign*m[i*4+axis])*e[i];
            if(1.2f*clip[3]+sign*clip[axis]+r<0)return false;
        }
        // Only discard if EVERY vertex of EVERY tree has zero coverage in
        // the existing tree shader. Canopy already represents these trees.
        return height*pixelsPerHeight/std::max(clip[3]-reach[3],1.f)>minimumPixels;
    }
};
}
