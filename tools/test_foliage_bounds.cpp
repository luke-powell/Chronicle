#include "foliage_bounds.hpp"
#include <cassert>
#include <cstdio>
#include <random>
int main(){
    float m[16]{};m[0]=1.2f;m[5]=1.6f;m[11]=1;m[14]=.1f;
    foliage::Bounds crossing;float edge[]{1.1f,0,1};crossing.Add(edge,.5f,2);
    assert(crossing.Visible(m,224));
    foliage::Bounds behind;float back[]{0,0,-10};behind.Add(back,1,2);assert(!behind.Visible(m,224));
    foliage::Bounds tiny;float far[]{0,0,10000};tiny.Add(far,.5f,2);assert(!tiny.Visible(m,224));
    std::mt19937 rng(3491);std::uniform_real_distribution<float> unit(-1,1);
    unsigned retained=0,rejected=0;
    for(int trial=0;trial<20000;++trial){
        foliage::Bounds b;float root[]{unit(rng)*200000,unit(rng)*50000,unit(rng)*200000};
        float radius=1+std::abs(unit(rng))*500,height=2*radius;
        b.Add(root,radius,height);bool visible=b.Visible(m,224);visible?++retained:++rejected;
        // Every sampled visible vertex must keep its whole batch. No tree
        // density reduction or earlier fade may be hidden in the bounds test.
        for(int sample=0;sample<12;++sample){
            float p[3];for(int i=0;i<3;++i)p[i]=root[i]+unit(rng)*radius;
            float w=p[2],x=m[0]*p[0],y=m[5]*p[1];
            if(w>0&&std::abs(x)<=w&&std::abs(y)<=w&&height*224/std::max(w,1.f)>.15f)assert(visible);
        }
    }
    assert(retained&&rejected);
    std::printf("PASS conservative forest pages: %u retained, %u rejected; visible vertices preserved\n",retained,rejected);
}
