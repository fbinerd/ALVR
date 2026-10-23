// Production mode-7 extrapolation on externally validated canonical recordings.
// Offline executable only; no SteamVR, ALVR server, game or headset launch.
#define main syntheticCheckerMain
#include "linux_native_motion_images.cpp"
#undef main
#include "native_motion_recording_metrics.hpp"
#include "platform/linux/depth/DepthPacket.h"
#include "platform/linux/motion/StereoDepth.hpp"
#include <deque>
#include <numeric>

namespace recordeddepth {
using Motion=alvr::motion::NativeMotion;
struct Frame { MhlDepthPacket packet; Pixels colour,depth; };
using Quaternion=std::array<double,4>;
Quaternion multiply(Quaternion a,Quaternion b) {
    return {a[3]*b[0]+a[0]*b[3]+a[1]*b[2]-a[2]*b[1],
        a[3]*b[1]-a[0]*b[2]+a[1]*b[3]+a[2]*b[0],
        a[3]*b[2]+a[0]*b[1]-a[1]*b[0]+a[2]*b[3],
        a[3]*b[3]-a[0]*b[0]-a[1]*b[1]-a[2]*b[2]};
}
Quaternion orientation(MhlDepthPacket const& p) {
    Quaternion q;double norm=0;
    for(unsigned i=0;i<4;i++){q[i]=p.eyes[0].pose.orientation[i];norm+=q[i]*q[i];}
    norm=std::sqrt(norm);for(auto& v:q)v/=norm;
    double dot=0;for(unsigned i=0;i<4;i++)dot+=q[i]*p.eyes[1].pose.orientation[i];
    if(std::abs(dot)<.99999)throw std::runtime_error("Canted-eye orientations need a per-eye rotation API");
    return q;
}
Motion::Rotation between(MhlDepthPacket const& source,MhlDepthPacket const& destination,bool translation) {
    // Mirrors BuildMotionRotation: conjugate(source)*destination and head
    // centre delta in source head coordinates. Per-eye FOV is never dummy.
    auto q=orientation(source),d=orientation(destination);Quaternion conjugate{-q[0],-q[1],-q[2],q[3]};
    auto delta=multiply(conjugate,d);Motion::Rotation out;
    for(unsigned i=0;i<4;i++)out.quaternion[i]=float(delta[i]);
    for(unsigned eye=0;eye<2;eye++)for(unsigned i=0;i<4;i++)
        (eye?out.rightTans:out.leftTans)[i]=std::tan(destination.eyes[eye].fov[i]);
    if(translation){
        Quaternion moved{0,0,0,0};
        for(unsigned i=0;i<3;i++)for(unsigned eye=0;eye<2;eye++)
            moved[i]+=.5*(destination.eyes[eye].pose.position[i]-source.eyes[eye].pose.position[i]);
        auto local=multiply(multiply(conjugate,moved),q);
        for(unsigned i=0;i<3;i++)out.translation[i]=float(local[i]);
    }
    return out;
}
std::vector<Frame> read(std::filesystem::path root,Size& colourSize,Size& depthSize) {
    std::ifstream headers(root/"headers.bin",std::ios::binary),colours(root/"colour.rgba8",std::ios::binary),depths(root/"depth.r32f",std::ios::binary);
    if(!headers||!colours||!depths)throw std::runtime_error("Cannot open fixture; run converter and sha256sum -c checksums.sha256 first");
    std::vector<Frame> frames;MhlDepthPacket p;
    while(headers.read(reinterpret_cast<char*>(&p),sizeof(p))){
        if(auto error=mhl_depth_validate(&p,sizeof(p)))throw std::runtime_error(error);
        if(p.transport!=MHL_TRANSPORT_RECORDING)throw std::runtime_error("Recording transport required");
        auto const& a=p.eyes[0];auto const& b=p.eyes[1];
        if(a.colour.width!=b.colour.width||a.colour.height!=b.colour.height||a.depth.width!=b.depth.width||a.depth.height!=b.depth.height)
            throw std::runtime_error("Unequal eye extents");
        Size cs{a.colour.width,a.colour.height},ds{a.depth.width,a.depth.height};
        if(frames.empty()){colourSize=cs;depthSize=ds;}
        else {
            auto const& old=frames.back().packet;
            if(cs.bytes()!=colourSize.bytes()||cs.width()!=colourSize.width()||ds.bytes()!=depthSize.bytes()||ds.width()!=depthSize.width()
                ||p.producer_id!=old.producer_id||p.producer_pid!=old.producer_pid||p.source_tag!=old.source_tag||p.frame_id!=old.frame_id+1
                ||p.display_time_ns<=old.display_time_ns||p.app_time_ns<old.app_time_ns)
                throw std::runtime_error("Fixture geometry/identity/clock changed");
            for(unsigned eye=0;eye<2;eye++)for(unsigned i=0;i<4;i++)
                if(std::abs(p.eyes[eye].fov[i]-old.eyes[eye].fov[i])>1e-6)
                    throw std::runtime_error("Changing FOV requires remapping retained source geometry");
        }
        Frame f{p,Pixels(cs.bytes()),Pixels(ds.bytes())};
        if(!colours.read(reinterpret_cast<char*>(f.colour.data()),f.colour.size())||!depths.read(reinterpret_cast<char*>(f.depth.data()),f.depth.size()))
            throw std::runtime_error("Truncated fixture planes");
        for(size_t i=0;i<f.depth.size();i+=4){float z;std::memcpy(&z,f.depth.data()+i,4);if(!std::isfinite(z)||z<0||std::signbit(z))throw std::runtime_error("Noncanonical depth");}
        orientation(p);frames.push_back(std::move(f));
    }
    if(headers.gcount()!=0||colours.peek()!=EOF||depths.peek()!=EOF||frames.size()<10)throw std::runtime_error("Truncated/trailing fixture or too few frames");
    return frames;
}
float depthAt(Frame const& f,Size colourSize,Size depthSize,unsigned x,unsigned y) {
    unsigned eye=x/colourSize.eyeWidth;
    unsigned dx=std::min(depthSize.eyeWidth-1,unsigned((uint64_t(2*(x%colourSize.eyeWidth)+1)*depthSize.eyeWidth)/(2*colourSize.eyeWidth)));
    unsigned dy=std::min(depthSize.height-1,unsigned((uint64_t(2*y+1)*depthSize.height)/(2*colourSize.height)));
    float z;std::memcpy(&z,f.depth.data()+(size_t(dy)*depthSize.width()+eye*depthSize.eyeWidth+dx)*4,4);return z;
}
bool red(Pixels const& rgba,size_t i) {return rgba[4*i]>150&&rgba[4*i]>2*rgba[4*i+1]&&rgba[4*i]>2*rgba[4*i+2];}
std::vector<uint8_t> dilate(std::vector<uint8_t> const& mask,Size s,unsigned radius) {
    unsigned stride=s.eyeWidth+1;std::vector<uint8_t> out(mask.size());
    std::vector<uint32_t> integral(size_t(stride)*(s.height+1));
    for(unsigned eye=0;eye<2;eye++){
        std::fill(integral.begin(),integral.end(),0);
        for(unsigned y=0;y<s.height;y++)for(unsigned x=0;x<s.eyeWidth;x++)
            integral[size_t(y+1)*stride+x+1]=mask[size_t(y)*s.width()+eye*s.eyeWidth+x]+integral[size_t(y)*stride+x+1]+integral[size_t(y+1)*stride+x]-integral[size_t(y)*stride+x];
        for(unsigned y=0;y<s.height;y++)for(unsigned x=0;x<s.eyeWidth;x++){
            unsigned x0=x>radius?x-radius:0,y0=y>radius?y-radius:0,x1=std::min(s.eyeWidth,x+radius+1),y1=std::min(s.height,y+radius+1);
            out[size_t(y)*s.width()+eye*s.eyeWidth+x]=integral[size_t(y1)*stride+x1]-integral[size_t(y0)*stride+x1]-integral[size_t(y1)*stride+x0]+integral[size_t(y0)*stride+x0]>0;
        }
    }
    return out;
}
struct Masks { std::vector<uint8_t> band,shadow; };
Masks masks(Frame const& truth,Frame const& latest,Size s,Size ds) {
    std::vector<uint8_t> edge(size_t(s.width())*s.height),plane(edge.size());
    auto discontinuity=[](float a,float b){return (a>0)!=(b>0)||(a>0&&b>0&&std::abs(a-b)>.02f&&std::abs(a-b)/std::max(a,b)>.05f);};
    for(unsigned y=0;y<s.height;y++)for(unsigned x=0;x<s.width();x++){
        size_t i=size_t(y)*s.width()+x;float z=depthAt(truth,s,ds,x,y),old=depthAt(latest,s,ds,x,y);
        edge[i]=(x%s.eyeWidth+1<s.eyeWidth&&discontinuity(z,depthAt(truth,s,ds,x+1,y)))||(y+1<s.height&&discontinuity(z,depthAt(truth,s,ds,x,y+1)));
        plane[i]=(z>=.45f&&z<=.55f&&red(truth.colour,i))||(old>=.45f&&old<=.55f&&red(latest.colour,i));
    }
    return {dilate(edge,s,24),dilate(plane,s,24)};
}
struct Totals {
    double absolute=0,squared=0,bandAbsolute=0;uint64_t pixels=0,bandPixels=0,shadowPixels=0,shadowEligible=0;
    void add(Totals const& t){absolute+=t.absolute;squared+=t.squared;bandAbsolute+=t.bandAbsolute;pixels+=t.pixels;bandPixels+=t.bandPixels;shadowPixels+=t.shadowPixels;shadowEligible+=t.shadowEligible;}
    double mae()const{return absolute/(3*pixels);}double psnr()const{return squared?10*std::log10(255.*255.*3*pixels/squared):100;}
    double bandMae()const{return bandPixels?bandAbsolute/(3*bandPixels):0;}
};
Totals score(Pixels const& prediction,Frame const& truth,Masks const& masks) {
    Totals t;t.pixels=prediction.size()/4;
    for(size_t i=0;i<t.pixels;i++){
        if(masks.band[i])t.bandPixels++;
        for(unsigned c=0;c<3;c++){double e=int(prediction[4*i+c])-int(truth.colour[4*i+c]);t.absolute+=std::abs(e);t.squared+=e*e;if(masks.band[i])t.bandAbsolute+=std::abs(e);}
        if(masks.shadow[i]&&!red(truth.colour,i)){t.shadowEligible++;if(red(prediction,i))t.shadowPixels++;}
    }
    return t;
}
void selfTest() {
    MhlDepthPacket source{},destination{};
    const float fov[4]={-.8f,1.1f,.9f,-.7f};
    for(unsigned eye=0;eye<2;eye++) {
        auto& a=source.eyes[eye];auto& b=destination.eyes[eye];
        a.pose.orientation[1]=a.pose.orientation[3]=float(std::sqrt(.5));
        b.pose.orientation[1]=float(std::sin(std::acos(-1.)/4+.1));
        b.pose.orientation[3]=float(std::cos(std::acos(-1.)/4+.1));
        a.pose.position[0]=b.pose.position[0]=eye?.0315f:-.0315f;
        b.pose.position[2]=-.1f;
        for(unsigned i=0;i<4;i++)a.fov[i]=b.fov[i]=fov[i];
        if(eye)a.fov[0]=b.fov[0]=-1.1f;
    }
    auto r=between(source,destination,true);
    auto near=[](double a,double b){if(std::abs(a-b)>1e-6)throw std::runtime_error("Rotation/FOV/translation self-test failed");};
    near(r.quaternion[1],std::sin(.1));near(r.quaternion[3],std::cos(.1));
    near(r.translation[0],.1);near(r.translation[1],0);near(r.translation[2],0);
    near(r.leftTans[0],std::tan(-.8f));near(r.rightTans[0],std::tan(-1.1f));
    auto noTranslation=between(source,destination,false);
    for(float t:noTranslation.translation)near(t,0);
    Size s{4,2};std::vector<uint8_t> mask(s.width()*s.height);mask[3]=1;
    auto band=dilate(mask,s,1);
    if(!band[2]||!band[3]||band[4]||band[5])throw std::runtime_error("Dilation crossed eye seam");
    std::cout<<"Recorded-pose quaternion, asymmetric FOV, translation and stereo mask checks passed\n";
}
}
int main(int argc,char** argv) {
 try {
    using namespace recordeddepth;
    if(argc==2&&std::string(argv[1])=="--self-test"){selfTest();return 0;}
    if(argc!=3)throw std::runtime_error("Usage: depth_recording_test VERIFIED_FIXTURE NEW_OUTPUT");
    std::filesystem::path root(argv[1]),output(argv[2]);
    if(std::filesystem::exists(output))throw std::runtime_error("Use a new output directory");
    Size size{},ds{};auto frames=read(root,size,ds);std::filesystem::create_directories(output);
    RecordingContext ctx;if(!ctx.meta.opticalFlowEnabled||ctx.meta.vendor!=alvr::Vendor::Nvidia)throw std::runtime_error("NVIDIA optical flow required");
    Images gpu(ctx,size);gpu.initialize();Images depth(ctx,ds);depth.format=vk::Format::eR32Sfloat;depth.initialize();
    std::ofstream csv(output/"metrics.csv"),summary(output/"summary.csv");
    csv<<"variant,index,frame,display_ns,mae,psnr,edge_band_mae,edge_pixels,shadow_fp,shadow_eligible,prepare_wall_ms,flow_wall_ms,warp_wall_ms,optical_gpu_ms,warp_gpu_ms\n";
    summary<<"variant,targets,mae,psnr,edge_band_mae,edge_pixels,shadow_fp,shadow_eligible,shadow_percent\n";
    // DEPTH_STEREO=1 adds a fourth variant: depth estimated from the two
    // colour eyes (StereoDepth) instead of the recorded depth, with translation.
    // Needs real stereo views (eyes apart by the IPD) and depth at colour extent.
    bool stereo=std::getenv("DEPTH_STEREO")!=nullptr;
    std::unique_ptr<alvr::motion::StereoDepth> estimator;
    alvr::motion::StereoParams stereoParams;
    if(stereo){
        if(ds.width()!=size.width()||ds.height!=size.height)throw std::runtime_error("Stereo variant needs depth at the colour extent");
        auto const& p=frames.front().packet;double ipd=0;
        for(unsigned i=0;i<3;i++){double d=p.eyes[1].pose.position[i]-p.eyes[0].pose.position[i];ipd+=d*d;}
        stereoParams.ipd=float(std::sqrt(ipd));
        if(stereoParams.ipd<.01f)throw std::runtime_error("Stereo variant needs real stereo views (identical eye poses)");
        for(unsigned eye=0;eye<2;eye++){
            auto& t=eye?stereoParams.rightTans:stereoParams.leftTans;
            t={float(std::tan(p.eyes[eye].fov[0])),float(std::tan(p.eyes[eye].fov[1])),float(std::tan(p.eyes[eye].fov[2])),float(std::tan(p.eyes[eye].fov[3]))};
        }
        estimator=std::make_unique<alvr::motion::StereoDepth>(ctx,vk::Extent2D{size.width(),size.height},vk::Format::eR8G8B8A8Unorm,
            std::getenv("STEREO_LUMA_MAX")?uint32_t(std::stoul(std::getenv("STEREO_LUMA_MAX"))):0u,
            std::getenv("STEREO_REDUCED")!=nullptr);
    }
    for(unsigned variant=0;variant<(stereo?4u:3u);variant++){
        // DEPTH_VARIANTS=2,3 runs only those variants (quick A/B iterations;
        // the no-depth baseline does not change with depth-path edits).
        if(auto only=std::getenv("DEPTH_VARIANTS");only&&(","+std::string(only)+",").find(","+std::to_string(variant)+",")==std::string::npos)continue;
        std::string name=variant==0?"no_depth":variant==1?"exact_depth":variant==2?"exact_depth_translation":"stereo_depth_translation";
        auto directory=output/name;std::filesystem::create_directories(directory);
        Motion motion(ctx,{size.width(),size.height},vk::Format::eR8G8B8A8Unorm);motion.setGapFill(true);motion.setStableResidual(true);
#ifdef ALVR_NATIVE_MOTION_TESTING
        // DEPTH_DIAGNOSTIC=1 paints the depth decision (red covered, green filled).
        if(variant&&std::getenv("DEPTH_DIAGNOSTIC"))motion.setDiagnosticFlags(2);
        // DEPTH_OCCLUSION=1 enables the experimental occlusion handling.
        motion.setDepthOcclusion(std::getenv("DEPTH_OCCLUSION")!=nullptr);
        // DEPTH_FLAGS=n sets extra warp flags on depth variants (experiments).
        if(variant&&std::getenv("DEPTH_FLAGS"))motion.setDiagnosticFlags(uint32_t(std::stoul(std::getenv("DEPTH_FLAGS"))));
#endif
        size_t latest=0;unsigned targets=0;double prepareMs=0,worst=-1,worstEdge=-1;Totals total;
        for(size_t i=0;i<frames.size();i++){
            auto const& f=frames[i];uint64_t clock=f.packet.display_time_ns;
            if(i%2==0){
                motion.waitIdle();gpu.transfer(&f.colour);
                if(variant==3){estimator->estimate(gpu.image,gpu.view,gpu.layout,{},{},vk::ImageLayout::eGeneral,stereoParams);}
                else if(variant)depth.transfer(&f.depth);
                auto transform=between(frames[latest].packet,f.packet,variant>=2);
                Motion::Depth input{depth.image,depth.layout,{ds.width(),ds.height}};
                Motion::Depth stereoInput{variant==3?estimator->internalDepthImage():vk::Image{},vk::ImageLayout::eGeneral,variant==3?estimator->depthExtent():vk::Extent2D{}};
                motion.capture(gpu.image,gpu.layout,clock,clock,&transform,variant==3?&stereoInput:variant?&input:nullptr);
                auto start=std::chrono::steady_clock::now();motion.prepare();prepareMs=elapsed(start);latest=i;
            }else if(i>=7){
                auto transform=between(frames[latest].packet,f.packet,variant>=2);
                if(!motion.predict(gpu.image,gpu.view,gpu.layout,clock,{},clock,&transform))throw std::runtime_error("Production gap-fill rejected target");
                auto image=gpu.transfer(nullptr);auto timing=motion.lastPredictionTiming();auto mask=masks(f,frames[latest],size,ds);auto s=score(image,f,mask);total.add(s);targets++;
                csv<<std::setprecision(12)<<name<<','<<i<<','<<f.packet.frame_id<<','<<clock<<','<<s.mae()<<','<<s.psnr()<<','<<s.bandMae()<<','<<s.bandPixels<<','<<s.shadowPixels<<','<<s.shadowEligible<<','<<prepareMs<<','<<timing.flowMs<<','<<timing.warpMs<<','<<timing.opticalGpuMs<<','<<timing.warpGpuMs<<'\n';
                auto save=[&](std::string prefix){writePpm(directory/(prefix+"_prediction.ppm"),image,size);writePpm(directory/(prefix+"_truth.ppm"),f.colour,size);Pixels diff=image;for(size_t j=0;j<diff.size();j++)diff[j]=j%4==3?255:uint8_t(std::min(255,4*std::abs(int(image[j])-int(f.colour[j]))));writePpm(directory/(prefix+"_diff_x4.ppm"),diff,size);std::ofstream(directory/(prefix+"_frame.txt"))<<f.packet.frame_id<<'\n';};
                // DEPTH_DUMP_INDEX=n saves that target for every variant (A/B by eye).
                if(std::getenv("DEPTH_DUMP_INDEX")&&int(i)==std::atoi(std::getenv("DEPTH_DUMP_INDEX")))save("dump");
                if(s.mae()>worst){worst=s.mae();save("worst_overall");}if(s.bandMae()>worstEdge){worstEdge=s.bandMae();save("worst_edge");}
            }
        }
        motion.waitIdle();summary<<std::setprecision(12)<<name<<','<<targets<<','<<total.mae()<<','<<total.psnr()<<','<<total.bandMae()<<','<<total.bandPixels<<','<<total.shadowPixels<<','<<total.shadowEligible<<',';
        if(total.shadowEligible)summary<<100.*total.shadowPixels/total.shadowEligible;else summary<<"nan";
        summary<<'\n';std::cout<<name<<": "<<targets<<" targets, MAE "<<total.mae()<<", band "<<total.bandMae()<<'\n';
    }
    if(!summary||!csv)throw std::runtime_error("Metric write failed");
 }catch(std::exception const& e){std::cerr<<e.what()<<'\n';return 1;}
}
