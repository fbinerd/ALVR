#pragma once
// A bounded four-plane CPU normalizer, owned by the online delivery worker.
// Never used on an application's frame thread. Each job writes disjoint bytes.
#include <array>
#include <barrier>
#include <functional>
#include <thread>
#include <utility>

namespace mhl::depth {
class PlaneWorkers {
    std::barrier<> start{4},finish{4};
    std::array<std::thread,3> workers;
    std::function<bool(unsigned)> job;
    std::array<int,4> success{};
    bool stopping=false; // Published by the start barrier.
    void execute(unsigned i) noexcept {
        try{success[i]=job(i);}catch(...){success[i]=0;}
    }
public:
    PlaneWorkers(){
        unsigned created=0;
        try{
            for(;created<workers.size();++created){
                workers[created]=std::thread([this,i=created+1]{
                    for(;;){start.arrive_and_wait();if(stopping)return;
                        execute(i);finish.arrive_and_wait();}
                });
            }
        }catch(...){
            stopping=true;
            for(unsigned missing=created;missing<workers.size();++missing)start.arrive_and_drop();
            start.arrive_and_wait();
            for(auto& worker:workers)if(worker.joinable())worker.join();
            throw;
        }
    }
    ~PlaneWorkers(){stopping=true;start.arrive_and_wait();for(auto& worker:workers)worker.join();}
    PlaneWorkers(PlaneWorkers const&)=delete;
    template<class F>bool normalize(F&& work){
        job=std::forward<F>(work);start.arrive_and_wait();execute(0);finish.arrive_and_wait();
        bool ok=true;for(auto value:success)ok=ok&&value;
        job={};return ok;
    }
};
}
