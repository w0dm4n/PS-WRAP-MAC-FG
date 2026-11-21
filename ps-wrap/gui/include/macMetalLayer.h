// PS-WRAP-MAC: CAMetalLayer สำหรับ VkSurface ของ MoltenVK บน Qt >= 6.10 — ดู src/macMetalLayer.mm
#ifndef PSWRAP_MACMETALLAYER_H
#define PSWRAP_MACMETALLAYER_H

// คืน CAMetalLayer* (เป็น void*) ของ NSView — ของ view เอง / ใน sublayers / สร้างใหม่ซ้อนเต็ม view
void *pswrapMetalLayerForView(void *nsview);
// ปรับ frame + contentsScale ของ CAMetalLayer ที่สร้างไว้ให้ตรง view (เรียกตอน resize / ย้ายจอ)
void pswrapMetalLayerSync(void *nsview);

// PS-WRAP-MAC-FG: ProMotion hint — while on, a CADisplayLink on the view asks the compositor for 120 Hz (min 60).
// Without it macOS adaptive refresh can settle the window at 48 Hz from the stream's present cadence (seen in a soak run).
// GUI thread only.
void pswrapSetHighRefreshHint(void *nsview, bool on);

#endif
