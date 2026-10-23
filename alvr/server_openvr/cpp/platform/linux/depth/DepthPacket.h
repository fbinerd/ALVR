#ifndef MHL_DEPTH_PACKET_H
#define MHL_DEPTH_PACKET_H

/* Source-neutral depth ABI. No OpenXR, Proton, Vulkan or pointer types.
 * Wire structs use the local Linux little-endian IEEE-754 ABI, with explicit
 * size/version gates. JSON recordings describe the same fields and planes.
 */
#include <stdint.h>
#include <stddef.h>
#include <math.h>

#ifdef __cplusplus
extern "C" {
#define MHL_DEPTH_ASSERT(condition,message) static_assert(condition,message)
#else
#define MHL_DEPTH_ASSERT(condition,message) _Static_assert(condition,message)
#endif

#define MHL_DEPTH_MAGIC UINT32_C(0x5044484d) /* MHDP */
#define MHL_DEPTH_VERSION 1u
#define MHL_DEPTH_EYES 2u
#define MHL_DEPTH_NO_FD UINT32_MAX

enum MhlDepthSource { MHL_SOURCE_OPENXR_DECLARED=1, MHL_SOURCE_STEREO=2,
    MHL_SOURCE_VULKAN_LAYER=3, MHL_SOURCE_SYNTHETIC=4 };
enum MhlDepthFlags { MHL_PACKET_VALID=1u, MHL_CLOCK_MONOTONIC=2u,
    MHL_TRACKING_ALIGNED=4u };
enum MhlDepthSpace { MHL_SPACE_LOCAL_UNALIGNED=0, MHL_SPACE_TRACKING=1 };
enum MhlDepthTransport { MHL_TRANSPORT_RECORDING=0, MHL_TRANSPORT_VULKAN_OPAQUE_FD=1,
    MHL_TRANSPORT_DMA_BUF=2 };
enum MhlDepthFormat {
    MHL_FORMAT_D16_UNORM=1, MHL_FORMAT_X8_D24_UNORM=2,
    MHL_FORMAT_D24_UNORM_S8=3, MHL_FORMAT_D32_SFLOAT=4,
    MHL_FORMAT_D32_SFLOAT_S8=5, MHL_FORMAT_R32_LINEAR=6,
    MHL_FORMAT_RGBA8_UNORM=16, MHL_FORMAT_RGBA8_SRGB=17,
    MHL_FORMAT_BGRA8_UNORM=18, MHL_FORMAT_BGRA8_SRGB=19,
    MHL_FORMAT_RGBA16F_LINEAR=20,
    MHL_CANONICAL_DEPTH_R32F=100, MHL_CANONICAL_COLOUR_RGBA16F=101
};
enum MhlDepthEncodingFlags { MHL_DEPTH_LINEAR=1u, MHL_DEPTH_REVERSED_Z=2u,
    MHL_DEPTH_INFINITE_FAR=4u };

typedef struct MhlDepthPose { float position[3], orientation[4]; } MhlDepthPose;
typedef struct MhlDepthRect { int32_t x,y;uint32_t width,height; } MhlDepthRect;
typedef struct MhlDepthImageSource {
    uint32_t format,native_format,width,height,layers,layer_index,samples,reserved;
    MhlDepthRect rect;
} MhlDepthImageSource;
typedef struct MhlDepthEncoding {
    uint32_t flags;
    float min_depth,max_depth,near_m,far_m,linear_scale,linear_bias;
    uint32_t reserved;
} MhlDepthEncoding;
typedef struct MhlDepthNormalizeParameters {
    uint32_t count,flags;
    float min_depth,max_depth,near_m,far_m,linear_scale,linear_bias;
} MhlDepthNormalizeParameters;
typedef struct MhlDepthPlane {
    uint32_t format,width,height,row_stride;
    uint64_t offset,bytes;
} MhlDepthPlane;
typedef struct MhlDepthEye {
    MhlDepthPose pose;
    float fov[4]; /* left,right,up,down radians; +X right,+Y up,-Z forward */
    MhlDepthImageSource depth_source,colour_source;
    MhlDepthEncoding encoding;
    uint32_t reserved;
    MhlDepthPlane depth,colour;
} MhlDepthEye;
typedef struct MhlDepthPacket {
    uint32_t magic,version,size,flags;
    uint32_t source_tag,eye_count,producer_pid,transport;
    float confidence;
    uint32_t pose_space,reserved0,reserved1;
    uint64_t producer_id,frame_id,app_time_ns,display_time_ns,display_period_ns;
    uint64_t buffer_size,timeline_value;
    uint32_t memory_fd_index,semaphore_fd_index;
    uint8_t device_uuid[16];
    MhlDepthEye eyes[MHL_DEPTH_EYES];
} MhlDepthPacket;

MHL_DEPTH_ASSERT(sizeof(float)==4,"IEEE float32 required");
MHL_DEPTH_ASSERT(sizeof(MhlDepthImageSource)==48,"source ABI");
MHL_DEPTH_ASSERT(sizeof(MhlDepthEncoding)==32,"encoding ABI");
MHL_DEPTH_ASSERT(sizeof(MhlDepthNormalizeParameters)==32,"normalizer push ABI");
MHL_DEPTH_ASSERT(sizeof(MhlDepthPlane)==32,"plane ABI");
MHL_DEPTH_ASSERT(sizeof(MhlDepthEye)==240,"eye ABI");
MHL_DEPTH_ASSERT(offsetof(MhlDepthPacket,eyes)==128,"packet eye offset");
MHL_DEPTH_ASSERT(sizeof(MhlDepthPacket)==608,"packet ABI");

static inline MhlDepthNormalizeParameters mhl_depth_normalize_parameters(uint32_t count,const MhlDepthEncoding *e) {
    MhlDepthNormalizeParameters p;
    p.count=count;p.flags=e->flags;p.min_depth=e->min_depth;p.max_depth=e->max_depth;
    p.near_m=e->near_m;p.far_m=e->far_m;p.linear_scale=e->linear_scale;p.linear_bias=e->linear_bias;
    return p;
}

/* Far endpoint/sky, nonfinite values and invalid input produce exactly +0. */
static inline float mhl_depth_to_metres(float sample,const MhlDepthEncoding *e) {
    if(!e || !isfinite(sample) || !isfinite(e->near_m) || e->near_m<=0)return 0;
    float z;
    if(e->flags&MHL_DEPTH_LINEAR){
        if(sample<=0 || !isfinite(e->linear_scale) || !isfinite(e->linear_bias))return 0;
        z=sample*e->linear_scale+e->linear_bias;
    }else{
        if(!isfinite(e->min_depth) || !isfinite(e->max_depth) || e->max_depth<=e->min_depth)return 0;
        float t=(sample-e->min_depth)/(e->max_depth-e->min_depth);
        if(t<0 || t>1)return 0;
        int reversed=(e->flags&MHL_DEPTH_REVERSED_Z)!=0;
        if((reversed && t<=0) || (!reversed && t>=1))return 0;
        /* Keep reversed-Z arithmetic stable near the far plane: avoid
         * cancelling tiny raw depth by computing 1-(1-t). */
        float inverse=(reversed?t:1-t)/e->near_m;
        if(!(e->flags&MHL_DEPTH_INFINITE_FAR)){
            if(!isfinite(e->far_m) || e->far_m<=e->near_m)return 0;
            inverse+=(reversed?1-t:t)/e->far_m;
        }
        if(inverse<=0)return 0;
        z=1/inverse;
    }
    if(!isfinite(z) || z<=0 || ((e->flags&MHL_DEPTH_LINEAR) && z<e->near_m))return 0;
    if(!(e->flags&MHL_DEPTH_INFINITE_FAR) && (!isfinite(e->far_m) || z>=e->far_m))return 0;
    return z;
}

/* Returns a stable diagnostic string, NULL for valid packets. Unknown source
 * tags remain legal so adding a producer does not require a consumer rewrite.
 */
static inline const char *mhl_depth_validate(const MhlDepthPacket *p,size_t available) {
    if(!p || available<sizeof(*p))return "truncated_header";
    if(p->magic!=MHL_DEPTH_MAGIC || p->version!=MHL_DEPTH_VERSION)return "version";
    if(p->size<sizeof(*p) || p->size>available || p->size>4096)return "header_size";
    if((p->flags&(MHL_PACKET_VALID|MHL_CLOCK_MONOTONIC))!=(MHL_PACKET_VALID|MHL_CLOCK_MONOTONIC))return "validity_or_clock";
    if(p->eye_count!=2 || !p->source_tag || !p->producer_id)return "identity";
    if(!p->frame_id || !p->app_time_ns || !p->display_time_ns)return "timestamp";
    if(!isfinite(p->confidence) || p->confidence<0 || p->confidence>1)return "confidence";
    if(p->pose_space>MHL_SPACE_TRACKING || ((p->flags&MHL_TRACKING_ALIGNED) && p->pose_space!=MHL_SPACE_TRACKING))return "pose_space";
    if(p->transport>MHL_TRANSPORT_DMA_BUF || !p->buffer_size || p->buffer_size>(UINT64_C(1)<<32))return "transport_or_buffer_size";
    if(p->transport!=MHL_TRANSPORT_RECORDING && (p->memory_fd_index!=0 || p->semaphore_fd_index!=1 || !p->timeline_value))return "transport_descriptors";
    const MhlDepthPlane *planes[4];
    for(unsigned eye=0;eye<2;eye++){
        const MhlDepthEye *e=&p->eyes[eye];
        float norm=0;for(unsigned i=0;i<3;i++)if(!isfinite(e->pose.position[i]))return "position";
        for(unsigned i=0;i<4;i++){if(!isfinite(e->pose.orientation[i]) || !isfinite(e->fov[i]) || fabsf(e->fov[i])>=1.570796327f)return "orientation_or_fov";norm+=e->pose.orientation[i]*e->pose.orientation[i];}
        if(fabsf(norm-1)>0.001f || e->fov[0]>=e->fov[1] || e->fov[3]>=e->fov[2])return "orientation_or_fov";
        const MhlDepthImageSource *sources[2]={&e->depth_source,&e->colour_source};
        for(unsigned i=0;i<2;i++){
            const MhlDepthImageSource *s=sources[i];
            if((!i && (s->format<1 || s->format>6)) || (i && (s->format<16 || s->format>20)))return "source_format";
            if(!s->width || !s->height || s->width>16384 || s->height>16384 || !s->layers || s->layer_index>=s->layers || !s->samples || s->samples>64 || (s->samples&(s->samples-1)))return "source_extent";
            if(s->rect.x<0 || s->rect.y<0 || !s->rect.width || !s->rect.height || (uint64_t)s->rect.x+s->rect.width>s->width || (uint64_t)s->rect.y+s->rect.height>s->height)return "source_rect";
        }
        const MhlDepthEncoding *z=&e->encoding;
        if(e->depth_source.format==MHL_FORMAT_R32_LINEAR && !(z->flags&MHL_DEPTH_LINEAR))return "linear_source_encoding";
        if(z->flags&~7u || !isfinite(z->near_m) || z->near_m<=0 || !isfinite(z->min_depth) || !isfinite(z->max_depth) || z->min_depth<0 || z->max_depth>1 || z->min_depth>=z->max_depth)return "depth_encoding";
        if(z->flags&MHL_DEPTH_INFINITE_FAR){if(z->far_m!=0)return "infinite_far";}
        else if(!isfinite(z->far_m) || z->far_m<=z->near_m)return "far_plane";
        if(!isfinite(z->linear_scale) || z->linear_scale<=0 || !isfinite(z->linear_bias))return "linear_parameters";
        const MhlDepthPlane *eye_planes[2]={&e->depth,&e->colour};
        for(unsigned i=0;i<2;i++){
            const MhlDepthPlane *plane=eye_planes[i];unsigned bpp=i?8:4;
            if(plane->format!=(i?MHL_CANONICAL_COLOUR_RGBA16F:MHL_CANONICAL_DEPTH_R32F) || plane->width!=sources[i]->rect.width || plane->height!=sources[i]->rect.height || (uint64_t)plane->row_stride<(uint64_t)plane->width*bpp || plane->row_stride%bpp)return "canonical_plane";
            if(plane->offset%bpp || plane->bytes!=(uint64_t)plane->row_stride*plane->height || plane->offset>p->buffer_size || plane->bytes>p->buffer_size-plane->offset)return "plane_bounds";
            planes[eye*2+i]=plane;
        }
    }
    for(unsigned i=0;i<4;i++)for(unsigned j=i+1;j<4;j++)if(planes[i]->offset<planes[j]->offset+planes[j]->bytes && planes[j]->offset<planes[i]->offset+planes[i]->bytes)return "plane_overlap";
    return NULL;
}

#undef MHL_DEPTH_ASSERT
#ifdef __cplusplus
}
#endif
#endif
