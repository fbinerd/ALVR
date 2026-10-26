#include "gpu_transport.hpp"
#include "payload_validation.h"
#include "plane_workers.hpp"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <dlfcn.h>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace mhl::depth {
namespace {
constexpr uint32_t Magic=0x4750444d,Version=1;
constexpr size_t Capacity=4;
constexpr uint64_t MaxBytes=256ull*1024*1024;
constexpr auto BufferUsage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT;
struct Wire {uint32_t magic=Magic,version=Version,kind=1,memoryType{};uint64_t allocationBytes{},lease{};MhlDepthPacket packet{};};
static_assert(sizeof(Wire)==640);
#define DEVICE_FUNCTIONS(X) \
 X(CreateBuffer) X(DestroyBuffer) X(GetBufferMemoryRequirements) X(AllocateMemory) X(FreeMemory) \
 X(BindBufferMemory) X(MapMemory) X(UnmapMemory) X(GetMemoryFdKHR) X(CreateSemaphore) X(DestroySemaphore) \
 X(GetSemaphoreFdKHR) X(ImportSemaphoreFdKHR) X(GetSemaphoreCounterValue) X(CreateCommandPool) X(DestroyCommandPool) \
 X(AllocateCommandBuffers) X(BeginCommandBuffer) X(EndCommandBuffer) X(CmdPipelineBarrier) X(CmdCopyBufferToImage) \
 X(CmdCopyImageToBuffer) X(CmdCopyBuffer) X(CreateFence) X(DestroyFence) X(GetFenceStatus) X(WaitForFences) \
 X(QueueSubmit) X(CreateImage) X(DestroyImage) X(GetImageMemoryRequirements) X(BindImageMemory) X(DeviceWaitIdle) X(DestroyDevice)
struct Api {
    PFN_vkGetInstanceProcAddr gipa{};PFN_vkGetDeviceProcAddr gdpa{};
#define FIELD(n) PFN_vk##n n{};
    DEVICE_FUNCTIONS(FIELD)
#undef FIELD
    explicit Api(VkDevice device) {
        static void* loader=dlopen("libvulkan.so.1",RTLD_NOW|RTLD_LOCAL);
        if(!loader)throw std::runtime_error("Vulkan loader unavailable");
        gipa=reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(loader,"vkGetInstanceProcAddr"));
        gdpa=reinterpret_cast<PFN_vkGetDeviceProcAddr>(dlsym(loader,"vkGetDeviceProcAddr"));
        if(!gipa||!gdpa)throw std::runtime_error("Vulkan loader entrypoints unavailable");
#define LOAD(n) n=reinterpret_cast<PFN_vk##n>(gdpa(device,"vk" #n));if(!n)throw std::runtime_error("Missing Vulkan entrypoint: " #n);
        DEVICE_FUNCTIONS(LOAD)
#undef LOAD
    }
};
void check(VkResult result,char const* operation) {
    if(result!=VK_SUCCESS)throw std::runtime_error(std::string(operation)+": VkResult "+std::to_string(result));
}
void closeFd(int& fd){if(fd>=0)close(fd);fd=-1;}
struct Received {Wire wire{};std::vector<int> fds;~Received(){for(int f:fds)if(f>=0)close(f);}};
int receive(int socket,Received& data) {
    char control[CMSG_SPACE(sizeof(int)*8)]{};iovec iov{&data.wire,sizeof(data.wire)};
    msghdr msg{};msg.msg_iov=&iov;msg.msg_iovlen=1;msg.msg_control=control;msg.msg_controllen=sizeof(control);
    auto count=recvmsg(socket,&msg,MSG_DONTWAIT|MSG_CMSG_CLOEXEC);
    if(count<0)return errno==EAGAIN||errno==EWOULDBLOCK?0:-1;
    if(!count)return -1;
    for(auto* c=CMSG_FIRSTHDR(&msg);c;c=CMSG_NXTHDR(&msg,c))if(c->cmsg_level==SOL_SOCKET&&c->cmsg_type==SCM_RIGHTS&&c->cmsg_len>=CMSG_LEN(0)){
        size_t n=(c->cmsg_len-CMSG_LEN(0))/sizeof(int);auto* f=reinterpret_cast<int*>(CMSG_DATA(c));data.fds.insert(data.fds.end(),f,f+n);
    }
    if(count!=sizeof(Wire)||(msg.msg_flags&(MSG_TRUNC|MSG_CTRUNC)))return -2;
    return 1;
}
bool sendWire(int socket,Wire const& wire,std::array<int,2> fds={-1,-1}) {
    iovec iov{const_cast<Wire*>(&wire),sizeof(wire)};msghdr msg{};msg.msg_iov=&iov;msg.msg_iovlen=1;
    char control[CMSG_SPACE(sizeof(int)*2)]{};
    if(fds[0]>=0){msg.msg_control=control;msg.msg_controllen=sizeof(control);auto* c=CMSG_FIRSTHDR(&msg);
        c->cmsg_level=SOL_SOCKET;c->cmsg_type=SCM_RIGHTS;c->cmsg_len=CMSG_LEN(sizeof(int)*2);memcpy(CMSG_DATA(c),fds.data(),sizeof(int)*2);}
    return sendmsg(socket,&msg,MSG_DONTWAIT|MSG_NOSIGNAL)==sizeof(wire);
}
bool sameUid(int socket){ucred peer{};socklen_t n=sizeof(peer);return !getsockopt(socket,SOL_SOCKET,SO_PEERCRED,&peer,&n)&&peer.uid==getuid();}
sockaddr_un address(std::string const& path){sockaddr_un a{};a.sun_family=AF_UNIX;if(path.size()>=sizeof(a.sun_path))throw std::runtime_error("Unix socket path too long");memcpy(a.sun_path,path.c_str(),path.size()+1);return a;}
std::array<uint8_t,16> uuid(Device d,Api& a){
    auto get=reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(a.gipa(d.instance,"vkGetPhysicalDeviceProperties2"));
    VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};props.pNext=&id;get(d.physical,&props);
    std::array<uint8_t,16> out;std::copy_n(id.deviceUUID,16,out.begin());return out;
}
uint32_t memoryType(Device d,Api& a,uint32_t bits,VkMemoryPropertyFlags flags,bool preferCached=false){
    auto get=reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(a.gipa(d.instance,"vkGetPhysicalDeviceMemoryProperties"));VkPhysicalDeviceMemoryProperties p;get(d.physical,&p);
    if(preferCached)for(uint32_t i=0;i<p.memoryTypeCount;i++)
        if((bits&(1u<<i))&&(p.memoryTypes[i].propertyFlags&(flags|VK_MEMORY_PROPERTY_HOST_CACHED_BIT))==(flags|VK_MEMORY_PROPERTY_HOST_CACHED_BIT))return i;
    for(uint32_t i=0;i<p.memoryTypeCount;i++)if((bits&(1u<<i))&&(p.memoryTypes[i].propertyFlags&flags)==flags)return i;
    throw std::runtime_error("No compatible Vulkan memory type");
}
struct Resource {
    Device device;Api& api;VkBuffer buffer{};VkDeviceMemory memory{},imageMemory{};VkImage image{};VkExtent2D extent{};
    VkSemaphore semaphore{};VkCommandPool pool{};VkCommandBuffer command{};VkFence fence{};uint32_t type{};uint64_t allocation{};Wire wire{};
    bool memoryFdConsumed=false,semaphoreFdConsumed=false,submitted=false;
    void* hostMapped=nullptr;
    uint64_t connectionEpoch=0;
    Resource(Device d,Api& a):device(d),api(a){}
    ~Resource(){
        if(hostMapped)api.UnmapMemory(device.device,memory);
        if(fence)api.DestroyFence(device.device,fence,nullptr);
        if(pool)api.DestroyCommandPool(device.device,pool,nullptr);
        if(image)api.DestroyImage(device.device,image,nullptr);
        if(imageMemory)api.FreeMemory(device.device,imageMemory,nullptr);
        if(buffer)api.DestroyBuffer(device.device,buffer,nullptr);
        if(memory)api.FreeMemory(device.device,memory,nullptr);
        if(semaphore)api.DestroySemaphore(device.device,semaphore,nullptr);
    }
    void* mapHost(){if(!hostMapped)check(api.MapMemory(device.device,memory,0,wire.allocationBytes?wire.allocationBytes:allocation,0,&hostMapped),"producer map");return hostMapped;}
    void unmapHost(){if(hostMapped){api.UnmapMemory(device.device,memory);hostMapped=nullptr;}}
    bool ready()const{return !submitted||api.GetFenceStatus(device.device,fence)==VK_SUCCESS;}
    void wait(){if(submitted)check(api.WaitForFences(device.device,1,&fence,VK_TRUE,10'000'000'000ull),"diagnostic fence wait");}
    void commands(){
        VkCommandPoolCreateInfo p{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};p.queueFamilyIndex=device.family;check(api.CreateCommandPool(device.device,&p,nullptr,&pool),"command pool");
        VkCommandBufferAllocateInfo c{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};c.commandPool=pool;c.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;c.commandBufferCount=1;check(api.AllocateCommandBuffers(device.device,&c,&command),"command buffer");
        VkCommandBufferBeginInfo b{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};b.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;check(api.BeginCommandBuffer(command,&b),"begin");
        VkFenceCreateInfo f{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};check(api.CreateFence(device.device,&f,nullptr,&fence),"fence");
    }
    void makeBuffer(uint64_t bytes,bool external,int fd=-1,uint32_t importedType=0,uint64_t importedAllocation=0){
        VkExternalMemoryBufferCreateInfo e{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};e.handleTypes=VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        VkBufferCreateInfo b{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};b.pNext=external?&e:nullptr;b.size=bytes;b.usage=BufferUsage;b.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
        check(api.CreateBuffer(device.device,&b,nullptr,&buffer),"buffer");VkMemoryRequirements req;api.GetBufferMemoryRequirements(device.device,buffer,&req);
        type=fd>=0?importedType:memoryType(device,api,req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,external&&!getenv("MHL_DEPTH_TEST_UNCACHED_EXPORT"));
        if(type>=32||!(req.memoryTypeBits&(1u<<type)))throw std::runtime_error("import memory type mismatch");
        allocation=fd>=0?importedAllocation:req.size;if(allocation<req.size||allocation>MaxBytes)throw std::runtime_error("allocation bounds");
        VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};dedicated.buffer=buffer;
        VkExportMemoryAllocateInfo ex{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};ex.handleTypes=VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;ex.pNext=&dedicated;
        VkImportMemoryFdInfoKHR im{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};im.handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;im.fd=fd;im.pNext=&dedicated;
        VkMemoryAllocateInfo m{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};m.allocationSize=allocation;m.memoryTypeIndex=type;m.pNext=external?(fd>=0?static_cast<void*>(&im):static_cast<void*>(&ex)):nullptr;
        check(api.AllocateMemory(device.device,&m,nullptr,&memory),"memory allocation/import");memoryFdConsumed=fd>=0;check(api.BindBufferMemory(device.device,buffer,memory,0),"bind buffer");
    }
    void makeSemaphore(int fd=-1){
        VkExportSemaphoreCreateInfo ex{VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};ex.handleTypes=VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
        VkSemaphoreTypeCreateInfo typeInfo{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};typeInfo.semaphoreType=VK_SEMAPHORE_TYPE_TIMELINE;typeInfo.pNext=fd<0?&ex:nullptr;
        VkSemaphoreCreateInfo s{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};s.pNext=&typeInfo;check(api.CreateSemaphore(device.device,&s,nullptr,&semaphore),"timeline semaphore");
        if(fd>=0){VkImportSemaphoreFdInfoKHR i{VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR};i.semaphore=semaphore;i.handleType=VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;i.fd=fd;check(api.ImportSemaphoreFdKHR(device.device,&i),"timeline import");semaphoreFdConsumed=true;}
    }
    void makeImage(MhlDepthPacket const& p){
        extent={p.eyes[0].depth.width*2,p.eyes[0].depth.height};
        VkImageCreateInfo i{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};i.imageType=VK_IMAGE_TYPE_2D;i.format=VK_FORMAT_R32_SFLOAT;i.extent={extent.width,extent.height,1};i.mipLevels=1;i.arrayLayers=1;i.samples=VK_SAMPLE_COUNT_1_BIT;i.tiling=VK_IMAGE_TILING_OPTIMAL;i.usage=VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;i.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
        check(api.CreateImage(device.device,&i,nullptr,&image),"private depth image");VkMemoryRequirements req;api.GetImageMemoryRequirements(device.device,image,&req);
        VkMemoryAllocateInfo m{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};m.allocationSize=req.size;m.memoryTypeIndex=memoryType(device,api,req.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        check(api.AllocateMemory(device.device,&m,nullptr,&imageMemory),"private image allocation");check(api.BindImageMemory(device.device,image,imageMemory,0),"private image bind");
    }
};
void payloadCheck(MhlDepthPacket const& p,void const* data,size_t bytes,PlaneWorkers& workers){
    if(auto error=mhl_depth_validate(&p,sizeof(p)))throw std::runtime_error(error);
    if(!data||bytes!=p.buffer_size||bytes>MaxBytes)throw std::runtime_error("payload size");
    std::array<char const*,4> errors{};
    bool checked=workers.normalize([&](unsigned i){
        auto const& plane=i%2?p.eyes[i/2].colour:p.eyes[i/2].depth;
        errors[i]=mhl_depth_validate_plane_pixels(&plane,i%2,data);return errors[i]==nullptr;
    });
    for(auto error:errors)if(error)throw std::runtime_error(error);
    if(!checked)throw std::runtime_error("payload validation failed");
}
}

struct OwnedDevice::Impl {Device d{};std::unique_ptr<Api> api;};
OwnedDevice::OwnedDevice(std::string const& name):p(std::make_unique<Impl>()){
    void* loader=dlopen("libvulkan.so.1",RTLD_NOW|RTLD_LOCAL);if(!loader)throw std::runtime_error("Vulkan loader");
    auto gipa=reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(loader,"vkGetInstanceProcAddr"));
    auto create=reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr,"vkCreateInstance"));
    VkApplicationInfo a{VK_STRUCTURE_TYPE_APPLICATION_INFO};a.pApplicationName="MHL depth transport offline";a.apiVersion=VK_API_VERSION_1_2;
    VkInstanceCreateInfo i{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};i.pApplicationInfo=&a;check(create(&i,nullptr,&p->d.instance),"instance");
    try {
        auto enumerate=reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(gipa(p->d.instance,"vkEnumeratePhysicalDevices"));
        auto props=reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(gipa(p->d.instance,"vkGetPhysicalDeviceProperties"));
        auto queues=reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(gipa(p->d.instance,"vkGetPhysicalDeviceQueueFamilyProperties"));
        uint32_t count=0;check(enumerate(p->d.instance,&count,nullptr),"devices");std::vector<VkPhysicalDevice> devices(count);check(enumerate(p->d.instance,&count,devices.data()),"devices");
        for(auto dev:devices){VkPhysicalDeviceProperties prop;props(dev,&prop);if(std::string(prop.deviceName).find(name)!=std::string::npos){p->d.physical=dev;break;}}
        if(!p->d.physical)throw std::runtime_error("Requested GPU not found");
        queues(p->d.physical,&count,nullptr);std::vector<VkQueueFamilyProperties> families(count);queues(p->d.physical,&count,families.data());bool found=false;
        for(uint32_t n=0;n<count;n++)if(families[n].queueFlags&VK_QUEUE_GRAPHICS_BIT){p->d.family=n;found=true;break;}
        if(!found)throw std::runtime_error("No graphics/transfer queue");
        float priority=1;VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};q.queueFamilyIndex=p->d.family;q.queueCount=1;q.pQueuePriorities=&priority;
        char const* extensions[]={VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME};
        VkPhysicalDeviceTimelineSemaphoreFeatures timeline{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};timeline.timelineSemaphore=VK_TRUE;
        VkDeviceCreateInfo d{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};d.pNext=&timeline;d.queueCreateInfoCount=1;d.pQueueCreateInfos=&q;d.enabledExtensionCount=2;d.ppEnabledExtensionNames=extensions;
        auto make=reinterpret_cast<PFN_vkCreateDevice>(gipa(p->d.instance,"vkCreateDevice"));check(make(p->d.physical,&d,nullptr,&p->d.device),"device");p->api=std::make_unique<Api>(p->d.device);
        auto queue=reinterpret_cast<PFN_vkGetDeviceQueue>(p->api->gdpa(p->d.device,"vkGetDeviceQueue"));queue(p->d.device,p->d.family,0,&p->d.queue);
    }catch(...){auto destroy=reinterpret_cast<PFN_vkDestroyInstance>(gipa(p->d.instance,"vkDestroyInstance"));destroy(p->d.instance,nullptr);throw;}
}
OwnedDevice::~OwnedDevice(){if(p->api){p->api->DeviceWaitIdle(p->d.device);p->api->DestroyDevice(p->d.device,nullptr);auto destroy=reinterpret_cast<PFN_vkDestroyInstance>(p->api->gipa(p->d.instance,"vkDestroyInstance"));destroy(p->d.instance,nullptr);}}
Device OwnedDevice::get()const{return p->d;}

struct Producer::Impl {
    PlaneWorkers validators;
    Device d;Api api;std::string path;int socket=-1;uint64_t serial=0;Counters stats{};
    std::unique_ptr<Resource> prepared;size_t preparedBytes=0;
    struct Loan {std::unique_ptr<Resource> resource;bool acknowledged=false;};std::deque<Loan> loans;
    Impl(Device d,std::string path):d(d),api(d.device),path(std::move(path)){}
    ~Impl(){api.DeviceWaitIdle(d.device);closeFd(socket);loans.clear();}
    void poll(){
        if(socket>=0)for(unsigned n=0;n<16;n++){
            Received r;int got=receive(socket,r);if(!got)break;
            if(got<0){closeFd(socket);break;}
            if(r.wire.magic!=Magic||r.wire.version!=Version||r.wire.kind!=2||!r.fds.empty()){stats.protocol++;continue;}
            for(auto& loan:loans)if(loan.resource->wire.lease==r.wire.lease
                &&loan.resource->wire.packet.producer_id==r.wire.packet.producer_id
                &&loan.resource->wire.packet.frame_id==r.wire.packet.frame_id
                &&!loan.acknowledged){loan.acknowledged=true;stats.acknowledged++;break;}
        }
        // No allocation is ever rewritten. After disconnect, exporter handles
        // can retire once its GPU work completes; imported/queued FDs retain
        // their own backing-allocation references on the same physical device.
        for(auto it=loans.begin();it!=loans.end();)if((it->acknowledged||socket<0)&&it->resource->ready())it=loans.erase(it);else ++it;
    }
};
Producer::Producer(Device d,std::string path):p(std::make_unique<Impl>(d,std::move(path))){}
Producer::~Producer()=default;
void Producer::reserveFresh(size_t bytes){
    if(!bytes||bytes>MaxBytes)return;
    p->poll();if(p->prepared&&p->preparedBytes==bytes)return;
    if(p->loans.size()>=Capacity)return;
    auto start=std::chrono::steady_clock::now();
    auto fresh=std::make_unique<Resource>(p->d,p->api);fresh->makeBuffer(bytes,true);fresh->makeSemaphore();
    memset(fresh->mapHost(),0,bytes); // Fault/initialize only before publication.
    p->prepared=std::move(fresh);p->preparedBytes=bytes;++p->stats.prefetched;
    p->stats.prefetchNs+=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count();
}
void Producer::poll(){p->poll();}
Counters Producer::counters()const{return p->stats;}
bool Producer::send(MhlDepthPacket const& packet,void const* bytes,size_t size){
    return produce(packet,[&](void* mapped,size_t requested){
        if(!bytes||size!=requested)throw std::runtime_error("payload size");
        (void)mapped;return bytes;
    });
}
bool Producer::produce(MhlDepthPacket const& packet,std::function<void const*(void*,size_t)> const& fill){
    p->poll();if(p->loans.size()>=Capacity){p->stats.busy++;return false;}
    if(p->socket<0){int s=socket(AF_UNIX,SOCK_SEQPACKET|SOCK_NONBLOCK|SOCK_CLOEXEC,0);auto addr=address(p->path);
        if(s<0)return false;if(connect(s,reinterpret_cast<sockaddr*>(&addr),sizeof(addr))||!sameUid(s)){close(s);return false;}p->socket=s;}
    if(auto error=mhl_depth_validate(&packet,sizeof(packet)))throw std::runtime_error(error);
    if(!fill||!packet.buffer_size||packet.buffer_size>MaxBytes)throw std::runtime_error("payload size");
    auto allocationStart=std::chrono::steady_clock::now();
    std::unique_ptr<Resource> r;
    if(p->prepared&&p->preparedBytes==packet.buffer_size)r=std::move(p->prepared);
    else{p->prepared.reset();r=std::make_unique<Resource>(p->d,p->api);r->makeBuffer(packet.buffer_size,true);r->makeSemaphore();}
    p->preparedBytes=0;void* mapped=r->mapHost();
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    auto getMemory=reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(p->api.gipa(p->d.instance,"vkGetPhysicalDeviceMemoryProperties"));
    getMemory(p->d.physical,&memoryProperties);
    VkPhysicalDeviceDriverProperties driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};properties.pNext=&driver;
    auto getProperties=reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(p->api.gipa(p->d.instance,"vkGetPhysicalDeviceProperties2"));
    if(getProperties)getProperties(p->d.physical,&properties);
    // Direct mapped normalization is validated on this driver only. Intel/NVK
    // retain CPU staging: scalar reads of their mapped export can be costly
    // despite HOST_CACHED being advertised. No unsupported fast-path guess.
    bool cached=getProperties&&driver.driverID==VK_DRIVER_ID_NVIDIA_PROPRIETARY
        &&(memoryProperties.memoryTypes[r->type].propertyFlags&VK_MEMORY_PROPERTY_HOST_CACHED_BIT)
        &&!getenv("MHL_DEPTH_TEST_STAGING_EXPORT");
    auto writeStart=std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point validationStart,validationEnd;
    try{
        auto canonical=fill(cached?mapped:nullptr,packet.buffer_size);
        if(!canonical){r->unmapHost();p->stats.rejected++;return false;}
        validationStart=std::chrono::steady_clock::now();payloadCheck(packet,canonical,packet.buffer_size,p->validators);
        validationEnd=std::chrono::steady_clock::now();
        if(canonical!=mapped)memcpy(mapped,canonical,packet.buffer_size);
    }catch(...){r->unmapHost();throw;}
    r->unmapHost();
    auto publishStart=std::chrono::steady_clock::now();
    r->wire.packet=packet;r->wire.packet.transport=MHL_TRANSPORT_VULKAN_OPAQUE_FD;r->wire.packet.timeline_value=1;r->wire.packet.memory_fd_index=0;r->wire.packet.semaphore_fd_index=1;
    auto id=uuid(p->d,p->api);std::copy(id.begin(),id.end(),r->wire.packet.device_uuid);r->wire.memoryType=r->type;r->wire.allocationBytes=r->allocation;r->wire.lease=++p->serial;
    r->commands();VkBufferMemoryBarrier b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};b.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT;b.srcQueueFamilyIndex=p->d.family;b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_EXTERNAL;b.buffer=r->buffer;b.size=VK_WHOLE_SIZE;
    p->api.CmdPipelineBarrier(r->command,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,0,0,nullptr,1,&b,0,nullptr);check(p->api.EndCommandBuffer(r->command),"producer end");
    uint64_t value=1;VkTimelineSemaphoreSubmitInfo t{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};t.signalSemaphoreValueCount=1;t.pSignalSemaphoreValues=&value;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.pNext=&t;submit.commandBufferCount=1;submit.pCommandBuffers=&r->command;submit.signalSemaphoreCount=1;submit.pSignalSemaphores=&r->semaphore;
    check(p->api.QueueSubmit(p->d.queue,1,&submit,r->fence),"producer submit");r->submitted=true;
    int memory=-1,semaphore=-1;
    try {
        VkMemoryGetFdInfoKHR m{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};m.memory=r->memory;m.handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;check(p->api.GetMemoryFdKHR(p->d.device,&m,&memory),"memory FD");
        VkSemaphoreGetFdInfoKHR s{VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR};s.semaphore=r->semaphore;s.handleType=VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;check(p->api.GetSemaphoreFdKHR(p->d.device,&s,&semaphore),"semaphore FD");
        bool sent=sendWire(p->socket,r->wire,{memory,semaphore});closeFd(memory);closeFd(semaphore);
        if(sent){
            auto elapsed=[](auto a,auto b){return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(b-a).count());};
            p->stats.validationNs+=elapsed(validationStart,validationEnd);p->stats.allocationNs+=elapsed(allocationStart,writeStart);
            p->stats.writeNs+=elapsed(writeStart,validationStart);p->stats.copyNs+=elapsed(validationEnd,publishStart);p->stats.publishNs+=elapsed(publishStart,std::chrono::steady_clock::now());
            p->stats.sent++;p->loans.push_back({std::move(r),false});return true;}
        closeFd(p->socket);p->loans.push_back({std::move(r),true});return false;
    }catch(...){closeFd(memory);closeFd(semaphore);r->wait();throw;}
}

struct Receiver::Impl {
    Device d;Api api;std::string path,flag;Logger log;Counters stats{};int server=-1,client=-1;bool enabled=false,ownsSocket=false;
    ino_t inode{};uint64_t epoch=0;std::chrono::steady_clock::time_point checked{},reported{};
    std::deque<std::unique_ptr<Received>> pending;
    // Keep two delivered images: a previous image is still promised valid
    // throughout the next present(). Older images retire behind a newer copy
    // fence on the SAME driver queue, which also covers prior consumer reads.
    std::deque<std::unique_ptr<Resource>> delivered;
    uint64_t lastProducer=0,lastFrame=0,lastDisplay=0;
    Impl(Device d,std::string path,std::string flag,Logger log):d(d),api(d.device),path(std::move(path)),flag(std::move(flag)),log(std::move(log)){}
    ~Impl(){api.DeviceWaitIdle(d.device);stop();delivered.clear();}
    void message(std::string text){if(log)log(text);}
    void ack(Wire const& w,uint64_t expectedEpoch=0){
        if(expectedEpoch&&expectedEpoch!=epoch)return;
        Wire a;a.kind=2;a.lease=w.lease;a.packet.producer_id=w.packet.producer_id;a.packet.frame_id=w.packet.frame_id;
        if(client>=0&&!sendWire(client,a))closeFd(client);
    }
    void stop(){pending.clear();closeFd(client);closeFd(server);if(ownsSocket){struct stat s{};if(!lstat(path.c_str(),&s)&&s.st_ino==inode&&S_ISSOCK(s.st_mode))unlink(path.c_str());ownsSocket=false;}}
    void update(){
        auto now=std::chrono::steady_clock::now();
        if(checked.time_since_epoch().count()&&now-checked<std::chrono::seconds(1))return;checked=now;
        int value=0;std::ifstream(flag)>>value;bool wanted=value==1;
        if(!wanted){if(enabled){stop();message("depth receiver disabled");}enabled=false;return;}
        if(server>=0){enabled=true;return;}
        int s=socket(AF_UNIX,SOCK_SEQPACKET|SOCK_NONBLOCK|SOCK_CLOEXEC,0);auto addr=address(path);
        if(s<0||bind(s,reinterpret_cast<sockaddr*>(&addr),sizeof(addr))){if(s>=0)close(s);message("depth receiver socket unavailable (existing paths are never replaced)");return;}
        struct stat state{};lstat(path.c_str(),&state);inode=state.st_ino;ownsSocket=true;server=s;
        if(chmod(path.c_str(),0600)||listen(server,1)){stop();return;}
        enabled=true;message("depth receiver enabled: "+path);
    }
    void report(){
        auto now=std::chrono::steady_clock::now();if(reported.time_since_epoch().count()&&now-reported<std::chrono::seconds(5))return;reported=now;
        std::ostringstream s;s<<"depth match matched="<<stats.accepted<<" late="<<stats.timestamp<<" unmatched="<<stats.missing<<" received="<<stats.received<<" accepted="<<stats.accepted<<" rejected="<<stats.rejected
            <<" timestamp="<<stats.timestamp<<" geometry="<<stats.geometry<<" pose="<<stats.pose<<" clock="<<stats.clock
            <<" protocol="<<stats.protocol<<" device="<<stats.device<<" not_ready="<<stats.notReady<<" missing="<<stats.missing;message(s.str());
    }
    void poll(){
        if(client<0&&server>=0){int s=accept4(server,nullptr,nullptr,SOCK_NONBLOCK|SOCK_CLOEXEC);if(s>=0){if(sameUid(s)){client=s;epoch++;}else close(s);}}
        if(client<0)return;
        for(unsigned n=0;n<16;n++){
            auto r=std::make_unique<Received>();int got=receive(client,*r);if(!got)break;
            if(got==-1){pending.clear();closeFd(client);break;}
            stats.received++;auto const& w=r->wire;auto const& p=w.packet;
            bool valid=got==1&&w.magic==Magic&&w.version==Version&&w.kind==1&&w.lease&&r->fds.size()==2
                &&p.buffer_size<=MaxBytes&&w.allocationBytes>=p.buffer_size&&w.allocationBytes<=MaxBytes
                &&!mhl_depth_validate(&p,sizeof(p))&&p.transport==MHL_TRANSPORT_VULKAN_OPAQUE_FD;
            if(!valid){stats.protocol++;stats.rejected++;ack(w);continue;}
            auto id=uuid(d,api);if(!std::equal(id.begin(),id.end(),p.device_uuid)){stats.device++;stats.rejected++;ack(w);continue;}
            if(pending.size()==Capacity){ack(pending.front()->wire);pending.pop_front();stats.busy++;stats.rejected++;}
            pending.push_back(std::move(r));
        }
    }
    enum Match {Good,Time,Geometry,Pose,Clock};
    Match match(MhlDepthPacket const& p,Query const& q){
        if(!q.clockMapped||!q.displayMonotonicNs||!q.targetTimestampNs||q.toleranceNs>1'000'000)return Clock;
        if(p.producer_id==lastProducer&&(p.frame_id<=lastFrame||p.display_time_ns<=lastDisplay))return Time;
        uint64_t delta=p.display_time_ns>q.displayMonotonicNs?p.display_time_ns-q.displayMonotonicNs:q.displayMonotonicNs-p.display_time_ns;
        if(delta>q.toleranceNs)return Time;
        if(!q.producerId||p.producer_id!=q.producerId||!q.fullStereoLayout)return Geometry;
        if(p.pose_space!=q.poseSpace||(q.requireTrackingAligned&&(!(p.flags&MHL_TRACKING_ALIGNED)||p.pose_space!=MHL_SPACE_TRACKING)))return Pose;
        if(p.eyes[0].depth.width!=p.eyes[1].depth.width||p.eyes[0].depth.height!=p.eyes[1].depth.height)return Geometry;
        for(unsigned eye=0;eye<2;eye++){
            auto const& e=p.eyes[eye];double dot=0,sourceNorm=0,expectedNorm=0;
            if(q.verifySourceUv){
                auto const& s=e.colour_source;
                float actual[4]={float(s.rect.x)/s.width,float(s.rect.y)/s.height,
                    float(uint64_t(s.rect.x)+s.rect.width)/s.width,float(uint64_t(s.rect.y)+s.rect.height)/s.height};
                for(unsigned i=0;i<4;i++)if(!std::isfinite(q.sourceUv[eye][i])||std::abs(actual[i]-q.sourceUv[eye][i])>1e-5)return Geometry;
            }
            for(unsigned i=0;i<4;i++){
                if(!std::isfinite(q.fov[eye][i])||std::abs(e.fov[i]-q.fov[eye][i])>1e-5)return Geometry;
                if(!std::isfinite(q.poses[eye].orientation[i]))return Pose;
                dot+=e.pose.orientation[i]*q.poses[eye].orientation[i];
                sourceNorm+=e.pose.orientation[i]*e.pose.orientation[i];expectedNorm+=q.poses[eye].orientation[i]*q.poses[eye].orientation[i];
            }
            if(std::abs(expectedNorm-1)>.001||std::abs(std::abs(dot)/std::sqrt(sourceNorm*expectedNorm)-1)>1e-6)return Pose;
            for(unsigned i=0;i<3;i++)if(!std::isfinite(q.poses[eye].position[i])||std::abs(e.pose.position[i]-q.poses[eye].position[i])>1e-4)return Pose;
        }
        return Good;
    }
    void copy(Resource& r){
        r.makeImage(r.wire.packet);r.commands();
        VkBufferMemoryBarrier buffer{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};buffer.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;buffer.srcQueueFamilyIndex=VK_QUEUE_FAMILY_EXTERNAL;buffer.dstQueueFamilyIndex=d.family;buffer.buffer=r.buffer;buffer.size=VK_WHOLE_SIZE;
        VkImageMemoryBarrier image{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};image.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;image.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;image.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;image.srcQueueFamilyIndex=image.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;image.image=r.image;image.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        api.CmdPipelineBarrier(r.command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,1,&buffer,1,&image);
        std::array<VkBufferImageCopy,2> regions{};
        for(unsigned eye=0;eye<2;eye++){auto const& p=r.wire.packet.eyes[eye].depth;auto& c=regions[eye];c.bufferOffset=p.offset;c.bufferRowLength=p.row_stride/4;c.bufferImageHeight=p.height;c.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};c.imageOffset={int32_t(eye*p.width),0,0};c.imageExtent={p.width,p.height,1};}
        api.CmdCopyBufferToImage(r.command,r.buffer,r.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,2,regions.data());
        image.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;image.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;image.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;image.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_TRANSFER_READ_BIT;
        api.CmdPipelineBarrier(r.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&image);
        check(api.EndCommandBuffer(r.command),"receiver end");
        uint64_t value=r.wire.packet.timeline_value;VkTimelineSemaphoreSubmitInfo t{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};t.waitSemaphoreValueCount=1;t.pWaitSemaphoreValues=&value;
        VkPipelineStageFlags wait=VK_PIPELINE_STAGE_TRANSFER_BIT;VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.pNext=&t;submit.waitSemaphoreCount=1;submit.pWaitSemaphores=&r.semaphore;submit.pWaitDstStageMask=&wait;submit.commandBufferCount=1;submit.pCommandBuffers=&r.command;
        check(api.QueueSubmit(d.queue,1,&submit,r.fence),"receiver queue copy");r.submitted=true;
    }
    void retire(){
        // ACK only after GPU copy completion, so the producer lease is safe.
        for(auto& r:delivered)if(r->wire.kind==1&&r->ready()){ack(r->wire,r->connectionEpoch);r->wire.kind=2;}
        while(delivered.size()>2&&delivered[1]->ready())delivered.pop_front();
    }
};
Receiver::Receiver(Device d,std::string path,std::string flag,Logger log):p(std::make_unique<Impl>(d,std::move(path),std::move(flag),std::move(log))){}
Receiver::~Receiver()=default;
Counters Receiver::counters()const{return p->stats;}
std::optional<Image> Receiver::acquire(Query const& query){
    p->update();p->retire();if(!p->enabled){p->stats.disabled++;return {};}
    p->poll();std::optional<Image> result;
    // Two history images plus four GPU-pending loans, including across peer
    // reconnects. Never grow private image retention on a stalled queue.
    if(p->delivered.size()>=Capacity+2){p->stats.busy++;p->stats.missing++;p->report();return {};}
    for(auto it=p->pending.begin();it!=p->pending.end();){
        auto& in=**it;auto reason=p->match(in.wire.packet,query);
        if(reason!=Impl::Good){
            // Future timestamps remain bounded and queued for a later query.
            if(reason==Impl::Time&&in.wire.packet.display_time_ns>query.displayMonotonicNs){++it;continue;}
            p->stats.rejected++;if(reason==Impl::Time)p->stats.timestamp++;else if(reason==Impl::Geometry)p->stats.geometry++;else if(reason==Impl::Pose)p->stats.pose++;else p->stats.clock++;
            p->ack(in.wire);it=p->pending.erase(it);continue;
        }
        auto r=std::make_unique<Resource>(p->d,p->api);r->wire=in.wire;r->connectionEpoch=p->epoch;
        // Duplicate before importing: Vulkan consumes successfully imported
        // FDs; the received originals remain owned by the RAII message.
        int memory=fcntl(in.fds[0],F_DUPFD_CLOEXEC,0),semaphore=fcntl(in.fds[1],F_DUPFD_CLOEXEC,0);
        try {
            if(memory<0||semaphore<0)throw std::runtime_error("FD duplication failed");
            r->makeBuffer(in.wire.packet.buffer_size,true,memory,in.wire.memoryType,in.wire.allocationBytes);memory=-1;
            r->makeSemaphore(semaphore);semaphore=-1;
            uint64_t value=0;check(p->api.GetSemaphoreCounterValue(p->d.device,r->semaphore,&value),"timeline readiness");
            if(value<in.wire.packet.timeline_value){p->stats.notReady++;closeFd(memory);closeFd(semaphore);++it;continue;}
            p->copy(*r);result=Image{r->image,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,r->extent,query.targetTimestampNs,r->wire.packet.frame_id};
            p->lastProducer=in.wire.packet.producer_id;p->lastFrame=in.wire.packet.frame_id;p->lastDisplay=in.wire.packet.display_time_ns;
            p->delivered.push_back(std::move(r));p->stats.accepted++;it=p->pending.erase(it);break;
        }catch(std::exception const& e){
            if(r->memoryFdConsumed)memory=-1;if(r->semaphoreFdConsumed)semaphore=-1;
            closeFd(memory);closeFd(semaphore);if(r->fence&&!r->ready())r->wait();p->stats.protocol++;p->stats.rejected++;p->message(std::string("depth import rejected: ")+e.what());p->ack(in.wire);it=p->pending.erase(it);
        }
    }
    if(!result)p->stats.missing++;p->report();return result;
}
namespace {
std::vector<uint8_t> readback(Device d,Api& api,Resource& source,bool image){
    source.wait();uint64_t bytes=image?uint64_t(source.extent.width)*source.extent.height*4:source.wire.packet.buffer_size;
    Resource staging(d,api);staging.makeBuffer(bytes,false);staging.commands();
    if(image){
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.srcAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT;b.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;b.oldLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;b.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.image=source.image;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        api.CmdPipelineBarrier(staging.command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&b);
        VkBufferImageCopy c{};c.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};c.imageExtent={source.extent.width,source.extent.height,1};api.CmdCopyImageToBuffer(staging.command,source.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,staging.buffer,1,&c);
        b.srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT;b.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_TRANSFER_READ_BIT;b.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;b.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        api.CmdPipelineBarrier(staging.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&b);
    }else{VkBufferCopy c{0,0,bytes};api.CmdCopyBuffer(staging.command,source.buffer,staging.buffer,1,&c);}
    VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;host.srcQueueFamilyIndex=host.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;host.buffer=staging.buffer;host.size=VK_WHOLE_SIZE;
    api.CmdPipelineBarrier(staging.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&host,0,nullptr);
    check(api.EndCommandBuffer(staging.command),"readback end");VkSubmitInfo s{VK_STRUCTURE_TYPE_SUBMIT_INFO};s.commandBufferCount=1;s.pCommandBuffers=&staging.command;check(api.QueueSubmit(d.queue,1,&s,staging.fence),"readback submit");staging.submitted=true;staging.wait();
    void* mapped=nullptr;check(api.MapMemory(d.device,staging.memory,0,bytes,0,&mapped),"readback map");std::vector<uint8_t> out(bytes);memcpy(out.data(),mapped,bytes);api.UnmapMemory(d.device,staging.memory);return out;
}
}
std::vector<uint8_t> Receiver::readDepthForTest(uint64_t frameId){
    if(p->delivered.empty())throw std::runtime_error("no accepted frame");
    for(auto it=p->delivered.rbegin();it!=p->delivered.rend();++it)if(!frameId||(*it)->wire.packet.frame_id==frameId)return readback(p->d,p->api,**it,true);
    throw std::runtime_error("frame no longer retained");
}
std::vector<uint8_t> Receiver::readAllocationForTest(){if(p->delivered.empty())throw std::runtime_error("no accepted frame");return readback(p->d,p->api,*p->delivered.back(),false);}
}
