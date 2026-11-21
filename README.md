# PS-WRAP-MAC-FG — GTA 6 (PS5) at 60 fps on a Mac

GTA 6 runs at **30 fps** on PS5. This fork of the PS-WRAP-MAC Remote Play client computes **an in-between frame for every pair of streamed frames** on your Mac. That takes you from 30 to **~60 fps on screen**, with no capture card and no changes to the console.

It is a fork of [PS-WRAP-MAC](https://github.com/iDevGIS/PS-WRAP), itself a fork of [chiaki-ng](https://github.com/streetpea/chiaki-ng), for Apple Silicon Macs.

## What it adds over PS-WRAP-MAC

| | PS-WRAP-MAC 0.14 | **PS-WRAP-MAC-FG** |
|---|---|---|
| Frame gen engine | custom optical-flow shader | **Apple VideoToolbox** (`VTFrameProcessor` low-latency), with a **RIFE 4.25** (Metal) fallback |
| Halos and ghosting around the character | visible | removed (frames are held where interpolation is unreliable) |
| Magenta bars, smeared edges on camera pans | yes | fixed (edge fill) |
| Frame pacing | ~54 fps, 58 % of frames on time | **~58–60 fps, ~94 % within ±4 ms** |
| Upscaling | High Quality preset | **High Quality + Spatial (FSRCNNX)** by default |
| H.265 freezes when Wi-Fi drops packets | possible | "IDR on FEC failure" on by default, plus a video watchdog |
| Display going to sleep while you're AFK | the stream freezes | the display stays on while streaming |
| Sound lost after an audio stall | never comes back | reopens automatically |

These numbers come from 120 Hz ScreenCaptureKit recordings of real gameplay, analyzed frame by frame (GTA 6, 5 GHz Wi-Fi, MacBook Pro M4 Pro).

## Requirements

- **Apple Silicon Mac** (M1 or newer; tested on an M4 Pro).
- **macOS 26 or newer** for the Apple engine, which gives the best results. On older macOS the app falls back to RIFE or the shader.
- **PS5** with Remote Play enabled.
- Network: ideally **Ethernet on the PS5**. Otherwise, 5 GHz Wi-Fi for both the PS5 and the Mac.

## 1. Set up the PS5

1. **Settings → System → Remote Play** → turn on **Enable Remote Play**.
2. **Settings → System → Power Saving → Features Available in Rest Mode** → turn on **Stay Connected to the Internet** and **Enable Turning On PS5 from Network**. This lets the Mac wake the console.
3. Keep **Settings → System → Remote Play → Link Device** at hand: it shows the PIN you'll need in step 3.

## 2. Build the app

```bash
git clone https://github.com/w0dm4n/PS-WRAP-MAC-FG.git
cd PS-WRAP-MAC-FG
git submodule update --init --recursive

scripts/setup-mac.sh                 # once: Homebrew deps (Qt 6, FFmpeg, SDL2, MoltenVK…)
scripts/build-libplacebo-mac.sh      # once: libplacebo 7.360 (Homebrew's 7.387 deadlocks at launch)
python3 -m venv .venv && .venv/bin/pip install protobuf
PATH="$PWD/.venv/bin:$PATH" scripts/deploy-mac-fg.sh
```

`deploy-mac-fg.sh` builds the app and the RIFE wrapper. It downloads [rife-metal](https://github.com/cinemore/rife-metal) v0.1.8 and applies a small patch. The result is a self-contained, ad-hoc signed app in **`dist/PS-WRAP-MAC-FG.app`**.

This build keeps its own settings (`com.ps-wrap-fg.PS-WRAP`) and its own keychain entries. It never overwrites the official PS-WRAP-MAC settings, and it reuses the official app's PS5 pairing if that app is installed.

## 3. Pair the PS5

Launch `dist/PS-WRAP-MAC-FG.app`. The PS5 shows up in the list when it is on and on the same network. Click it and enter your PSN account, then the PIN shown on the PS5 under **Link Device**.

> Launch the app from Finder or with `open`, not directly from a terminal. macOS grants the "Local Network" permission to whichever app does the launching. Started from a terminal, the app fails with *No route to host*.

## 4. Recommended settings

These are this build's defaults. Check them in **Settings** and in the stream menu:

| Setting | Value |
|---|---|
| Stream resolution / fps | **1080p / 30 fps**. GTA 6 runs at 30 fps, and the Mac does the doubling. |
| Bitrate | **50 Mbps** (drop to 30–40 if it stutters on Wi-Fi) |
| Codec | **H.265** |
| Request IDR Frame on FEC Failure | **On** (prevents H.265 freezes) |
| Rendering (Placebo preset) | **High Quality + Spatial** (FSRCNNX upscaling) |
| Frame Gen | **On**, engine **Auto** (Apple → RIFE → shader) |
| Snaps | **Clean** (no ghosting on camera cuts). **Smooth** is more fluid but can leave a one-frame trail. |
| Controller overlay | Off |

### In GTA 6

- **Motion blur: off.** Blur confuses the interpolation and causes trails.
- Other settings don't affect frame gen. Keep the picture mode you prefer.

### Launch the stream directly in fullscreen

```bash
open -n dist/PS-WRAP-MAC-FG.app --args --fullscreen stream "<your PS5 name>" <your PS5 IP>
```

- `--fullscreen` keeps the correct aspect ratio. The PS5 outputs 16:9 and a MacBook screen is 16:10, so you get thin black bars at the top and bottom.
- `--zoom` fills the screen by cropping ~6 % on each side (the minimap may get clipped).
- `--stretch` fills the screen by distorting the image. Not recommended.

Your PS5's name and IP are shown in the app, or in your router's admin page.

## Known limitations

- **60 fps maximum.** The game outputs 30 frames per second and one frame is added between each pair. Going higher would add latency and artifacts.
- **Latency:** interpolation has to wait for the next frame, which adds about one frame (~33 ms) on top of Remote Play's own latency. Fine for GTA, less so for competitive games.
- **Objects very close to the camera** that move fast (bushes, posts, reflections) can still smear slightly.
- On a **sudden camera cut**, the app briefly shows 30 fps rather than a wrong frame (Clean mode).
- **Wi-Fi packet loss** causes dips (40–50 fps for a moment). Ethernet fixes this.

## Environment variables (debugging)

| Variable | Effect |
|---|---|
| `PSWRAP_VT_EDGEFILL=0` | disables edge fill |
| `PSWRAP_NO_REFRESH_HINT=1` | stops pinning the ProMotion display at 120 Hz while frame gen is on |
| `PSWRAP_FG_MARK=1` | marks generated frames (to check them in a recording) |
| `PSWRAP_VT_DEBUG=1`, `PSWRAP_PRESENT_LOG=1` | verbose interpolation and presentation logs |

## Credits and license

- [PS-WRAP](https://github.com/iDevGIS/PS-WRAP) by iDevGIS: the base of this fork (see [README.PS-WRAP.md](README.PS-WRAP.md)).
- [chiaki-ng](https://github.com/streetpea/chiaki-ng) / Chiaki: the open-source Remote Play client.
- [libplacebo](https://code.videolan.org/videolan/libplacebo), [FSRCNNX](https://github.com/igv/FSRCNN-TensorFlow).
- [rife-metal](https://github.com/cinemore/rife-metal) (Apache-2.0) and the [Practical-RIFE](https://github.com/hzwer/Practical-RIFE) 4.25 weights (MIT).

Released under the **GNU AGPL v3** (see [LICENSE](LICENSE)), like the original project.

Unofficial project, not affiliated with Sony Interactive Entertainment or Rockstar Games. PlayStation, PS5 and DualSense are trademarks of Sony; Grand Theft Auto is a trademark of Take-Two Interactive.
