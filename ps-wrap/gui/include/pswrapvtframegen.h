// PS-WRAP-MAC: Frame Gen using Apple's VideoToolbox low-latency frame interpolation (macOS 26+).
//
// Runs on the decoder/frame thread, before the hardware frame is handed to the renderer:
//   real frame N arrives (VideoToolbox CVPixelBuffer) → generate mid(N-1, N) → prepare both (hw→sw) on the frame thread
//   → a dedicated pacer thread hands mid, then N, to the renderer on the display refresh grid (CVDisplayLink),
//     half a source interval apart → the renderer just shows an evenly paced 2x-rate stream.
// Scene cuts (luma histogram + difference spike) → no mid frame, N-1 stays on screen for that slot (no ghost blend).
// The VT session needs a slow first-time warm-up (~1–4 s): it is started on a background thread; frames pass through until ready.
// Only for 8-bit SDR streams (engine accepts '420v' only) — HDR / other formats fall back to the shader Frame Gen.
#pragma once

#include <chiaki/ffmpegdecoder.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <deque>
#include <functional>
#include <memory>
#include <thread>

class PsWrapVtInterp;

class PsWrapVtFrameGen
{
public:
	using Prepare = std::function<bool(ChiakiFfmpegFrame &)>;   // frame thread: make it presentable (false = drop, frame freed)
	using Present = std::function<void(ChiakiFfmpegFrame)>;     // any thread: hand to the renderer

	PsWrapVtFrameGen(Prepare prepare, Present present);
	~PsWrapVtFrameGen();

	static bool osSupported();

	// Takes ownership of frame.frame. Delivers (now or later) the real frame and possibly a generated frame before it.
	// Returns false (frame untouched, caller prepares + presents it now) if this frame can't use the VT path.
	bool handle(ChiakiFfmpegFrame &frame);
	// Deliver whatever is held back right now (e.g. Frame Gen switched off) and forget the previous frame.
	void flush();
	void reset();

	// interpolator engine (PsWrapVtInterp::setEngine): 0 VT · 1 RIFE · 2 hybrid. A change restarts the warm-up.
	void setEngine(int e) { if (e != engine_mode) { engine_mode = e; engine_changed = true; } }

	bool active() const { return active_.load(std::memory_order_relaxed); }   // generating frames right now
	double lastInterpMs() const { return last_ms_.load(std::memory_order_relaxed); }

private:
	void startWarmup(int w, int h);
	void enqueue(int64_t at_us, ChiakiFfmpegFrame f);
	void pacerLoop();

	Prepare prepare;
	Present present;
	std::unique_ptr<PsWrapVtInterp> vt;          // owned by the frame thread once ready
	std::thread warm_thread;
	std::atomic<int> warm_state{0};              // 0 idle · 1 warming · 2 ready · 3 failed
	PsWrapVtInterp *warm_result = nullptr;
	int warm_w = 0, warm_h = 0;

	AVFrame *prev = nullptr;                     // last real frame (hw ref) — "previous" input of the next interpolation
	double prev_pts = 0.0;
	// output pacing: every delivery is put on the display refresh grid (CVDisplayLink), one frame per refresh slot,
	// generated / real alternating every half source interval; slack beyond half an interval drops a generated frame
	struct Out { int64_t at; ChiakiFfmpegFrame f; };
	std::deque<Out> outq;                        // guarded by m
	std::mutex m;
	std::condition_variable cv;
	bool quit = false;
	std::thread pacer;
	int64_t last_slot_us = 0;
	int64_t exp_arrival_us = 0;                  // PLL: expected arrival time of the next source frame
	double jitter_est_us = 2000.0;               // late-arrival envelope
	double proc_est_us = 14000.0;                // VT + prepare envelope
	double interval_s = 1.0 / 30.0;              // source frame interval (from pts)
	std::atomic<bool> active_{false};
	std::atomic<double> last_ms_{0.0};
	int consecutive_failures = 0;
	int fast_holds = 0, stat_frames = 0, rife_pairs = 0;
	int engine_mode = 0;
	bool engine_changed = false;
};
