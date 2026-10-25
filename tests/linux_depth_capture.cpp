// Offline only: production query provider + registration + unmodified capture.
#include "platform/linux/depth/ProductionDepthSource.hpp"
#include "alvr_server/Settings.h"
#include <cstdarg>
#include <filesystem>
#include <iostream>
#include <spawn.h>
#include <sys/wait.h>
#include <thread>
extern char** environ;
namespace alvr::motion {extern int depthTestOpticalExecutions;}
const Settings* Settings_Instance(){static Settings s=[] {Settings v{};strcpy(v.m_renderGpu,"Intel");return v;}();return &s;}
void Info(char const* f,...){va_list a;va_start(a,f);vprintf(f,a);va_end(a);puts("");}
void Warn(char const* f,...){va_list a;va_start(a,f);vfprintf(stderr,f,a);va_end(a);fputs("\n",stderr);}
void Error(char const* f,...){va_list a;va_start(a,f);vfprintf(stderr,f,a);va_end(a);fputs("\n",stderr);}
using namespace mhl::depth;
using Clock=std::chrono::steady_clock;
namespace fs=std::filesystem;
static void check(bool b,char const* why){if(!b)throw std::runtime_error(why);}
static uint64_t ns(){return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();}
struct GpuImage {
    alvr::VkContext& ctx;vk::Image image;vk::DeviceMemory memory;vk::CommandPool pool;vk::CommandBuffer command;
    GpuImage(alvr::VkContext& c):ctx(c){
        image=ctx.dev.createImage({.imageType=vk::ImageType::e2D,.format=vk::Format::eR8G8B8A8Unorm,.extent={32,16,1},.mipLevels=1,.arrayLayers=1,.samples=vk::SampleCountFlagBits::e1,.tiling=vk::ImageTiling::eOptimal,.usage=vk::ImageUsageFlagBits::eTransferSrc|vk::ImageUsageFlagBits::eTransferDst|vk::ImageUsageFlagBits::eSampled|vk::ImageUsageFlagBits::eStorage});
        auto req=ctx.dev.getImageMemoryRequirements(image);uint32_t type=0;while(!(req.memoryTypeBits&(1u<<type)))++type;
        memory=ctx.dev.allocateMemory({.allocationSize=req.size,.memoryTypeIndex=type});ctx.dev.bindImageMemory(image,memory,0);
        pool=ctx.dev.createCommandPool({.queueFamilyIndex=ctx.meta.queueFamily});command=ctx.dev.allocateCommandBuffers({.commandPool=pool,.level=vk::CommandBufferLevel::ePrimary,.commandBufferCount=1})[0];
        command.begin(vk::CommandBufferBeginInfo{});vk::ImageMemoryBarrier b{.dstAccessMask=vk::AccessFlagBits::eTransferWrite,.oldLayout=vk::ImageLayout::eUndefined,.newLayout=vk::ImageLayout::eGeneral,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=image,.subresourceRange={vk::ImageAspectFlagBits::eColor,0,1,0,1}};
        command.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe,vk::PipelineStageFlagBits::eTransfer,{},nullptr,nullptr,b);
        command.clearColorImage(image,vk::ImageLayout::eGeneral,vk::ClearColorValue(std::array<float,4>{.2f,.3f,.4f,1.f}),b.subresourceRange);command.end();
        ctx.useQueue([&](auto q){q.submit(vk::SubmitInfo{.commandBufferCount=1,.pCommandBuffers=&command});q.waitIdle();});
    }
    ~GpuImage(){ctx.dev.destroy(pool);ctx.dev.destroy(image);ctx.dev.free(memory);}
};
// Read the EXACT VkImage returned by the production callback, not a parallel
// receiver/test packet. Blocking staging is confined to this executable.
static void checkDepth(alvr::VkContext& ctx,alvr::motion::NativeMotion::Depth const& d,unsigned id){
    check(d.extent==vk::Extent2D{32,16},"callback extent");
    auto buffer=ctx.dev.createBuffer({.size=2048,.usage=vk::BufferUsageFlagBits::eTransferDst});auto req=ctx.dev.getBufferMemoryRequirements(buffer);auto props=ctx.physDev.getMemoryProperties();uint32_t type=0;
    while(type<props.memoryTypeCount && (!(req.memoryTypeBits&(1u<<type)) || (props.memoryTypes[type].propertyFlags&(vk::MemoryPropertyFlagBits::eHostVisible|vk::MemoryPropertyFlagBits::eHostCoherent))!=(vk::MemoryPropertyFlagBits::eHostVisible|vk::MemoryPropertyFlagBits::eHostCoherent)))++type;
    check(type<props.memoryTypeCount,"staging memory");auto mem=ctx.dev.allocateMemory({.allocationSize=req.size,.memoryTypeIndex=type});ctx.dev.bindBufferMemory(buffer,mem,0);
    auto pool=ctx.dev.createCommandPool({.queueFamilyIndex=ctx.meta.queueFamily});auto command=ctx.dev.allocateCommandBuffers({.commandPool=pool,.level=vk::CommandBufferLevel::ePrimary,.commandBufferCount=1})[0];command.begin(vk::CommandBufferBeginInfo{});
    vk::ImageMemoryBarrier b{.srcAccessMask=vk::AccessFlagBits::eShaderRead|vk::AccessFlagBits::eTransferWrite,.dstAccessMask=vk::AccessFlagBits::eTransferRead,.oldLayout=d.layout,.newLayout=vk::ImageLayout::eTransferSrcOptimal,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=d.image,.subresourceRange={vk::ImageAspectFlagBits::eColor,0,1,0,1}};
    command.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,vk::PipelineStageFlagBits::eTransfer,{},nullptr,nullptr,b);
    vk::BufferImageCopy copy{.imageSubresource={vk::ImageAspectFlagBits::eColor,0,0,1},.imageExtent={32,16,1}};command.copyImageToBuffer(d.image,vk::ImageLayout::eTransferSrcOptimal,buffer,copy);
    b.srcAccessMask=vk::AccessFlagBits::eTransferRead;b.dstAccessMask=vk::AccessFlagBits::eShaderRead;b.oldLayout=vk::ImageLayout::eTransferSrcOptimal;b.newLayout=d.layout;
    command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,vk::PipelineStageFlagBits::eAllCommands,{},nullptr,nullptr,b);
    vk::BufferMemoryBarrier host{.srcAccessMask=vk::AccessFlagBits::eTransferWrite,.dstAccessMask=vk::AccessFlagBits::eHostRead,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.buffer=buffer,.size=2048};
    command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,vk::PipelineStageFlagBits::eHost,{},nullptr,host,nullptr);command.end();
    ctx.useQueue([&](auto q){q.submit(vk::SubmitInfo{.commandBufferCount=1,.pCommandBuffers=&command});q.waitIdle();});auto data=static_cast<float*>(ctx.dev.mapMemory(mem,0,2048));
    for(unsigned y=0;y<16;y++)for(unsigned x=0;x<32;x++)check(data[y*32+x]==float(id)+(x>=16?.25f:0),"callback image wrong frame/pixels");
    ctx.dev.unmapMemory(mem);ctx.dev.destroy(pool);ctx.dev.destroy(buffer);ctx.dev.free(mem);
}
struct EncoderHook {
    using DepthSource=std::function<std::optional<alvr::motion::NativeMotion::Depth>(uint64_t)>;
    DepthSource source;void setDepthSource(DepthSource s){source=std::move(s);}
};
int main(int argc,char** argv){signal(SIGPIPE,SIG_IGN);pid_t producer=-1;int control=-1;try{
    check(argc==3,"Usage: depth_capture_test ONLINE_TEST_EXECUTABLE NEW_OUTPUT");fs::path root=fs::absolute(argv[2]);check(!fs::exists(root),"fresh output required");fs::create_directories(root);
    auto now=ns();Calibration c{};c.producerId=42;c.referenceSpace=84;c.monotonicAnchor=now+1000000000;c.targetAnchor=1000000000;c.validFrom=now;c.validUntil=now+60000000000ull;c.uncertaintyNs=100;
    auto cp=(root/"depth_calibration").string(),socket=(root/"depth_receiver.sock").string();std::ofstream(cp,std::ios::binary).write((char*)&c,sizeof(c));chmod(cp.c_str(),0600);
    alvr::VkContext ctx(std::vector<uint8_t>{});check(ctx.physDev.getProperties().vendorID==0x8086,"Intel required");
    {
        EncoderHook encoder;alvr::depth::ProductionDepthSource binding(root.string()+"/");Device device;device.instance=ctx.instance;device.physical=ctx.physDev;device.device=ctx.dev;device.family=ctx.meta.queueFamily;ctx.useQueue([&](auto q){device.queue=q;});
        alvr::depth::ComposedDepthPicture pic;pic.viewsValid=pic.unwarpedSingleLayer=true;
        for(unsigned e=0;e<2;e++){pic.localEyes[e]={{e?.032f:-.032f,0,0},{0,0,0,1}};pic.fov[e]={-.8f,.9f,.85f,-.75f};pic.sourceUv[e]={0,0,1,1};}
        binding.prepare(encoder,device,pic);check(!encoder.source&&!fs::exists(socket),"default off changed hook/socket");
        std::ofstream(root/"diag_depth_receiver")<<"1\n";std::this_thread::sleep_for(std::chrono::milliseconds(1005));binding.prepare(encoder,device,pic,[](auto const& s){std::cout<<s<<'\n';});check(bool(encoder.source),"production registration");encoder.source(0);check(fs::exists(socket),"ingress missing");
        // Test-only estimator constructor substitution; never enabled on a live
        // Intel driver. No execute/predict/prepare call is allowed in this test.
        ctx.meta.opticalFlowEnabled=true;ctx.meta.opticalFlowQueueFamily=ctx.meta.queueFamily;
        alvr::motion::NativeMotion capture(ctx,{32,16},vk::Format::eR8G8B8A8Unorm);capture.setGapFill(true);GpuImage colour(ctx);
        int fd[2];check(pipe(fd)==0,"control pipe");posix_spawn_file_actions_t actions;posix_spawn_file_actions_init(&actions);posix_spawn_file_actions_adddup2(&actions,fd[0],0);posix_spawn_file_actions_addclose(&actions,fd[1]);
        std::string exe=fs::absolute(argv[1]),gpu="Intel";char* args[]={exe.data(),(char*)"--producer",cp.data(),gpu.data(),socket.data(),nullptr};check(posix_spawn(&producer,exe.c_str(),&actions,nullptr,args,environ)==0,"spawn");posix_spawn_file_actions_destroy(&actions);close(fd[0]);control=fd[1];
        unsigned deferredAttachments=0;
        for(unsigned id=1;id<=60;id++){
            pic.timestamp=c.targetAnchor+id*13888889ull;binding.prepare(encoder,device,pic);check(!encoder.source(pic.timestamp+1),"wrong callback timestamp accepted");
            check(write(control,"x",1)==1,"control write");std::optional<alvr::motion::NativeMotion::Depth> depth;auto deadline=Clock::now()+std::chrono::seconds(10);
            while(!(depth=encoder.source(pic.timestamp))){check(Clock::now()<deadline,"production callback timeout");std::this_thread::sleep_for(std::chrono::milliseconds(1));}
            checkDepth(ctx,*depth,id);
            // Alternate direct game depth and the post-send attachment contract.
            capture.capture(colour.image,vk::ImageLayout::eGeneral,pic.timestamp,c.monotonicAnchor+id*13888889ull,nullptr,id%2?nullptr:&*depth);
            if(id%2){check(capture.attachDepth(*depth,true),"deferred depth attachment rejected");++deferredAttachments;}
            capture.waitIdle();if(id>1)check(capture.depthActive(),"NativeMotion did not retain both depth pictures");
            encoder.source(pic.timestamp);
        }
        check(deferredAttachments==30,"deferred attachment count");
        check(!alvr::motion::depthTestOpticalExecutions,"optical flow executed");close(control);control=-1;int status;check(waitpid(producer,&status,0)==producer&&WIFEXITED(status)&&!WEXITSTATUS(status),"producer failed");producer=-1;
        // Disable live-read switch; owned images remain valid through next
        // present, while ingress is removed and no depth is returned.
        std::ofstream(root/"diag_depth_receiver")<<"0\n";std::this_thread::sleep_for(std::chrono::milliseconds(1005));binding.prepare(encoder,device,pic);check(!encoder.source(pic.timestamp),"disabled source returned depth");check(!fs::exists(socket),"disable left listening socket");
        std::cout<<"PASS production_callback_to_real_NativeMotion_capture=60 byte_exact_images=60 optical_flow_calls=0 deferred_attachments=30 default_off=PASS live_disable=PASS\n";
    }ctx.destroy();
}catch(std::exception const& e){if(control>=0)close(control);if(producer>0){kill(producer,SIGTERM);waitpid(producer,nullptr,0);}std::cerr<<e.what()<<'\n';return 1;}}
