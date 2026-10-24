// Offline cross-process replay of canonical packets through real GPU FDs.
// No SteamVR, dashboard, game, mock, ADB or headset is started.
#include "platform/linux/depth/gpu_transport.hpp"
#include "platform/linux/depth/GameDepthSource.hpp"
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <spawn.h>
#include <stdexcept>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
extern char** environ;
namespace fs=std::filesystem;
using namespace mhl::depth;
using Clock=std::chrono::steady_clock;
struct Frame{MhlDepthPacket p{};std::vector<uint8_t> bytes;};
void require(bool test,char const* reason){if(!test)throw std::runtime_error(reason);}
std::vector<fs::path> files(fs::path root){std::vector<fs::path> out;for(auto const& e:fs::directory_iterator(root))if(e.is_directory()&&e.path().filename().string().starts_with("frame_"))out.push_back(e.path());std::sort(out.begin(),out.end());return out;}
Frame read(fs::path root){
    Frame f;std::ifstream h(root/"packet.bin",std::ios::binary);require(bool(h.read(reinterpret_cast<char*>(&f.p),sizeof(f.p))),"packet header truncated");require(h.peek()==EOF,"header trailing bytes");
    if(auto error=mhl_depth_validate(&f.p,sizeof(f.p)))throw std::runtime_error(error);
    require(f.p.buffer_size<=256ull*1024*1024,"packet too large");f.bytes.resize(f.p.buffer_size);std::ifstream b(root/"payload.bin",std::ios::binary);require(bool(b.read(reinterpret_cast<char*>(f.bytes.data()),f.bytes.size())),"payload truncated");require(b.peek()==EOF,"payload trailing bytes");return f;
}
Query query(Frame const& f,bool aligned=false){
    Query q;q.targetTimestampNs=f.p.frame_id;q.displayMonotonicNs=f.p.display_time_ns;q.producerId=f.p.producer_id;q.clockMapped=q.fullStereoLayout=true;
    q.requireTrackingAligned=aligned;q.poseSpace=f.p.pose_space;
    for(unsigned e=0;e<2;e++){q.poses[e]=f.p.eyes[e].pose;std::copy_n(f.p.eyes[e].fov,4,q.fov[e].begin());}return q;
}
std::vector<uint8_t> stereo(Frame const& f){
    auto const& l=f.p.eyes[0].depth;auto const& r=f.p.eyes[1].depth;require(l.width==r.width&&l.height==r.height,"unequal eyes");std::vector<uint8_t> out(size_t(l.width)*2*l.height*4);
    for(unsigned y=0;y<l.height;y++)for(unsigned e=0;e<2;e++){auto const& p=f.p.eyes[e].depth;memcpy(out.data()+(size_t(y)*2*l.width+e*l.width)*4,f.bytes.data()+p.offset+y*p.row_stride,l.width*4);}return out;
}
void pollDelay(){std::this_thread::sleep_for(std::chrono::milliseconds(1));}
int produce(fs::path root,std::string socket,std::string gpu,std::string mode){
    OwnedDevice device(gpu);Producer producer(device.get(),socket);auto frames=files(root);size_t count=mode=="negative"?4:mode=="binding"?1:frames.size();
    for(size_t i=0;i<count;i++){
        auto f=read(frames[i]);if(mode=="binding"){f.p.flags|=MHL_TRACKING_ALIGNED;f.p.pose_space=MHL_SPACE_TRACKING;f.p.source_tag=MHL_SOURCE_SYNTHETIC;}auto end=Clock::now()+std::chrono::seconds(15);
        while(!producer.send(f.p,f.bytes.data(),f.bytes.size())){require(Clock::now()<end,"producer timeout/backpressure");pollDelay();}
    }
    auto end=Clock::now()+std::chrono::seconds(15);while(producer.counters().acknowledged<count){producer.poll();require(Clock::now()<end,"ACK timeout");pollDelay();}
    std::cout<<"producer sent="<<producer.counters().sent<<" acknowledged="<<producer.counters().acknowledged<<'\n';return 0;
}
pid_t child(std::string exe,fs::path root,std::string socket,std::string gpu,std::string mode){
    std::string input=root.string();
    char* args[]={exe.data(),const_cast<char*>("--producer"),input.data(),socket.data(),gpu.data(),mode.data(),nullptr};pid_t pid{};
    int error=posix_spawn(&pid,exe.c_str(),nullptr,nullptr,args,environ);if(error)throw std::runtime_error("spawn failed");return pid;
}
void reap(pid_t pid){int status{};auto end=Clock::now()+std::chrono::seconds(20);for(;;){auto got=waitpid(pid,&status,WNOHANG);if(got==pid){require(WIFEXITED(status)&&WEXITSTATUS(status)==0,"producer failed");return;}require(got>=0&&Clock::now()<end,"producer exit timeout");pollDelay();}}
struct ExactEncoderHook {
    using Depth=alvr::motion::NativeMotion::Depth;
    using DepthSource=std::function<std::optional<Depth>(uint64_t)>;
    DepthSource source;void setDepthSource(DepthSource s){source=std::move(s);}
};
int main(int argc,char** argv){
    try {
        if(argc==7&&std::string(argv[1])=="--producer")return produce(argv[2],argv[3],argv[4],std::string(argv[5]));
        // argv producer contains six tokens including executable.
        if(argc==6&&std::string(argv[1])=="--producer")return produce(argv[2],argv[3],argv[4],std::string(argv[5]));
        require(argc==4,"Usage: depth_transport_test RECORDING NEW_OUTPUT GPU_NAME_SUBSTRING");
        fs::path input=fs::absolute(argv[1]),output=fs::absolute(argv[2]);require(!fs::exists(output),"fresh output required");fs::create_directories(output);
        auto paths=files(input);require(paths.size()>=60,"need >=60 recorded frames");OwnedDevice device(argv[3]);
        std::string socket=(output/"depth.sock").string(),flag=(output/"enabled").string();
        auto logger=[](std::string const& line){std::cout<<line<<'\n';};
        Receiver receiver(device.get(),socket,flag,logger);auto first=read(paths[0]);auto q=query(first);
        require(!receiver.acquire(q)&&!fs::exists(socket),"off default opened socket");std::ofstream(flag)<<"1\n";
        auto end=Clock::now()+std::chrono::seconds(3);while(!fs::exists(socket)){receiver.acquire(q);require(Clock::now()<end,"enable switch timeout");pollDelay();}
        auto producer=child(fs::absolute(argv[0]).string(),input,socket,argv[3],"positive");
        std::vector<uint8_t> previous;uint64_t previousId=0,total=0;
        for(auto const& path:paths){auto f=read(path);auto expected=stereo(f);auto request=query(f);std::optional<Image> got;
            end=Clock::now()+std::chrono::seconds(15);while(!(got=receiver.acquire(request))){require(Clock::now()<end,"receiver frame timeout");pollDelay();}
            require(got->frameId==f.p.frame_id&&got->targetTimestampNs==request.targetTimestampNs,"wrong frame stamp");
            require(receiver.readDepthForTest()==expected,"depth SBS differs from canonical recording");
            require(receiver.readAllocationForTest()==f.bytes,"imported allocation differs from canonical recording");
            if(previousId)require(receiver.readDepthForTest(previousId)==previous,"previous depth overwritten before next present returned");
            previous=std::move(expected);previousId=f.p.frame_id;total+=f.bytes.size();
        }
        receiver.acquire(q);reap(producer);require(receiver.counters().accepted==paths.size(),"accepted count mismatch");
        std::cout<<"BYTE_EXACT frames="<<paths.size()<<" allocation_bytes="<<total<<" previous_image_lifetime=PASS\n";
        // Negative matching cases use a separate receiver/socket connection.
        std::string ns=(output/"negative.sock").string();Receiver negative(device.get(),ns,flag,logger);negative.acquire(q);auto bad=child(fs::absolute(argv[0]).string(),input,ns,argv[3],"negative");
        for(unsigned i=0;i<4;i++){
            auto f=read(paths[i]);auto wrong=query(f);if(i==0)wrong.displayMonotonicNs++;if(i==1)wrong.fov[0][0]+=.01f;if(i==2)wrong.poses[0].position[0]+=.01f;if(i==3)wrong.clockMapped=false;
            end=Clock::now()+std::chrono::seconds(15);while(negative.counters().rejected<=i){require(!negative.acquire(wrong),"invalid query accepted");require(Clock::now()<end,"negative case timeout");pollDelay();}
        }
        reap(bad);auto c=negative.counters();require(c.timestamp&&c.geometry&&c.pose&&c.clock&&!c.accepted,"strict match counters failed");
        std::cout<<"NEGATIVE timestamp="<<c.timestamp<<" geometry="<<c.geometry<<" pose="<<c.pose<<" clock="<<c.clock<<'\n';
        ExactEncoderHook encoder;{
            alvr::depth::GameDepthSource binding(encoder,device.get(),(output/"binding.sock").string(),(output/"absent-switch").string(),[](uint64_t)->std::optional<Query>{return {};});
            require(bool(encoder.source),"hook not registered");require(!encoder.source(123),"unmapped/off callback accepted depth");
        }require(!encoder.source,"hook not detached");
        std::cout<<"ENCODER_HOOK signature/off-default/detach=PASS\n";
        {
            auto aligned=query(first,true);aligned.poseSpace=MHL_SPACE_TRACKING;
            std::string bs=(output/"binding-positive.sock").string();
            alvr::depth::GameDepthSource binding(encoder,device.get(),bs,flag,[aligned](uint64_t)->std::optional<Query>{return aligned;});
            encoder.source(aligned.targetTimestampNs);
            auto bp=child(fs::absolute(argv[0]).string(),input,bs,argv[3],"binding");
            std::optional<ExactEncoderHook::Depth> value;end=Clock::now()+std::chrono::seconds(15);
            while(!(value=encoder.source(aligned.targetTimestampNs))){require(Clock::now()<end,"positive hook timeout");pollDelay();}
            require(bool(value->image)&&value->layout==vk::ImageLayout::eShaderReadOnlyOptimal,"positive hook image invalid");
            require(value->extent.width==first.p.eyes[0].depth.width*2,"positive hook extent invalid");
            // Poll once the copy completes to release the producer lease.
            for(unsigned i=0;i<20;i++){encoder.source(aligned.targetTimestampNs);pollDelay();}
            reap(bp);
        }
        std::cout<<"ENCODER_HOOK synthetic_aligned_packet=PASS (not a live-space calibration)\n";
        std::ofstream(output/"result.txt")<<"PASS frames="<<paths.size()<<" byte_exact_allocation="<<total<<" byte_exact_sbs=true previous_image_lifetime=true strict_negative_matches=true hook=true\n";
    }catch(std::exception const& e){std::cerr<<e.what()<<'\n';return 1;}
}
