#ifndef MHL_DEPTH_PAYLOAD_VALIDATION_H
#define MHL_DEPTH_PAYLOAD_VALIDATION_H
/* Shared CPU recording and GPU-export boundary validation; packet v1 unchanged. */
#include "depth_packet.h"
#include <string.h>
/* Pixel scan only: caller must first validate packet/plane bounds. */
static inline const char *mhl_depth_validate_plane_pixels(const MhlDepthPlane *plane,unsigned role,const void *payload) {
        const unsigned char *base=(const unsigned char *)payload+plane->offset;
        for(uint32_t y=0;y<plane->height;y++)for(uint32_t x=0;x<plane->width;x++){
            const unsigned char *pixel=base+(size_t)y*plane->row_stride+x*(role?8:4);
            if(role){
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
                uint64_t bits;memcpy(&bits,pixel,8);
                // Each masked half exponent occupies a separate 16-bit lane.
                // Adding one exponent unit sets bit15 only for exponent31;
                // no lane can carry into its neighbour.
                if(((bits&UINT64_C(0x7c007c007c007c00))+UINT64_C(0x0400040004000400))&UINT64_C(0x8000800080008000))return "nonfinite_colour";
#else
                for(unsigned c=0;c<4;c++)if((pixel[2*c+1]&0x7c)==0x7c)return "nonfinite_colour";
#endif
            }else{
                uint32_t bits;memcpy(&bits,pixel,4);
                // IEEE R32F: all nonnegative finite values sort below +Inf;
                // negative values (including -0) and NaNs sort above it.
                if(bits>=UINT32_C(0x7f800000))return "invalid_canonical_depth";
            }
        }
    return NULL;
}
static inline const char *mhl_depth_validate_payload_bytes(const MhlDepthPacket *p,const void *payload,size_t bytes) {
    const char *error=mhl_depth_validate(p,sizeof(*p));
    if(error)return error;
    if(!payload || bytes!=p->buffer_size)return "payload_size";
    for(unsigned eye=0;eye<2;eye++)for(unsigned role=0;role<2;role++){
        const MhlDepthPlane *plane=role?&p->eyes[eye].colour:&p->eyes[eye].depth;
        error=mhl_depth_validate_plane_pixels(plane,role,payload);if(error)return error;
    }
    return NULL;
}
#endif
