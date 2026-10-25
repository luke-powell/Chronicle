#include "terrain_kernel.hpp"
#include <fstream>
#include <cstdio>
int main(){
 auto &shell=canyon::TownShell();std::ifstream input("port/build/norune-shell.bin",std::ios::binary);
 if(!input.read(reinterpret_cast<char *>(shell.heights.data()),sizeof(shell.heights)))return 1;
 canyon::RestoreShellRidges();
 std::ofstream out("port/build/slot-canyon-map.csv");out<<"x,z,height\n";
 for(int z=-21000;z<=21000;z+=48)for(int x=-24000;x<=26000;x+=48)out<<x<<','<<z<<','<<terrain::LandHeight(x,z)<<'\n';
 std::puts("Preview sampled from production terrain kernel with retail cliff shell");
}
