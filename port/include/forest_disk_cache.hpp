#pragma once
#include "forest_kernel.hpp"
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <thread>
#include <atomic>

namespace forest {
inline void TrimPopulationCache(const std::filesystem::path &root);
inline uint64_t CacheHash(const void *data,size_t size){uint64_t h=14695981039346656037ull;auto p=static_cast<const unsigned char *>(data);while(size--){h^=*p++;h*=1099511628211ull;}return h;}
inline std::optional<std::vector<Tree>> LoadPopulation(const std::filesystem::path &path,const std::string &key){
    std::ifstream file(path,std::ios::binary);std::array<uint64_t,4> header{};
    if(!file.read(reinterpret_cast<char *>(header.data()),sizeof(header)) || header[0]!=0x3245524f464344ull || header[1]!=key.size() || header[2]>2000000)return {};
    std::string stored(key.size(),'\0');if(!file.read(stored.data(),stored.size())||stored!=key)return {};
    std::vector<Tree> trees(header[2]);
    if(!file.read(reinterpret_cast<char *>(trees.data()),trees.size()*sizeof(Tree))||file.peek()!=std::char_traits<char>::eof())return {};
    if(CacheHash(trees.data(),trees.size()*sizeof(Tree))!=header[3])return {};
    for(auto &t:trees)if(!std::isfinite(t.x)||!std::isfinite(t.y)||!std::isfinite(t.z)||!std::isfinite(t.yaw)||!std::isfinite(t.scale)||t.scale<=0||t.species<0||t.species>6)return {};
    return trees;
}
inline void SavePopulation(const std::filesystem::path &path,const std::string &key,const std::vector<Tree> &trees){
    auto temporary=path;temporary+=std::string(".")+std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id()))+".tmp";
    std::ofstream file(temporary,std::ios::binary|std::ios::trunc);if(!file)return;
    std::array<uint64_t,4> header{0x3245524f464344ull,key.size(),trees.size(),CacheHash(trees.data(),trees.size()*sizeof(Tree))};
    file.write(reinterpret_cast<const char *>(header.data()),sizeof(header));file.write(key.data(),key.size());file.write(reinterpret_cast<const char *>(trees.data()),trees.size()*sizeof(Tree));
    bool valid=bool(file);file.close();std::error_code error;
    if(valid){std::filesystem::rename(temporary,path,error);
        if(error&&std::filesystem::exists(path)){std::filesystem::remove(path,error);if(!error)std::filesystem::rename(temporary,path,error);}
    }
    if(!valid||error)std::filesystem::remove(temporary,error);
    static std::atomic<unsigned> writes=0;if(valid&&++writes%128==0)TrimPopulationCache(path.parent_path());
}
// Only derived .forest files immediately within this directory are eligible.
// Keep disk use bounded across world travel and generator revisions.
inline void TrimPopulationCache(const std::filesystem::path &root){
    std::error_code error;std::filesystem::create_directories(root,error);if(error)return;
    std::vector<std::pair<std::filesystem::file_time_type,std::filesystem::path>> files;uintmax_t total=0;
    for(std::filesystem::directory_iterator it(root,error),end;!error&&it!=end;it.increment(error)){
        if(!it->is_regular_file(error)||it->path().extension()!=".forest")continue;
        auto bytes=it->file_size(error);if(error)break;total+=bytes;files.emplace_back(it->last_write_time(error),it->path());
    }
    std::sort(files.begin(),files.end());
    for(auto &[time,path]:files){if(total<=512ull*1024*1024)break;auto bytes=std::filesystem::file_size(path,error);if(!error&&std::filesystem::remove(path,error))total-=bytes;}
}
}
