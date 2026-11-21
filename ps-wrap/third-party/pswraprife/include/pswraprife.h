// PS-WRAP-MAC-FG: C ABI of libPsWrapRife (RIFE v4.25 on Metal via rife-metal)
#pragma once
#include <CoreVideo/CoreVideo.h>
#ifdef __cplusplus
extern "C" {
#endif
// tier: 0 = hq (full-res IFNet), 1 = balanced (half-res), 2 = fast (quarter-res). Returns NULL on failure.
void *pswrap_rife_create(const char *model_path, int tier);
// prev/cur/out: BGRA, same size (IOSurface-backed). Returns 0 on success.
int pswrap_rife_interpolate(void *handle, CVPixelBufferRef prev, CVPixelBufferRef cur, CVPixelBufferRef out, float t);
void pswrap_rife_destroy(void *handle);
#ifdef __cplusplus
}
#endif
