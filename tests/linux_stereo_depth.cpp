#include "alvr_server/Settings.h"
#include "platform/linux/motion/StereoDepth.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

const Settings* Settings_Instance() {
    static Settings settings = [] {
        Settings value {};
        std::strcpy(value.m_renderGpu, "NVIDIA");
        return value;
    }();
    return &settings;
}

void Info(const char* format, ...) {
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    puts("");
}

void Warn(const char* format, ...) {
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputs("\n", stderr);
}

void Error(const char* format, ...) {
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputs("\n", stderr);
}

// VkContext uses explicit destroy(); declare it first so all resources die before it.
struct RecordingContext : alvr::VkContext {
    RecordingContext() : alvr::VkContext(std::vector<uint8_t>{}) {}
    ~RecordingContext() {
        try { destroy(); }
        catch (...) { Warn("Stereo checker: Vulkan context teardown failed"); }
    }
};

namespace {

enum RegionType : uint8_t {
    REGION_SKY = 0,
    REGION_NEAR_BOX = 1,
    REGION_SLANTED_FLOOR = 2,
    REGION_UNTEXTURED = 3,
    REGION_REPEATED = 4,
    REGION_MID_WALL = 5,
    REGION_FAR_WALL = 6,
    REGION_COUNT = 7
};

const char* regionName(RegionType r) {
    switch (r) {
        case REGION_SKY: return "Sky (+0 invalid)";
        case REGION_NEAR_BOX: return "Near Box (0.5 m)";
        case REGION_SLANTED_FLOOR: return "Slanted Floor (0.8-5 m)";
        case REGION_UNTEXTURED: return "Untextured Patch (2.0 m)";
        case REGION_REPEATED: return "Repeated Pattern (2.8 m)";
        case REGION_MID_WALL: return "Mid Wall (3.2 m)";
        case REGION_FAR_WALL: return "Far Wall (4.8 m)";
        default: return "Unknown";
    }
}

struct SceneHit {
    float depth = 0.0f; // in metres along -Z
    RegionType region = REGION_SKY;
    uint8_t r = 0, g = 0, b = 0;
};

// Casts ray: origin = (camX, 0, 0), direction = (rx, ry, -1.0)
SceneHit castRay(float camX, float rx, float ry) {
    SceneHit hit;
    hit.depth = 0.0f;
    hit.region = REGION_SKY;
    // Sky background gradient
    float skyT = std::clamp((ry + 0.5f) * 0.8f, 0.0f, 1.0f);
    hit.r = uint8_t(60.0f + 60.0f * (1.0f - skyT));
    hit.g = uint8_t(100.0f + 70.0f * (1.0f - skyT));
    hit.b = uint8_t(160.0f + 95.0f * (1.0f - skyT));

    float closestDepth = 1e9f;

    // 1. Near box at Z = -0.5 m (depth = 0.5 m)
    {
        float d = 0.5f;
        float x = camX + d * rx;
        float y = d * ry;
        if (std::abs(x) <= 0.22f && std::abs(y) <= 0.18f) {
            if (d < closestDepth) {
                closestDepth = d;
                hit.depth = d;
                hit.region = REGION_NEAR_BOX;
                // High-contrast multi-frequency checker on box face
                float u = (x + 0.22f) / 0.44f;
                float v = (y + 0.18f) / 0.36f;
                int cu = int(u * 28.0f);
                int cv = int(v * 24.0f);
                bool c = ((cu ^ cv) & 1);
                float pat = c ? 0.90f : 0.25f;
                pat += 0.10f * std::sin(x * 120.0f) * std::cos(y * 120.0f);
                hit.r = uint8_t(std::clamp(pat * 240.0f, 0.0f, 255.0f));
                hit.g = uint8_t(std::clamp(pat * 140.0f, 0.0f, 255.0f));
                hit.b = uint8_t(std::clamp(pat * 50.0f, 0.0f, 255.0f));
            }
        }
    }

    // 2. Slanted floor at Y = -0.55 m
    if (ry < -0.06f) {
        float d = -0.55f / ry;
        if (d >= 0.7f && d <= 5.5f) {
            float x = camX + d * rx;
            if (std::abs(x) <= 3.5f && d < closestDepth) {
                closestDepth = d;
                hit.depth = d;
                hit.region = REGION_SLANTED_FLOOR;
                // Floor checker grid
                float fx = x * 3.5f;
                float fz = -d * 3.5f;
                int ix = int(std::floor(fx));
                int iz = int(std::floor(fz));
                bool c = ((ix ^ iz) & 1);
                float pat = c ? 0.85f : 0.35f;
                pat += 0.08f * std::sin(fx * 14.0f);
                hit.r = uint8_t(std::clamp(pat * 170.0f, 0.0f, 255.0f));
                hit.g = uint8_t(std::clamp(pat * 180.0f, 0.0f, 255.0f));
                hit.b = uint8_t(std::clamp(pat * 190.0f, 0.0f, 255.0f));
            }
        }
    }

    // 3. Untextured patch at Z = -2.0 m (depth = 2.0 m)
    {
        float d = 2.0f;
        float x = camX + d * rx;
        float y = d * ry;
        if (x >= -1.7f && x <= -0.7f && y >= 0.05f && y <= 0.85f) {
            if (d < closestDepth) {
                closestDepth = d;
                hit.depth = d;
                hit.region = REGION_UNTEXTURED;
                // Flat solid uniform color (no texture gradients)
                hit.r = 135;
                hit.g = 145;
                hit.b = 155;
            }
        }
    }

    // 4. Repeated-pattern patch at Z = -2.8 m (depth = 2.8 m)
    {
        float d = 2.8f;
        float x = camX + d * rx;
        float y = d * ry;
        if (x >= 0.7f && x <= 1.7f && y >= 0.05f && y <= 0.85f) {
            if (d < closestDepth) {
                closestDepth = d;
                hit.depth = d;
                hit.region = REGION_REPEATED;
                // High-frequency periodic vertical stripes
                float stripe = 0.5f + 0.5f * std::sin(x * 65.0f);
                float val = (stripe > 0.5f) ? 0.88f : 0.18f;
                hit.r = uint8_t(val * 220.0f);
                hit.g = uint8_t(val * 210.0f);
                hit.b = uint8_t(val * 70.0f);
            }
        }
    }

    // 5. Mid wall at Z = -3.2 m (depth = 3.2 m)
    {
        float d = 3.2f;
        float x = camX + d * rx;
        float y = d * ry;
        if (std::abs(x) <= 3.2f && y >= -0.55f && y <= 1.4f) {
            if (d < closestDepth) {
                closestDepth = d;
                hit.depth = d;
                hit.region = REGION_MID_WALL;
                // Textured brick / stone pattern
                float bx = x * 5.0f;
                float by = y * 8.0f;
                int iby = int(std::floor(by));
                if (iby & 1) bx += 0.5f;
                int ibx = int(std::floor(bx));
                float mx = std::abs(bx - ibx - 0.5f);
                float my = std::abs(by - iby - 0.5f);
                bool mortar = (mx > 0.44f || my > 0.42f);
                float pat = mortar ? 0.35f : (0.65f + 0.20f * std::sin(x * 40.0f + y * 30.0f));
                hit.r = uint8_t(std::clamp(pat * 190.0f, 0.0f, 255.0f));
                hit.g = uint8_t(std::clamp(pat * 125.0f, 0.0f, 255.0f));
                hit.b = uint8_t(std::clamp(pat * 95.0f, 0.0f, 255.0f));
            }
        }
    }

    // 6. Far wall at Z = -4.8 m (depth = 4.8 m)
    {
        float d = 4.8f;
        float x = camX + d * rx;
        float y = d * ry;
        if (std::abs(x) <= 4.0f && y >= -0.55f && y <= 2.2f) {
            if (d < closestDepth) {
                closestDepth = d;
                hit.depth = d;
                hit.region = REGION_FAR_WALL;
                // High-contrast multi-scale geometric wall pattern
                float pat = 0.55f + 0.25f * std::sin(x * 12.0f) * std::cos(y * 12.0f)
                          + 0.15f * std::sin(x * 28.0f + y * 19.0f);
                hit.r = uint8_t(std::clamp(pat * 110.0f, 0.0f, 255.0f));
                hit.g = uint8_t(std::clamp(pat * 160.0f, 0.0f, 255.0f));
                hit.b = uint8_t(std::clamp(pat * 190.0f, 0.0f, 255.0f));
            }
        }
    }

    return hit;
}

void writePpm(const std::filesystem::path& path, const uint8_t* rgba, uint32_t width, uint32_t height) {
    std::ofstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("Cannot write PPM: " + path.string());
    f << "P6\n" << width << " " << height << "\n255\n";
    for (size_t i = 0; i < size_t(width) * height; ++i) {
        f.write(reinterpret_cast<const char*>(rgba + i * 4), 3);
    }
}

void writeDepthPpm(const std::filesystem::path& path, const float* depth, uint32_t width, uint32_t height) {
    std::vector<uint8_t> rgb(width * height * 3);
    for (size_t i = 0; i < size_t(width) * height; ++i) {
        float d = depth[i];
        if (d <= 0.0f) {
            // Invalid / sky -> Black
            rgb[i * 3 + 0] = 0;
            rgb[i * 3 + 1] = 0;
            rgb[i * 3 + 2] = 0;
        } else {
            // Normalized depth [0.4 m, 5.0 m]
            float t = std::clamp((d - 0.4f) / 4.6f, 0.0f, 1.0f);
            // Turbo-style colormap (Near = Red/Yellow, Mid = Green, Far = Blue)
            float r = std::clamp(1.5f - std::abs(t * 4.0f - 1.0f), 0.0f, 1.0f);
            float g = std::clamp(1.5f - std::abs(t * 4.0f - 2.0f), 0.0f, 1.0f);
            float b = std::clamp(1.5f - std::abs(t * 4.0f - 3.0f), 0.0f, 1.0f);
            rgb[i * 3 + 0] = uint8_t(r * 255.0f);
            rgb[i * 3 + 1] = uint8_t(g * 255.0f);
            rgb[i * 3 + 2] = uint8_t(b * 255.0f);
        }
    }
    std::ofstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("Cannot write depth PPM: " + path.string());
    f << "P6\n" << width << " " << height << "\n255\n";
    f.write(reinterpret_cast<const char*>(rgb.data()), rgb.size());
}

void writePfm(const std::filesystem::path& path, const float* depth, uint32_t width, uint32_t height) {
    std::ofstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("Cannot write PFM: " + path.string());
    f << "Pf\n" << width << " " << height << "\n-1.0\n";
    // PFM row order is bottom to top
    for (int y = int(height) - 1; y >= 0; --y) {
        f.write(reinterpret_cast<const char*>(depth + y * width), width * sizeof(float));
    }
}

struct GpuFixture {
    alvr::VkContext& ctx;
    uint32_t width = 0, height = 0;
    vk::Image colorImage {};
    vk::ImageView colorView {};
    vk::DeviceMemory colorMemory {};
    vk::Buffer colorStaging {};
    vk::DeviceMemory colorStagingMemory {};
    void* colorMapped = nullptr;

    vk::Image depthImage {};
    vk::ImageView depthView {};
    vk::DeviceMemory depthMemory {};
    vk::Buffer depthStaging {};
    vk::DeviceMemory depthStagingMemory {};
    void* depthMapped = nullptr;

    vk::CommandPool pool {};
    vk::CommandBuffer command {};
    vk::Fence fence {};

    GpuFixture(alvr::VkContext& c, uint32_t w, uint32_t h)
        : ctx(c), width(w), height(h) {
        initialize();
    }

    ~GpuFixture() {
        try { ctx.dev.waitIdle(); } catch (...) {}
        if (colorMapped) ctx.dev.unmapMemory(colorStagingMemory);
        if (depthMapped) ctx.dev.unmapMemory(depthStagingMemory);
        ctx.dev.destroy(colorView);
        ctx.dev.destroy(colorImage);
        ctx.dev.free(colorMemory);
        ctx.dev.destroy(colorStaging);
        ctx.dev.free(colorStagingMemory);

        ctx.dev.destroy(depthView);
        ctx.dev.destroy(depthImage);
        ctx.dev.free(depthMemory);
        ctx.dev.destroy(depthStaging);
        ctx.dev.free(depthStagingMemory);

        ctx.dev.destroy(fence);
        ctx.dev.destroy(pool);
    }

    uint32_t findMemory(uint32_t bits, vk::MemoryPropertyFlags flags) {
        auto props = ctx.physDev.getMemoryProperties();
        for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
            if ((bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & flags) == flags) {
                return i;
            }
        }
        throw std::runtime_error("No memory type matching flags");
    }

    void initialize() {
        pool = ctx.dev.createCommandPool(vk::CommandPoolCreateInfo {
            .flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
            .queueFamilyIndex = ctx.meta.queueFamily,
        });
        command = ctx.dev.allocateCommandBuffers(vk::CommandBufferAllocateInfo {
            .commandPool = pool,
            .level = vk::CommandBufferLevel::ePrimary,
            .commandBufferCount = 1,
        })[0];
        fence = ctx.dev.createFence({});

        // 1. Color Image (RGBA8)
        colorImage = ctx.dev.createImage(vk::ImageCreateInfo {
            .imageType = vk::ImageType::e2D,
            .format = vk::Format::eR8G8B8A8Unorm,
            .extent = { width, height, 1 },
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled,
            .sharingMode = vk::SharingMode::eExclusive,
        });
        auto req = ctx.dev.getImageMemoryRequirements(colorImage);
        colorMemory = ctx.dev.allocateMemory(vk::MemoryAllocateInfo {
            .allocationSize = req.size,
            .memoryTypeIndex = findMemory(req.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal),
        });
        ctx.dev.bindImageMemory(colorImage, colorMemory, 0);
        colorView = ctx.dev.createImageView(vk::ImageViewCreateInfo {
            .image = colorImage,
            .viewType = vk::ImageViewType::e2D,
            .format = vk::Format::eR8G8B8A8Unorm,
            .subresourceRange = { vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 },
        });

        size_t colorBytes = size_t(width) * height * 4;
        colorStaging = ctx.dev.createBuffer(vk::BufferCreateInfo {
            .size = colorBytes,
            .usage = vk::BufferUsageFlagBits::eTransferSrc,
            .sharingMode = vk::SharingMode::eExclusive,
        });
        req = ctx.dev.getBufferMemoryRequirements(colorStaging);
        colorStagingMemory = ctx.dev.allocateMemory(vk::MemoryAllocateInfo {
            .allocationSize = req.size,
            .memoryTypeIndex = findMemory(req.memoryTypeBits, vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent),
        });
        ctx.dev.bindBufferMemory(colorStaging, colorStagingMemory, 0);
        colorMapped = ctx.dev.mapMemory(colorStagingMemory, 0, VK_WHOLE_SIZE);

        // 2. Depth Image (R32F)
        depthImage = ctx.dev.createImage(vk::ImageCreateInfo {
            .imageType = vk::ImageType::e2D,
            .format = vk::Format::eR32Sfloat,
            .extent = { width, height, 1 },
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferSrc,
            .sharingMode = vk::SharingMode::eExclusive,
        });
        req = ctx.dev.getImageMemoryRequirements(depthImage);
        depthMemory = ctx.dev.allocateMemory(vk::MemoryAllocateInfo {
            .allocationSize = req.size,
            .memoryTypeIndex = findMemory(req.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal),
        });
        ctx.dev.bindImageMemory(depthImage, depthMemory, 0);
        depthView = ctx.dev.createImageView(vk::ImageViewCreateInfo {
            .image = depthImage,
            .viewType = vk::ImageViewType::e2D,
            .format = vk::Format::eR32Sfloat,
            .subresourceRange = { vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 },
        });

        size_t depthBytes = size_t(width) * height * sizeof(float);
        depthStaging = ctx.dev.createBuffer(vk::BufferCreateInfo {
            .size = depthBytes,
            .usage = vk::BufferUsageFlagBits::eTransferDst,
            .sharingMode = vk::SharingMode::eExclusive,
        });
        req = ctx.dev.getBufferMemoryRequirements(depthStaging);
        depthStagingMemory = ctx.dev.allocateMemory(vk::MemoryAllocateInfo {
            .allocationSize = req.size,
            .memoryTypeIndex = findMemory(req.memoryTypeBits, vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent),
        });
        ctx.dev.bindBufferMemory(depthStaging, depthStagingMemory, 0);
        depthMapped = ctx.dev.mapMemory(depthStagingMemory, 0, VK_WHOLE_SIZE);
    }

    void uploadColor(const uint8_t* rgba) {
        std::memcpy(colorMapped, rgba, size_t(width) * height * 4);
        command.reset();
        command.begin(vk::CommandBufferBeginInfo {});

        vk::ImageMemoryBarrier b1 {
            .srcAccessMask = {},
            .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eTransferDstOptimal,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = colorImage,
            .subresourceRange = { vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 },
        };
        command.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe, vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, b1);

        vk::BufferImageCopy region {
            .imageSubresource = { vk::ImageAspectFlagBits::eColor, 0, 0, 1 },
            .imageExtent = { width, height, 1 },
        };
        command.copyBufferToImage(colorStaging, colorImage, vk::ImageLayout::eTransferDstOptimal, region);

        vk::ImageMemoryBarrier b2 {
            .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
            .dstAccessMask = vk::AccessFlagBits::eShaderRead,
            .oldLayout = vk::ImageLayout::eTransferDstOptimal,
            .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = colorImage,
            .subresourceRange = { vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 },
        };
        command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eComputeShader, {}, {}, {}, b2);

        command.end();
        ctx.dev.resetFences(fence);
        vk::SubmitInfo submit { .commandBufferCount = 1, .pCommandBuffers = &command };
        ctx.useQueue([&](auto& q) { q.submit(submit, fence); });
        ctx.dev.waitForFences(fence, true, 1'000'000'000);
    }

    void downloadDepth(float* depth) {
        command.reset();
        command.begin(vk::CommandBufferBeginInfo {});

        vk::ImageMemoryBarrier b1 {
            .srcAccessMask = vk::AccessFlagBits::eShaderWrite,
            .dstAccessMask = vk::AccessFlagBits::eTransferRead,
            .oldLayout = vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eTransferSrcOptimal,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = depthImage,
            .subresourceRange = { vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 },
        };
        command.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader, vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, b1);

        vk::BufferImageCopy region {
            .imageSubresource = { vk::ImageAspectFlagBits::eColor, 0, 0, 1 },
            .imageExtent = { width, height, 1 },
        };
        command.copyImageToBuffer(depthImage, vk::ImageLayout::eTransferSrcOptimal, depthStaging, region);

        vk::MemoryBarrier b2 {
            .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
            .dstAccessMask = vk::AccessFlagBits::eHostRead,
        };
        command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eHost, {}, b2, {}, {});

        command.end();
        ctx.dev.resetFences(fence);
        vk::SubmitInfo submit { .commandBufferCount = 1, .pCommandBuffers = &command };
        ctx.useQueue([&](auto& q) { q.submit(submit, fence); });
        ctx.dev.waitForFences(fence, true, 1'000'000'000);

        std::memcpy(depth, depthMapped, size_t(width) * height * sizeof(float));
    }
};

struct TestResults {
    uint32_t eyeWidth = 0, height = 0;
    double wallMs = 0, opticalGpuMs = 0, depthGpuMs = 0;

    struct RegionStats {
        std::string name;
        uint32_t totalPixels = 0;
        uint32_t validPixels = 0;
        double validFraction = 0.0;
        double medianRelErr = 0.0;
        double p90RelErr = 0.0;
        double meanRelErr = 0.0;
        double maeMetres = 0.0;
    };
    std::vector<RegionStats> regions;
    RegionStats edgeStats;
    RegionStats overallStats;
};

TestResults runResolutionTest(
    alvr::VkContext& ctx,
    uint32_t eyeWidth,
    uint32_t height,
    const alvr::motion::StereoParams& params,
    const std::filesystem::path& outDir,
    const std::string& prefix
) {
    uint32_t fullWidth = eyeWidth * 2;
    TestResults res;
    res.eyeWidth = eyeWidth;
    res.height = height;

    std::cout << "\n======================================================\n";
    std::cout << "Rendering analytic stereo scene: " << eyeWidth << "x" << height
              << " per eye (full " << fullWidth << "x" << height << ")\n";
    std::cout << "======================================================\n";

    std::vector<uint8_t> color(fullWidth * height * 4);
    std::vector<float> groundTruth(fullWidth * height);
    std::vector<uint8_t> regionMap(fullWidth * height);
    std::vector<uint8_t> isEdge(fullWidth * height, 0);

    float ipd = params.ipd;
    float camLeftX = -ipd / 2.0f;
    float camRightX = ipd / 2.0f;

    // Render analytic scene for both eyes
    for (uint32_t y = 0; y < height; ++y) {
        float ty = (float(y) + 0.5f) / float(height);
        for (uint32_t x = 0; x < fullWidth; ++x) {
            bool rightEye = (x >= eyeWidth);
            uint32_t ex = rightEye ? (x - eyeWidth) : x;
            float tx = (float(ex) + 0.5f) / float(eyeWidth);

            auto const& tans = rightEye ? params.rightTans : params.leftTans;
            float rx = tans.left + tx * (tans.right - tans.left);
            float ry = tans.up + ty * (tans.down - tans.up);
            float camX = rightEye ? camRightX : camLeftX;

            SceneHit hit = castRay(camX, rx, ry);

            size_t idx = size_t(y) * fullWidth + x;
            color[idx * 4 + 0] = hit.r;
            color[idx * 4 + 1] = hit.g;
            color[idx * 4 + 2] = hit.b;
            color[idx * 4 + 3] = 255;
            groundTruth[idx] = hit.depth;
            regionMap[idx] = hit.region;
        }
    }

    // Detect depth edges in ground truth
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < fullWidth; ++x) {
            bool rightEye = (x >= eyeWidth);
            uint32_t minX = rightEye ? eyeWidth : 0;
            uint32_t maxX = rightEye ? fullWidth - 1 : eyeWidth - 1;

            size_t idx = size_t(y) * fullWidth + x;
            float d = groundTruth[idx];
            if (d <= 0.0f) continue;

            bool edge = false;
            for (int dy = -1; dy <= 1 && !edge; ++dy) {
                int ny = int(y) + dy;
                if (ny < 0 || ny >= int(height)) continue;
                for (int dx = -1; dx <= 1 && !edge; ++dx) {
                    if (dx == 0 && dy == 0) continue;
                    int nx = int(x) + dx;
                    if (nx < int(minX) || nx > int(maxX)) continue;
                    float nd = groundTruth[size_t(ny) * fullWidth + nx];
                    if (std::abs(nd - d) > 0.35f) {
                        edge = true;
                    }
                }
            }
            if (edge) isEdge[idx] = 1;
        }
    }

    // Save inputs / ground truth previews
    writePpm(outDir / (prefix + "_color_stereo.ppm"), color.data(), fullWidth, height);
    writeDepthPpm(outDir / (prefix + "_depth_gt.ppm"), groundTruth.data(), fullWidth, height);
    writePfm(outDir / (prefix + "_depth_gt.pfm"), groundTruth.data(), fullWidth, height);

    // Initialize GPU and StereoDepth
    std::cout << "Initializing StereoDepth GPU pipeline...\n";
    GpuFixture fixture(ctx, fullWidth, height);
    fixture.uploadColor(color.data());

    alvr::motion::StereoDepth estimator(
        ctx,
        vk::Extent2D { fullWidth, height },
        vk::Format::eR8G8B8A8Unorm,
        // STEREO_LUMA_MAX caps the estimator luma (0 = full resolution).
        std::getenv("STEREO_LUMA_MAX") ? uint32_t(std::stoul(std::getenv("STEREO_LUMA_MAX")))
                                       : alvr::motion::StereoDepth::DefaultLumaDimension
    );

    // Warm-up run
    alvr::motion::StereoDepthTiming warmTiming {};
    estimator.estimate(
        fixture.colorImage,
        fixture.colorView,
        vk::ImageLayout::eShaderReadOnlyOptimal,
        fixture.depthImage,
        fixture.depthView,
        vk::ImageLayout::eGeneral,
        params,
        &warmTiming
    );

    // Timed run
    std::cout << "Running stereo depth estimator...\n";
    alvr::motion::StereoDepthTiming timing {};
    estimator.estimate(
        fixture.colorImage,
        fixture.colorView,
        vk::ImageLayout::eShaderReadOnlyOptimal,
        fixture.depthImage,
        fixture.depthView,
        vk::ImageLayout::eGeneral,
        params,
        &timing
    );

    res.wallMs = timing.totalWallMs;
    res.opticalGpuMs = timing.opticalGpuMs;
    res.depthGpuMs = timing.depthGpuMs;

    std::cout << "Done! Wall time: " << std::fixed << std::setprecision(2) << timing.totalWallMs << " ms"
              << " (Luma: " << timing.lumaWallMs << " ms, Flow: " << timing.flowWallMs << " ms"
              << ", Depth/Fill: " << timing.depthWallMs << " ms)\n";
    std::cout << "GPU times: OpticalFlow: " << timing.opticalGpuMs << " ms, DepthPass: " << timing.depthGpuMs << " ms\n";

    // Download estimated depth
    std::vector<float> estimatedDepth(fullWidth * height);
    fixture.downloadDepth(estimatedDepth.data());

    // Save outputs previews
    writeDepthPpm(outDir / (prefix + "_depth_est.ppm"), estimatedDepth.data(), fullWidth, height);
    writePfm(outDir / (prefix + "_depth_est.pfm"), estimatedDepth.data(), fullWidth, height);

    // Relative error map for preview
    std::vector<uint8_t> errorPpm(fullWidth * height * 3, 0);
    for (size_t i = 0; i < fullWidth * height; ++i) {
        float gt = groundTruth[i];
        float est = estimatedDepth[i];
        if (gt <= 0.0f) {
            // Sky
            errorPpm[i * 3 + 0] = (est > 0.0f) ? 255 : 30; // False positive = Red
            errorPpm[i * 3 + 1] = 30;
            errorPpm[i * 3 + 2] = 30;
        } else if (est <= 0.0f) {
            // Missed valid pixel (Hole / rejected)
            errorPpm[i * 3 + 0] = 60;
            errorPpm[i * 3 + 1] = 60;
            errorPpm[i * 3 + 2] = 200; // Blue
        } else {
            float relErr = std::abs(est - gt) / gt;
            float t = std::clamp(relErr / 0.15f, 0.0f, 1.0f); // 0% = Green, >=15% = Red
            errorPpm[i * 3 + 0] = uint8_t(t * 255.0f);
            errorPpm[i * 3 + 1] = uint8_t((1.0f - t) * 255.0f);
            errorPpm[i * 3 + 2] = 0;
        }
    }
    std::ofstream fErr(outDir / (prefix + "_depth_error.ppm"), std::ios::binary);
    if (fErr) {
        fErr << "P6\n" << fullWidth << " " << height << "\n255\n";
        fErr.write(reinterpret_cast<const char*>(errorPpm.data()), errorPpm.size());
    }

    // Per-region statistics computation
    struct Acc {
        uint32_t total = 0;
        uint32_t valid = 0;
        std::vector<double> relErrors;
        std::vector<double> absErrors;
    };
    std::vector<Acc> accRegions(REGION_COUNT);
    Acc accEdge;
    Acc accOverall;

    for (size_t i = 0; i < fullWidth * height; ++i) {
        float gt = groundTruth[i];
        float est = estimatedDepth[i];
        RegionType r = RegionType(regionMap[i]);

        accRegions[r].total++;
        if (gt > 0.0f) {
            accOverall.total++;
            if (isEdge[i]) accEdge.total++;
        }

        if (est > 0.0f) {
            accRegions[r].valid++;
            if (gt > 0.0f) {
                accOverall.valid++;
                if (isEdge[i]) accEdge.valid++;

                double err = std::abs(double(est) - double(gt));
                double rel = err / double(gt);

                accRegions[r].relErrors.push_back(rel);
                accRegions[r].absErrors.push_back(err);
                accOverall.relErrors.push_back(rel);
                accOverall.absErrors.push_back(err);

                if (isEdge[i]) {
                    accEdge.relErrors.push_back(rel);
                    accEdge.absErrors.push_back(err);
                }
            }
        }
    }

    auto computeStats = [](const std::string& name, Acc& acc) -> TestResults::RegionStats {
        TestResults::RegionStats s;
        s.name = name;
        s.totalPixels = acc.total;
        s.validPixels = acc.valid;
        s.validFraction = acc.total ? (double(acc.valid) / double(acc.total)) : 0.0;
        if (!acc.relErrors.empty()) {
            std::sort(acc.relErrors.begin(), acc.relErrors.end());
            std::sort(acc.absErrors.begin(), acc.absErrors.end());
            size_t n = acc.relErrors.size();
            s.medianRelErr = acc.relErrors[n / 2];
            s.p90RelErr = acc.relErrors[size_t(0.90 * (n - 1))];
            double sumRel = 0.0, sumAbs = 0.0;
            for (size_t i = 0; i < n; ++i) {
                sumRel += acc.relErrors[i];
                sumAbs += acc.absErrors[i];
            }
            s.meanRelErr = sumRel / n;
            s.maeMetres = sumAbs / n;
        }
        return s;
    };

    for (int r = 0; r < REGION_COUNT; ++r) {
        res.regions.push_back(computeStats(regionName(RegionType(r)), accRegions[r]));
    }
    res.edgeStats = computeStats("Depth Edges (jump > 0.35m)", accEdge);
    res.overallStats = computeStats("Overall All-Valid", accOverall);

    // Print summary table
    std::cout << "\n----------------------------------------------------------------------------------------------------\n";
    std::cout << std::left << std::setw(30) << "Region"
              << std::right << std::setw(12) << "Valid Px"
              << std::setw(12) << "Total Px"
              << std::setw(12) << "Valid %"
              << std::setw(16) << "Median Rel Err"
              << std::setw(16) << "P90 Rel Err"
              << std::setw(14) << "MAE (m)" << "\n";
    std::cout << "----------------------------------------------------------------------------------------------------\n";
    for (auto const& reg : res.regions) {
        if (reg.totalPixels == 0) continue;
        std::cout << std::left << std::setw(30) << reg.name
                  << std::right << std::setw(12) << reg.validPixels
                  << std::setw(12) << reg.totalPixels
                  << std::setw(11) << std::fixed << std::setprecision(1) << (reg.validFraction * 100.0) << "%";
        if (reg.name.find("Sky") != std::string::npos) {
            std::cout << std::setw(16) << "n/a (sky)" << std::setw(16) << "n/a" << std::setw(14) << "n/a\n";
        } else {
            std::cout << std::setw(15) << std::setprecision(2) << (reg.medianRelErr * 100.0) << "%"
                      << std::setw(15) << (reg.p90RelErr * 100.0) << "%"
                      << std::setw(14) << std::setprecision(4) << reg.maeMetres << "\n";
        }
    }
    std::cout << "----------------------------------------------------------------------------------------------------\n";
    std::cout << std::left << std::setw(30) << res.edgeStats.name
              << std::right << std::setw(12) << res.edgeStats.validPixels
              << std::setw(12) << res.edgeStats.totalPixels
              << std::setw(11) << std::setprecision(1) << (res.edgeStats.validFraction * 100.0) << "%"
              << std::setw(15) << std::setprecision(2) << (res.edgeStats.medianRelErr * 100.0) << "%"
              << std::setw(15) << (res.edgeStats.p90RelErr * 100.0) << "%"
              << std::setw(14) << std::setprecision(4) << res.edgeStats.maeMetres << "\n";
    std::cout << std::left << std::setw(30) << res.overallStats.name
              << std::right << std::setw(12) << res.overallStats.validPixels
              << std::setw(12) << res.overallStats.totalPixels
              << std::setw(11) << std::setprecision(1) << (res.overallStats.validFraction * 100.0) << "%"
              << std::setw(15) << std::setprecision(2) << (res.overallStats.medianRelErr * 100.0) << "%"
              << std::setw(15) << (res.overallStats.p90RelErr * 100.0) << "%"
              << std::setw(14) << std::setprecision(4) << res.overallStats.maeMetres << "\n";
    std::cout << "----------------------------------------------------------------------------------------------------\n";

    return res;
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 2) {
            std::cerr << "Usage: stereo_depth_test <OUTPUT_DIR>\n";
            return 1;
        }

        // Ensure Vulkan optical-flow environment
        setenv("ALVR_NATIVE_MOTION", "1", 0);
        setenv("ALVR_VULKAN_DEVICE", "NVIDIA", 0);

        std::filesystem::path outDir(argv[1]);
        std::filesystem::create_directories(outDir);

        std::cout << "Initializing offscreen Vulkan context (RecordingContext)...\n";
        RecordingContext ctx;

        // Quest-2 realistic asymmetric FOV tangents
        // Left eye: outer (-52 deg), inner (+42 deg), up (+47 deg), down (-45 deg)
        // Right eye: inner (-42 deg), outer (+52 deg), up (+47 deg), down (-45 deg)
        alvr::motion::StereoParams params;
        params.ipd = 0.063f; // 63 mm default
        params.leftTans = {
            std::tan(-52.0f * float(M_PI) / 180.0f), // -1.2799
            std::tan( 42.0f * float(M_PI) / 180.0f), //  0.9004
            std::tan( 47.0f * float(M_PI) / 180.0f), //  1.0724
            std::tan(-45.0f * float(M_PI) / 180.0f), // -1.0000
        };
        params.rightTans = {
            std::tan(-42.0f * float(M_PI) / 180.0f), // -0.9004
            std::tan( 52.0f * float(M_PI) / 180.0f), //  1.2799
            std::tan( 47.0f * float(M_PI) / 180.0f), //  1.0724
            std::tan(-45.0f * float(M_PI) / 180.0f), // -1.0000
        };
        params.maxVerticalError = 2.0f;
        params.maxConsistencyError = 2.0f;
        params.fillRadius = 4;
        params.fillMinNeighbors = 2;

        std::cout << "Configured IPD: " << params.ipd << " m\n";
        std::cout << "Left Eye Tans:  [" << params.leftTans.left << ", " << params.leftTans.right
                  << ", " << params.leftTans.up << ", " << params.leftTans.down << "]\n";
        std::cout << "Right Eye Tans: [" << params.rightTans.left << ", " << params.rightTans.right
                  << ", " << params.rightTans.up << ", " << params.rightTans.down << "]\n";

        // STEREO_BENCH=1: timing only, in the driver's configuration (luma cap
        // 640, see Encoder.hpp), with full and reduced (luma grid) output, at
        // the stream sizes used live. Textured random stereo pair with a
        // constant disparity; 5 warm-up runs, then 50 timed runs each.
        if (std::getenv("STEREO_BENCH")) {
            std::cout << "\nsize/eye     output   wall mean  p95    max   | flow GPU mean | depth GPU mean\n";
            for (auto [w, h] : { std::pair<uint32_t, uint32_t> { 1832, 1920 }, { 2048, 2144 } }) {
                uint32_t full = w * 2;
                std::vector<uint8_t> rgba(size_t(full) * h * 4);
                uint32_t seed = 12345;
                auto rnd = [&] { seed = seed * 1664525u + 1013904223u; return uint8_t(seed >> 24); };
                uint32_t cellsX = (w + 64) / 8 + 1, cellsY = h / 8 + 1;
                std::vector<uint8_t> tex(size_t(cellsX) * cellsY);
                for (auto& v : tex) v = rnd();
                for (uint32_t y = 0; y < h; ++y)
                    for (uint32_t x = 0; x < full; ++x) {
                        uint32_t eye = x / w, ex = x % w;
                        // Blocky texture (8 px cells) so the flow has structure;
                        // the right eye sees it 24 px to the left.
                        uint32_t sx = (ex + (eye ? 24 : 0)) / 8, sy = y / 8;
                        uint8_t v = tex[size_t(sy) * cellsX + sx];
                        uint8_t* p = &rgba[(size_t(y) * full + x) * 4];
                        p[0] = v; p[1] = uint8_t(255 - v); p[2] = uint8_t(v / 2 + 64); p[3] = 255;
                    }
                GpuFixture fixture(ctx, full, h);
                fixture.uploadColor(rgba.data());
                for (bool reduced : { false, true }) {
                    alvr::motion::StereoDepth estimator(
                        ctx, vk::Extent2D { full, h }, vk::Format::eR8G8B8A8Unorm,
                        std::getenv("STEREO_LUMA_MAX") ? uint32_t(std::stoul(std::getenv("STEREO_LUMA_MAX"))) : 640u, reduced
                    );
                    std::vector<double> wall, flow, depth;
                    for (int run = 0; run < 55; ++run) {
                        alvr::motion::StereoDepthTiming t {};
                        estimator.estimate(
                            fixture.colorImage, fixture.colorView, vk::ImageLayout::eShaderReadOnlyOptimal,
                            reduced ? vk::Image {} : fixture.depthImage,
                            reduced ? vk::ImageView {} : fixture.depthView,
                            vk::ImageLayout::eGeneral, params, &t
                        );
                        if (run < 5) continue;
                        wall.push_back(t.totalWallMs);
                        flow.push_back(t.opticalGpuMs);
                        depth.push_back(t.depthGpuMs);
                    }
                    auto mean = [](std::vector<double> const& v) {
                        double s = 0;
                        for (double x : v) s += x;
                        return s / v.size();
                    };
                    std::vector<double> sorted = wall;
                    std::sort(sorted.begin(), sorted.end());
                    std::cout << std::fixed << std::setprecision(2) << w << "x" << h << "  "
                              << (reduced ? "reduced" : "full   ") << "  " << std::setw(6) << mean(wall)
                              << "  " << std::setw(5) << sorted[size_t(sorted.size() * 0.95)] << "  "
                              << std::setw(5) << sorted.back() << " | " << std::setw(6) << mean(flow)
                              << "        | " << std::setw(6) << mean(depth) << "\n";
                }
            }
            return 0;
        }

        // Run 1: 1024x640 per eye
        auto res1024 = runResolutionTest(ctx, 1024, 640, params, outDir, "stereo_1024x640");

        // Run 2: 1832x1920 per eye
        auto res1832 = runResolutionTest(ctx, 1832, 1920, params, outDir, "stereo_1832x1920");

        // Summary timing table
        std::cout << "\n======================================================\n";
        std::cout << "ESTIMATOR TIMING COMPARISON:\n";
        std::cout << "======================================================\n";
        std::cout << std::left << std::setw(25) << "Resolution / eye"
                  << std::right << std::setw(15) << "Total Wall (ms)"
                  << std::setw(18) << "OpticalFlow GPU (ms)"
                  << std::setw(16) << "DepthPass GPU (ms)" << "\n";
        std::cout << "--------------------------------------------------------------------\n";
        std::cout << std::left << std::setw(25) << "1024 x 640"
                  << std::right << std::setw(15) << std::fixed << std::setprecision(2) << res1024.wallMs
                  << std::setw(18) << res1024.opticalGpuMs
                  << std::setw(16) << res1024.depthGpuMs << "\n";
        std::cout << std::left << std::setw(25) << "1832 x 1920"
                  << std::right << std::setw(15) << std::fixed << std::setprecision(2) << res1832.wallMs
                  << std::setw(18) << res1832.opticalGpuMs
                  << std::setw(16) << res1832.depthGpuMs << "\n";
        std::cout << "======================================================\n";

        std::cout << "\nPASS: Stereo depth estimator offline validation completed successfully!\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: Exception in stereo depth validation: " << e.what() << std::endl;
        return 1;
    }
}
