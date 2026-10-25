#include "forest_disk_cache.hpp"
#include <cassert>
#include <cstdio>
int main(){
 auto dir=std::filesystem::temp_directory_path()/std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
 std::filesystem::create_directory(dir);auto path=dir/"sample.forest";
 std::vector<forest::Tree> trees{{1e12,-1e12,123.123,2.1,1.4,987654321,2}};
 forest::SavePopulation(path,"kernel-and-edits",trees);
 auto copy=forest::LoadPopulation(path,"kernel-and-edits");assert(copy&&copy->size()==1);
 assert(copy->front().x==trees.front().x&&copy->front().y==trees.front().y&&copy->front().id==trees.front().id);
 assert(!forest::LoadPopulation(path,"different-edits"));
 {std::fstream file(path,std::ios::in|std::ios::out|std::ios::binary);file.seekp(-1,std::ios::end);file.put(42);}
 assert(!forest::LoadPopulation(path,"kernel-and-edits"));
 forest::SavePopulation(path,"kernel-and-edits",trees);assert(forest::LoadPopulation(path,"kernel-and-edits"));
 std::filesystem::remove(path);std::filesystem::remove(dir);
 std::puts("PASS persistent populations preserve exact doubles, reject changed edits/corrupt data, and repair invalid cache files");
}
