#pragma once
#include <chrono>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <deque>

// Immutable procedural results, shared across workers. A miss installs its
// future before computing, so adjacent chunks never repeat the same expensive
// query. Computation and waits happen outside the lock. FIFO eviction is bounded
// and never evicts in-flight entries; callers own copies of completed results.
template<class Key,class Value,size_t Capacity> class SharedMemo {
    std::mutex mutex;
    std::map<Key,std::shared_future<Value>> entries;
    std::deque<Key> order;
public:
    template<class F> Value Get(const Key &key,F compute) {
        std::shared_future<Value> future;
        std::shared_ptr<std::promise<Value>> producer;
        {
            std::lock_guard lock(mutex);
            auto it=entries.find(key);
            if(it!=entries.end())future=it->second;
            else {
                producer=std::make_shared<std::promise<Value>>();
                future=producer->get_future().share();
                entries.emplace(key,future);order.push_back(key);
                size_t attempts=order.size();
                while(entries.size()>Capacity && attempts--){
                    auto oldest=order.front();order.pop_front();
                    auto entry=entries.find(oldest);
                    if(entry->second.wait_for(std::chrono::seconds(0))==std::future_status::ready)entries.erase(entry);
                    else order.push_back(oldest);
                }
            }
        }
        if(producer){try{producer->set_value(compute());}catch(...){producer->set_exception(std::current_exception());}}
        return future.get();
    }
};
