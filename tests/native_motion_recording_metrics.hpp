#pragma once
// Shared numerical metrics for offscreen recording and projection checks.
// VkContext uses explicit destroy(), not an automatic destructor. Declare this
// before Images/NativeMotion so all their resources die before device/instance.
struct RecordingContext : alvr::VkContext {
    RecordingContext() : alvr::VkContext(std::vector<uint8_t>{}) {}
    ~RecordingContext() {
        try { destroy(); }
        catch (...) { Warn("Recording checker: Vulkan context teardown failed"); }
    }
};
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
