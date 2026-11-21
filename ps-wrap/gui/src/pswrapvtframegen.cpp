// PS-WRAP-MAC: Frame Gen via Apple VideoToolbox frame interpolation (see pswrapvtframegen.h)
#include "pswrapvtframegen.h"
#include "pswrapvtinterp.h"

#include <QLoggingCategory>
#include <pthread.h>
#include <unistd.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>

extern "C" {
#include <libavutil/dict.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>
}

#include <CoreVideo/CoreVideo.h>

#include <algorithm>
#include <cmath>
#include <cstring>

Q_DECLARE_LOGGING_CATEGORY(chiakiGui)

namespace {

CVPixelBufferRef pixelBuffer(const AVFrame *f)
{
	if (!f || f->format != AV_PIX_FMT_VIDEOTOOLBOX || !f->data[3])
		return nullptr;
	return reinterpret_cast<CVPixelBufferRef>(f->data[3]);
}

void releasePixelBuffer(void *, uint8_t *data)
{
	CVPixelBufferRelease(reinterpret_cast<CVPixelBufferRef>(data));
}

// wrap a +1 CVPixelBuffer as a VideoToolbox AVFrame shaped like `like` (takes the reference)
AVFrame *wrapPixelBuffer(CVPixelBufferRef pb, const AVFrame *like)
{
	AVFrame *m = av_frame_alloc();
	if (!m) {
		CVPixelBufferRelease(pb);
		return nullptr;
	}
	av_frame_copy_props(m, like);
	m->format = AV_PIX_FMT_VIDEOTOOLBOX;
	m->width = like->width;
	m->height = like->height;
	m->buf[0] = av_buffer_create(reinterpret_cast<uint8_t *>(pb), 1, releasePixelBuffer, nullptr, AV_BUFFER_FLAG_READONLY);
	if (!m->buf[0]) {
		CVPixelBufferRelease(pb);
		av_frame_free(&m);
		return nullptr;
	}
	m->data[3] = reinterpret_cast<uint8_t *>(pb);
	if (like->hw_frames_ctx)
		m->hw_frames_ctx = av_buffer_ref(like->hw_frames_ctx);
	return m;
}

} // namespace

PsWrapVtFrameGen::PsWrapVtFrameGen(Prepare prepare, Present present)
	: prepare(std::move(prepare)), present(std::move(present))
{
	pacer = std::thread([this]() { pacerLoop(); });
}

PsWrapVtFrameGen::~PsWrapVtFrameGen()
{
	{
		std::lock_guard<std::mutex> lk(m);
		quit = true;
	}
	cv.notify_all();
	if (pacer.joinable())
		pacer.join();
	if (warm_thread.joinable())
		warm_thread.join();
	if (!vt && warm_result)
		delete warm_result;
	reset();
}

// presents queued frames at their slot (own thread, user-interactive QoS → sub-ms wakeups)
void PsWrapVtFrameGen::pacerLoop()
{
	pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
	std::unique_lock<std::mutex> lk(m);
	while (!quit) {
		if (outq.empty()) {
			cv.wait(lk);
			continue;
		}
		const int64_t wait_us = outq.front().at - pswrapNowUs();
		if (wait_us > 2500) {   // coarse: condvar (can overshoot by ~1-2 ms)
			cv.wait_for(lk, std::chrono::microseconds(wait_us - 2000));
			continue;
		}
		if (wait_us > 100) {    // fine: short sleeps without the lock (new frames can still be queued meanwhile)
			lk.unlock();
			usleep(useconds_t(std::min<int64_t>(wait_us - 50, 500)));
			lk.lock();
			continue;
		}
		ChiakiFfmpegFrame f = outq.front().f;
		const int64_t slot = outq.front().at;
		outq.pop_front();
		lk.unlock();
		static const bool pace_log = getenv("PSWRAP_VT_PACE_LOG") != nullptr;
		if (pace_log)
			fprintf(stderr, "pace slot=%lld late=%lld\n", (long long)slot, (long long)(pswrapNowUs() - slot));
		present(f);
		lk.lock();
	}
}

bool PsWrapVtFrameGen::osSupported()
{
	static const int supported = PsWrapVtInterp::isAvailable() ? 1 : 0;
	return supported != 0;
}

static bool warmOne(PsWrapVtInterp *v, int w, int h)
{
	// first interpolation compiles/loads the model (1–4 s) — do it here, on two blank frames
	CVPixelBufferRef a = nullptr, b = nullptr;
	const void *keys[] = {kCVPixelBufferIOSurfacePropertiesKey};
	CFDictionaryRef empty = CFDictionaryCreate(nullptr, nullptr, nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	const void *vals[] = {empty};
	CFDictionaryRef attrs = CFDictionaryCreate(nullptr, keys, vals, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	CVPixelBufferCreate(nullptr, size_t(w), size_t(h), kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, attrs, &a);
	CVPixelBufferCreate(nullptr, size_t(w), size_t(h), kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, attrs, &b);
	CFRelease(attrs);
	CFRelease(empty);
	bool ok = false;
	if (a && b) {
		for (CVPixelBufferRef pb : {a, b}) {
			CVPixelBufferLockBaseAddress(pb, 0);
			for (size_t p = 0; p < CVPixelBufferGetPlaneCount(pb); p++)
				memset(CVPixelBufferGetBaseAddressOfPlane(pb, p), p ? 128 : 16,
				       CVPixelBufferGetBytesPerRowOfPlane(pb, p) * CVPixelBufferGetHeightOfPlane(pb, p));
			CVPixelBufferUnlockBaseAddress(pb, 0);
		}
		if (CVPixelBufferRef m = v->interpolate(a, b, 0.5f)) {
			CVPixelBufferRelease(m);
			ok = true;
		}
	}
	if (a) CVPixelBufferRelease(a);
	if (b) CVPixelBufferRelease(b);
	return ok;
}

void PsWrapVtFrameGen::startWarmup(int w, int h)
{
	if (warm_thread.joinable())
		warm_thread.join();
	if (warm_result && !vt)
		delete warm_result;
	warm_result = nullptr;
	warm_w = w;
	warm_h = h;
	warm_state.store(1);
	const int eng = engine_mode;
	warm_thread = std::thread([this, w, h, eng]() {
		// The engine only accepts some frame sizes (measured on macOS 27.2: 1920x1080, 1536x864, 1280x720, 1280x800, 1024x768 ok;
		// 1872x1106, 1600x900, 960x540, 1920x1088 fail with "Processor is not initialized") → try native-fit, then 1080, then 720
		PsWrapVtInterp *v = nullptr;
		bool ok = false;
		for (int work_h : {0, 1080, 720}) {
			v = new PsWrapVtInterp();
			v->setEngine(eng);
			v->setWorkingHeight(work_h);
			ok = warmOne(v, w, h);
			if (ok)
				break;
			delete v;
			v = nullptr;
		}
		if (ok) {
			warm_result = v;
			warm_state.store(2);
		} else {
			delete v;
			warm_state.store(3);
		}
	});
}

void PsWrapVtFrameGen::enqueue(int64_t at_us, ChiakiFfmpegFrame f)
{
	if (!prepare(f))   // hw → sw transfer etc. done here, so the pacer only hands over
		return;
	{
		std::lock_guard<std::mutex> lk(m);
		outq.push_back({at_us, f});
	}
	cv.notify_all();
}

void PsWrapVtFrameGen::flush()
{
	std::deque<Out> q;
	{
		std::lock_guard<std::mutex> lk(m);
		q.swap(outq);
	}
	for (Out &o : q)
		present(o.f);
	if (prev)
		av_frame_free(&prev);
	last_slot_us = 0;
	exp_arrival_us = 0;
	active_.store(false);
}

void PsWrapVtFrameGen::reset()
{
	{
		std::lock_guard<std::mutex> lk(m);
		for (Out &o : outq)
			av_frame_free(&o.f.frame);
		outq.clear();
	}
	if (prev)
		av_frame_free(&prev);
	last_slot_us = 0;
	exp_arrival_us = 0;
	active_.store(false);
}

bool PsWrapVtFrameGen::handle(ChiakiFfmpegFrame &frame)
{
	const int64_t arrival = pswrapNowUs();
	AVFrame *f = frame.frame;
	CVPixelBufferRef cur = pixelBuffer(f);
	if (!cur || CVPixelBufferGetPixelFormatType(cur) != kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange || frame.recovered) {
		flush();
		return false;
	}
	const int w = int(CVPixelBufferGetWidth(cur)), h = int(CVPixelBufferGetHeight(cur));
	if (engine_changed && warm_state.load() != 1) {   // engine switched: rebuild the interpolator
		engine_changed = false;
		flush();
		vt.reset();
		warm_state.store(0);
		warm_w = warm_h = 0;
	}

	// session warm-up (background) — pass frames through until the engine is ready
	if (!vt || vt->workWidth() == 0 || w != warm_w || h != warm_h) {
		const int st = warm_state.load();
		if ((st == 0 || w != warm_w || h != warm_h) && st != 1) {
			vt.reset();
			startWarmup(w, h);
			qCInfo(chiakiGui) << "PSWRAP framegen (Apple VT): warming up interpolation engine for" << w << "x" << h;
		} else if (st == 2 && !vt) {
			if (warm_thread.joinable())
				warm_thread.join();
			vt.reset(warm_result);
			warm_result = nullptr;
			qCInfo(chiakiGui) << "PSWRAP framegen (Apple VT): ready, working size" << vt->workWidth() << "x" << vt->workHeight();
		}
		if (!vt) {
			if (warm_state.load() == 3) {
				static bool logged = false;
				if (!logged) {
					logged = true;
					qCWarning(chiakiGui) << "PSWRAP framegen (Apple VT): engine unavailable for" << w << "x" << h << "— using shader Frame Gen";
				}
			}
			flush();
			prev = av_frame_clone(f);
			prev_pts = frame.pts;
			return false;
		}
	}

	ChiakiFfmpegFrame mid = {};
	bool cut = false, scene_cut = false;
	double cut_ms = 0.0;
	CVPixelBufferRef p = pixelBuffer(prev);
	if (p && int(CVPixelBufferGetWidth(p)) == w && int(CVPixelBufferGetHeight(p)) == h) {
		const double d = frame.pts - prev_pts;
		if (d > 0.004 && d < 0.1)
			interval_s = interval_s * 0.8 + d * 0.2;
		const int64_t c0 = pswrapNowUs();
		cut = scene_cut = vt->looksLikeCut(p, cur);
		cut_ms = (pswrapNowUs() - c0) / 1000.0;
		if (!cut) {
			if (CVPixelBufferRef m = vt->interpolate(p, cur, 0.5f)) {
				last_ms_.store(vt->lastMs(), std::memory_order_relaxed);
				if (vt->lastUsedRife())
					rife_pairs++;
				consecutive_failures = 0;
				static const bool mark = getenv("PSWRAP_FG_MARK") != nullptr;   // test: tag generated frames (white 16x16, top-left)
				if (mark && CVPixelBufferLockBaseAddress(m, 0) == kCVReturnSuccess) {
					uint8_t *y = static_cast<uint8_t *>(CVPixelBufferGetBaseAddressOfPlane(m, 0));
					const size_t st = CVPixelBufferGetBytesPerRowOfPlane(m, 0);
					const int uval = 16 + int(std::min(1.0, vt->lastUnmatched()) * 200.0);   // 2nd block encodes the untrackable fraction
					for (int r = 0; r < 16 && y; r++) {   // [white][black][u] — unambiguous even over a bright sky
						memset(y + size_t(r) * st, 235, 16);
						memset(y + size_t(r) * st + 16, 16, 16);
						memset(y + size_t(r) * st + 32, uval, 16);
					}
					CVPixelBufferUnlockBaseAddress(m, 0);
				}
				mid.frame = wrapPixelBuffer(m, f);
				if (mid.frame) {
					av_dict_set(&mid.frame->metadata, "pswrap_generated", "1", 0);   // presenter: never drop a real frame for it
					mid.frame->pts = (prev->pts != AV_NOPTS_VALUE && f->pts != AV_NOPTS_VALUE) ? (prev->pts + f->pts) / 2 : f->pts;
					mid.frame->best_effort_timestamp = mid.frame->pts;
					mid.pts = 0.5 * (prev_pts + frame.pts);
					mid.duration = interval_s * 0.5;
					mid.recovered = false;
				}
			} else if (vt->lastTooFast()) {
				cut = true;   // pan beyond the engine's range: hold this slot rather than show a smeared frame
				fast_holds++;
			} else if (++consecutive_failures == 30) {
				qCWarning(chiakiGui) << "PSWRAP framegen (Apple VT): interpolation keeps failing — disabling";
				warm_state.store(3);
				vt.reset();
			}
		}
	}
	av_frame_free(&prev);
	prev = av_frame_clone(f);
	prev_pts = frame.pts;

	if (!mid.frame && !(cut && active_.load())) {
		flush();
		prev = av_frame_clone(f);
		prev_pts = frame.pts;
		return false;   // first frame / failure → show the real frame now
	}
	static const bool vt_debug = getenv("PSWRAP_VT_DEBUG") != nullptr;
	if (vt_debug && vt)
		qCInfo(chiakiGui).nospace() << "[vtdbg] g=" << vt->lastGlobalMotion() << " unmatched=" << vt->lastUnmatched()
			<< " hold=" << (vt->lastTooFast() ? 1 : 0) << " rife=" << (vt->lastUsedRife() ? 1 : 0) << " ms=" << vt->lastMs() << " rep=" << vt->lastRepairedBlocks() << " rms=" << vt->lastRepairMs() << " cut=" << (scene_cut ? 1 : 0) << " mid=" << (mid.frame ? 1 : 0);
	if (++stat_frames % 300 == 0) {
		qCInfo(chiakiGui).nospace() << "PSWRAP framegen (Apple VT): last 300 frames: fast-pan holds " << fast_holds << ", RIFE pairs " << rife_pairs
			<< ", interp " << last_ms_.load() << " ms, L " << (proc_est_us + jitter_est_us) / 1000.0 << " ms, g " << (vt ? vt->lastGlobalMotion() : 0.0);
		fast_holds = 0;
		rife_pairs = 0;
	}
	if (scene_cut)
		qCInfo(chiakiGui) << "PSWRAP framegen (Apple VT): scene cut — holding";
	active_.store(true);

	// ---- pacing: an even grid of half source intervals on the stream clock ----
	// (Snapping to the display's vsync grid was tried: with ProMotion's variable refresh the CVDisplayLink period/phase
	// wander and made the spacing worse. An even 2x cadence lands one frame per refresh at 60 Hz and two at 120 Hz.)
	const double half_us = interval_s * 0.5e6;
	double P = half_us;
	// Fixed refresh exactly 2x the stream (60 Hz display, 30 fps stream): snap to the display's vsync grid, mid-period
	// (measured: the free-running cadence beats against vsync → ~1 held slot per second at 60 Hz). Not at 120 Hz
	// ProMotion, where the free-running cadence was better.
	double vP = 0.0;
	int64_t vV = 0;
	// off by default: measured worse on real GTA at 60 Hz (77% vs 87% within ±2 ms, 421 vs 31 dropped generated frames)
	static const int vsync_mode = getenv("PSWRAP_VT_VSYNC") ? atoi(getenv("PSWRAP_VT_VSYNC")) : 0;
	const bool snap = vsync_mode != 0 && pswrapDisplayVsync(&vP, &vV) &&
		(vsync_mode == 1 || std::fabs(vP - half_us) < 0.06 * half_us);
	if (snap)
		P = vP;
	auto onGrid = [&](int64_t t) -> int64_t {   // first grid point (vsync + P/2) at or after t
		if (!snap)
			return t;
		const double k = std::ceil((double(t) - double(vV) - 0.5 * vP) / vP);
		return vV + int64_t(0.5 * vP + k * vP);
	};
	// fixed-latency schedule: generated frame at (expected arrival + L), real frame half an interval later.
	// expected arrival = PLL on the stream clock (absorbs network jitter); L = processing (VT + prepare) + jitter, adaptive
	const double interval_us = interval_s * 1e6;
	if (exp_arrival_us == 0 || std::llabs(arrival - exp_arrival_us) > int64_t(interval_us)) {
		exp_arrival_us = arrival;   // (re)sync
	} else {
		const double e = double(arrival - exp_arrival_us);
		exp_arrival_us += int64_t(0.05 * e);
		// late-arrival envelope: fast attack, slow release (≈ a high percentile, not the all-time max)
		const double late = std::max(0.0, e);
		jitter_est_us = late > jitter_est_us ? 0.5 * (jitter_est_us + late) : 0.97 * jitter_est_us + 0.03 * late;
	}
	const int64_t base = exp_arrival_us;
	exp_arrival_us += int64_t(interval_us);
	const double proc_us = double(pswrapNowUs() - arrival) + 2500.0;   // + hw→sw transfer of both frames (prepare)
	proc_est_us = proc_us > proc_est_us ? 0.5 * (proc_est_us + proc_us) : 0.97 * proc_est_us + 0.03 * proc_us;
	const double L = std::min(interval_us, proc_est_us + jitter_est_us + 1500.0);
	const int64_t earliest = pswrapNowUs() + 2500;   // after prepare (hw→sw transfer)
	const int64_t planned = base + int64_t(L);
	if (getenv("PSWRAP_VT_PACE_LOG"))
		fprintf(stderr, "plan arrival_late=%lld proc=%.1f (interp %.1f, cut %.1f) L=%.1f planned-arrival=%.1f ms\n", (long long)(arrival - base) / 1000,
		        proc_us / 1000, vt ? vt->lastMs() : -1.0, cut_ms, L / 1000, double(planned - arrival) / 1000);
	// keep a strict cadence (one frame every half interval) while it stays within [frame ready, plan + 0.35 slot];
	// re-anchor only when it breaks — small latency-estimate changes never shift the cadence
	int64_t s_mid;
	const int64_t cadence = last_slot_us ? last_slot_us + int64_t(P) : 0;
	if (!cadence) {
		s_mid = onGrid(std::max(earliest, planned));
	} else if (cadence < earliest) {
		s_mid = onGrid(earliest);   // frame came late — the previous frame stays one slot longer
	} else if (cadence > planned + int64_t(0.35 * P) && mid.frame) {
		av_frame_free(&mid.frame);   // latency crept up (clock drift) — drop one generated frame to catch up
		s_mid = cadence;
	} else {
		s_mid = cadence;
	}
	if (snap)   // keep the cadence locked to the (slowly drifting) vsync phase
		s_mid = onGrid(s_mid - int64_t(0.5 * vP));
	const int64_t s_real = (mid.frame || cut) ? s_mid + int64_t(P) : s_mid;
	frame.duration = interval_s * 0.5;
	if (mid.frame)
		enqueue(s_mid, mid);
	enqueue(s_real, frame);
	frame.frame = nullptr;
	last_slot_us = s_real;
	return true;
}
