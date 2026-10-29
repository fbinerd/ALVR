#pragma once
// Runtime-only paired observations. Packet-v1 clocks/units are unchanged.
#include "clock_space_bridge.hpp"
#include <atomic>
#include <fstream>
#include <cstdlib>
#include <chrono>
#include <cerrno>
#include <stdexcept>
#include <deque>
#include <mutex>
#include <thread>
#include <sstream>
#include <sys/socket.h>
#include <sys/un.h>

namespace mhl::depth {
inline uint64_t runtimeNow(){return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
inline int runtimeDepthMode(){
    thread_local uint64_t checked=0;thread_local int mode=0;auto now=runtimeNow();
    if(now-checked>=1000000000ull){checked=now;mode=0;
        if(auto home=getenv("HOME"))std::ifstream(std::string(home)+"/.config/alvr/diag_depth_receiver")>>mode;}
    return mode;
}
inline MhlDepthPose inversePose(MhlDepthPose p){
    for(unsigned i=0;i<3;i++)p.orientation[i]=-p.orientation[i];
    MhlDepthPose t{{-p.position[0],-p.position[1],-p.position[2]},{0,0,0,1}};
    auto q=p;std::fill_n(q.position,3,0.f);return compose(q,t);
}
inline double positionError(MhlDepthPose const& a,MhlDepthPose const& b){
    double s=0;for(unsigned i=0;i<3;i++){double d=a.position[i]-b.position[i];s+=d*d;}return std::sqrt(s);
}
inline double rotationError(MhlDepthPose const& a,MhlDepthPose const& b){
    double d=0;for(unsigned i=0;i<4;i++)d+=a.orientation[i]*b.orientation[i];return 1-std::min(1.,std::abs(d));
}
struct RuntimeObservation {
    uint64_t target{},display{},observed{},poseTarget{};uint32_t owner{},reserved{};
    MhlDepthPose head{{0,0,0},{0,0,0,1}};
    std::array<MhlDepthPose,2> localEyes{};
    std::array<std::array<float,4>,2> fov{};
};
struct RuntimeFrame {
    uint32_t magic=0x5244484d,version=1,size=sizeof(RuntimeFrame),reserved=0;
    uint64_t reference{};MhlDepthPacket packet{};
};
inline sockaddr_un registrationAddress(std::string const& path){
    sockaddr_un a{};a.sun_family=AF_UNIX;
    if(path.size()>=sizeof(a.sun_path))throw std::runtime_error("calibration socket path too long");
    std::memcpy(a.sun_path,path.c_str(),path.size()+1);return a;
}
inline bool registrationPeer(int fd){ucred c{};socklen_t n=sizeof(c);return !getsockopt(fd,SOL_SOCKET,SO_PEERCRED,&c,&n)&&c.uid==getuid();}
class CalibrationPublisher {
    std::string path;int fd=-1;uint64_t retry=0;
public:
    explicit CalibrationPublisher(std::string p):path(std::move(p)){}
    ~CalibrationPublisher(){if(fd>=0)close(fd);}
    void send(MhlDepthPacket const& packet,uint64_t reference){
        // Optional bootstrap must not disable existing calibrated delivery.
        if(path.size()>=sizeof(sockaddr_un{}.sun_path))return;
        if(fd<0){auto now=runtimeNow();if(now<retry)return;retry=now+100000000;
            fd=socket(AF_UNIX,SOCK_SEQPACKET|SOCK_NONBLOCK|SOCK_CLOEXEC,0);auto a=registrationAddress(path);
            if(fd<0)return;if(connect(fd,reinterpret_cast<sockaddr*>(&a),sizeof(a))||!registrationPeer(fd)){close(fd);fd=-1;return;}}
        RuntimeFrame f;f.reference=reference;f.packet=packet;
        auto n=::send(fd,&f,sizeof(f),MSG_DONTWAIT|MSG_NOSIGNAL);
        if(n!=sizeof(f)&&errno!=EAGAIN&&errno!=EWOULDBLOCK){close(fd);fd=-1;}
    }
};
// Pure pairing/rigid-fit engine, independently tested without a runtime/GPU.
inline bool finitePose(MhlDepthPose const& p){
    float n=0;for(float x:p.position)if(!std::isfinite(x))return false;
    for(float x:p.orientation){if(!std::isfinite(x))return false;n+=x*x;}
    return std::abs(n-1)<.001f;
}
// A measured rigid origin is shared by the fit and exact-frame lookup.
inline bool runtimeGeometryMatches(MhlDepthPose const& origin,RuntimeObservation const& d,RuntimeFrame const& r){
    if(d.owner!=r.packet.producer_pid||!finitePose(d.head))return false;
    for(unsigned eye=0;eye<2;eye++){
        if(!finitePose(r.packet.eyes[eye].pose)||!finitePose(d.localEyes[eye]))return false;
        for(unsigned i=0;i<4;i++)if(!std::isfinite(d.fov[eye][i])||!std::isfinite(r.packet.eyes[eye].fov[i])||std::abs(r.packet.eyes[eye].fov[i]-d.fov[eye][i])>1e-5)return false;
        auto actual=compose(origin,r.packet.eyes[eye].pose),expected=compose(d.head,d.localEyes[eye]);
        if(positionError(actual,expected)>1e-4||rotationError(actual,expected)>1e-6)return false;
    }
    return true;
}
// Metadata only: no image/slot lifetime is inferred from this bounded cache.
// Returned times are the original xrConvertTimeToTimespecTimeKHR values, never
// an affine extrapolation of a pose-history tag. Still/near-duplicate poses
// within the 100ms runtime gate deliberately remain ambiguous.
class RuntimeFrameAssociations {
    std::deque<RuntimeFrame> frames;
public:
    enum class Status { matched,missing,ambiguous };
    struct Result {Status status=Status::missing;uint64_t display=0,frame=0;};
    void offer(RuntimeFrame const& r){
        auto const& p=r.packet;
        if(p.magic!=MHL_DEPTH_MAGIC||p.version!=1||p.size!=608||!p.frame_id||!p.producer_id||!p.producer_pid||!r.reference||
           p.eye_count!=2||!(p.flags&MHL_PACKET_VALID)||!(p.flags&MHL_CLOCK_MONOTONIC)||!p.display_time_ns||!p.app_time_ns)return;
        // Authenticated duplicate metadata must not turn one frame into two
        // candidates. Conflicting duplicates make that frame unusable.
        for(auto& old:frames)if(old.packet.producer_id==p.producer_id&&old.reference==r.reference&&old.packet.frame_id==p.frame_id){
            if(std::memcmp(&old.packet,&r.packet,sizeof(p)))old.packet.flags=0;
            return;
        }
        frames.push_back(r);while(frames.size()>128)frames.pop_front();
    }
    Result resolve(Calibration const& c,RuntimeObservation const& d,uint64_t now)const{
        Result result;
        if(c.version!=4||!valid(c,now)||!d.poseTarget||!d.target||!d.display||!d.owner)return result;
        for(auto const& r:frames){
            auto const& p=r.packet;
            if(p.producer_id!=c.producerId||r.reference!=c.referenceSpace||!(p.flags&MHL_PACKET_VALID)||
               !valid(c,p.display_time_ns)||p.app_time_ns>now||now-p.app_time_ns>250000000)continue;
            auto delta=d.display>p.display_time_ns?d.display-p.display_time_ns:p.display_time_ns-d.display;
            if(delta>100000000||!runtimeGeometryMatches(c.trackingFromReference,d,r))continue;
            if(result.status==Status::matched)return {Status::ambiguous,0,0};
            result={Status::matched,p.display_time_ns,p.frame_id};
        }
        return result;
    }
};
class RuntimePairer {
    std::optional<MhlDepthPose> origin;
    bool originTrusted=false;
    std::deque<ClockPair> clocks;
    uint64_t producer=0,reference=0,lastFrame=0;uint32_t clockVersion=0;
    bool exactFrames=false;
public:
    explicit RuntimePairer(bool exact=false):exactFrames(exact){}
    uint64_t matched=0,unmatched=0,late=0,resets=0,clockResets=0;double lastPoseError=0;
    bool knownFor(RuntimeFrame const& r)const{return origin&&originTrusted&&r.packet.producer_id==producer&&r.reference==reference;}
    bool poseCompatible(RuntimeObservation const& d,RuntimeFrame const& r)const{
        return knownFor(r)&&runtimeGeometryMatches(*origin,d,r);
    }
    std::optional<Calibration> add(RuntimeObservation const& d,RuntimeFrame const& r){
        auto const& p=r.packet;
        auto delta=d.display>p.display_time_ns?d.display-p.display_time_ns:p.display_time_ns-d.display;
        if(p.magic!=MHL_DEPTH_MAGIC||p.version!=1||p.size!=608||!finitePose(d.head)||
           !finitePose(p.eyes[0].pose)||!finitePose(p.eyes[1].pose)||
           !finitePose(d.localEyes[0])||!finitePose(d.localEyes[1])||
           !d.target||!d.display||!d.owner||p.producer_pid!=d.owner||!p.producer_id||!r.reference||
           p.eye_count!=2||!(p.flags&MHL_CLOCK_MONOTONIC)||delta>100000000){++unmatched;return {};}
        if(exactFrames&&!d.poseTarget){++unmatched;return {};}
        if(delta>1000000&&(!d.poseTarget||!poseCompatible(d,r))){++unmatched;return {};}
        if(p.producer_id!=producer||r.reference!=reference){origin.reset();originTrusted=false;clocks.clear();clockVersion=0;producer=p.producer_id;reference=r.reference;lastFrame=0;}
        if(clockVersion==3&&!d.poseTarget){++unmatched;return {};}
        uint32_t domain=exactFrames?4:d.poseTarget?3:2;
        if(clockVersion&&clockVersion!=domain){clocks.clear();++clockResets;}
        clockVersion=domain;auto pairedTag=domain==4?p.display_time_ns:domain==3?d.poseTarget:d.display;
        if(p.frame_id<=lastFrame){++late;return {};}
        for(unsigned eye=0;eye<2;eye++)for(unsigned i=0;i<4;i++)
            if(!std::isfinite(d.fov[eye][i])||!std::isfinite(p.eyes[eye].fov[i])||std::abs(p.eyes[eye].fov[i]-d.fov[eye][i])>1e-5){++unmatched;return {};}
        // Derive head-in-XrSpace from the independently measured local eye,
        // not an assumed symmetric/identity IPD or a forced zero origin.
        auto referenceHead=compose(p.eyes[0].pose,inversePose(d.localEyes[0]));
        auto candidate=compose(d.head,inversePose(referenceHead));
        auto right=compose(candidate,p.eyes[1].pose),expected=compose(d.head,d.localEyes[1]);
        if(positionError(right,expected)>1e-4||rotationError(right,expected)>1e-6){++unmatched;return {};}
        // A first tight-clock pair can be a startup transient. Do not let
        // its origin filter out subsequent correct tight-clock bootstrap pairs.
        // Trust the origin only after a complete coherent calibration.
        if(origin){
            lastPoseError=positionError(compose(*origin,referenceHead),d.head);
            if(lastPoseError>.0001||rotationError(*origin,candidate)>1e-6){origin.reset();originTrusted=false;clocks.clear();++resets;}
        }
        if(!origin)origin=candidate;
        if(!clocks.empty()&&(pairedTag<=clocks.back().target||p.display_time_ns<=clocks.back().monotonic)){++late;return {};}
        if(!clocks.empty()){
            __int128 change=(__int128(p.display_time_ns)-pairedTag)-(__int128(clocks.back().monotonic)-clocks.back().target);
            if(change>1000000||change< -1000000){clocks.clear();++clockResets;}
        }
        lastFrame=p.frame_id;++matched;
        // Domains3/4 use geometry to associate the frame. Domain4 retains
        // only the public source clock/origin; a pose tag is never fitted.
        // Runtime prediction may slip and remains a bounded coarse gate.
        clocks.push_back({pairedTag,p.display_time_ns,domain>=3?100000:delta+100000});
        while(clocks.size()>32)clocks.pop_front();
        if(clocks.size()<8)return {};
        auto result=calibrate(producer,reference,clocks.front(),clocks.back(),*origin,p.display_time_ns+3000000000ull);
        if(result){originTrusted=true;result->version=domain;} // Explicitly versioned source clock/tag domain
        return result;
    }
};
class RuntimeCalibrationService {
    std::string socketPath,file;Logger log;std::atomic<bool> stopping=false;
    std::mutex mutex;std::deque<RuntimeObservation> offered;RuntimeFrameAssociations associations;
    bool exactFrames=false;
    std::atomic<uint64_t> lookupMatched=0,lookupMissing=0,lookupAmbiguous=0,lookupContended=0;
    std::thread worker;
    void run() noexcept {
        int server=-1,client=-1;ino_t socketInode=0,fileInode=0;
        try{
            server=socket(AF_UNIX,SOCK_SEQPACKET|SOCK_NONBLOCK|SOCK_CLOEXEC,0);auto address=registrationAddress(socketPath);
            if(server<0||bind(server,reinterpret_cast<sockaddr*>(&address),sizeof(address)))throw std::runtime_error("calibration socket unavailable; existing paths preserved");
            struct stat state{};lstat(socketPath.c_str(),&state);socketInode=state.st_ino;
            if(chmod(socketPath.c_str(),0600)||listen(server,1))throw std::runtime_error("calibration listen");
            RuntimePairer pairer(exactFrames);std::deque<RuntimeObservation> observations;std::deque<RuntimeFrame> pending;
            uint64_t written=0,reported=0;std::optional<Calibration> last;
            while(!stopping){
                {std::lock_guard lock(mutex);for(auto const& d:offered)observations.push_back(d);offered.clear();}
                while(observations.size()>128)observations.pop_front();
                if(client<0){client=accept4(server,nullptr,nullptr,SOCK_NONBLOCK|SOCK_CLOEXEC);if(client>=0&&!registrationPeer(client)){close(client);client=-1;}}
                if(client>=0)for(unsigned count=0;count<16;count++){
                    RuntimeFrame r{};auto n=recv(client,&r,sizeof(r),MSG_DONTWAIT|MSG_TRUNC);
                    if(!n){close(client);client=-1;break;}if(n<0)break;
                    ucred cred{};socklen_t size=sizeof(cred);
                    if(n==sizeof(r)&&r.magic==0x5244484d&&r.version==1&&r.size==sizeof(r)&&!r.reserved&&
                       !getsockopt(client,SOL_SOCKET,SO_PEERCRED,&cred,&size)&&uint32_t(cred.pid)==r.packet.producer_pid){pending.push_back(r);
                        if(exactFrames){std::lock_guard guard(mutex);associations.offer(r);}}
                    else ++pairer.unmatched;
                }
                while(pending.size()>32){pending.pop_front();++pairer.late;}
                auto now=runtimeNow();
                for(auto it=pending.begin();it!=pending.end();){
                    auto best=observations.end();uint64_t distance=1000001;bool ambiguous=false;
                    for(auto d=observations.begin();d!=observations.end();++d){
                        if(d->owner!=it->packet.producer_pid)continue;
                        auto t=d->display>it->packet.display_time_ns?d->display-it->packet.display_time_ns:it->packet.display_time_ns-d->display;
                        bool pose=pairer.poseCompatible(*d,*it);
                        bool admissible=(t<=1000000&&(!pairer.knownFor(*it)||pose))||(t<=100000000&&d->poseTarget&&pose);
                        if(admissible){if(best!=observations.end())ambiguous=true;if(best==observations.end()||t<distance){best=d;distance=t;}}
                    }
                    if(best!=observations.end()){
                        if(ambiguous)++pairer.unmatched;
                        else if(auto c=pairer.add(*best,*it))last=c;
                        observations.erase(best);it=pending.erase(it);
                    }else if(now>it->packet.app_time_ns&&now-it->packet.app_time_ns>250000000){++pairer.late;it=pending.erase(it);}
                    else ++it;
                }
                if(last&&valid(*last,now)&&now-written>=500000000){
                    std::string temp=file+".tmp."+std::to_string(getpid());int fd=open(temp.c_str(),O_CREAT|O_EXCL|O_WRONLY|O_CLOEXEC,0600);
                    if(fd>=0){bool ok=write(fd,&*last,sizeof(*last))==sizeof(*last);close(fd);
                        if(ok&&!rename(temp.c_str(),file.c_str())){struct stat state{};lstat(file.c_str(),&state);fileInode=state.st_ino;written=now;}else unlink(temp.c_str());}
                }
                if(now-reported>=5000000000ull){
                    reported=now;std::ostringstream s;s<<"Game depth calibration matched="<<pairer.matched<<" late="<<pairer.late<<" unmatched="<<pairer.unmatched<<" origin_resets="<<pairer.resets<<" clock_resets="<<pairer.clockResets<<" pose_error_mm="<<pairer.lastPoseError*1000;
                    if(last)s<<" calibration_version="<<last->version<<" clock_offset_ns="<<int64_t(last->monotonicAnchor-last->targetAnchor)<<" uncertainty_ns="<<last->uncertaintyNs;
                    if(exactFrames)s<<" association_matched="<<lookupMatched.load()<<" association_missing="<<lookupMissing.load()
                        <<" association_ambiguous="<<lookupAmbiguous.load()<<" association_contended="<<lookupContended.load();
                    if(log)log(s.str());
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }catch(std::exception const& e){if(log)log(std::string("Game depth calibration disabled: ")+e.what());}
        if(client>=0)close(client);if(server>=0)close(server);
        for(auto const& item:{std::pair{socketPath,socketInode},std::pair{file,fileInode}}){struct stat s{};if(item.second&&!lstat(item.first.c_str(),&s)&&s.st_ino==item.second)unlink(item.first.c_str());}
    }
public:
    RuntimeCalibrationService(std::string socket,std::string record,Logger logger,bool exact=false):socketPath(std::move(socket)),file(std::move(record)),log(std::move(logger)),exactFrames(exact),worker([this]{run();}){}
    ~RuntimeCalibrationService(){stopping=true;worker.join();}
    std::optional<uint64_t> resolve(Calibration const& c,RuntimeObservation const& d){
        if(!exactFrames)return {};
        std::unique_lock guard(mutex,std::try_to_lock);
        if(!guard.owns_lock()){++lookupContended;return {};}
        auto r=associations.resolve(c,d,runtimeNow());
        if(r.status==RuntimeFrameAssociations::Status::matched){++lookupMatched;return r.display;}
        if(r.status==RuntimeFrameAssociations::Status::ambiguous)++lookupAmbiguous;else ++lookupMissing;
        return {};
    }
    void observe(RuntimeObservation const& d){
        if(!d.target||!d.display||!d.owner)return;
        std::unique_lock lock(mutex,std::try_to_lock);if(!lock.owns_lock()||offered.size()>=128)return;offered.push_back(d);
    }
};
}
