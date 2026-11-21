// PS-WRAP-MAC: Apple VideoToolbox low-latency frame interpolation (VTFrameProcessor +
// VTLowLatencyFrameInterpolationConfiguration, macOS 26+) on decoded CVPixelBuffers.
//
// Runtime-loaded through the Objective-C runtime so the app still builds with older SDKs
// (Xcode 16 SDK has no VTFrameProcessor headers) and still runs on macOS < 26 (isAvailable() == false).
//
// Limits observed on macOS 27.2 / M4 Pro: only '420v' (NV12 video range, 8 bit) in/out,
// maximumDimensionForSpatialScaleFactor(1) = 1920, maximumPixelCountForSpatialScaleFactor(1) = 1920*1080.
// Larger inputs are downscaled to a working size that fits; the generated frame is scaled back up.
//
// Not thread safe — use from one thread (the decoder/frame thread).
#pragma once

#include <CoreVideo/CoreVideo.h>
#include <cstdint>

// Monotonic clock (µs, mach_absolute_time) + display refresh grid from a CVDisplayLink on the main display.
int64_t pswrapNowUs();
// period_us / last_vsync_us of the main display; false if unavailable (then pace on the stream clock only)
bool pswrapDisplayVsync(double *period_us, int64_t *last_vsync_us);

class PsWrapVtInterp
{
public:
	PsWrapVtInterp();
	~PsWrapVtInterp();
	PsWrapVtInterp(const PsWrapVtInterp &) = delete;
	PsWrapVtInterp &operator=(const PsWrapVtInterp &) = delete;

	static bool isAvailable();
	static bool rifeAvailable();       // libPsWrapRife + rife-v4.25.rmw found
	// 0 = Apple VT · 1 = RIFE v4.25 (Metal) · 2 = hybrid: VT, RIFE for the pairs VT can't track (fast pans / snaps)
	void setEngine(int e) { engine = e; }   // set before the first interpolate()
	bool lastUsedRife() const { return last_used_rife; }
	static constexpr double kHoldUnmatchedRife = 0.60;   // hybrid/RIFE: hold only above this untrackable fraction
	static constexpr double kHoldMotionRife = 260.0;     // … or this pan (1080p px per pair)   // OS has VTLowLatencyFrameInterpolationConfiguration and it reports supported

	// Force a working height (0 = largest size the engine accepts, i.e. native for 1080p).
	void setWorkingHeight(int h) { forced_work_h = h; }

	// Generate the frame at `phase` (0..1) between prev and cur. Returns a new +1 retained
	// CVPixelBuffer (NV12 '420v', same size as cur) or nullptr on failure. Caller releases.
	// Buffers of another pixel format are converted (VTPixelTransferSession).
	CVPixelBufferRef interpolate(CVPixelBufferRef prev, CVPixelBufferRef cur, float phase);

	// Scene-cut / discontinuity detector on the luma plane (cheap, CPU, subsampled).
	// Returns true if prev → cur looks like a cut (interpolating would make a ghost blend).
	bool looksLikeCut(CVPixelBufferRef prev, CVPixelBufferRef cur);

	// dominant translation prev → cur (px), false if none is clear
	static bool globalMotion(CVPixelBufferRef a, CVPixelBufferRef b, int *gx, int *gy);
	static double unmatchedFraction(CVPixelBufferRef a, CVPixelBufferRef b, double *worst_region = nullptr);
	double lastUnmatchedRegion() const { return last_unmatched_region; }
	static constexpr double kHoldRegion = 0.75;   // > this fraction of one 3x3 screen region untrackable → hold (VT)
	double lastUnmatched() const { return last_unmatched; }
	// local repair of melted / ghosted blocks in the generated frame (see pswrapvtinterp.mm)
	static int edgeFill(CVPixelBufferRef prev, CVPixelBufferRef cur, CVPixelBufferRef gen, int dx, int dy);
	double lastEdgeMs() const { return last_edge_ms; }
	static int localRepair(CVPixelBufferRef prev, CVPixelBufferRef cur, CVPixelBufferRef gen);
	void setLocalRepair(bool on) { local_repair = on; }
	int lastRepairedBlocks() const { return last_repaired; }
	double lastRepairMs() const { return last_repair_ms; }
	static constexpr double kHoldUnmatched = 0.27;      // > this fraction of textured blocks untrackable → hold (real GTA: 0.3–0.45 still had melted frames)
	void setHoldFastPans(bool on) { hold_fast_pans = on; }
	bool lastTooFast() const { return too_fast; }        // last interpolate() refused: pan beyond the engine's range
	int lastGlobalX() const { return last_gx; }
	int lastGlobalY() const { return last_gy; }
	double lastGlobalMotion() const { return last_gm; }   // 1080p px
	static constexpr double kHoldMotion = 100.0;         // 1080p px of camera motion per frame pair: above → hold

	void reset();                // end session (e.g. stream restart / size change)
	bool broken() const { return failed; }
	int workWidth() const { return work_w; }
	int workHeight() const { return work_h; }
	double lastMs() const { return last_ms; }

private:
	bool ensure(int w, int h);
	CVPixelBufferRef toWork(CVPixelBufferRef src);   // +1 retained, '420v' at work size
	struct Impl;
	Impl *d;
	int src_w = 0, src_h = 0, work_w = 0, work_h = 0;
	int forced_work_h = 0;
	int64_t pts = 0;
	bool failed = false;
	double last_ms = 0.0;
	double mad_ema = -1.0;
	bool hold_fast_pans = true;
	int engine = 0;
	bool last_used_rife = false;
	bool too_fast = false;
	int last_gx = 0, last_gy = 0;
	double last_gm = 0.0;
	double last_unmatched = 0.0;
	double last_unmatched_region = 0.0;
	double last_edge_ms = 0.0;
	bool local_repair = true;
	int last_repaired = 0;
	double last_repair_ms = 0.0;   // running mean abs luma diff between consecutive frames (cut detector)
};
