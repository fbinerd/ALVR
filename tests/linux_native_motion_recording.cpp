// Reuse the checker's Vulkan image upload/readback and context stubs. This is
// a separate offline executable; it is never linked into the driver.
#define main syntheticCheckerMain
#include "linux_native_motion_images.cpp"
#undef main

struct Scores { double mae=0, psnr=0, gradient=0, block=0, temporal=0; };
Scores scoreRecording(Pixels const& prediction, Pixels const& truth, Size size,
                      std::vector<float>& previousError) {
    Scores s; double mse=0; size_t gradients=0, boundaries=0;
    std::vector<float> error(prediction.size()/4*3);
    for (uint32_t y=0; y<size.height; ++y) for(uint32_t x=0; x<size.width(); ++x)
        for (unsigned c=0; c<3; ++c) {
            size_t q=(size_t(y)*size.width()+x)*3+c;
            double e=int(prediction[(q/3)*4+c])-int(truth[(q/3)*4+c]);
            error[q]=float(e); s.mae+=std::abs(e); mse+=e*e;
            if(!previousError.empty()) s.temporal+=std::abs(e-previousError[q]);
            if(x && x!=size.eyeWidth) {
                double g=std::abs(e-error[q-3]); s.gradient+=g; ++gradients;
                if ((x%size.eyeWidth)%8==0) {s.block+=g; ++boundaries;}
            }
            if(y) {s.gradient+=std::abs(e-error[q-size.width()*3]); ++gradients;}
        }
    double n=error.size(); s.mae/=n; mse/=n; s.psnr=mse?10*std::log10(255*255/mse):100;
    s.gradient/=gradients; s.block/=boundaries;
    s.temporal=previousError.empty()?std::numeric_limits<double>::quiet_NaN():s.temporal/n;
    previousError=std::move(error); return s;
}
int main(int argc,char** argv) {
 try {
    if(argc!=6 && argc!=7) throw std::runtime_error("Usage: recording_test RAW_RGBA MANIFEST OUTPUT EYE_WIDTH HEIGHT");
    Size size{uint32_t(std::stoul(argv[4])),uint32_t(std::stoul(argv[5]))};
    std::filesystem::path output(argv[3]); std::filesystem::create_directories(output);
    std::ifstream input(argv[1],std::ios::binary), manifest(argv[2]);
    if(!input||!manifest) throw std::runtime_error("Cannot open recording inputs");
    alvr::VkContext ctx(std::vector<uint8_t>{});
    if(!ctx.meta.opticalFlowEnabled||ctx.meta.vendor!=alvr::Vendor::Nvidia) throw std::runtime_error("NVIDIA optical flow required");
    Images gpu(ctx,size); gpu.initialize();
    alvr::motion::NativeMotion motion(ctx,{size.width(),size.height},vk::Format::eR8G8B8A8Unorm);
    motion.setGapFill(true);
    Pixels truth(size_t(size.width())*size.height*4), latest;
    std::vector<float> synthError, repeatError;
    std::ofstream csv(output/"metrics.csv");
    csv<<"frame,pose_ns,clock_ns,yaw_rad,variant,mae,psnr,gradient_mae,block8_gradient_mae,temporal_error_delta,flow_wall_ms,warp_wall_ms,optical_gpu_ms,warp_gpu_ms\n";
    size_t index=0,targets=0; uint64_t pose,clock; double yaw; unsigned frame;
    double worst=-1, prepareMs=0, latestYaw=0;
    bool rotation=argc==7;
    auto rotationBetween=[](double delta) {
        alvr::motion::NativeMotion::Rotation r;
        r.quaternion[1]=float(std::sin(delta/2)); r.quaternion[3]=float(std::cos(delta/2));
        for(auto t: {r.leftTans,r.rightTans}) {
            t[0]=-std::tan(1.0f); t[1]=std::tan(1.0f); t[2]=std::tan(1.0f); t[3]=-std::tan(1.0f);
        }
        return r;
    };
    while(manifest>>frame>>pose>>clock>>yaw) {
        if(!input.read(reinterpret_cast<char*>(truth.data()),truth.size())) throw std::runtime_error("Truncated raw recording");
        if(index%2==0) {
            motion.waitIdle(); gpu.transfer(&truth); auto transform=rotationBetween(index==0?0:yaw-latestYaw);
            motion.capture(gpu.image,gpu.layout,pose,clock,rotation?&transform:nullptr);
            latestYaw=yaw;
            auto prepareStart=std::chrono::steady_clock::now();
            motion.prepare(); prepareMs=elapsed(prepareStart); latest=truth;
        } else if(index>=3) {
            auto transform=rotationBetween(yaw-latestYaw);
            if(!motion.predict(gpu.image,gpu.view,gpu.layout,pose,{},clock,rotation?&transform:nullptr)) throw std::runtime_error("Gap-fill target rejected");
            auto image=gpu.transfer(nullptr); auto timing=motion.lastPredictionTiming();
            auto synth=scoreRecording(image,truth,size,synthError);
            auto repeat=scoreRecording(latest,truth,size,repeatError);
            for(auto const& row: {std::make_pair("synth",synth),std::make_pair("repeat",repeat)}) {
                auto const& s=row.second;
                csv<<frame<<','<<pose<<','<<clock<<','<<yaw<<','<<row.first<<','<<s.mae<<','<<s.psnr<<','<<s.gradient<<','<<s.block<<','<<s.temporal<<','<<(row.first==std::string("synth")?prepareMs+timing.flowMs:0)<<','<<(row.first==std::string("synth")?timing.warpMs:0)<<','<<(row.first==std::string("synth")?timing.opticalGpuMs:0)<<','<<(row.first==std::string("synth")?timing.warpGpuMs:0)<<'\n';
            }
            if(synth.mae>worst) {
                worst=synth.mae; writePpm(output/"worst_synth.ppm",image,size); writePpm(output/"worst_truth.ppm",truth,size);
                Pixels diff=truth; for(size_t i=0;i<diff.size();++i) diff[i]=i%4==3?255:uint8_t(std::min(255,4*std::abs(int(image[i])-int(truth[i]))));
                writePpm(output/"worst_diff_x4.ppm",diff,size);
                std::ofstream(output/"worst_frame.txt")<<frame<<'\n';
            }
            ++targets;
        }
        ++index;
    }
    if(targets<2) throw std::runtime_error("Too few target frames");
    std::cout<<"Evaluated "<<targets<<" targets (same NativeMotion gap-fill implementation)\n";
 } catch(std::exception const& e) {std::cerr<<e.what()<<'\n'; return 1;}
}
