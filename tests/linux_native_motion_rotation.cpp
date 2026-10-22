// Independent world-space renderer, asymmetric per-eye projection and GPU flow.
#define main syntheticCheckerMain
#include "linux_native_motion_images.cpp"
#undef main
#include "native_motion_recording_metrics.hpp"

using Rotation=alvr::motion::NativeMotion::Rotation;
Rotation asymmetric(double yaw) {
    Rotation r; r.quaternion[1]=std::sin(yaw/2);r.quaternion[3]=std::cos(yaw/2);
    float left[]={-1.28f,0.86f,1.08f,-0.94f},right[]={-0.83f,1.31f,1.02f,-1.00f};
    std::copy_n(left,4,r.leftTans);std::copy_n(right,4,r.rightTans);return r;
}
Pixels worldImage(Size size,double yaw) {
    Pixels pixels(size.bytes());auto projection=asymmetric(0);
    for(uint32_t y=0;y<size.height;y++) for(uint32_t x=0;x<size.width();x++) {
        bool right=x>=size.eyeWidth;auto t=right?projection.rightTans:projection.leftTans;
        double rx=t[0]+(t[1]-t[0])*(double(x%size.eyeWidth)+.5)/size.eyeWidth;
        double ry=t[2]+(t[3]-t[2])*(double(y)+.5)/size.height;
        // Independent yaw matrix maps the camera ray to a fixed world panorama.
        double wx=std::cos(yaw)*rx-std::sin(yaw),wz=-std::sin(yaw)*rx-std::cos(yaw);
        double longitude=std::atan2(wx,-wz),latitude=std::atan2(ry,std::hypot(wx,wz));
        for(unsigned c=0;c<3;c++) {
            double value=128+45*std::sin(longitude*(11+c*3)+.4*c)
                +35*std::cos(latitude*(13+c*2))
                +22*std::sin(longitude*21+latitude*17+c);
            pixels[(size_t(y)*size.width()+x)*4+c]=uint8_t(std::clamp(std::lround(value),0l,255l));
        }
        pixels[(size_t(y)*size.width()+x)*4+3]=255;
    }
    return pixels;
}
Pixels interior(Pixels pixels,Size size) {
    // Exclude only the known image-disocclusion border from numerical scoring.
    for(uint32_t y=0;y<size.height;y++) for(uint32_t x=0;x<size.width();x++)
        if(y<size.height/10 || y>=size.height*9/10 || x%size.eyeWidth<size.eyeWidth/10 || x%size.eyeWidth>=size.eyeWidth*9/10)
            for(unsigned c=0;c<3;c++) pixels[(size_t(y)*size.width()+x)*4+c]=0;
    return pixels;
}
int main(int argc,char** argv) {
 try {
    if(argc!=2) throw std::runtime_error("Usage: rotation_test OUTPUT");
    std::filesystem::path output(argv[1]);std::filesystem::create_directories(output);
    Size size{768,640};RecordingContext ctx;Images gpu(ctx,size);gpu.initialize();
    double correct[2]{};
    for(unsigned variant=0;variant<3;variant++) {
        alvr::motion::NativeMotion motion(ctx,{size.width(),size.height},vk::Format::eR8G8B8A8Unorm);
        motion.setGapFill(true);motion.setStableResidual(variant==1);
        auto transform=[&](double yaw) {auto r=asymmetric(yaw);if(variant==2) for(auto t:{r.leftTans,r.rightTans}) {t[0]=-std::tan(1.f);t[1]=std::tan(1.f);t[2]=std::tan(1.f);t[3]=-std::tan(1.f);}return r;};
        for(unsigned i=0;i<2;i++) {
            auto pixels=worldImage(size,i*.025);gpu.transfer(&pixels);auto r=transform(i?.025:0);
            motion.capture(gpu.image,gpu.layout,First+i*2*Tick,First+i*2*Tick,&r);motion.waitIdle();
        }
        motion.prepare();auto residual=motion.diagnosticResidualMagnitudes();std::sort(residual.begin(),residual.end());
        auto r=transform(.0125);if(!motion.predict(gpu.image,gpu.view,gpu.layout,First+3*Tick,{},First+3*Tick,&r)) throw std::runtime_error("Rotation rejected");
        auto prediction=gpu.transfer(nullptr),truth=worldImage(size,.0375);std::vector<float> errors;
        auto metrics=scoreRecording(interior(prediction,size),interior(truth,size),size,errors);
        double median=residual[residual.size()/2],p95=residual[size_t(.95*(residual.size()-1))];
        std::cout<<"rotation variant="<<variant<<" mae="<<metrics.mae<<" block8="<<metrics.block
            <<" residual_median_px="<<median<<" residual_p95_px="<<p95<<'\n';
        writePpm(output/("rotation_"+std::to_string(variant)+".ppm"),prediction,size);
        if(variant<2) {
            correct[variant]=metrics.mae;
            if(metrics.mae>.65 || metrics.block>.65 || median>.75 || p95>1.5) throw std::runtime_error("Asymmetric rotation or residual regression");
        } else if(metrics.mae<std::max(correct[0],correct[1])*2) throw std::runtime_error("Wrong-FOV negative control failed");
    }
    for(bool stable:{false,true}) for(bool noise:{false,true}) {
        alvr::motion::NativeMotion motion(ctx,{size.width(),size.height},vk::Format::eR8G8B8A8Unorm);
        motion.setGapFill(true);motion.setStableResidual(stable);auto pixels=worldImage(size,0),rPixels=pixels;auto r=asymmetric(0);
        for(unsigned i=0;i<2;i++) {
            auto input=pixels;
            if(noise && i==0) for(size_t j=0;j<input.size();j++)
                if(j%4!=3 && j%7==0) input[j]=uint8_t(std::min(255,int(input[j])+1));
            gpu.transfer(&input);motion.capture(gpu.image,gpu.layout,First+i*2*Tick,First+i*2*Tick,&r);motion.waitIdle();}
        if(!motion.predict(gpu.image,gpu.view,gpu.layout,First+3*Tick,{},First+3*Tick,&r)) throw std::runtime_error("Still rejected");
        auto predicted=gpu.transfer(nullptr);
        double error=0;for(size_t j=0;j<predicted.size();j++) if(j%4!=3) error+=std::abs(int(predicted[j])-int(rPixels[j]));
        error/=predicted.size()/4*3;
        std::cout<<"static mode="<<(stable?7:5)<<" one_code_noise="<<noise<<" newest_error="<<error<<'\n';
        if((!noise || stable) && predicted!=rPixels) throw std::runtime_error("Still image changed");
    }
    std::cout<<"PASS asymmetric projection, wrong-FOV control and bit-identical static textures\n";
 } catch(std::exception const& e) {std::cerr<<e.what()<<'\n';return 1;}
}
