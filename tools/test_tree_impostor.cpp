#include "tree_impostor.hpp"
#include <cassert>
#include <cstdio>
#include <string>
struct Vertex {float position[3]{},uv[2]{};};
struct Source {std::vector<Vertex> vertices;std::vector<unsigned> indices;std::string texture;};
int main(){
    Source cube;cube.texture="native";
    for(int i=0;i<8;++i){Vertex v;v.position[0]=(i&1)?1:-1;v.position[1]=(i&2)?2:0;v.position[2]=(i&4)?1:-1;cube.vertices.push_back(v);}
    cube.indices={0,1,2,2,1,3,4,6,5,6,7,5,0,4,1,1,4,5,2,3,6,6,3,7,0,2,4,4,2,6,1,5,3,3,5,7};
    auto atlas=impostor::Bake(std::vector<Source>{cube},[](const auto &name,float,float){assert(name=="native");return std::array<uint8_t,4>{20,80,30,128};});
    assert(atlas.centre==1&&atlas.radius>1.73f&&atlas.radius<1.81f);
    for(int row=0;row<impostor::Elevations;++row)for(int col=0;col<impostor::Angles;++col){
        unsigned covered=0;
        for(int y=0;y<impostor::Cell;++y)for(int x=0;x<impostor::Cell;++x){
            auto p=((row*impostor::Cell+y)*impostor::Width+col*impostor::Cell+x)*4;
            if(atlas.pixels[p+3]){++covered;assert(atlas.pixels[p]==20&&atlas.pixels[p+1]==80&&atlas.pixels[p+2]==30&&atlas.pixels[p+3]==128);}
        }
        assert(covered>100&&covered<impostor::Cell*impostor::Cell);
    }
    auto transparent=impostor::Bake(std::vector<Source>{cube},[](const auto &,float,float){return std::array<uint8_t,4>{255,0,0,0};});
    assert(std::all_of(transparent.pixels.begin(),transparent.pixels.end(),[](auto v){return v==0;}));
    std::puts("PASS 24 native-texture views, stable world bounds, texture colors and alpha-cutout rasterization");
}
