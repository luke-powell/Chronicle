#include "terrain_kernel.hpp"
#include <cassert>
#include <cstdio>
int main(){
    const auto d=canyon::Generate(0,0);
    double datum=terrain::ContinentalLandHeight(0,0);
    int checked=0;double minimum=1e9,maximum=-1e9;
    for(int i=0;i<64;i++){
        double a=i*6.283185307179586/64,dx=std::cos(a),dz=std::sin(a),lo=1000,hi=25000;
        for(int j=0;j<40;j++){double r=(lo+hi)/2;if(canyon::Outline(r*dx,r*dz,d.seed)<0)lo=r;else hi=r;}
        double r=(lo+hi)/2;
        // Both the external apron and the canyon floors share ground level.
        assert(std::abs(terrain::LandHeight((r+100)*dx,(r+100)*dz)-datum)<.05);
        // Where no channel crosses the district's perimeter, the mesa stands
        // above that datum and returns to it through an exterior cliff face.
        auto top=canyon::At((r-400)*dx,(r-400)*dz);
        if(canyon::Profile(canyon::WallDistance((r-400)*dx,(r-400)*dz))>199){assert(terrain::LandHeight((r-400)*dx,(r-400)*dz)>datum+150);minimum=std::min(minimum,top.height);maximum=std::max(maximum,top.height);checked++;}
    }
    assert(checked>32);assert(minimum>150&&maximum<270&&maximum-minimum>30);
    double last=90;
    for(size_t i=1;i<canyon::ProfileCuts.size();i++){
        double angle=std::atan2(canyon::ProfileHeights[i]-canyon::ProfileHeights[i-1],canyon::ProfileCuts[i]-canyon::ProfileCuts[i-1])*180/3.141592653589793;
        assert(angle<last);last=angle;
    }
    assert(last<15);
    std::printf("PASS %d raised mesa perimeter sections; ground-level external apron; progressively rounded polygon crown (last face %.2f degrees)\n",checked,last);
}
