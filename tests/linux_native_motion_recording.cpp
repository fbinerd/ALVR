// Reuse the checker's Vulkan image upload/readback and context stubs. This is
// a separate offline executable; it is never linked into the driver.
#define main syntheticCheckerMain
#include "linux_native_motion_images.cpp"
#undef main

#include "native_motion_recording_metrics.hpp"
int main(int argc,char** argv) {
 try {
    if(argc<6 || argc>8) throw std::runtime_error("Usage: recording_test RAW_RGBA MANIFEST OUTPUT EYE_WIDTH HEIGHT");
    Size size{uint32_t(std::stoul(argv[4])),uint32_t(std::stoul(argv[5]))};
    std::filesystem::path output(argv[3]); std::filesystem::create_directories(output);
    std::ifstream input(argv[1],std::ios::binary), manifest(argv[2]);
    if(!input||!manifest) throw std::runtime_error("Cannot open recording inputs");
    RecordingContext ctx;
    if(!ctx.meta.opticalFlowEnabled||ctx.meta.vendor!=alvr::Vendor::Nvidia) throw std::runtime_error("NVIDIA optical flow required");
    Images gpu(ctx,size); gpu.initialize();
    alvr::motion::NativeMotion motion(ctx,{size.width(),size.height},vk::Format::eR8G8B8A8Unorm);
    motion.setGapFill(true);
    Pixels truth(size_t(size.width())*size.height*4), latest;
    std::vector<float> synthError, repeatError;
    std::ofstream csv(output/"metrics.csv");
    csv<<"frame,pose_ns,clock_ns,yaw_rad,variant,mae,psnr,gradient_mae,block8_gradient_mae,temporal_error_delta,flow_wall_ms,warp_wall_ms,optical_gpu_ms,warp_gpu_ms,still_scene_error\n";
    size_t index=0,targets=0; uint64_t pose,clock; double yaw; unsigned frame;
    double worst=-1, prepareMs=0, latestYaw=0;
    bool rotation=argc>=7;
    motion.setConfidenceFallback(argc==8 && std::string(argv[7])=="confidence");
    motion.setStableResidual(argc==8 && std::string(argv[7])=="stable");
    auto rotationBetween=[](double delta) {
        alvr::motion::NativeMotion::Rotation r;
        r.quaternion[1]=float(std::sin(delta/2)); r.quaternion[3]=float(std::cos(delta/2));
        for(auto t: {r.leftTans,r.rightTans}) {
            t[0]=-std::tan(1.0f); t[1]=std::tan(1.0f); t[2]=std::tan(1.0f); t[3]=-std::tan(1.0f);
        }
        return r;
    };
    // "interpolate": the odd target is synthesized after the NEXT even
    // picture is captured, halfway between the two (mode 9). Otherwise the
    // odd target is extrapolated from the two previous even pictures.
    bool const interpolate=argc==8 && std::string(argv[7])=="interpolate";
    if(interpolate) { motion.setStableResidual(true); motion.setInterpolate(true); }
    Pixels pendingTruth, shownBefore;
    bool pending=false; unsigned pendingFrame=0; uint64_t pendingPose=0,pendingClock=0; double pendingYaw=0;
    auto evaluate=[&](Pixels const& target, unsigned targetFrame, uint64_t targetPose, uint64_t targetClock,
                      double targetYaw, Pixels const& repeated) {
        auto transform=rotationBetween(targetYaw-latestYaw);
        if(!motion.predict(gpu.image,gpu.view,gpu.layout,targetPose,{},targetClock,rotation?&transform:nullptr)) throw std::runtime_error("Gap-fill target rejected");
        auto image=gpu.transfer(nullptr); auto timing=motion.lastPredictionTiming();
        auto synth=scoreRecording(image,target,size,synthError);
        auto repeat=scoreRecording(repeated,target,size,repeatError);
        double stillError=0;
        for(size_t pixel=0;pixel<image.size();pixel++) if(pixel%4!=3)
            stillError+=std::abs(int(image[pixel])-int(repeated[pixel]));
        stillError/=image.size()/4*3;
        for(auto const& row: {std::make_pair("synth",synth),std::make_pair("repeat",repeat)}) {
            auto const& s=row.second;
            csv<<targetFrame<<','<<targetPose<<','<<targetClock<<','<<targetYaw<<','<<row.first<<','<<s.mae<<','<<s.psnr<<','<<s.gradient<<','<<s.block<<','<<s.temporal<<','<<(row.first==std::string("synth")?prepareMs+timing.flowMs:0)<<','<<(row.first==std::string("synth")?timing.warpMs:0)<<','<<(row.first==std::string("synth")?timing.opticalGpuMs:0)<<','<<(row.first==std::string("synth")?timing.warpGpuMs:0)<<','<<(row.first==std::string("synth")?stillError:0)<<'\n';
        }
        // Optional image export for independent controller-silhouette scoring.
        // Existing metrics and normal evaluator output remain unchanged.
        if(auto value=std::getenv("MOTION_DUMP_FRAMES");value&&std::string(value)=="1") {
            auto stem="frame_"+std::to_string(targetFrame);
            writePpm(output/(stem+"_synth.ppm"),image,size);
            writePpm(output/(stem+"_truth.ppm"),target,size);
            writePpm(output/(stem+"_repeat.ppm"),repeated,size);
        }
        if(synth.mae>worst) {
            worst=synth.mae; writePpm(output/"worst_synth.ppm",image,size); writePpm(output/"worst_truth.ppm",target,size);
            Pixels diff=target; for(size_t i=0;i<diff.size();++i) diff[i]=i%4==3?255:uint8_t(std::min(255,4*std::abs(int(image[i])-int(target[i]))));
            writePpm(output/"worst_diff_x4.ppm",diff,size);
            std::ofstream(output/"worst_frame.txt")<<targetFrame<<'\n';
        }
        ++targets;
    };
    while(manifest>>frame>>pose>>clock>>yaw) {
        if(!input.read(reinterpret_cast<char*>(truth.data()),truth.size())) throw std::runtime_error("Truncated raw recording");
        if(index%2==0) {
            shownBefore=latest;
            motion.waitIdle(); gpu.transfer(&truth); auto transform=rotationBetween(index==0?0:yaw-latestYaw);
            motion.capture(gpu.image,gpu.layout,pose,clock,rotation?&transform:nullptr);
            latestYaw=yaw;
            auto prepareStart=std::chrono::steady_clock::now();
            motion.prepare(); prepareMs=elapsed(prepareStart); latest=truth;
            // The headset would otherwise keep showing the previous real picture.
            if(interpolate && pending && index>=8)
                evaluate(pendingTruth,pendingFrame,pendingPose,pendingClock,pendingYaw,shownBefore);
            pending=false;
        } else if(interpolate) {
            pendingTruth=truth; pending=true; pendingFrame=frame; pendingPose=pose; pendingClock=clock; pendingYaw=yaw;
        } else if(index>=7) { // gap filling waits for four real pictures (warm-up)
            evaluate(truth,frame,pose,clock,yaw,latest);
        }
        ++index;
    }
    if(targets<2) throw std::runtime_error("Too few target frames");
    std::cout<<"Evaluated "<<targets<<" targets (same NativeMotion gap-fill implementation)\n";
 } catch(std::exception const& e) {std::cerr<<e.what()<<'\n'; return 1;}
}
