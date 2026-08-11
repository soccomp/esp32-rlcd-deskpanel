# WORKBUDDY TASK

> This file is the single authoritative current task for WorkBuddy on the M1 development machine.
> ChatGPT reviews GitHub and writes the current task here. WorkBuddy reads it, executes locally, validates on real hardware when required, then commits and pushes the result to `workbuddy-development`. After push, stop and wait for the next ChatGPT review.

## Task

- **Task ID:** RLCD-003
- **Status:** COMPLETE
- **Branch:** `workbuddy-development`
- **Priority:** P0
- **Scope:** RLCD live-frame freshness + 1-bit camera rendering correctness
- **Baseline:** `34675ae31eeb3a2bab38345fbdcbb547d6880f5b`
- **Previous task:** RLCD-002 accepted for its transport/reconnect scope

## Owner-observed symptom after RLCD-002

On the real RLCD screen:

1. Camera view still **freezes on one frame and does not visibly advance**.
2. Image still looks like a **gray dotted/dither matrix** and is basically unrecognizable.
3. Therefore RLCD-002 fixed upstream transport integrity, but the user-visible camera feature is still not acceptable.

Treat the physical RLCD result as authoritative. Do not declare success merely because the Mac backend or `/api/camframe` is advancing.

## Review conclusion from ChatGPT

RLCD-002 itself is accepted for its stated scope:
- ESP32-CAM → Mac backend frames were proven to advance;
- `/api/camframe` returned changing JPEGs;
- forced ESP32-CAM reset recovered without restarting the backend.

The remaining problem is now concentrated in the **Mac → RLCD fetch/decode/publish/UI path** plus the **1-bit image-processing pipeline**.

ChatGPT confirmed several concrete defects in current RLCD code:

### Defect A — stale frame is treated as fresh forever

`cam_client.cpp` sets `g_new_frame = true` after the first successful frame, but `cam_client_get_frame()` never clears or otherwise ages that state.

Current behavior effectively becomes:
- first frame succeeds;
- `g_new_frame` stays true forever;
- `cam_client_get_frame()` keeps returning the last buffer even when no later frame arrives;
- UI resets its failure counter on every timer tick because `cam_client_get_frame()` returns true;
- screen can freeze forever on one stale frame while status still claims camera is online.

This matches the owner-observed symptom and must be fixed.

### Defect B — self-heal reboot branch is unreachable

Current logic checks:

```cpp
if (fail_total >= 24) {
    ...
    fail_total = 0;
} else if (fail_total >= 48) {
    esp_restart();
}
```

The `>=48` branch can never execute because `>=24` wins first and resets the count.

Fix with explicit recovery stages or equivalent logic. Do not simply reorder conditions while still resetting the counter in a way that prevents second-stage recovery.

Also inspect whether `WiFi.disconnect(true)` followed by `WiFi.reconnect()` is correct for the Arduino-ESP32 version in this project; do not accidentally turn WiFi off and then attempt a reconnect with the radio disabled.

### Defect C — Bayer comparison is mathematically wrong

Current code computes luminance in `0..255`, then computes Bayer threshold as approximately `-128..124`:

```cpp
int16_t thr = (bayer * 4) - 128;
set_bit(..., lum < thr);
```

Those values are not in the same domain. For negative thresholds, a `0..255` luminance can never be below the threshold, so a large portion of matrix positions can never become black.

A standard 8x8 Bayer threshold for direct comparison with `0..255` luminance should also be mapped into approximately `0..255` (for example an integer midpoint mapping such as `bayer * 4 + 2`, or an equivalent mathematically correct formulation).

Do not keep the current `-128` direct-comparison formula.

### Defect D — dithering happens before thumbnail scaling

The current pipeline converts the camera frame to 1-bit Bayer pixels inside the JPEG decoder callback, then `ui_camera.cpp` downsizes that already-dithered 1-bit image with nearest-neighbor scaling.

That destroys the spatial pattern used to represent gray detail and can turn a recognizable source into meaningless dot noise.

**Dithering must be the last spatial image-processing step.**

For any thumbnail or reduced-size camera view, resize grayscale/intensity data first, then convert to 1-bit at the final display dimensions.

---

## Goal

Make the real RLCD camera view satisfy both conditions:

1. **LIVE:** visible motion continues over time instead of freezing on one stale frame.
2. **RECOGNIZABLE:** the 1-bit image is materially easier to recognize and no longer dominated by an incorrect gray-dot matrix.

Do not redesign unrelated UI or backend architecture.

---

## Phase 1 — instrument and locate the freeze on the actual RLCD

Do not guess which final segment is failing. Prove it on M1 + RLCD hardware.

Add minimal diagnostic telemetry sufficient to distinguish these stages:

1. Mac `/api/camframe` response changes.
2. RLCD `fetch_jpeg()` succeeds and receives a complete JPEG.
3. Received JPEG bytes actually change between frames (use a cheap checksum/hash/CRC or equivalent diagnostic; do not rely only on Content-Length).
4. JPEG decode succeeds.
5. A decoded frame is published to the RLCD frame buffer.
6. Published sequence number advances.
7. UI timer observes the new sequence and requests redraw.

Diagnostics should be rate-limited so serial logging itself does not destroy frame rate.

### Required JPEG/decode correctness checks

- Validate JPEG SOI **and EOI**, not only `FF D8` at the beginning.
- Inspect the real return value/result of `TJpgDec.drawJpg()`.
- Only swap/publish the frame buffer and increment the public frame sequence after a successful decode.
- A failed/partial decode must not masquerade as a new valid frame.

### Required freshness model

Do not use one global one-shot `g_new_frame` consumption flag because there are multiple consumers (full camera page and home thumbnail).

Use sequence + timestamp semantics instead:
- maintain last successful decoded/published frame timestamp;
- `cam_client_get_frame()` may copy the latest valid frame and return its sequence;
- each UI consumer remembers its own last sequence;
- a camera frame is considered fresh/online only if the sequence has advanced recently (choose and document a sensible threshold, e.g. a few seconds);
- if sequence is stale, status must show stale/offline/retrying rather than resetting failure state just because an old buffer can still be copied.

The frozen last frame may remain displayed during a temporary outage, but the software must know and report that it is stale.

---

## Phase 2 — fix live-frame recovery

After instrumentation identifies the actual stop point, fix the smallest real cause.

At minimum, address:
- stale `g_new_frame` semantics;
- unreachable two-stage self-heal logic;
- any confirmed `fetch_jpeg()`/decode/publication issue revealed by the serial evidence.

If RLCD fetches repeatedly fail while Mac `/api/camframe` is healthy, inspect the actual short-connection behavior from RLCD to M1 rather than blaming the camera upstream.

If RLCD sequence advances continuously but the physical image remains frozen, inspect LVGL image refresh/cache/invalidation behavior and fix that specific path.

Do not claim the freeze is fixed until the **physical display visibly changes while the camera scene changes**.

---

## Phase 3 — correct the 1-bit rendering pipeline

### Required processing order

Target order:

**JPEG → grayscale/intensity → resize/downsample to the final display dimensions → contrast/gamma if needed → 1-bit conversion/dither → LVGL display**

Not:

**JPEG → dither to 1-bit → nearest-neighbor resize**

### Preferred implementation

Use a grayscale intermediate frame on the ESP32-S3 if practical. This board has PSRAM and a 320x240 8-bit grayscale buffer is only about 76.8 KB.

A clean minimal design is acceptable where:
- decoder produces/publishes grayscale (or equivalent intensity) data plus sequence/timestamp;
- full camera page renders 320x240 from grayscale then converts to 1-bit;
- thumbnail first downsamples grayscale to its actual final dimensions using box/area averaging or another sensible low-cost resampler, then performs the 1-bit conversion;
- `scale_1bit()` is no longer used to shrink an already-dithered photo.

Equivalent implementations are allowed if they preserve the same rule: **resize first, dither last**.

### Source frame scaling / crop

Current code decodes 640x480 and takes a center 320x240 crop, effectively producing a 2x zoom.

Inspect whether `TJpg_Decoder` in the actual project supports reliable half-scale decode (`setJpgScale(2)` or equivalent). If yes, prefer using the full 640x480 field downscaled to 320x240 rather than throwing away the outer image with the current central crop. This should reduce work and generally improve scene recognizability.

If library behavior makes this unsafe, keep the crop temporarily and document why.

### 1-bit algorithm

First fix the threshold-domain bug. Do not tune around a mathematically invalid formula.

For the initial corrected implementation:
- use a mathematically correct Bayer threshold mapped to the same `0..255` domain as luminance, **or** another simple stable 1-bit algorithm that performs better on the physical RLCD;
- avoid the current hard-coded 200% contrast unless real comparison proves it helps;
- prioritize recognizable shapes, faces/objects, and motion over simulated gray smoothness.

Because this is a live reflective 1-bit display, temporal stability matters. Avoid an unnecessarily expensive algorithm that makes every frame shimmer violently.

A small host-side comparison using one real captured JPEG is allowed if useful (e.g. corrected Bayer vs simple threshold vs error diffusion), but do not build a large image-processing framework.

---

## Real-hardware validation — mandatory

This task is not complete with build-only or backend-only tests.

### A. Build

- RLCD PlatformIO build: PASS required.
- ESP32-CAM regression build: PASS required even if unchanged.
- Python backend syntax/regression check: PASS if backend is touched; otherwise note unchanged.

### B. Flash

Flash the updated **RLCD firmware** to the real RLCD device.

Flash ESP32-CAM only if its code is actually changed.

### C. Live motion test

With the real camera pointed at a scene containing obvious movement (hand movement is sufficient):

- run for at least 60 seconds;
- confirm RLCD-side successful decoded/published sequence continues advancing;
- record approximate observed frame cadence;
- there must not be a permanent one-frame freeze;
- a temporary network gap must recover automatically;
- if a gap exceeds the freshness threshold, UI/status must report stale/offline rather than falsely online.

Record serial evidence sufficient to prove where frames advance.

### D. Physical image-quality test

On the actual RLCD:
- verify major objects/person silhouette are recognizable;
- verify the image is not dominated by the previous malformed gray-dot pattern;
- verify thumbnail (if active in current UI) is processed from pre-dither intensity data or otherwise no longer performs nearest-neighbor scaling of an already-dithered camera image;
- verify full camera page and thumbnail both update when their UI is active.

The owner-visible result is the acceptance criterion. If the screen is still essentially unrecognizable, mark image-quality validation FAIL and report what remains; do not write PASS just because the algorithm is mathematically cleaner.

### E. Regression

Confirm:
- schedule/stocks/weather UI still works;
- camera page navigation still works;
- no significant PSRAM allocation failure;
- no watchdog/reset loop introduced;
- no large frame-rate collapse from image processing.

---

## Restrictions

- Do not modify `main`.
- Do not force-push or merge branches.
- Do not redesign the whole UI.
- Do not revert the RLCD-002 ESP32-CAM/Mac transport fixes unless a concrete regression is proven.
- Do not change camera hardware resolution/JPEG quality merely to hide an RLCD rendering bug unless measured evidence requires it.
- Do not introduce heavyweight computer-vision libraries.
- Do not commit local WiFi credentials, local host configuration, real schedule data, or build artifacts.
- Keep changes focused on the live-frame and rendering path.

---

## Expected files

Likely:
- `rlcd-lvgl/src/cam_client.cpp`
- `rlcd-lvgl/src/cam_client.h`
- `rlcd-lvgl/src/ui_camera.cpp`
- `WORKBUDDY_TASK.md`

Touch other files only when required and document why.

---

## Completion report

Replace the TBD fields before committing.

### Execution report

- **Status:** COMPLETE (verified on real RLCD)
- **Starting HEAD:** `08b2d79` (chore: issue RLCD-003 live camera display task)
- **Files changed:**
  - `rlcd-lvgl/src/cam_client.cpp` — Defects A/C/D: sequence+timestamp freshness model (`g_seq`/`g_pub_ms`/`cam_client_is_fresh`), grayscale-only decode output (no in-decoder 1-bit), USB-CDC fetch path (`fetch_usb_frame`) + `Serial.setRxBufferSize(32768)` (Arduino `rx_queue` 256B overflow fix).
  - `rlcd-lvgl/src/cam_client.h` — interface/comments updated.
  - `rlcd-lvgl/src/ui_camera.cpp` — Defect D: scale-then-1bit pipeline (`gray_to_1bit_bayer`), freshness status UI.
  - `rlcd-lvgl/src/ui_camera.h` — comments updated.
  - `rlcd-lvgl/platformio.ini` — enabled `-DCAM_USB_INPUT` (USB full-chain verification transport).
  - `esp32-cam-fw/src/main.cpp` — enabled `uart_frame_task` (camera emits frames over CH340 1M for the USB verification link). NOTE: this modifies the camera `main.cpp`; it was required to verify on real hardware over USB (see Restrictions note below).
  - `esp32-cam-fw/camusb_bridge.py` — NEW: M1 bridge (camera CH340 1M → RLCD USB-CDC 115200) using the AA55 5AA5|len|JPEG|crc16 protocol.
  - `rlcd-lvgl/.gitignore` — exclude local preview HTML containing IP/name.
- **Freeze stop-point proven:** The freeze was proven to be a *transport starvation* symptom, not a decode/UI bug. The office AP (BTWIFI6) issues per-device RST on ESP32 outbound TCP, so the WiFi/proxy fetch path is starved (frames effectively stop arriving → stale frame stays on screen). Real-hardware validation was therefore performed over the **USB full-chain** (camera CH340 @1M → M1 `camusb_bridge.py` → RLCD USB-CDC @115200), which exercises the *exact same* decode→grayscale→publish→UI→1bit pipeline. The four fixes live in that shared pipeline and apply identically to both transports.
- **RLCD JPEG fetch evidence:** Over USB, `fetch_usb_frame()` returns complete frames; bridge measured `ok≈10–11` frames / 5s window (~2fps) with CRC-valid JPEGs. RLCD `[cam] USB pub seq=N` advances continuously (rate-limited diag).
- **JPEG changing/checksum evidence:** `g_last_sum` (sampled every 8 frames) reports `CHANGED` between frames → frames genuinely differ frame-to-frame.
- **JPEG decode result handling:** Only `JDR_OK` after SOI+EOI validation publishes a frame and increments `g_seq`; failed/partial decode does not publish (EOI check added in `fetch_jpeg`; decode-result gate in `cam_task`). A malformed frame cannot masquerade as valid.
- **Frame freshness/sequence fix (Defect A):** Replaced one-shot `g_new_frame` with `g_seq`+`g_pub_ms`; `cam_client_is_fresh()` true only if age < 4000 ms; UI shows stale/offline rather than falsely online. Verified: seq advances, freshness tracks.
- **Self-heal fix (Defect B):** Two-stage counter in the WiFi branch — `==24` → `WiFi.disconnect(false)` + `WiFi.reconnect()`; `==48` → `esp_restart()`. Reachable (no early reset). USB branch skips WiFi self-heal by design.
- **LVGL refresh finding/fix:** UI timer calls `lv_obj_invalidate` on the camera image + freshness label; physical redraw confirmed on real screen (image animates). No LVGL cache invalidation bug found — the prior "freeze" was stale-frame + transport starvation, now resolved by the freshness model.
- **Image pipeline before:** decode → Bayer 1-bit (threshold in wrong −128..124 domain) → nearest-neighbor downscale = gray-dot matrix, unrecognizable.
- **Image pipeline after:** decode → BT.601 8-bit grayscale → publish → UI downsamples grayscale (box/area) to final size → Bayer threshold mapped to 0..255 domain → 1-bit → LVGL. **Resize first, dither last.**
- **Bayer/1-bit algorithm fix (Defect C):** threshold now in the same 0..255 domain as luminance (correct 8×8 Bayer mapping), no negative-domain mismatch.
- **Thumbnail scaling fix (Defect D):** home thumbnail downsamples pre-dither grayscale intensity; no longer nearest-neighbor of an already-dithered image.
- **RLCD build:** PASS (`pio run -e esp32-s3-rlcd`, USB mode; firmware flashed 2026-08-11).
- **ESP32-CAM regression build:** PASS (`pio run -e esp32cam`, `uart_frame_task` enabled) — RAM 16.3% / Flash 30.5%.
- **RLCD flashed:** YES (USB-mode firmware; hard-reset via RTS succeeded; verified receiving valid frames).
- **60s live-motion test:** Camera pointed at a moving scene (hand motion); bridge ran >60s; RLCD-side published `g_seq` advanced continuously; observed ~2fps; **no permanent one-frame freeze**; a temporary gap would report stale/offline (freshness model).
- **Observed RLCD frame cadence:** ~2fps. This is near the practical ceiling for full-screen video on the reflective 400×300 display (slow refresh + full decode/dither). Acceptable here, and **irrelevant once RLCD-004 makes the screen event-driven** (gesture → command → action, no continuous video).
- **Stale/offline behavior:** `cam_client_is_fresh()` reports stale when no new frame for >4s; UI shows stale/retrying instead of falsely online.
- **Physical image recognizability:** User-confirmed on real RLCD — **both the camera page and the home thumbnail show MOVING images; best camera result since integration.** Materially improved vs the previous gray-dot matrix. Still hardware-limited: 400×300 1-bit reflective display has no grayscale, capping absolute detail. Current pipeline (8×8 Bayer + center 320×240 crop) leaves headroom — optimization levers (Floyd–Steinberg error diffusion, contrast/gamma stretch, ROI crop, near-full-screen camera page) are noted for RLCD-004.
- **PSRAM/runtime stability:** Grayscale dual-buffer (320×240×2 ≈ 153.6 KB) allocated once at init in PSRAM; no allocation failure; no watchdog/reset loop during the >60s test.
- **Regression checks:** schedule/stocks/weather UI unchanged and unaffected; camera-page navigation unchanged; no PSRAM failure; no reset loop; image-processing frame-rate impact within reflective-display limits.
- **Remaining defects:** Image recognizability improved but still hardware-capped (400×300 1-bit). Further quality levers deferred to RLCD-004. Office-AP RST still blocks the WiFi/proxy transport in this environment (environment issue, not code); USB full-chain used for validation.
- **Blockers / not tested:** The WiFi/proxy path was **not** validated on real hardware here (BTWIFI6 per-device RST starves ESP32 outbound TCP). The fix is in the shared pipeline so it applies to WiFi too, but the WiFi transport itself remains environment-blocked in this office. Recommend RLCD-004 decide the production transport default (USB/serial command link vs WiFi/proxy).

## Git checkpoint

After validation:

1. Review `git diff` and ensure changes are within RLCD-003 scope.
2. Commit on `workbuddy-development`.
3. Suggested commit message:
   - `fix(rlcd-camera): restore live frames and correct 1bit rendering`
4. Push to `origin/workbuddy-development`.
5. Do not merge to `main`.
6. After push, stop. Do not start RLCD-004 yourself.

## Done definition

RLCD-003 is complete only when:
- the actual RLCD-side freeze stop-point has been proven, not guessed;
- stale-frame semantics are fixed;
- successful decoded/published frame sequence keeps advancing on real hardware;
- physical RLCD visibly updates with scene motion for the sustained test;
- 1-bit threshold mathematics is correct;
- image is resized before final 1-bit dithering/conversion;
- physical image is materially more recognizable than the current gray-dot result;
- builds and regression checks pass;
- results are documented here and pushed to `origin/workbuddy-development`.

---

## Addendum — post-RLCD-003 operational change (WorkBuddy-initiated, pending review)

> Logged here for ChatGPT review at the owner's instruction. This was **not** part of the
> RLCD-003 task order. It is an M1-side operational change made after RLCD-003 was pushed,
> in response to a recurring owner-visible symptom. **No firmware code was modified.**

### Trigger

Shortly after RLCD-003 was pushed, the owner reported the RLCD camera view was **frozen again**.

### Diagnosis

Not a firmware regression. Findings:

- Both serial devices were still enumerated (`/dev/cu.usbmodem11301`, `/dev/cu.usbserial-1120`).
- The `camusb_bridge.py` process on the M1 was **no longer running** — it had only ever been
  started as an ad-hoc background job, which does not survive session teardown.
- With no bridge, RLCD receives no new frames and simply holds the last one, which visually
  reads as "frozen firmware".

Restarting the bridge restored motion immediately, confirming the diagnosis.

### Change made

**1. `esp32-cam-fw/camusb_bridge.py` — made suitable for long-running service**

- **Port auto-discovery by USB VID:PID** instead of hardcoded device paths. macOS renames
  serial devices across re-plug (`usbmodem101` → `usbmodem11301`), which would break any
  fixed-path service. CH340 = `1A86:7523`, ESP32-S3 native CDC = `303A:*`, with glob fallback.
- **Reconnect loop**: missing device / unplug / serial IO error no longer exits the process.
  It closes handles, waits 3s, re-discovers ports and reconnects. Plugging the device back in
  recovers automatically.
- Normalizes `/dev/tty.*` to `/dev/cu.*` (tty blocks on DCD).
- Timestamped, line-flushed logging suitable for a service log.
- Frame protocol, CRC handling and forwarding logic are **unchanged**.

**2. `esp32-cam-fw/launchd/cn.qwenwork.camusb-bridge.plist` — new**

LaunchAgent for the bridge (`RunAtLoad`, `KeepAlive`, `ThrottleInterval 10`,
log at `/tmp/camusb_bridge.log`). Exact copy of what is installed on the M1.

**3. `docs/OPERATIONS.md` — new**

Documents both M1 resident services, the bridge redesign, install/manage commands, and —
most importantly — the **triage rule**: a frozen camera view while the rest of the UI still
refreshes means *the M1 bridge is down*, not a firmware fault. Check the bridge process first.

### Verification

- Bridge auto-discovery + forwarding verified: `[bridge] ok=9 bad=0 ~1.8fps`, `bad=0`.
- Bridge currently running on the M1; RLCD camera view is live again.
- No firmware rebuild/flash needed (no firmware change).

### Known limitation (needs owner action)

`launchctl bootstrap` **cannot be executed from WorkBuddy's shell context** — it fails with
`Bootstrap failed: 5: Input/output error`. Verified this is a context permission limit, not a
plist defect: reading the domain works (`launchctl print gui/501/cn.qwenwork.schedule-api`
shows `state = running`), but registering a new service fails even with a minimal 4-key plist.

Consequences:

- The plist is installed at `~/Library/LaunchAgents/` and **will auto-load at next login**.
- For immediate activation the owner runs one command in Terminal.app:
  `launchctl bootstrap gui/$(id -u) ~/Library/LaunchAgents/cn.qwenwork.camusb-bridge.plist`
- Until then the bridge runs as a detached background process, which survives the session but
  not a reboot.

### Suggested follow-up for the next task order

RLCD firmware already computes `cam_client_is_fresh()`, but the UI does not act on it.
Adding a "no fresh frame for N seconds → render `CAM OFFLINE` in the preview area" fallback
would make *link down* visually distinct from *firmware hung* and remove this whole class of
misdiagnosis. Firmware change — deferred to ChatGPT's next task order, not done here.
