// PS-WRAP-MAC: Apple VideoToolbox low-latency frame interpolation (see pswrapvtinterp.h)
#include "pswrapvtinterp.h"

#import <Foundation/Foundation.h>
#import <CoreMedia/CoreMedia.h>
#import <VideoToolbox/VideoToolbox.h>
#include <objc/runtime.h>
#include <dlfcn.h>
#include <string>
#include <unistd.h>
#include <mach/mach_time.h>

#include <algorithm>
#include <atomic>
#import <CoreGraphics/CoreGraphics.h>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <vector>

// --- declarations for the macOS 26 classes (not in older SDKs; resolved at runtime) ---
@protocol PWVTLLFIConfiguration <NSObject>
+ (BOOL)isSupported;
+ (NSInteger)maximumDimensionForSpatialScaleFactor:(NSInteger)s;
+ (NSInteger)maximumPixelCountForSpatialScaleFactor:(NSInteger)s;
- (instancetype)initWithFrameWidth:(NSInteger)w frameHeight:(NSInteger)h numberOfInterpolatedFrames:(NSInteger)n;
- (NSDictionary *)destinationPixelBufferAttributes;
- (NSDictionary *)sourcePixelBufferAttributes;
@end
@protocol PWVTFrame <NSObject>
- (instancetype)initWithBuffer:(CVPixelBufferRef)buffer presentationTimeStamp:(CMTime)pts;
@end
@protocol PWVTLLFIParameters <NSObject>
- (instancetype)initWithSourceFrame:(id)source previousFrame:(id)previous interpolationPhase:(NSArray<NSNumber *> *)phase
                  destinationFrames:(NSArray *)destinations;
@end
@protocol PWVTFrameProcessor <NSObject>
- (BOOL)startSessionWithConfiguration:(id)configuration error:(NSError **)error;
- (BOOL)processWithParameters:(id)parameters error:(NSError **)error;
- (void)endSession;
@end

namespace {

Class vtClass(const char *name)
{
	static bool loaded = false;
	if (!loaded) {
		dlopen("/System/Library/Frameworks/VideoToolbox.framework/VideoToolbox", RTLD_NOW | RTLD_GLOBAL);
		loaded = true;
	}
	return objc_getClass(name);
}

double nowMs()
{
	static mach_timebase_info_data_t tb = {};
	if (!tb.denom)
		mach_timebase_info(&tb);
	return double(mach_absolute_time()) * tb.numer / tb.denom / 1e6;
}

void logLine(const char *fmt, NSError *err)
{
	fprintf(stderr, "PSWRAP vtinterp: %s %s\n", fmt, err ? err.description.UTF8String : "");
}

} // namespace

int64_t pswrapNowUs()
{
	static mach_timebase_info_data_t tb = {};
	if (!tb.denom)
		mach_timebase_info(&tb);
	return int64_t(double(mach_absolute_time()) * tb.numer / tb.denom / 1000.0);
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
namespace {
std::atomic<int64_t> g_vsync_host{0};    // mach host time of the latest vsync
std::atomic<int64_t> g_vsync_period{0};  // in host time units
CVReturn vsyncCallback(CVDisplayLinkRef, const CVTimeStamp *now, const CVTimeStamp *, CVOptionFlags, CVOptionFlags *, void *)
{
	if (now && (now->flags & kCVTimeStampHostTimeValid)) {
		const int64_t prev = g_vsync_host.exchange(int64_t(now->hostTime));
		const int64_t d = int64_t(now->hostTime) - prev;
		if (prev && d > 0) {
			const int64_t p = g_vsync_period.load();
			g_vsync_period.store(p ? (p * 7 + d) / 8 : d);
		}
	}
	return kCVReturnSuccess;
}
} // namespace

bool pswrapDisplayVsync(double *period_us, int64_t *last_vsync_us)
{
	static CVDisplayLinkRef link = nullptr;
	static bool tried = false;
	if (!tried) {
		tried = true;
		if (CVDisplayLinkCreateWithActiveCGDisplays(&link) == kCVReturnSuccess && link) {
			CVDisplayLinkSetCurrentCGDisplay(link, CGMainDisplayID());
			CVDisplayLinkSetOutputCallback(link, vsyncCallback, nullptr);
			if (CVDisplayLinkStart(link) != kCVReturnSuccess) {
				CVDisplayLinkRelease(link);
				link = nullptr;
			}
		}
	}
	const int64_t host = g_vsync_host.load(), per = g_vsync_period.load();
	if (!link || !host || per <= 0)
		return false;
	static mach_timebase_info_data_t tb = {};
	if (!tb.denom)
		mach_timebase_info(&tb);
	*period_us = double(per) * tb.numer / tb.denom / 1000.0;
	*last_vsync_us = int64_t(double(host) * tb.numer / tb.denom / 1000.0);
	return *period_us > 2000.0 && *period_us < 60000.0;
}
#pragma clang diagnostic pop

struct PsWrapVtInterp::Impl
{
	id<PWVTFrameProcessor> proc = nil;
	CVPixelBufferPoolRef work_pool = nullptr;   // source-side buffers at work size ('420v')
	CVPixelBufferPoolRef dst_pool = nullptr;    // generated frame at work size
	CVPixelBufferPoolRef out_pool = nullptr;    // generated frame scaled back to source size
	VTPixelTransferSessionRef xfer = nullptr;
	CVPixelBufferRef prev_work = nullptr;       // converted prev (reused when prev == last cur)
	CVPixelBufferRef prev_key = nullptr;        // the source buffer prev_work was made from (weak identity)
	CVPixelBufferRef cur_work = nullptr;
	CVPixelBufferRef cur_key = nullptr;
	bool session = false;
	// RIFE engine (libPsWrapRife, dlopen'ed)
	void *rife = nullptr;
	CVPixelBufferPoolRef bgra_pool = nullptr;
	CVPixelBufferRef cur_bgra = nullptr;       // BGRA of the last cur (reused as next prev)
	CVPixelBufferRef cur_bgra_key = nullptr;
};

// ---- RIFE (Practical-RIFE v4.25 via rife-metal, MIT / Apache-2.0) — optional third engine ----
namespace {
struct RifeApi {
	void *(*create)(const char *, int) = nullptr;
	int (*interp)(void *, CVPixelBufferRef, CVPixelBufferRef, CVPixelBufferRef, float) = nullptr;
	void (*destroy)(void *) = nullptr;
	std::string model;
	bool ok = false;
};
static std::string exeDir()
{
	NSString *p = [[NSBundle mainBundle] executablePath];
	return p ? std::string([[p stringByDeletingLastPathComponent] UTF8String]) : std::string();
}
static const RifeApi &rifeApi()
{
	static RifeApi api;
	static bool tried = false;
	if (tried)
		return api;
	tried = true;
	@autoreleasepool {
		const std::string dir = exeDir();
		std::vector<std::string> libs;
		if (const char *e = getenv("PSWRAP_RIFE_LIB"))
			libs.push_back(e);
		libs.push_back(dir + "/../Frameworks/libPsWrapRife.dylib");
		void *h = nullptr;
		for (const std::string &l : libs)
			if ((h = dlopen(l.c_str(), RTLD_NOW | RTLD_LOCAL)))
				break;
		if (!h)
			return api;
		api.create = reinterpret_cast<void *(*)(const char *, int)>(dlsym(h, "pswrap_rife_create"));
		api.interp = reinterpret_cast<int (*)(void *, CVPixelBufferRef, CVPixelBufferRef, CVPixelBufferRef, float)>(dlsym(h, "pswrap_rife_interpolate"));
		api.destroy = reinterpret_cast<void (*)(void *)>(dlsym(h, "pswrap_rife_destroy"));
		std::vector<std::string> models;
		if (const char *e = getenv("PSWRAP_RIFE_MODEL"))
			models.push_back(e);
		models.push_back(dir + "/../Resources/rife/rife-v4.25.rmw");
		for (const std::string &m : models)
			if (access(m.c_str(), R_OK) == 0) {
				api.model = m;
				break;
			}
		api.ok = api.create && api.interp && api.destroy && !api.model.empty();
	}
	return api;
}
static CVPixelBufferPoolRef makeBgraPool(int w, int h)
{
	NSDictionary *attrs = @{
		(id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA),
		(id)kCVPixelBufferWidthKey: @(w),
		(id)kCVPixelBufferHeightKey: @(h),
		(id)kCVPixelBufferIOSurfacePropertiesKey: @{},
		(id)kCVPixelBufferMetalCompatibilityKey: @YES,
	};
	CVPixelBufferPoolRef pool = nullptr;
	CVPixelBufferPoolCreate(kCFAllocatorDefault, nullptr, (__bridge CFDictionaryRef)attrs, &pool);
	return pool;
}
// colour metadata so VTPixelTransferSession converts YUV↔RGB with the right matrix
static void tagBt709(CVPixelBufferRef b)
{
	CVBufferSetAttachment(b, kCVImageBufferYCbCrMatrixKey, kCVImageBufferYCbCrMatrix_ITU_R_709_2, kCVAttachmentMode_ShouldPropagate);
	CVBufferSetAttachment(b, kCVImageBufferColorPrimariesKey, kCVImageBufferColorPrimaries_ITU_R_709_2, kCVAttachmentMode_ShouldPropagate);
	CVBufferSetAttachment(b, kCVImageBufferTransferFunctionKey, kCVImageBufferTransferFunction_ITU_R_709_2, kCVAttachmentMode_ShouldPropagate);
}
} // namespace

bool PsWrapVtInterp::rifeAvailable()
{
	return rifeApi().ok;
}

PsWrapVtInterp::PsWrapVtInterp() : d(new Impl) {}

PsWrapVtInterp::~PsWrapVtInterp()
{
	reset();
	if (d->xfer) {
		VTPixelTransferSessionInvalidate(d->xfer);
		CFRelease(d->xfer);
	}
	delete d;
}

bool PsWrapVtInterp::isAvailable()
{
	Class c = vtClass("VTLowLatencyFrameInterpolationConfiguration");
	if (!c || !vtClass("VTFrameProcessor") || !vtClass("VTFrameProcessorFrame") || !vtClass("VTLowLatencyFrameInterpolationParameters"))
		return false;
	@autoreleasepool {
		return [(id<PWVTLLFIConfiguration>)c isSupported];
	}
}

void PsWrapVtInterp::reset()
{
	@autoreleasepool {
		if (d->proc) {
			if (d->session)
				[d->proc endSession];
			[d->proc release];
			d->proc = nil;
		}
	}
	d->session = false;
	if (d->rife) {
		rifeApi().destroy(d->rife);
		d->rife = nullptr;
	}
	if (d->cur_bgra) {
		CVPixelBufferRelease(d->cur_bgra);
		d->cur_bgra = nullptr;
	}
	d->cur_bgra_key = nullptr;
	if (d->bgra_pool) {
		CVPixelBufferPoolRelease(d->bgra_pool);
		d->bgra_pool = nullptr;
	}
	for (CVPixelBufferPoolRef *p : {&d->work_pool, &d->dst_pool, &d->out_pool})
		if (*p) {
			CVPixelBufferPoolRelease(*p);
			*p = nullptr;
		}
	for (CVPixelBufferRef *b : {&d->prev_work, &d->cur_work})
		if (*b) {
			CVPixelBufferRelease(*b);
			*b = nullptr;
		}
	d->prev_key = d->cur_key = nullptr;
	src_w = src_h = work_w = work_h = 0;
	failed = false;
	mad_ema = -1.0;
}

static CVPixelBufferPoolRef makePool(int w, int h)
{
	NSDictionary *attrs = @{
		(id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange),
		(id)kCVPixelBufferWidthKey: @(w),
		(id)kCVPixelBufferHeightKey: @(h),
		(id)kCVPixelBufferIOSurfacePropertiesKey: @{},
		(id)kCVPixelBufferMetalCompatibilityKey: @YES,
	};
	CVPixelBufferPoolRef pool = nullptr;
	if (CVPixelBufferPoolCreate(kCFAllocatorDefault, nullptr, (__bridge CFDictionaryRef)attrs, &pool) != kCVReturnSuccess)
		return nullptr;
	return pool;
}

bool PsWrapVtInterp::ensure(int w, int h)
{
	if (d->session && w == src_w && h == src_h)
		return true;
	if (failed && w == src_w && h == src_h)
		return false;
	reset();
	src_w = w;
	src_h = h;
	if (engine == 2) {   // hybrid: VT session (below) + RIFE for the pairs VT can't track
		const RifeApi &api = rifeApi();
		const int tier = getenv("PSWRAP_RIFE_TIER") ? atoi(getenv("PSWRAP_RIFE_TIER")) : 2;
		d->rife = api.ok ? api.create(api.model.c_str(), tier) : nullptr;
		if (!d->rife)
			fprintf(stderr, "PSWRAP vtinterp: RIFE unavailable — hybrid runs as VT only\n");
	}
	if (engine == 1) {   // RIFE: works at the source size (rife-metal pads internally)
		const RifeApi &api = rifeApi();
		const int tier = getenv("PSWRAP_RIFE_TIER") ? atoi(getenv("PSWRAP_RIFE_TIER")) : 2;
		d->rife = api.ok ? api.create(api.model.c_str(), tier) : nullptr;
		work_w = w;
		work_h = h;
		d->work_pool = makePool(w, h);
		d->dst_pool = makePool(w, h);
		d->bgra_pool = makeBgraPool(w, h);
		if (!d->xfer && VTPixelTransferSessionCreate(kCFAllocatorDefault, &d->xfer) != noErr)
			d->xfer = nullptr;
		if (!d->rife || !d->work_pool || !d->dst_pool || !d->bgra_pool || !d->xfer) {
			failed = true;
			return false;
		}
		d->session = true;
		fprintf(stderr, "PSWRAP vtinterp: RIFE v4.25 engine %dx%d tier %d\n", w, h, tier);
		return true;
	}
	Class cc = vtClass("VTLowLatencyFrameInterpolationConfiguration");
	Class pc = vtClass("VTFrameProcessor");
	if (!cc || !pc) {
		failed = true;
		return false;
	}
	@autoreleasepool {
		const NSInteger maxDim = [(id<PWVTLLFIConfiguration>)cc maximumDimensionForSpatialScaleFactor:1];
		const NSInteger maxPix = [(id<PWVTLLFIConfiguration>)cc maximumPixelCountForSpatialScaleFactor:1];
		// working size: same aspect, fits the engine limits, even dimensions
		double s = 1.0;
		if (forced_work_h > 0)
			s = double(forced_work_h) / h;   // may upscale (e.g. 960x540 → 1280x720: the engine rejects some sizes)
		if (maxDim > 0)
			s = std::min(s, double(maxDim) / std::max(w, h));
		if (maxPix > 0)
			s = std::min(s, std::sqrt(double(maxPix) / (double(w) * h)));
		work_w = std::max(2, int(std::floor(w * s / 2.0)) * 2);
		work_h = std::max(2, int(std::floor(h * s / 2.0)) * 2);
		id<PWVTLLFIConfiguration> conf = [[(id)cc alloc] initWithFrameWidth:work_w frameHeight:work_h numberOfInterpolatedFrames:1];
		if (!conf) {
			logLine("configuration init failed", nil);
			failed = true;
			return false;
		}
		d->proc = [[(id)pc alloc] init];
		NSError *err = nil;
		if (![d->proc startSessionWithConfiguration:conf error:&err]) {
			logLine("startSession failed:", err);
			[conf release];
			failed = true;
			return false;
		}
		[conf release];
		d->session = true;
	}
	d->work_pool = makePool(work_w, work_h);
	d->dst_pool = makePool(work_w, work_h);
	if (d->rife) {
		d->bgra_pool = makeBgraPool(work_w, work_h);
		// warm RIFE now (graph build ~0.3 s) so the first fast pan doesn't stall the stream
		CVPixelBufferRef a = nullptr, b = nullptr, o = nullptr;
		if (d->bgra_pool && CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, d->bgra_pool, &a) == kCVReturnSuccess &&
		    CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, d->bgra_pool, &b) == kCVReturnSuccess &&
		    CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, d->bgra_pool, &o) == kCVReturnSuccess) {
			if (rifeApi().interp(d->rife, a, b, o, 0.5f) != 0) {
				rifeApi().destroy(d->rife);
				d->rife = nullptr;
				fprintf(stderr, "PSWRAP vtinterp: RIFE warm-up failed — VT only\n");
			}
		}
		if (a) CVPixelBufferRelease(a);
		if (b) CVPixelBufferRelease(b);
		if (o) CVPixelBufferRelease(o);
	}
	if (work_w != w || work_h != h)
		d->out_pool = makePool(w, h);
	if (!d->xfer && VTPixelTransferSessionCreate(kCFAllocatorDefault, &d->xfer) != noErr)
		d->xfer = nullptr;
	if (!d->work_pool || !d->dst_pool || (work_w != w && !d->out_pool) || !d->xfer) {
		failed = true;
		return false;
	}
	fprintf(stderr, "PSWRAP vtinterp: session %dx%d (source %dx%d)\n", work_w, work_h, w, h);
	return true;
}

CVPixelBufferRef PsWrapVtInterp::toWork(CVPixelBufferRef src)
{
	const bool direct = CVPixelBufferGetPixelFormatType(src) == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange &&
	                    int(CVPixelBufferGetWidth(src)) == work_w && int(CVPixelBufferGetHeight(src)) == work_h &&
	                    CVPixelBufferGetIOSurface(src) != nullptr;
	if (direct)
		return CVPixelBufferRetain(src);
	CVPixelBufferRef out = nullptr;
	if (CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, d->work_pool, &out) != kCVReturnSuccess)
		return nullptr;
	if (VTPixelTransferSessionTransferImage(d->xfer, src, out) != noErr) {
		CVPixelBufferRelease(out);
		return nullptr;
	}
	return out;
}


// ---- global (camera) motion: dominant translation prev → cur on the luma plane, CPU, ~1-2 ms at 1080p ----
namespace {
struct Thumb { int w = 0, h = 0; std::vector<uint16_t> px; };
// box-average luma into (w/f x h/f)
static bool makeThumb(CVPixelBufferRef pb, int f, Thumb &t)
{
	if (CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess)
		return false;
	const uint8_t *y = static_cast<const uint8_t *>(CVPixelBufferGetBaseAddressOfPlane(pb, 0));
	const size_t st = CVPixelBufferGetBytesPerRowOfPlane(pb, 0);
	const int W = int(CVPixelBufferGetWidthOfPlane(pb, 0)), H = int(CVPixelBufferGetHeightOfPlane(pb, 0));
	t.w = W / f;
	t.h = H / f;
	t.px.assign(size_t(t.w) * t.h, 0);
	if (y)
		for (int ty = 0; ty < t.h; ty++)
			for (int tx = 0; tx < t.w; tx++) {
				unsigned sum = 0;
				for (int yy = 0; yy < f; yy += 2)   // every other row / column: plenty for a box average
					for (int xx = 0; xx < f; xx += 2)
						sum += y[size_t(ty * f + yy) * st + size_t(tx * f + xx)];
				t.px[size_t(ty) * t.w + tx] = uint16_t(sum * 4 * 16 / unsigned(f * f));   // luma * 16
			}
	CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
	return y != nullptr;
}
static double thumbSad(const Thumb &a, const Thumb &b, int dx, int dy, int margin)
{
	uint64_t s = 0;
	int n = 0;
	for (int y = margin; y < a.h - margin; y++) {
		const uint16_t *pa = &a.px[size_t(y) * a.w];
		const uint16_t *pb = &b.px[size_t(y + dy) * b.w + dx];
		for (int x = margin; x < a.w - margin; x++)
			s += uint64_t(std::abs(int(pa[x]) - int(pb[x])));
		n += a.w - 2 * margin;
	}
	return n ? double(s) / n : 1e9;
}
} // namespace

// test override: PSWRAP_VT_HOLD_PX=<px> (0 = never hold)
static double qEnvHoldPx()
{
	const char *e = getenv("PSWRAP_VT_HOLD_PX");
	if (!e)
		return PsWrapVtInterp::kHoldMotion;
	const double v = atof(e);
	return v <= 0.0 ? 1e9 : v;
}

// Fraction of 64x64-px blocks (1080p) of `a` that find no good match in `b` within ±96 px — i.e. content the engine
// cannot track: camera snapping through/around an obstacle, huge parallax, or motion beyond its range.
double PsWrapVtInterp::unmatchedFraction(CVPixelBufferRef a, CVPixelBufferRef b, double *worst_region)
{
	if (worst_region)
		*worst_region = 0.0;
	Thumb ta, tb;
	const int f = std::max(8, int(CVPixelBufferGetHeight(a)) / 68);   // ~1/16 at 1080p
	if (!makeThumb(a, f, ta) || !makeThumb(b, f, tb) || ta.w != tb.w || ta.h != tb.h || ta.w < 40 || ta.h < 24)
		return 0.0;
	const int B = 4, R = 6;
	int blocks = 0, bad = 0;
	int rt[9] = {}, rb[9] = {};   // per 3x3 screen region: textured / untrackable blocks
	for (int by = R; by + B + R <= ta.h; by += B)
		for (int bx = R; bx + B + R <= ta.w; bx += B) {
			// block activity (sum of |p - mean|): flat blocks always "match"; the match error is judged relative to it
			int sum = 0;
			for (int y = 0; y < B; y++)
				for (int x = 0; x < B; x++)
					sum += ta.px[size_t(by + y) * ta.w + bx + x];
			const int mean = sum / (B * B);
			int act = 0;
			for (int y = 0; y < B; y++)
				for (int x = 0; x < B; x++)
					act += std::abs(int(ta.px[size_t(by + y) * ta.w + bx + x]) - mean);
			if (act < 16 * 6 * B * B)
				continue;
			blocks++;
			const int reg = std::min(2, (by * 3) / ta.h) * 3 + std::min(2, (bx * 3) / ta.w);
			rt[reg]++;
			int best = 1 << 30;
			for (int dy = -R; dy <= R; dy++)
				for (int dx = -R; dx <= R; dx++) {
					int sad = 0;
					for (int y = 0; y < B; y++) {
						const uint16_t *pa = &ta.px[size_t(by + y) * ta.w + bx];
						const uint16_t *pb = &tb.px[size_t(by + y + dy) * tb.w + bx + dx];
						for (int x = 0; x < B; x++)
							sad += std::abs(int(pa[x]) - int(pb[x]));
					}
					best = std::min(best, sad);
				}
			// no offset explains the block: error ≥ 75% of its own activity (sub-texel shifts on a 1/16 grid stay well below)
			if (best > (act * 3) / 4)
			{
				bad++;
				rb[reg]++;
			}
		}
	if (worst_region)
		for (int r = 0; r < 9; r++)
			if (rt[r] >= 6)
				*worst_region = std::max(*worst_region, double(rb[r]) / rt[r]);
	return blocks >= 8 ? double(bad) / blocks : 0.0;
}

bool PsWrapVtInterp::globalMotion(CVPixelBufferRef a, CVPixelBufferRef b, int *gx, int *gy)
{
	*gx = *gy = 0;
	Thumb ta, tb;
	if (!makeThumb(a, 8, ta) || !makeThumb(b, 8, tb) || ta.w != tb.w || ta.h != tb.h || ta.w < 64 || ta.h < 40)
		return false;
	// coarse: 1/16 (2x2 of the 1/8 thumbs)
	Thumb ca, cb;
	for (auto pr : {std::make_pair(&ta, &ca), std::make_pair(&tb, &cb)}) {
		const Thumb &f = *pr.first;
		Thumb &c = *pr.second;
		c.w = f.w / 2;
		c.h = f.h / 2;
		c.px.resize(size_t(c.w) * c.h);
		for (int y = 0; y < c.h; y++)
			for (int x = 0; x < c.w; x++)
				c.px[size_t(y) * c.w + x] = uint16_t((f.px[size_t(2 * y) * f.w + 2 * x] + f.px[size_t(2 * y) * f.w + 2 * x + 1] +
				                                      f.px[size_t(2 * y + 1) * f.w + 2 * x] + f.px[size_t(2 * y + 1) * f.w + 2 * x + 1]) / 4);
	}
	const int RX = 16, RY = 6;   // ±256 x ±96 px at 1/16
	const int margin = std::max(RX, RY) + 1;
	const double sad0 = thumbSad(ca, cb, 0, 0, margin);
	double best = sad0;
	int bx = 0, by = 0;
	for (int dy = -RY; dy <= RY; dy++)
		for (int dx = -RX; dx <= RX; dx++) {
			const double c = thumbSad(ca, cb, dx, dy, margin) * (1.0 + 0.002 * (std::abs(dx) + std::abs(dy)));
			if (c < best) { best = c; bx = dx; by = dy; }
		}
	// refine at 1/8 ±2
	int fx = 2 * bx, fy = 2 * by;
	const int m8 = 2 * margin + 2;
	double bestf = thumbSad(ta, tb, fx, fy, m8);
	const int cx = fx, cy = fy;
	for (int dy = -2; dy <= 2; dy++)
		for (int dx = -2; dx <= 2; dx++) {
			const double c = thumbSad(ta, tb, cx + dx, cy + dy, m8);
			if (c < bestf) { bestf = c; fx = cx + dx; fy = cy + dy; }
		}
	const double sad0f = thumbSad(ta, tb, 0, 0, m8);
	if (!(bestf < 0.75 * sad0f))   // no clear dominant translation
		return false;
	*gx = fx * 8;
	*gy = fy * 8;
	return true;
}

// ---- local repair of the generated frame -------------------------------------------------------------------------
// Calibrated against ground truth (fgtools/harness/calib_repair*.py): where VT and a block matcher disagree, VT is right
// ~90% of the time, so blocks are NOT replaced just for disagreeing. Only the "melt" signature is repaired: a textured
// 32x32 block (1080p) that is about equally detailed in both real frames (no occlusion change) but whose detail
// collapses in the generated frame (gradient energy < 50% of both) — thin poles / chain-link fences turning to mush.
// Such a block is replaced by a motion-compensated midpoint 0.5*(prev(x - v/2) + cur(x + v/2)) from a symmetric
// quarter-res block search, only if that match is good; feathered over the block grid so no seams show.
namespace {
struct Plane { const uint8_t *p; size_t st; int w, h; };
static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static int gradEnergy(const Plane &y, int x0, int y0, int B)
{
	int e = 0;
	for (int yy = y0; yy < y0 + B - 2; yy += 2) {
		const uint8_t *r = y.p + size_t(yy) * y.st, *r2 = y.p + size_t(yy + 2) * y.st;
		for (int xx = x0; xx < x0 + B - 2; xx += 2)
			e += std::abs(int(r[xx + 2]) - int(r[xx])) + std::abs(int(r2[xx]) - int(r[xx]));
	}
	return e;
}
static void quarter(const Plane &y, std::vector<uint8_t> &q, int qw, int qh)
{
	q.resize(size_t(qw) * qh);
	for (int j = 0; j < qh; j++)
		for (int i = 0; i < qw; i++) {
			const uint8_t *a = y.p + size_t(j * 4 + 1) * y.st + i * 4 + 1, *b = a + 2 * y.st;
			q[size_t(j) * qw + i] = uint8_t((a[0] + a[2] + b[0] + b[2] + 2) >> 2);
		}
}
} // namespace

// ---- screen-edge fill on pans -------------------------------------------------------------------------------------
// During a camera pan the mid frame needs content at the screen edge that only one real frame has (scrolling in: only
// cur; scrolling out: only prev). The interpolator has no data there and smears it. With the global pan d (prev → cur,
// px): mid(x) = prev(x - d/2) = cur(x + d/2). On the side where prev(x - d/2) falls off-screen take cur(x + d/2), on
// the opposite side take prev(x - d/2); band = |d|/2 + margin, feathered.
int PsWrapVtInterp::edgeFill(CVPixelBufferRef prev, CVPixelBufferRef cur, CVPixelBufferRef gen, int dx, int dy)
{
	if (std::abs(dx) < 16 && std::abs(dy) < 16)
		return 0;
	if (CVPixelBufferLockBaseAddress(prev, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess)
		return 0;
	if (CVPixelBufferLockBaseAddress(cur, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) {
		CVPixelBufferUnlockBaseAddress(prev, kCVPixelBufferLock_ReadOnly);
		return 0;
	}
	if (CVPixelBufferLockBaseAddress(gen, 0) != kCVReturnSuccess) {
		CVPixelBufferUnlockBaseAddress(prev, kCVPixelBufferLock_ReadOnly);
		CVPixelBufferUnlockBaseAddress(cur, kCVPixelBufferLock_ReadOnly);
		return 0;
	}
	const int W = int(CVPixelBufferGetWidth(gen)), H = int(CVPixelBufferGetHeight(gen));
	const int margin = std::max(16, W / 80), feather = std::max(24, W / 60);
	const int hx = dx / 2, hy = dy / 2;   // half pan
	// band widths: left/right/top/bottom; weight w(x,y) = max over the four bands; source per band
	// the interpolator's smear reaches ~1.5x the missing strip → band 1.5·|d/2| + margin on the incoming side
	const int bwL = hx > 0 ? hx * 3 / 2 + margin : (hx < 0 ? -hx / 2 + margin : 0);   // left: incoming when content moves right (dx>0)
	const int bwR = hx < 0 ? -hx * 3 / 2 + margin : (hx > 0 ? hx / 2 + margin : 0);
	const int bwT = hy > 0 ? hy * 3 / 2 + margin : (hy < 0 ? -hy / 2 + margin : 0);
	const int bwB = hy < 0 ? -hy * 3 / 2 + margin : (hy > 0 ? hy / 2 + margin : 0);
	auto bandW = [&](int dist, int bw) -> float {   // 1 inside the band, feathered to 0 over `feather` px
		if (bw <= 0)
			return 0.f;
		if (dist < bw - feather)
			return 1.f;
		if (dist >= bw)
			return 0.f;
		return float(bw - dist) / float(feather);
	};
	int touched = 0;
	// static content (HUD, letterbox): same in prev and cur at the same position → keep the interpolator's pixel
	const uint8_t *yp0 = static_cast<const uint8_t *>(CVPixelBufferGetBaseAddressOfPlane(prev, 0));
	const uint8_t *yc0 = static_cast<const uint8_t *>(CVPixelBufferGetBaseAddressOfPlane(cur, 0));
	const size_t syp = CVPixelBufferGetBytesPerRowOfPlane(prev, 0), syc = CVPixelBufferGetBytesPerRowOfPlane(cur, 0);
	// per 16x16 block: mean |prev - cur| at the same position < 3 → static (HUD) — block-wise so textured areas don't speckle
	const int SB = 16, sbw = (W + SB - 1) / SB, sbh = (H + SB - 1) / SB;
	std::vector<int8_t> stat(size_t(sbw) * sbh, -1);
	auto isStatic = [&](int fx, int fy) {
		const int bx = fx / SB, by = fy / SB;
		int8_t &v = stat[size_t(by) * sbw + bx];
		if (v < 0) {
			int sum = 0, n = 0;
			for (int y = by * SB; y < std::min(H, by * SB + SB); y += 2)
				for (int x = bx * SB; x < std::min(W, bx * SB + SB); x += 2, n++)
					sum += std::abs(int(yp0[size_t(y) * syp + x]) - int(yc0[size_t(y) * syc + x]));
			v = (n && sum < 3 * n) ? 1 : 0;
		}
		return v == 1;
	};
	for (size_t pl = 0; pl < 2; pl++) {
		const uint8_t *sp = static_cast<const uint8_t *>(CVPixelBufferGetBaseAddressOfPlane(prev, pl));
		const uint8_t *sc = static_cast<const uint8_t *>(CVPixelBufferGetBaseAddressOfPlane(cur, pl));
		uint8_t *g = static_cast<uint8_t *>(CVPixelBufferGetBaseAddressOfPlane(gen, pl));
		const size_t stp = CVPixelBufferGetBytesPerRowOfPlane(prev, pl), stc = CVPixelBufferGetBytesPerRowOfPlane(cur, pl),
		             stg = CVPixelBufferGetBytesPerRowOfPlane(gen, pl);
		if (!sp || !sc || !g || !yp0 || !yc0)
			continue;
		const int sub = pl ? 2 : 1, bpp = pl ? 2 : 1;
		const int pw_ = W / sub, ph_ = H / sub;
		auto doPixel = [&](int x, int y, int fx, int fy) {
			const float wl = bandW(fx, bwL), wr = bandW(W - 1 - fx, bwR), wt = bandW(fy, bwT), wb = bandW(H - 1 - fy, bwB);
			const float w = std::max(std::max(wl, wr), std::max(wt, wb));
			if (w <= 0.f || isStatic(fx, fy))
				return;
			const int px_ = fx - hx, py_ = fy - hy;
			const bool use_cur = px_ < 0 || px_ >= W || py_ < 0 || py_ >= H;
			const int sx = std::clamp(use_cur ? fx + hx : px_, 0, W - 1) / sub, sy = std::clamp(use_cur ? fy + hy : py_, 0, H - 1) / sub;
			const uint8_t *src = (use_cur ? sc + size_t(sy) * stc : sp + size_t(sy) * stp) + size_t(sx) * bpp;
			uint8_t *o = g + size_t(y) * stg + size_t(x) * bpp;
			for (int c = 0; c < bpp; c++)
				o[c] = uint8_t(o[c] + (int(src[c]) - int(o[c])) * w + 0.5f);
			touched++;
		};
		for (int y = 0; y < ph_; y++) {
			const int fy = y * sub;
			const bool rowBand = (bwT > 0 && fy < bwT) || (bwB > 0 && H - 1 - fy < bwB);
			if (rowBand) {
				for (int x = 0; x < pw_; x++)
					doPixel(x, y, x * sub, fy);
				continue;
			}
			for (int x = 0; x < std::min(pw_, (bwL + sub - 1) / sub); x++)
				doPixel(x, y, x * sub, fy);
			for (int x = std::max(0, pw_ - (bwR + sub - 1) / sub); x < pw_; x++)
				doPixel(x, y, x * sub, fy);
		}
	}
	CVPixelBufferUnlockBaseAddress(gen, 0);
	CVPixelBufferUnlockBaseAddress(cur, kCVPixelBufferLock_ReadOnly);
	CVPixelBufferUnlockBaseAddress(prev, kCVPixelBufferLock_ReadOnly);
	return touched;
}

int PsWrapVtInterp::localRepair(CVPixelBufferRef prev, CVPixelBufferRef cur, CVPixelBufferRef gen)
{
	if (CVPixelBufferLockBaseAddress(prev, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess)
		return 0;
	if (CVPixelBufferLockBaseAddress(cur, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) {
		CVPixelBufferUnlockBaseAddress(prev, kCVPixelBufferLock_ReadOnly);
		return 0;
	}
	if (CVPixelBufferLockBaseAddress(gen, 0) != kCVReturnSuccess) {
		CVPixelBufferUnlockBaseAddress(prev, kCVPixelBufferLock_ReadOnly);
		CVPixelBufferUnlockBaseAddress(cur, kCVPixelBufferLock_ReadOnly);
		return 0;
	}
	auto plane = [](CVPixelBufferRef b, size_t i) {
		return Plane{static_cast<const uint8_t *>(CVPixelBufferGetBaseAddressOfPlane(b, i)), CVPixelBufferGetBytesPerRowOfPlane(b, i),
		             int(CVPixelBufferGetWidthOfPlane(b, i)), int(CVPixelBufferGetHeightOfPlane(b, i))};
	};
	const Plane yp = plane(prev, 0), yc = plane(cur, 0), yg = plane(gen, 0);
	const Plane up = plane(prev, 1), uc = plane(cur, 1);
	uint8_t *gy = static_cast<uint8_t *>(CVPixelBufferGetBaseAddressOfPlane(gen, 0));
	uint8_t *guv = static_cast<uint8_t *>(CVPixelBufferGetBaseAddressOfPlane(gen, 1));
	const size_t gys = CVPixelBufferGetBytesPerRowOfPlane(gen, 0), guvs = CVPixelBufferGetBytesPerRowOfPlane(gen, 1);
	int repaired = 0;
	const int W = yg.w, H = yg.h;
	if (yp.p && yc.p && yg.p && up.p && uc.p && guv && W == yp.w && H == yp.h && W >= 640 && H >= 360) {
		const int B = W >= 1600 ? 32 : 16;
		const int bw = W / B, bh = H / B;
		const int n = (B / 2 - 1) * (B / 2 - 1);
		std::vector<float> wgt(size_t(bw) * bh, 0.f);
		std::vector<int16_t> vx(size_t(bw) * bh, 0), vy(size_t(bw) * bh, 0);   // v/2 in full-res px
		static thread_local std::vector<uint8_t> qp, qc;
		bool have_q = false;
		const int qw = W / 4, qh = H / 4, QB = B / 4, RX = 12, RY = 6;
		for (int by = 1; by < bh - 1; by++)
			for (int bx = 1; bx < bw - 1; bx++) {
				const int x0 = bx * B, y0 = by * B;
				const int ep = gradEnergy(yp, x0, y0, B), ec = gradEnergy(yc, x0, y0, B);
				const int lo = std::min(ep, ec), hi = std::max(ep, ec);
				if (lo < 8 * n || hi * 10 > lo * 16)   // flat, or detail changes between the real frames (occlusion)
					continue;
				const int eg = gradEnergy(yg, x0, y0, B);
				if (eg * 100 >= lo * 62)   // detail kept (≥ 62%)
					continue;
				if (!have_q) {
					quarter(yp, qp, qw, qh);
					quarter(yc, qc, qw, qh);
					have_q = true;
				}
				const int qx = x0 / 4, qy = y0 / 4;
				int best = 1 << 30, bdx = 0, bdy = 0;
				for (int dy = -RY; dy <= RY; dy++)
					for (int dx = -RX; dx <= RX; dx++) {
						const int ya = qy - dy, xa = qx - dx, yb = qy + dy, xb = qx + dx;
						if (std::min({ya, yb, xa, xb}) < 0 || std::max(ya, yb) + QB > qh || std::max(xa, xb) + QB > qw)
							continue;
						int sad = 0;
						for (int j = 0; j < QB && sad < best; j++) {
							const uint8_t *ra = &qp[size_t(ya + j) * qw + xa], *rb = &qc[size_t(yb + j) * qw + xb];
							for (int i = 0; i < QB; i++)
								sad += std::abs(int(ra[i]) - int(rb[i]));
						}
						if (sad < best) { best = sad; bdx = dx; bdy = dy; }
					}
				if (best > 5 * QB * QB)   // no trustworthy midpoint → leave VT's block
					continue;
				const size_t k = size_t(by) * bw + bx;
				wgt[k] = 1.f;
				vx[k] = int16_t(bdx * 4);
				vy[k] = int16_t(bdy * 4);
				repaired++;
			}
		if (repaired) {
			// feather: one-block ring at half weight using the neighbour's vector; per-pixel bilinear weight
			std::vector<float> w2 = wgt;
			for (int by = 0; by < bh; by++)
				for (int bx = 0; bx < bw; bx++) {
					const size_t k = size_t(by) * bw + bx;
					if (wgt[k] > 0.f)
						continue;
					for (int j = -1; j <= 1 && w2[k] == 0.f; j++)
						for (int i = -1; i <= 1; i++) {
							const int nx = bx + i, ny = by + j;
							if (nx < 0 || ny < 0 || nx >= bw || ny >= bh)
								continue;
							const size_t kk = size_t(ny) * bw + nx;
							if (wgt[kk] >= 1.f) {
								w2[k] = 0.5f;
								vx[k] = vx[kk];
								vy[k] = vy[kk];
								break;
							}
						}
				}
			for (int by = 0; by < bh; by++)
				for (int bx = 0; bx < bw; bx++) {
					const size_t k = size_t(by) * bw + bx;
					if (w2[k] <= 0.f)
						continue;
					const int dx = vx[k], dy = vy[k];
					for (int yy = by * B; yy < (by + 1) * B; yy++) {
						const float fy = (yy + 0.5f) / B - 0.5f;
						const int y0b = clampi(int(std::floor(fy)), 0, bh - 1), y1b = clampi(y0b + 1, 0, bh - 1);
						const float ty = std::clamp(fy - std::floor(fy), 0.f, 1.f);
						const int ya = clampi(yy - dy, 0, H - 1), yb = clampi(yy + dy, 0, H - 1);
						for (int xx = bx * B; xx < (bx + 1) * B; xx++) {
							const float fx = (xx + 0.5f) / B - 0.5f;
							const int x0b = clampi(int(std::floor(fx)), 0, bw - 1), x1b = clampi(x0b + 1, 0, bw - 1);
							const float tx = std::clamp(fx - std::floor(fx), 0.f, 1.f);
							const float w = (w2[size_t(y0b) * bw + x0b] * (1 - tx) + w2[size_t(y0b) * bw + x1b] * tx) * (1 - ty) +
							                (w2[size_t(y1b) * bw + x0b] * (1 - tx) + w2[size_t(y1b) * bw + x1b] * tx) * ty;
							if (w <= 0.01f)
								continue;
							const int xa = clampi(xx - dx, 0, W - 1), xb = clampi(xx + dx, 0, W - 1);
							const int mc = (int(yp.p[size_t(ya) * yp.st + xa]) + int(yc.p[size_t(yb) * yc.st + xb]) + 1) >> 1;
							uint8_t &o = gy[size_t(yy) * gys + xx];
							o = uint8_t(o + (mc - int(o)) * w + 0.5f);
							if (!(yy & 1) && !(xx & 1)) {
								const int cy = yy / 2, cx = xx / 2;
								const int cya = clampi(ya / 2, 0, up.h - 1), cxa = clampi(xa / 2, 0, up.w - 1);
								const int cyb = clampi(yb / 2, 0, uc.h - 1), cxb = clampi(xb / 2, 0, uc.w - 1);
								for (int c = 0; c < 2; c++) {
									const int mcc = (int(up.p[size_t(cya) * up.st + cxa * 2 + c]) + int(uc.p[size_t(cyb) * uc.st + cxb * 2 + c]) + 1) >> 1;
									uint8_t &oc = guv[size_t(cy) * guvs + cx * 2 + c];
									oc = uint8_t(oc + (mcc - int(oc)) * w + 0.5f);
								}
							}
						}
					}
				}
		}
	}
	CVPixelBufferUnlockBaseAddress(gen, 0);
	CVPixelBufferUnlockBaseAddress(cur, kCVPixelBufferLock_ReadOnly);
	CVPixelBufferUnlockBaseAddress(prev, kCVPixelBufferLock_ReadOnly);
	return repaired;
}

CVPixelBufferRef PsWrapVtInterp::interpolate(CVPixelBufferRef prev, CVPixelBufferRef cur, float phase)
{
	if (!prev || !cur)
		return nullptr;
	const int w = int(CVPixelBufferGetWidth(cur)), h = int(CVPixelBufferGetHeight(cur));
	if (int(CVPixelBufferGetWidth(prev)) != w || int(CVPixelBufferGetHeight(prev)) != h)
		return nullptr;
	if (!ensure(w, h))
		return nullptr;
	const double t0 = nowMs();
	// convert (or reuse the conversion of last call's cur as this call's prev)
	CVPixelBufferRef pw = nullptr, cw = nullptr;
	if (prev == d->cur_key && d->cur_work)
		pw = CVPixelBufferRetain(d->cur_work);
	else
		pw = toWork(prev);
	cw = toWork(cur);
	if (!pw || !cw) {
		if (pw) CVPixelBufferRelease(pw);
		if (cw) CVPixelBufferRelease(cw);
		return nullptr;
	}
	if (d->cur_work)
		CVPixelBufferRelease(d->cur_work);
	d->cur_work = CVPixelBufferRetain(cw);
	d->cur_key = cur;

	// Fast camera pans: the engine handles up to roughly 100 px of motion per frame pair at 1080p; beyond that the
	// background melts and halos / ghosts appear around the camera-followed character (measured: fgtools/harness, eval/pan).
	// Pre-shifting by part of the global motion only moves the ghosting onto the character, so: hold instead
	// (no generated frame for that pair — a brief 30 fps step during a very fast flick, but never a smeared frame).
	too_fast = false;
	last_gx = last_gy = 0;
	last_unmatched = 0.0;
	last_used_rife = engine == 1;
	if (hold_fast_pans) {
		// engine 0 (VT): hold beyond its range · engine 2 (hybrid): such pairs go to RIFE instead, hold only beyond RIFE's
		static const double max_unmatched = getenv("PSWRAP_VT_UNMATCHED") ? atof(getenv("PSWRAP_VT_UNMATCHED")) : kHoldUnmatched;
		static const double max_unmatched_rife = getenv("PSWRAP_RIFE_UNMATCHED") ? atof(getenv("PSWRAP_RIFE_UNMATCHED")) : kHoldUnmatchedRife;
		static const double hold_px = qEnvHoldPx();
		const bool can_rife = (engine == 1 || engine == 2) && d->rife;
		last_unmatched = unmatchedFraction(pw, cw, &last_unmatched_region);
		int gx = 0, gy = 0;
		double gm = 0.0;
		if (globalMotion(pw, cw, &gx, &gy)) {
			last_gx = gx;
			last_gy = gy;
			gm = std::hypot(double(gx), double(gy)) * 1080.0 / double(work_h);   // in 1080p pixels
		}
		last_gm = gm;
		// a single screen region that is mostly untrackable (a near-camera object sweeping out of view, a camera
		// snap around an obstacle) melts locally even when the global fraction is moderate
		static const double max_region = getenv("PSWRAP_VT_REGION") ? atof(getenv("PSWRAP_VT_REGION")) : kHoldRegion;
		// medium pans (≥ 56 px per pair) with a moderate untrackable share already ghost the player in real GTA footage
		// (soak 2026-11-21: shop-front orbit at 64–80 px, u ≈ 0.21–0.25) → hold those too
		const bool medium_pan_risky = gm >= 56.0 && (last_unmatched > 0.18 || last_unmatched_region > 0.40);
		const bool beyond_vt = last_unmatched > max_unmatched || gm > hold_px || last_unmatched_region > max_region || medium_pan_risky;
		const bool beyond_rife = last_unmatched > max_unmatched_rife || gm > kHoldMotionRife;
		if (engine == 2 && can_rife && beyond_vt)
			last_used_rife = true;
		if (can_rife ? (last_used_rife ? beyond_rife : beyond_vt) : beyond_vt) {
			too_fast = true;
			CVPixelBufferRelease(pw);
			CVPixelBufferRelease(cw);
			return nullptr;
		}
	}

	CVPixelBufferRef dst = nullptr;
	if (CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, d->dst_pool, &dst) != kCVReturnSuccess) {
		CVPixelBufferRelease(pw);
		CVPixelBufferRelease(cw);
		return nullptr;
	}
	bool ok = false;
	if (last_used_rife && d->rife) {
		// NV12 → BGRA (GPU, VTPixelTransferSession) · RIFE · BGRA → NV12
		auto toBgra = [&](CVPixelBufferRef src) -> CVPixelBufferRef {
			CVPixelBufferRef b = nullptr;
			if (CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, d->bgra_pool, &b) != kCVReturnSuccess)
				return nullptr;
			if (CFTypeRef m = CVBufferCopyAttachment(src, kCVImageBufferYCbCrMatrixKey, nullptr))
				CFRelease(m);
			else
				tagBt709(src);
			if (VTPixelTransferSessionTransferImage(d->xfer, src, b) != noErr) {
				CVPixelBufferRelease(b);
				return nullptr;
			}
			return b;
		};
		CVPixelBufferRef bp = nullptr;
		if (prev == d->cur_bgra_key && d->cur_bgra)
			bp = CVPixelBufferRetain(d->cur_bgra);
		else
			bp = toBgra(pw);
		CVPixelBufferRef bc = toBgra(cw);
		CVPixelBufferRef bo = nullptr;
		if (bp && bc && CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, d->bgra_pool, &bo) == kCVReturnSuccess) {
			ok = rifeApi().interp(d->rife, bp, bc, bo, phase) == 0;
			if (ok) {
				tagBt709(bo);
				CVBufferSetAttachment(dst, kCVImageBufferYCbCrMatrixKey, kCVImageBufferYCbCrMatrix_ITU_R_709_2, kCVAttachmentMode_ShouldPropagate);
				ok = VTPixelTransferSessionTransferImage(d->xfer, bo, dst) == noErr;
			}
		}
		if (d->cur_bgra)
			CVPixelBufferRelease(d->cur_bgra);
		d->cur_bgra = bc ? CVPixelBufferRetain(bc) : nullptr;
		d->cur_bgra_key = cur;
		if (bp) CVPixelBufferRelease(bp);
		if (bc) CVPixelBufferRelease(bc);
		if (bo) CVPixelBufferRelease(bo);
	} else
	@autoreleasepool {
		Class fc = vtClass("VTFrameProcessorFrame");
		Class prc = vtClass("VTLowLatencyFrameInterpolationParameters");
		const CMTime tp = CMTimeMake(pts, 600), tc = CMTimeMake(pts + 20, 600), td = CMTimeMake(pts + 10, 600);
		pts += 20;
		id fp = [[(id)fc alloc] initWithBuffer:pw presentationTimeStamp:tp];
		id fcur = [[(id)fc alloc] initWithBuffer:cw presentationTimeStamp:tc];
		id fd = [[(id)fc alloc] initWithBuffer:dst presentationTimeStamp:td];
		id par = nil;
		if (fp && fcur && fd)
			par = [[(id)prc alloc] initWithSourceFrame:fcur previousFrame:fp interpolationPhase:@[@(phase)] destinationFrames:@[fd]];
		NSError *err = nil;
		ok = par && [d->proc processWithParameters:par error:&err];
		if (!ok) {
			static int logged = 0;
			if (logged++ < 5)
				logLine("process failed:", err);
		}
		[par release];
		[fp release];
		[fcur release];
		[fd release];
	}
	last_edge_ms = 0.0;
	static const bool edge_env = !getenv("PSWRAP_VT_EDGEFILL") || atoi(getenv("PSWRAP_VT_EDGEFILL")) != 0;
	if (ok && edge_env && (last_gx || last_gy)) {
		const double e0 = nowMs();
		edgeFill(pw, cw, dst, last_gx, last_gy);
		last_edge_ms = nowMs() - e0;
	}
	last_repaired = 0;
	last_repair_ms = 0.0;
	// off by default: measured neutral-to-slightly-negative on ground truth and no visible gain on real GTA (see report)
	static const bool repair_env = getenv("PSWRAP_VT_REPAIR") && atoi(getenv("PSWRAP_VT_REPAIR")) != 0;
	if (ok && local_repair && repair_env) {
		const double r0 = nowMs();
		last_repaired = localRepair(pw, cw, dst);
		last_repair_ms = nowMs() - r0;
	}
	CVPixelBufferRelease(pw);
	CVPixelBufferRelease(cw);
	if (!ok) {
		CVPixelBufferRelease(dst);
		return nullptr;
	}
	if (d->out_pool) {
		CVPixelBufferRef up = nullptr;
		if (CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, d->out_pool, &up) != kCVReturnSuccess ||
		    VTPixelTransferSessionTransferImage(d->xfer, dst, up) != noErr) {
			if (up) CVPixelBufferRelease(up);
			CVPixelBufferRelease(dst);
			return nullptr;
		}
		CVPixelBufferRelease(dst);
		dst = up;
	}
	// carry colour attachments so the renderer interprets the generated frame like the source
	CVBufferPropagateAttachments(cur, dst);
	last_ms = nowMs() - t0;
	return dst;
}

bool PsWrapVtInterp::looksLikeCut(CVPixelBufferRef prev, CVPixelBufferRef cur)
{
	if (!prev || !cur || CVPixelBufferGetPlaneCount(prev) < 1 || CVPixelBufferGetPlaneCount(cur) < 1)
		return false;
	if (CVPixelBufferLockBaseAddress(prev, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess)
		return false;
	if (CVPixelBufferLockBaseAddress(cur, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) {
		CVPixelBufferUnlockBaseAddress(prev, kCVPixelBufferLock_ReadOnly);
		return false;
	}
	const uint8_t *a = static_cast<const uint8_t *>(CVPixelBufferGetBaseAddressOfPlane(prev, 0));
	const uint8_t *b = static_cast<const uint8_t *>(CVPixelBufferGetBaseAddressOfPlane(cur, 0));
	const size_t sa = CVPixelBufferGetBytesPerRowOfPlane(prev, 0), sb = CVPixelBufferGetBytesPerRowOfPlane(cur, 0);
	const int w = int(std::min(CVPixelBufferGetWidthOfPlane(prev, 0), CVPixelBufferGetWidthOfPlane(cur, 0)));
	const int h = int(std::min(CVPixelBufferGetHeightOfPlane(prev, 0), CVPixelBufferGetHeightOfPlane(cur, 0)));
	bool cut = false;
	if (a && b && w > 16 && h > 16) {
		int ha[32] = {}, hb[32] = {};
		double sad = 0.0;
		int n = 0;
		for (int y = 4; y < h; y += 8)
			for (int x = 4; x < w; x += 8) {
				const int va = a[y * sa + x], vb = b[y * sb + x];
				sad += std::abs(va - vb);
				ha[va >> 3]++;
				hb[vb >> 3]++;
				n++;
			}
		const double mad = sad / std::max(1, n);
		int hd = 0;
		for (int i = 0; i < 32; i++)
			hd += std::abs(ha[i] - hb[i]);
		const double hist = double(hd) / (2.0 * std::max(1, n));   // 0 = same distribution, 1 = disjoint
		// a cut changes the luma distribution AND spikes the frame difference vs. the recent motion level
		const double base = mad_ema < 0.0 ? mad : mad_ema;
		cut = (hist > 0.30 && mad > 20.0 && mad > 2.5 * base) || (hist > 0.55 && mad > 30.0);
		if (!cut)
			mad_ema = mad_ema < 0.0 ? mad : 0.8 * mad_ema + 0.2 * mad;
	}
	CVPixelBufferUnlockBaseAddress(prev, kCVPixelBufferLock_ReadOnly);
	CVPixelBufferUnlockBaseAddress(cur, kCVPixelBufferLock_ReadOnly);
	return cut;
}
