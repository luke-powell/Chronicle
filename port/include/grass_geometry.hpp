#pragma once
#include <algorithm>
#include <array>
#include <map>
#include <numeric>
#include <vector>
#include <cmath>

namespace grass {
// Retail foliage contains disconnected cards spread across a whole scene.
// Weld by position (UV seams duplicate vertices), then extract each connected
// tuft. Keep its UVs and normals; anchor each tuft at its own ground contact.
template<class Source> std::vector<Source> Split(const std::vector<Source> &sources,float maximumHeight=8) {
    std::vector<Source> result;
    for(const auto &src:sources){
        std::vector<unsigned> parent(src.vertices.size());
        std::iota(parent.begin(),parent.end(),0);
        auto root=[&](unsigned n){while(parent[n]!=n){parent[n]=parent[parent[n]];n=parent[n];}return n;};
        auto join=[&](unsigned a,unsigned b){parent[root(a)]=root(b);};
        std::map<std::array<long long,3>,unsigned> welded;
        for(unsigned i=0;i<src.vertices.size();++i){
            auto &p=src.vertices[i].position;
            std::array<long long,3> key{std::llround(p[0]*10000),std::llround(p[1]*10000),std::llround(p[2]*10000)};
            auto [it,added]=welded.emplace(key,i);if(!added)join(i,it->second);
        }
        for(size_t i=0;i+2<src.indices.size();i+=3){join(src.indices[i],src.indices[i+1]);join(src.indices[i],src.indices[i+2]);}
        std::map<unsigned,Source> parts;
        for(auto i:src.indices){auto &part=parts[root(i)];part.texture=src.texture;part.indices.push_back(unsigned(part.vertices.size()));part.vertices.push_back(src.vertices[i]);}
        for(auto &[id,part]:parts){
            std::array<float,3> lo{1e30f,1e30f,1e30f},hi{-1e30f,-1e30f,-1e30f};
            for(auto &v:part.vertices)for(int k=0;k<3;++k){lo[k]=std::min(lo[k],v.position[k]);hi[k]=std::max(hi[k],v.position[k]);}
            float width=std::max(hi[0]-lo[0],hi[2]-lo[2]);
            if(hi[1]-lo[1]<.01f)continue;
            float scale=std::min({1.f,width>0?12.f/width:1.f,maximumHeight/(hi[1]-lo[1])});
            for(auto &v:part.vertices){v.position[0]=(v.position[0]-(lo[0]+hi[0])*.5f)*scale;v.position[1]=(v.position[1]-lo[1])*scale;v.position[2]=(v.position[2]-(lo[2]+hi[2])*.5f)*scale;}
            result.push_back(std::move(part));
        }
    }
    return result;
}
// Same diagonal as terrain indices {a,c,b,b,c,d}; bilinear sampling floats
// above/below the actual mesh on a non-planar quad.
inline double TriangleHeight(double a,double b,double c,double d,double u,double v){
    return u+v<=1?a*(1-u-v)+b*u+c*v:b*(1-v)+c*(1-u)+d*(u+v-1);
}
}
