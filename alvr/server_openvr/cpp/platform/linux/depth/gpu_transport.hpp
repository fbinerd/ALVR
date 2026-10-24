#pragma once
// Source-neutral Linux Vulkan buffer transport. Packet v1 remains unchanged.
// All queue operations belong to one externally synchronized worker/thread.
#include "depth_packet.h"
#include <vulkan/vulkan.h>
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace mhl::depth {
struct Device {
    VkInstance instance{};
    VkPhysicalDevice physical{};
    VkDevice device{};
    VkQueue queue{};
    uint32_t family{};
};
// Owned device for offline producers. Borrowed driver devices use Device.
class OwnedDevice {
public:
    explicit OwnedDevice(std::string const& nameSubstring);
    ~OwnedDevice();
    OwnedDevice(OwnedDevice const&)=delete;
    OwnedDevice& operator=(OwnedDevice const&)=delete;
    Device get() const;
private:
    struct Impl;std::unique_ptr<Impl> p;
};
struct Counters {
    uint64_t received{},accepted{},rejected{},timestamp{},geometry{},pose{},clock{},
        protocol{},device{},notReady{},missing{},disabled{},busy{},sent{},acknowledged{};
};
struct Query {
    uint64_t targetTimestampNs{},displayMonotonicNs{},toleranceNs{},producerId{};
    bool clockMapped=false,fullStereoLayout=false,requireTrackingAligned=true;
    uint32_t poseSpace=MHL_SPACE_TRACKING;
    std::array<MhlDepthPose,2> poses{};
    std::array<std::array<float,4>,2> fov{};
};
struct Image {
    VkImage image{};VkImageLayout layout{};VkExtent2D extent{};
    uint64_t targetTimestampNs{},frameId{};
};
using Logger=std::function<void(std::string const&)>;

class Producer {
public:
    Producer(Device device,std::string socketPath);
    ~Producer();
    Producer(Producer const&)=delete;
    Producer& operator=(Producer const&)=delete;
    // Worker-only: copies a validated canonical allocation to owned exportable
    // Vulkan memory. Never borrows a raw capture slot or sends a file path.
    bool send(MhlDepthPacket const&,void const* canonical,size_t bytes);
    void poll();
    Counters counters() const;
private:
    struct Impl;std::unique_ptr<Impl> p;
};

class Receiver {
public:
    Receiver(Device device,std::string socketPath,std::string enableFile,Logger={});
    ~Receiver();
    Receiver(Receiver const&)=delete;
    Receiver& operator=(Receiver const&)=delete;
    // Present-thread only. Switch reread <=1 second; absent/0 means no socket.
    // Query timestamps MUST already be mapped into the packet's monotonic clock.
    // Submitted image operations use the supplied driver device AND queue.
    std::optional<Image> acquire(Query const&);
    Counters counters() const;
    // Offline diagnostics only: waits for the copy, reads back owned GPU data.
    // The driver callback never calls this blocking method.
    std::vector<uint8_t> readDepthForTest(uint64_t frameId=0);
    std::vector<uint8_t> readAllocationForTest();
private:
    struct Impl;std::unique_ptr<Impl> p;
};
}
