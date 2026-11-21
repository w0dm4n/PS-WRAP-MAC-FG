// PS-WRAP: Frame generation — สร้างเฟรมกลางระหว่างเฟรมจริง 2 เฟรม (สตรีม 30/60 → จอ 60/120) ด้วย motion estimation บน GPU
//
// ใช้กับ render thread เท่านั้น (ทุกเมธอดเรียกจาก QmlMainWindow::render)
//   push()        วาดเฟรมจริงใหม่ลง texture ขนาดจอ (crop/border/upscaler เหมือนวาดจอปกติ) — เฟรมก่อนหน้าเลื่อนเป็น "prev"
//   interpolate() หา motion prev → cur แล้วสร้างเฟรมกลางที่ t (0..1)
//   present()     วาด cur หรือเฟรมกลางลงจอเต็มกรอบ พร้อม overlay ของจอ (QML / จุด REC)
//
// PS-WRAP-MAC-FG rewrite (occlusion-aware):
//   luma pyramid 1/4 … 1/32 (box-filtered = also suppresses H.265 block noise)
//   two motion fields, forward (on prev's grid) and backward (on cur's grid), coarse-to-fine block matching:
//     1/32 full search ±8 texels (≈ ±256 screen px) → refine 1/16, 1/8, 1/4 (neighbour candidates + ±1/±0.5 steps,
//     smoothness term) + 3x3 vector median
//   decision pass at 1/4: candidate vectors (fixed-point lookups of both fields + neighbours + zero) are checked against
//     forward/backward consistency → both frames visible = blend · visible only in prev / only in cur (occlusion /
//     disocclusion around a moving character) = take that frame only — never blend both, which is what makes halos
//   full-res pass picks per pixel among the 4 nearest decisions (pixel-accurate edges) and samples
//   scene cut (luma histogram + difference) → hold the previous frame instead of a ghost blend
#pragma once

#include <libplacebo/dispatch.h>
#include <libplacebo/gpu.h>
#include <libplacebo/renderer.h>

class PsWrapFrameGen
{
public:
	PsWrapFrameGen(pl_gpu gpu, pl_log log);
	~PsWrapFrameGen();
	PsWrapFrameGen(const PsWrapFrameGen &) = delete;
	PsWrapFrameGen &operator=(const PsWrapFrameGen &) = delete;

	// src = เฟรมวิดีโอ · target = เฟรมจอ (ใช้ขนาด/สี/crop) · params = render params ของจอ
	bool push(pl_renderer renderer, const pl_frame &src, const pl_frame &target, const pl_render_params &params);
	bool hasPair() const;                 // มี prev + cur ขนาดเดียวกัน → interpolate ได้
	bool interpolate(float t);
	// mid=false → เฟรมจริงล่าสุด · target ต้องมี overlay ของจอแล้ว (crop จะถูกตั้งเป็นเต็มจอ)
	bool present(pl_renderer renderer, bool mid, const pl_frame &target);
	void reset();                         // ลืมเฟรมเก่า (เช่น สตรีมเริ่มใหม่ / ขนาดจอเปลี่ยน)

private:
	static constexpr int kLevels = 4;     // 1/4, 1/8, 1/16, 1/32
	bool ensure(int w, int h);
	void destroyTextures();
	pl_tex makeTex(int w, int h, pl_fmt fmt);
	bool lumaPyramid(int idx);
	bool motion();
	bool run(pl_tex target, const char *name, const char *header, const char *body,
	         const struct pl_shader_desc *descs, int num_descs,
	         const struct pl_shader_var *vars, int num_vars);

	pl_gpu gpu;
	pl_log log;
	pl_dispatch dp = nullptr;
	pl_fmt fmt_rgba = nullptr;   // เฟรมขนาดจอ (16 bit ถ้ามี — HDR ไม่เสียความละเอียด)
	pl_fmt fmt_luma = nullptr;
	pl_fmt fmt_mv = nullptr;     // vector หน่วยพิกเซลจอ
	pl_fmt fmt_va = nullptr;     // vertex attribute vec2

	int width = 0, height = 0;
	int l0_factor = 4;                    // screen px per level-0 luma texel
	pl_tex frame[2] = {};                 // เฟรมจริงที่วาดแล้ว
	pl_tex luma[2][kLevels] = {};         // [frame][level] level 0 = 1/l0_factor (~480 px wide)
	bool valid[2] = {};
	int cur = 0;                          // index ของเฟรมล่าสุด · prev = 1 - cur
	pl_tex mvf[kLevels] = {}, mvb[kLevels] = {};   // forward (prev grid) / backward (cur grid) per level
	pl_tex mvtmp = nullptr;               // median scratch at 1/4
	pl_tex decision = nullptr;            // 1/4: xy = vector, z = weight of prev, w = cost
	pl_tex cutinfo = nullptr;             // 1x1: x = 1 when prev → cur is a scene cut
	pl_tex mid = nullptr;
	bool motion_ready = false;   // motion ของคู่ปัจจุบันคำนวณแล้ว
	bool failed = false;         // shader ใช้ไม่ได้บน GPU นี้ — เลิกพยายาม
};
