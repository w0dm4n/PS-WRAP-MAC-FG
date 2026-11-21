// PS-WRAP: Frame generation (ดู pswrapframegen.h)
#include <pswrapframegen.h>

#include <libplacebo/shaders/custom.h>

#include <QLoggingCategory>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <map>
#include <string>

Q_DECLARE_LOGGING_CATEGORY(chiakiGui)

namespace {

constexpr auto kTexCaps = static_cast<pl_fmt_caps>(PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_LINEAR);

pl_shader_desc sampled(const char *name, pl_tex tex, bool linear)
{
	pl_shader_desc d = {};
	d.desc.name = name;
	d.desc.type = PL_DESC_SAMPLED_TEX;
	d.binding.object = tex;
	d.binding.address_mode = PL_TEX_ADDRESS_CLAMP;
	d.binding.sample_mode = linear ? PL_TEX_SAMPLE_LINEAR : PL_TEX_SAMPLE_NEAREST;
	return d;
}

pl_shader_var vec2Var(const char *name, const float *v)
{
	pl_shader_var s = {};
	s.var = pl_var_vec2(name);
	s.data = v;
	return s;
}

pl_shader_var floatVar(const char *name, const float *v)
{
	pl_shader_var s = {};
	s.var = pl_var_float(name);
	s.data = v;
	return s;
}

// ---- shaders ---- (fg_pos = ตำแหน่งพิกเซลของ target ที่จุดกึ่งกลาง เช่น (0.5, 0.5))
// vectors are stored in screen (target) pixels: v = displacement prev → cur

// เฟรมสี → luma level 0 (factor fac ≥ 4 so level 0 stays ~480 px wide at any screen size): box filter with
// ntap x ntap bilinear taps (each averages 2x2) — also smooths H.265 blocking before matching
const char *kLuma4Body = R"(
	vec2 px = 1.0 / src_size;
	vec3 w = vec3(0.2126, 0.7152, 0.0722);
	vec2 c = fg_pos * fac;
	int n = int(ntap);
	float l = 0.0;
	for (int y = 0; y < n; y++)
	for (int x = 0; x < n; x++) {
		vec2 o = 2.0 * (vec2(float(x), float(y)) - 0.5 * float(n - 1));
		l += dot(texture(tex_src, (c + o) * px).rgb, w);
	}
	color = vec4(l / float(n * n), 0.0, 0.0, 1.0);
)";

// luma ย่อครึ่ง: bilinear ที่กลาง 2x2
const char *kHalfBody = R"(
	color = vec4(texture(tex_src, fg_pos / out_size).r, 0.0, 0.0, 1.0);
)";

// 1/32: full search ±8 texels · 3x3 patch · tex_a = this frame's grid, tex_b = the other frame; sgn flips backward → prev→cur
const char *kCoarseHeader = R"(
float fg_cost(vec2 q, vec2 d) {
	float c = 0.0;
	for (int y = -1; y <= 1; y++)
	for (int x = -1; x <= 1; x++) {
		vec2 o = vec2(float(x), float(y));
		c += abs(texture(tex_a, (q + o) / lsize).r - texture(tex_b, (q + o + d) / lsize).r);
	}
	return c;
}
)";
const char *kCoarseBody = R"(
	vec2 q = fg_pos;
	float best = fg_cost(q, vec2(0.0)) - 0.004;
	vec2 bd = vec2(0.0);
	for (int y = -8; y <= 8; y++)
	for (int x = -8; x <= 8; x++) {
		vec2 d = vec2(float(x), float(y));
		float c = fg_cost(q, d) + 0.002 * length(d);
		if (c < best) { best = c; bd = d; }
	}
	vec2 base = bd;
	for (int y = -1; y <= 1; y++)
	for (int x = -1; x <= 1; x++) {
		vec2 d = base + vec2(float(x), float(y));
		float c = fg_cost(q, d) + 0.002 * length(d);
		if (c < best) { best = c; bd = d; }
	}
	color = vec4(sgn * bd * scale, best, 1.0);
)";

// refine (1/16, 1/8, 1/4): candidates = coarser vectors of the 3x3 neighbourhood, then ±1 / ±0.5 texel steps · 5x5 patch
// + smoothness toward the coarser (median-filtered) prediction
const char *kRefineHeader = R"(
float fg_cost(vec2 q, vec2 d) {
	float c = 0.0;
	int r = int(prad);
	for (int y = -r; y <= r; y++)
	for (int x = -r; x <= r; x++) {
		vec2 o = vec2(float(x), float(y));
		c += abs(texture(tex_a, (q + o) / lsize).r - texture(tex_b, (q + o + d) / lsize).r);
	}
	return c;
}
)";
const char *kRefineBody = R"(
	vec2 q = fg_pos;
	vec2 cuv = q / lsize;
	vec2 mpx = 1.0 / mvsize;
	vec2 pred = sgn * texture(tex_mv, cuv).xy / scale;
	float best = 1e9;
	vec2 bd = vec2(0.0);
	for (int y = -1; y <= 1; y++)
	for (int x = -1; x <= 1; x++) {
		vec2 d = sgn * texture(tex_mv, cuv + mpx * vec2(float(x), float(y))).xy / scale;
		float c = fg_cost(q, d) + lam * length(d - pred);
		if (c < best) { best = c; bd = d; }
	}
	{
		float c = fg_cost(q, vec2(0.0)) + lam * length(pred) - 0.004;
		if (c < best) { best = c; bd = vec2(0.0); }
	}
	for (int s = 0; s < 2; s++) {
		float st = s == 0 ? 1.0 : 0.5;
		vec2 base = bd;
		for (int y = -1; y <= 1; y++)
		for (int x = -1; x <= 1; x++) {
			if (x == 0 && y == 0)
				continue;
			vec2 d = base + st * vec2(float(x), float(y));
			float c = fg_cost(q, d) + lam * length(d - pred);
			if (c < best) { best = c; bd = d; }
		}
	}
	color = vec4(sgn * bd * scale, best, 1.0);
)";

// median ของ vector 3x3 (ตัวที่ห่างตัวอื่นรวมน้อยสุด) — ตัด vector หลุดเดี่ยวๆ
const char *kMedianBody = R"(
	vec2 uv = fg_pos / mvsize;
	vec2 px = 1.0 / mvsize;
	vec2 c[9];
	int k = 0;
	for (int y = -1; y <= 1; y++)
	for (int x = -1; x <= 1; x++)
		c[k++] = texture(tex_mv, uv + px * vec2(float(x), float(y))).xy;
	float bestd = 1e20;
	vec2 bv = c[4];
	for (int i = 0; i < 9; i++) {
		float d = 0.0;
		for (int j = 0; j < 9; j++)
			d += length(c[i] - c[j]);
		if (d < bestd) { bestd = d; bv = c[i]; }
	}
	color = vec4(bv, texture(tex_mv, uv).z, 1.0);
)";

// scene cut: motion-compensated error stays high everywhere (a pan / occlusion doesn't) — average of the coarse forward
// matching cost on a 16x9 grid, plus the raw luma difference (1x1 output, one invocation, ~300 fetches)
const char *kCutBody = R"(
	float cost = 0.0;
	float mad = 0.0;
	for (int y = 0; y < 9; y++)
	for (int x = 0; x < 16; x++) {
		vec2 uv = (vec2(float(x), float(y)) + 0.5) / vec2(16.0, 9.0);
		cost += texture(tex_mv, uv).z / 9.0;
		mad += abs(texture(tex_a, uv).r - texture(tex_b, uv).r);
	}
	cost /= 144.0;
	mad /= 144.0;
	float cut = (cost > 0.075 && mad > 0.08) ? 1.0 : 0.0;
	color = vec4(cut, cost, mad, 1.0);
)";

// decision at 1/4: pick a vector + which frame(s) to sample, using forward/backward consistency for occlusion
const char *kDecideHeader = R"(
vec2 fg_ff(vec2 x) { return texture(tex_mf, x / s4 / lsize).xy; }
vec2 fg_fb(vec2 x) { return texture(tex_mb, x / s4 / lsize).xy; }
float fg_err(vec2 qa, vec2 qb) {
	float e = 0.0;
	for (int y = -1; y <= 1; y++)
	for (int x = -1; x <= 1; x++) {
		vec2 o = vec2(float(x), float(y));
		e += abs(texture(tex_la, (qa / s4 + o) / lsize).r - texture(tex_lb, (qb / s4 + o) / lsize).r);
	}
	return e / 9.0;
}
vec4 fg_best;
void fg_consider(vec2 pd, vec2 v, float bias) {
	vec2 qa = pd - t_mix * v;
	vec2 qb = pd + (1.0 - t_mix) * v;
	float tau = 1.5 * max(s4.x, s4.y) + 0.12 * length(v);
	bool oka = length(fg_ff(qa) - v) < tau;
	bool okb = length(fg_fb(qb) - v) < tau;
	float e = fg_err(qa, qb);
	float cost;
	float wa;
	if (oka && okb) { cost = e; wa = 0.5; }
	else if (oka) { cost = c_occ + 0.25 * e; wa = 1.0; }
	else if (okb) { cost = c_occ + 0.25 * e; wa = 0.0; }
	else { cost = c_bad + e; wa = 0.5; }
	cost += bias;
	if (cost < fg_best.w)
		fg_best = vec4(v, wa, cost);
}
)";
const char *kDecideBody = R"(
	vec2 pd = fg_pos * s4;   // screen px
	fg_best = vec4(0.0, 0.0, 0.5, 1e9);
	fg_consider(pd, vec2(0.0), -0.003);   // static HUD / still areas
	vec2 v = fg_ff(pd);
	v = fg_ff(pd - t_mix * v);
	v = fg_ff(pd - t_mix * v);
	fg_consider(pd, v, 0.0);
	v = fg_fb(pd);
	v = fg_fb(pd + (1.0 - t_mix) * v);
	v = fg_fb(pd + (1.0 - t_mix) * v);
	fg_consider(pd, v, 0.0);
	for (int r = 0; r < 2; r++) {
		vec2 o = s4 * (r == 0 ? 4.0 : 12.0);
		fg_consider(pd, fg_ff(pd + vec2(o.x, 0.0)), 0.002);
		fg_consider(pd, fg_ff(pd - vec2(o.x, 0.0)), 0.002);
		fg_consider(pd, fg_ff(pd + vec2(0.0, o.y)), 0.002);
		fg_consider(pd, fg_ff(pd - vec2(0.0, o.y)), 0.002);
		fg_consider(pd, fg_fb(pd + vec2(o.x, 0.0)), 0.002);
		fg_consider(pd, fg_fb(pd - vec2(o.x, 0.0)), 0.002);
		fg_consider(pd, fg_fb(pd + vec2(0.0, o.y)), 0.002);
		fg_consider(pd, fg_fb(pd - vec2(0.0, o.y)), 0.002);
	}
	color = fg_best;
)";

// full-res synthesis: per pixel, choose among the 4 nearest decisions (pixel-accurate edges) and sample
const char *kSynthHeader = R"(
vec3 fg_col;
float fg_eval(vec2 uv, vec2 px, vec4 dec) {
	vec2 oa = -t_mix * dec.xy * px;
	vec2 ob = (1.0 - t_mix) * dec.xy * px;
	vec3 a = texture(tex_a, uv + oa).rgb;
	vec3 b = texture(tex_b, uv + ob).rgb;
	float wa = dec.z;
	float cost = dec.w;
	if (wa > 0.25 && wa < 0.75) {
		vec3 lw = vec3(0.2126, 0.7152, 0.0722);
		vec2 dx = vec2(2.0 * px.x, 0.0);
		vec2 dy = vec2(0.0, 2.0 * px.y);
		float e = abs(dot(a - b, lw));
		e += abs(dot(texture(tex_a, uv + oa + dx).rgb - texture(tex_b, uv + ob + dx).rgb, lw));
		e += abs(dot(texture(tex_a, uv + oa - dx).rgb - texture(tex_b, uv + ob - dx).rgb, lw));
		e += abs(dot(texture(tex_a, uv + oa + dy).rgb - texture(tex_b, uv + ob + dy).rgb, lw));
		e += abs(dot(texture(tex_a, uv + oa - dy).rgb - texture(tex_b, uv + ob - dy).rgb, lw));
		cost = 0.5 * cost + 0.5 * (e / 5.0);
		wa = 1.0 - t_mix;
	}
	fg_col = mix(b, a, wa);
	return cost;
}
void fg_quick(vec2 uv, vec2 px, vec4 dec) {
	vec3 a = texture(tex_a, uv - t_mix * dec.xy * px).rgb;
	vec3 b = texture(tex_b, uv + (1.0 - t_mix) * dec.xy * px).rgb;
	float wa = (dec.z > 0.25 && dec.z < 0.75) ? 1.0 - t_mix : dec.z;
	fg_col = mix(b, a, wa);
}
)";
const char *kSynthBody = R"(
	vec2 px = 1.0 / full_size;
	vec2 uv = fg_pos * px;
	if (texture(tex_cut, vec2(0.5)).x > 0.5) {
		color = vec4(texture(tex_a, uv).rgb, 1.0);   // scene cut: hold the previous frame
	} else {
		vec2 dp = uv * dsize - 0.5;
		vec2 base = floor(dp);
		vec2 f = dp - base;
		float bestc = 1e9;
		vec3 bestcol = vec3(0.0);
		vec4 d0 = texture(tex_d, (base + 0.5) / dsize);
		vec4 d1 = texture(tex_d, (base + vec2(1.0, 0.0) + 0.5) / dsize);
		vec4 d2 = texture(tex_d, (base + vec2(0.0, 1.0) + 0.5) / dsize);
		vec4 d3 = texture(tex_d, (base + vec2(1.0, 1.0) + 0.5) / dsize);
		vec3 dd = abs(d1.xyz - d0.xyz) + abs(d2.xyz - d0.xyz) + abs(d3.xyz - d0.xyz);
		bool same = max(max(dd.x, dd.y), dd.z) < 0.3;
		for (int k = 0; k < (same ? 1 : 4); k++) {
			vec2 o = vec2(float(k & 1), float(k >> 1));
			vec4 dec = k == 0 ? d0 : (k == 1 ? d1 : (k == 2 ? d2 : d3));
			float c = same ? 0.0 : fg_eval(uv, px, dec);
			if (same) { fg_quick(uv, px, dec); }
			// slight preference for the closest decision texel
			c += 0.004 * length(o - f);
			if (c < bestc) { bestc = c; bestcol = fg_col; }
		}
		color = vec4(bestcol, 1.0);
	}
)";

} // namespace

PsWrapFrameGen::PsWrapFrameGen(pl_gpu gpu, pl_log log)
	: gpu(gpu), log(log)
{
	dp = pl_dispatch_create(log, gpu);
	// BLITTABLE: pl_render_image clears the part of the target outside the video crop (letterbox) with pl_tex_clear,
	// which needs blit_dst — without it the clear fails ("Validation failed: dst->params.blit_dst") and the bars show garbage/magenta
	constexpr auto kFrameCaps = static_cast<pl_fmt_caps>(kTexCaps | PL_FMT_CAP_BLITTABLE);
	fmt_rgba = pl_find_fmt(gpu, PL_FMT_UNORM, 4, 16, 0, kFrameCaps);
	if (!fmt_rgba)
		fmt_rgba = pl_find_fmt(gpu, PL_FMT_UNORM, 4, 8, 0, kFrameCaps);
	fmt_luma = pl_find_fmt(gpu, PL_FMT_FLOAT, 1, 16, 0, kTexCaps);
	if (!fmt_luma)
		fmt_luma = pl_find_fmt(gpu, PL_FMT_UNORM, 1, 16, 0, kTexCaps);
	fmt_mv = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 16, 0, kTexCaps);
	fmt_va = pl_find_vertex_fmt(gpu, PL_FMT_FLOAT, 2);
	if (!dp || !fmt_rgba || !fmt_luma || !fmt_mv || !fmt_va) {
		qCWarning(chiakiGui) << "PSWRAP framegen: GPU lacks required formats — disabled";
		failed = true;
	}
}

PsWrapFrameGen::~PsWrapFrameGen()
{
	destroyTextures();
	if (dp)
		pl_dispatch_destroy(&dp);
}

void PsWrapFrameGen::destroyTextures()
{
	for (int i = 0; i < 2; i++) {
		if (frame[i])
			pl_tex_destroy(gpu, &frame[i]);
		for (int l = 0; l < kLevels; l++)
			if (luma[i][l])
				pl_tex_destroy(gpu, &luma[i][l]);
	}
	for (int l = 0; l < kLevels; l++) {
		if (mvf[l])
			pl_tex_destroy(gpu, &mvf[l]);
		if (mvb[l])
			pl_tex_destroy(gpu, &mvb[l]);
	}
	pl_tex *all[] = {&mvtmp, &decision, &cutinfo, &mid};
	for (pl_tex *t : all)
		if (*t)
			pl_tex_destroy(gpu, t);
	width = height = 0;
	reset();
}

void PsWrapFrameGen::reset()
{
	valid[0] = valid[1] = false;
	motion_ready = false;
}

pl_tex PsWrapFrameGen::makeTex(int w, int h, pl_fmt fmt)
{
	pl_tex_params p = {};
	p.w = std::max(1, w);
	p.h = std::max(1, h);
	p.format = fmt;
	p.renderable = true;
	p.sampleable = true;
	p.blit_dst = (fmt->caps & PL_FMT_CAP_BLITTABLE) != 0;   // needed for pl_tex_clear (letterbox clear in pl_render_image)
	p.debug_tag = PL_DEBUG_TAG;
	pl_tex t = pl_tex_create(gpu, &p);
	if (t && p.blit_dst) {
		const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
		pl_tex_clear(gpu, t, black);   // never show uninitialised memory
	}
	return t;
}

bool PsWrapFrameGen::ensure(int w, int h)
{
	if (failed || w <= 0 || h <= 0)
		return false;
	if (w == width && h == height && frame[0])
		return true;
	destroyTextures();
	int lw[kLevels], lh[kLevels];
	// level 0 ≈ 480 px wide whatever the screen (1080p screen: 1/4 · 3456 px Retina: 1/7) → cost independent of display size
	l0_factor = std::max(4, int(std::lround(w / 480.0)));
	lw[0] = (w + l0_factor - 1) / l0_factor;
	lh[0] = (h + l0_factor - 1) / l0_factor;
	for (int l = 1; l < kLevels; l++) {
		lw[l] = (lw[l - 1] + 1) / 2;
		lh[l] = (lh[l - 1] + 1) / 2;
	}
	bool ok = true;
	for (int i = 0; i < 2; i++) {
		ok = ok && (frame[i] = makeTex(w, h, fmt_rgba));
		for (int l = 0; l < kLevels; l++)
			ok = ok && (luma[i][l] = makeTex(lw[l], lh[l], fmt_luma));
	}
	for (int l = 0; l < kLevels; l++) {
		ok = ok && (mvf[l] = makeTex(lw[l], lh[l], fmt_mv));
		ok = ok && (mvb[l] = makeTex(lw[l], lh[l], fmt_mv));
	}
	ok = ok && (mvtmp = makeTex(lw[0], lh[0], fmt_mv));
	ok = ok && (decision = makeTex(lw[0], lh[0], fmt_mv));
	ok = ok && (cutinfo = makeTex(1, 1, fmt_mv));
	ok = ok && (mid = makeTex(w, h, fmt_rgba));
	if (!ok) {
		qCWarning(chiakiGui) << "PSWRAP framegen: texture allocation failed at" << w << "x" << h;
		destroyTextures();
		return false;
	}
	width = w;
	height = h;
	return true;
}

bool PsWrapFrameGen::run(pl_tex target, const char *name, const char *header, const char *body,
                         const pl_shader_desc *descs, int num_descs, const pl_shader_var *vars, int num_vars)
{
	pl_shader sh = pl_dispatch_begin(dp);
	if (!sh)
		return false;
	const float w = float(target->params.w), h = float(target->params.h);
	const float c00[2] = {0.0f, 0.0f}, c10[2] = {w, 0.0f}, c01[2] = {0.0f, h}, c11[2] = {w, h};
	pl_shader_va va = {};
	va.attr.name = "fg_pos";
	va.attr.fmt = fmt_va;
	va.data[0] = c00;
	va.data[1] = c10;
	va.data[2] = c01;
	va.data[3] = c11;
	pl_custom_shader cs = {};
	cs.description = name;
	cs.header = header;
	cs.body = body;
	cs.input = PL_SHADER_SIG_NONE;
	cs.output = PL_SHADER_SIG_COLOR;
	cs.descriptors = descs;
	cs.num_descriptors = num_descs;
	cs.variables = vars;
	cs.num_variables = num_vars;
	cs.vertex_attribs = &va;
	cs.num_vertex_attribs = 1;
	cs.output_w = target->params.w;
	cs.output_h = target->params.h;
	if (!pl_shader_custom(sh, &cs)) {
		pl_dispatch_abort(dp, &sh);
		qCWarning(chiakiGui) << "PSWRAP framegen: shader setup failed:" << name;
		return false;
	}
	pl_dispatch_params dpp = {};
	dpp.shader = &sh;
	dpp.target = target;
	// PSWRAP_FG_PROFILE=1: per-pass GPU time via pl_timer (offline harness / debugging only)
	static const bool profile = qEnvironmentVariableIntValue("PSWRAP_FG_PROFILE") != 0;
	static std::map<std::string, std::pair<pl_timer, std::pair<double, int>>> acc;
	std::string key = std::string(name) + " " + std::to_string(target->params.w) + "x" + std::to_string(target->params.h);
	if (profile) {
		auto &a = acc[key];
		if (!a.first)
			a.first = pl_timer_create(gpu);
		dpp.timer = a.first;
	}
	if (!pl_dispatch_finish(dp, &dpp)) {
		qCWarning(chiakiGui) << "PSWRAP framegen: dispatch failed:" << name;
		return false;
	}
	if (profile) {
		pl_gpu_finish(gpu);
		auto &a = acc[key];
		uint64_t ns;
		while ((ns = pl_timer_query(gpu, a.first)) != 0) {
			a.second.first += ns / 1e6;
			if (++a.second.second % 100 == 0)
				fprintf(stderr, "PSWRAP fg profile %-40s %.3f ms\n", key.c_str(), a.second.first / a.second.second);
		}
	}
	return true;
}

bool PsWrapFrameGen::lumaPyramid(int idx)
{
	{
		const float src_size[2] = {float(width), float(height)};
		const float fac = float(l0_factor), ntap = float((l0_factor + 1) / 2);
		const pl_shader_desc d[] = {sampled("tex_src", frame[idx], true)};
		const pl_shader_var v[] = {vec2Var("src_size", src_size), floatVar("fac", &fac), floatVar("ntap", &ntap)};
		if (!run(luma[idx][0], "pswrap fg luma0", nullptr, kLuma4Body, d, 1, v, 3))
			return false;
	}
	for (int l = 1; l < kLevels; l++) {
		const float out_size[2] = {float(luma[idx][l]->params.w), float(luma[idx][l]->params.h)};
		const pl_shader_desc d[] = {sampled("tex_src", luma[idx][l - 1], true)};
		const pl_shader_var v[] = {vec2Var("out_size", out_size)};
		if (!run(luma[idx][l], "pswrap fg luma half", nullptr, kHalfBody, d, 1, v, 1))
			return false;
	}
	return true;
}

bool PsWrapFrameGen::push(pl_renderer renderer, const pl_frame &src, const pl_frame &target, const pl_render_params &params)
{
	const pl_tex tt = target.num_planes > 0 ? target.planes[0].texture : nullptr;
	if (!renderer || !tt || !ensure(tt->params.w, tt->params.h))
		return false;
	pl_dispatch_reset_frame(dp);
	cur = 1 - cur;
	valid[cur] = false;
	motion_ready = false;

	pl_frame t = target;
	t.num_planes = 1;
	t.planes[0] = {};
	t.planes[0].texture = frame[cur];
	t.planes[0].components = 4;
	t.planes[0].component_mapping[0] = PL_CHANNEL_R;
	t.planes[0].component_mapping[1] = PL_CHANNEL_G;
	t.planes[0].component_mapping[2] = PL_CHANNEL_B;
	t.planes[0].component_mapping[3] = PL_CHANNEL_A;
	t.repr = pl_color_repr_rgb;
	t.repr.alpha = PL_ALPHA_INDEPENDENT;
	t.repr.bits.sample_depth = fmt_rgba->component_depth[0];
	t.repr.bits.color_depth = fmt_rgba->component_depth[0];
	t.overlays = nullptr;
	t.num_overlays = 0;
	pl_render_params p = params;
	p.background_transparency = 0.0f;
	if (!pl_render_image(renderer, &src, &t, &p))
		return false;
	if (!lumaPyramid(cur)) {
		failed = true;
		return false;
	}
	valid[cur] = true;
	return true;
}

bool PsWrapFrameGen::hasPair() const
{
	return !failed && valid[0] && valid[1];
}

bool PsWrapFrameGen::motion()
{
	const int prev = 1 - cur;
	const int top = kLevels - 1;
	for (int dir = 0; dir < 2; dir++) {
		// forward: grid = prev, search in cur · backward: grid = cur, search in prev (stored as prev → cur)
		const int ga = dir == 0 ? prev : cur, gb = dir == 0 ? cur : prev;
		pl_tex *mv = dir == 0 ? mvf : mvb;
		const float sgn = dir == 0 ? 1.0f : -1.0f;
		{
			const float ls[2] = {float(luma[0][top]->params.w), float(luma[0][top]->params.h)};
			const float sc[2] = {float(width) / ls[0], float(height) / ls[1]};
			const pl_shader_desc d[] = {sampled("tex_a", luma[ga][top], true), sampled("tex_b", luma[gb][top], true)};
			const pl_shader_var v[] = {vec2Var("lsize", ls), vec2Var("scale", sc), floatVar("sgn", &sgn)};
			if (!run(mv[top], "pswrap fg motion coarse", kCoarseHeader, kCoarseBody, d, 2, v, 3))
				return false;
		}
		for (int l = top - 1; l >= 0; l--) {
			const float ls[2] = {float(luma[0][l]->params.w), float(luma[0][l]->params.h)};
			const float sc[2] = {float(width) / ls[0], float(height) / ls[1]};
			const float ms[2] = {float(mv[l + 1]->params.w), float(mv[l + 1]->params.h)};
			const float lam = l == 0 ? 0.008f : 0.02f;   // cost scales with the patch (3x3 at level 0, 5x5 above)
			const float prad = l == 0 ? 1.0f : 2.0f;
			const pl_shader_desc d[] = {sampled("tex_a", luma[ga][l], true), sampled("tex_b", luma[gb][l], true),
			                            sampled("tex_mv", mv[l + 1], false)};
			const pl_shader_var v[] = {vec2Var("lsize", ls), vec2Var("scale", sc), vec2Var("mvsize", ms),
			                           floatVar("sgn", &sgn), floatVar("lam", &lam), floatVar("prad", &prad)};
			if (!run(mv[l], "pswrap fg motion refine", kRefineHeader, kRefineBody, d, 3, v, 6))
				return false;
			if (l == 0) {   // 3x3 vector median at 1/4 (outliers on repetitive texture / compression noise)
				const pl_shader_desc dm[] = {sampled("tex_mv", mv[0], false)};
				const pl_shader_var vm[] = {vec2Var("mvsize", ls)};
				if (!run(mvtmp, "pswrap fg motion median", nullptr, kMedianBody, dm, 1, vm, 1))
					return false;
				std::swap(mv[0], mvtmp);
			}
		}
	}
	{
		const float ls[2] = {float(luma[0][top]->params.w), float(luma[0][top]->params.h)};
		const pl_shader_desc d[] = {sampled("tex_a", luma[prev][top], true), sampled("tex_b", luma[cur][top], true),
		                            sampled("tex_mv", mvf[top], false)};
		if (!run(cutinfo, "pswrap fg cut", nullptr, kCutBody, d, 3, nullptr, 0))
			return false;
	}
	return true;
}

bool PsWrapFrameGen::interpolate(float t)
{
	if (!hasPair())
		return false;
	const int prev = 1 - cur;
	if (!motion_ready) {
		if (!motion()) {
			failed = true;
			return false;
		}
		motion_ready = true;
	}
	const float tm = std::clamp(t, 0.0f, 1.0f);
	{
		const float ls[2] = {float(decision->params.w), float(decision->params.h)};
		const float s4[2] = {float(width) / ls[0], float(height) / ls[1]};
		const float c_occ = 0.05f, c_bad = 0.12f;
		const pl_shader_desc d[] = {sampled("tex_la", luma[prev][0], true), sampled("tex_lb", luma[cur][0], true),
		                            sampled("tex_mf", mvf[0], false), sampled("tex_mb", mvb[0], false)};
		const pl_shader_var v[] = {vec2Var("lsize", ls), vec2Var("s4", s4), floatVar("t_mix", &tm),
		                           floatVar("c_occ", &c_occ), floatVar("c_bad", &c_bad)};
		if (!run(decision, "pswrap fg decide", kDecideHeader, kDecideBody, d, 4, v, 5)) {
			failed = true;
			return false;
		}
	}
	const float full[2] = {float(width), float(height)};
	const float ds[2] = {float(decision->params.w), float(decision->params.h)};
	const pl_shader_desc d[] = {sampled("tex_a", frame[prev], true), sampled("tex_b", frame[cur], true),
	                            sampled("tex_d", decision, false), sampled("tex_cut", cutinfo, false)};
	const pl_shader_var v[] = {vec2Var("full_size", full), floatVar("t_mix", &tm), vec2Var("dsize", ds)};
	if (!run(mid, "pswrap fg interpolate", kSynthHeader, kSynthBody, d, 4, v, 3)) {
		failed = true;
		return false;
	}
	return true;
}

bool PsWrapFrameGen::present(pl_renderer renderer, bool use_mid, const pl_frame &target)
{
	const pl_tex src_tex = use_mid ? mid : frame[cur];
	const pl_tex tt = target.num_planes > 0 ? target.planes[0].texture : nullptr;
	if (!renderer || !src_tex || !valid[cur] || !tt || tt->params.w != width || tt->params.h != height)
		return false;   // ขนาดจอเปลี่ยนหลังวาดเฟรมไว้ → ให้ทางปกติวาดแทน
	pl_frame s = {};
	s.num_planes = 1;
	s.planes[0].texture = src_tex;
	s.planes[0].components = 4;
	s.planes[0].component_mapping[0] = PL_CHANNEL_R;
	s.planes[0].component_mapping[1] = PL_CHANNEL_G;
	s.planes[0].component_mapping[2] = PL_CHANNEL_B;
	s.planes[0].component_mapping[3] = PL_CHANNEL_A;
	s.repr = pl_color_repr_rgb;
	s.repr.alpha = PL_ALPHA_NONE;
	s.repr.bits.sample_depth = fmt_rgba->component_depth[0];
	s.repr.bits.color_depth = fmt_rgba->component_depth[0];
	s.color = target.color;   // วาดไว้ใน color space ของจอแล้ว → ไม่แปลงซ้ำ
	s.crop = {0.0f, 0.0f, float(width), float(height)};
	pl_frame t = target;
	t.crop = {0.0f, 0.0f, float(width), float(height)};
	pl_render_params p = pl_render_fast_params;
	p.dither_params = &pl_dither_default_params;
	p.background_transparency = 0.0f;
	return pl_render_image(renderer, &s, &t, &p);
}
