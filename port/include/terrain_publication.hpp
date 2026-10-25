#pragma once
#include "terrain_kernel.hpp"
#include <set>
#include <numeric>
#include <optional>
namespace terrain {
template<class Key,class Edges> std::vector<Key> AdaptLocal(const std::vector<Key> &active,const std::vector<Key> &target,double x,double z,Edges edges){
    Selection current;for(auto &k:active)current.emplace(k.first,0);
    std::optional<Tile> parent;double nearest=8000;int finestTarget=MaxLevel+1;
    for(auto &k:target){auto t=k.first;double distance=Distance(t,x,z);
        if(distance>=8000 || t.level>finestTarget || (t.level==finestTarget && distance>=nearest))continue;
        double wx=t.left()+t.size()/2,wz=t.top()+t.size()/2;int level=LevelAt(current,wx,wz);
        if(level<=t.level)continue;
        double size=std::ldexp(BaseSize,level);parent=Tile{int64_t(std::floor(wx/size)),int64_t(std::floor(wz/size)),level};nearest=distance;finestTarget=t.level;
    }
    std::optional<Tile> merge;
    const bool fixedBudget=active.size()==target.size();
    if(fixedBudget || !parent || active.size()>target.size()+128){
        Selection wanted;for(auto &k:target)wanted.emplace(k.first,0);
        double farthest=-1;
        for(auto &k:active){auto t=k.first;if(t.level>=MaxLevel)continue;
            Tile p{int64_t(std::floor(t.x/2.)),int64_t(std::floor(t.z/2.)),t.level+1};
            bool allowed=true;
            for(int dz=0;dz<2;++dz)for(int dx=0;dx<2;++dx){Tile child{p.x*2+dx,p.z*2+dz,t.level};
                int level=LevelAt(wanted,child.left()+child.size()/2,child.top()+child.size()/2);
                if(!current.contains(child)||(level>=0&&level<p.level))allowed=false;
            }
            // A centre sample alone misses a finer target in a child's
            // corner. Such a merge would undo pending near refinement.
            if(allowed)for(const auto &wantedKey:target){auto w=wantedKey.first;
                if(w.level<p.level && w.left()<p.left()+p.size() && w.left()+w.size()>p.left() &&
                   w.top()<p.top()+p.size() && w.top()+w.size()>p.top()){allowed=false;break;}
            }
            double distance=Distance(p,x,z);
            // Retire the cheapest fine tiles behind travel first. Pairing a
            // near refinement with a huge distant forest rebuild starves the
            // player of detail even when all the near meshes are ready.
            if(allowed && (!merge || p.level<merge->level || (p.level==merge->level && distance>farthest))){merge=p;farthest=distance;}
        }
    }
    // A moving ring transfers a tile budget from its trailing edge to its
    // leading edge. Publish a coarsening and refinement together, never grow
    // the rendered tile count while waiting for the horizon.
    if(fixedBudget && (!merge || !parent))return {};
    if(merge){
        for(int dz=0;dz<2;++dz)for(int dx=0;dx<2;++dx)current.erase(Tile{merge->x*2+dx,merge->z*2+dz,merge->level-1});
        current.emplace(*merge,0);
    }
    if(parent && (!merge || fixedBudget)){
        current.erase(*parent);
        for(int dz=0;dz<2;++dz)for(int dx=0;dx<2;++dx)current.emplace(Tile{parent->x*2+dx,parent->z*2+dz,parent->level-1},0);
    }
    if(!merge && !parent)return {};
    std::vector<Key> result;for(auto &[t,value]:current)result.emplace_back(t,edges(t,current));return result;
}
template<class Key> struct Publication {std::vector<Key> before,after;};
// Replace independent changed regions atomically. Unchanged tiles separate the
// regions; touching changed tiles stay together so stitching never sees half
// of a refinement/coarsening transition. No skirts or altered geometry needed.
template<class Key> std::vector<Publication<Key>> Publications(const std::vector<Key> &before,const std::vector<Key> &after){
    std::set<Key> old(before.begin(),before.end()),next(after.begin(),after.end());
    std::vector<std::pair<Key,bool>> changed;
    for(auto &k:before)if(!next.contains(k))changed.emplace_back(k,false);
    for(auto &k:after)if(!old.contains(k))changed.emplace_back(k,true);
    std::vector<size_t> parent(changed.size());std::iota(parent.begin(),parent.end(),0);
    auto root=[&](size_t i){while(parent[i]!=i){parent[i]=parent[parent[i]];i=parent[i];}return i;};
    for(size_t i=0;i<changed.size();++i)for(size_t j=0;j<i;++j){
        auto a=changed[i].first.first,b=changed[j].first.first;
        double dx=std::min(a.left()+a.size(),b.left()+b.size())-std::max(a.left(),b.left());
        double dz=std::min(a.top()+a.size(),b.top()+b.size())-std::max(a.top(),b.top());
        if(dx>=0&&dz>=0)parent[root(i)]=root(j);
    }
    std::map<size_t,Publication<Key>> groups;
    for(size_t i=0;i<changed.size();++i){auto &g=groups[root(i)];(changed[i].second?g.after:g.before).push_back(changed[i].first);}
    std::vector<Publication<Key>> result;for(auto &[id,g]:groups)result.push_back(std::move(g));return result;
}
}
