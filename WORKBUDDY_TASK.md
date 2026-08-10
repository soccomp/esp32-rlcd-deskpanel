# WORKBUDDY TASK

> This file is the single authoritative current task for WorkBuddy on the M1 development machine.
> ChatGPT reviews GitHub and writes the current task here. WorkBuddy reads it, executes locally, validates on real hardware when required, then commits and pushes the result to `workbuddy-development`. After push, stop and wait for the next ChatGPT review.

## Task

- **Task ID:** RLCD-002
- **Status:** OPEN
- **Branch:** `workbuddy-development`
- **Priority:** P0
- **Scope:** Camera transport integrity and reconnect reliability only
- **Baseline:** `6ec8740d90de666cfd0cbe047edb6baa4e209ce5`
- **Previous task:** RLCD-001 accepted by ChatGPT review

## Context / review result

RLCD-001 is accepted.

The collaboration loop is now verified on the M1:
- local checkout tracks `origin/workbuddy-development`;
- no local-only work was found;
- RLCD PlatformIO build passes;
- ESP32-CAM PlatformIO build passes;
- the build-breaking escaped-JSON defect in `rlcd-lvgl/src/schedule_data.h` was correctly documented and fixed because the preflight could not otherwise complete.

Do not revisit RLCD-001 unless this task exposes a regression.

## Goal

Fix the two highest-risk defects in the camera transport path before any image-quality tuning:

1. **ESP32-CAM latest-JPEG publication / serving must be frame-consistent and race-safe.**
2. **Mac backend MJPEG reconnect must start with clean parser state and recover reliably after a stream break.**

This task is about transport correctness and reconnect stability. Do **not** tune Bayer dithering, contrast, crop, thumbnail scaling, UI layout, or RLCD online/offline presentation in this round.

## Files in scope

Primary files:
- `esp32-cam-fw/src/main.cpp`
- `rlcd-lvgl/parse_schedule.py`
- `WORKBUDDY_TASK.md` for the completion report

Only touch another file if it is strictly required to implement or test these fixes, and document why.

---

## Issue A — ESP32-CAM JPEG frame integrity / concurrency

### Current risk observed by ChatGPT

The current design has two JPEG buffers, but metadata and network serving are not a complete frame-ownership protocol.

Current pattern includes shared state similar to:
- `g_last_jpeg[2]`
- one shared `g_last_len`
- `g_jpeg_idx`
- `g_last_seq`

The grab task writes the non-current buffer, then updates length/index/sequence. HTTP handlers later snapshot an index and serve from that buffer.

There are two distinct risks to verify and fix:

1. **Metadata mismatch:** a reader can observe an index associated with one frame and a shared length associated with another frame if publication is not coherent.
2. **Buffer reuse during network send:** even if length becomes per-buffer, a slow reader may still be sending buffer A after the grabber has published B and then starts reusing A for a later frame. Therefore, merely changing `g_last_len` into `g_last_len[2]` is not automatically a complete fix.

### Required behavior

A served JPEG must be an immutable, internally consistent snapshot for the entire network write:
- buffer bytes belong to one frame;
- length belongs to that same frame;
- sequence belongs to that same frame;
- grabber cannot overwrite the bytes while the HTTP handler is sending them.

### Implementation guidance

Choose the smallest robust design after inspecting the real code.

A preferred simple approach is:
- maintain latest-frame storage owned by the grabber;
- use a mutex/critical section only for the short memory-copy/snapshot operation;
- copy the current JPEG into a dedicated send/staging buffer while protected;
- release the lock before any potentially slow `client.write()` / network operation;
- serve the staging snapshot unlocked.

This avoids holding a camera/grabber lock across WiFi transmission while also preventing frame mutation during send.

An equivalent design is acceptable if it proves the same ownership guarantees. Do not add unnecessary architecture layers.

### Also inspect while in this code

- `/capture` and `/stream` must use the same safe snapshot semantics.
- The no-frame `503` response should advertise the actual connection behavior. If the handler closes the connection after one request, do not claim `Connection: keep-alive`.
- Preserve the existing native `WiFiServer` architecture unless a concrete defect requires changing it.
- Do not tune JPEG quality/frame size in this task unless required to reproduce or fix the transport defect.

---

## Issue B — Mac backend MJPEG reconnect parser state

### Current risk observed by ChatGPT

In `rlcd-lvgl/parse_schedule.py`, camera MJPEG parsing uses `_recv_buf` as shared receive state. `_recv_line()` explicitly operates on the global buffer, while the reconnect/error path in `_cam_grabber_loop()` assigns `_recv_buf = b""` without clearly sharing the same binding.

In Python, if `_cam_grabber_loop()` assigns `_recv_buf` without `global _recv_buf`, that assignment is local to the function. The parser's real global receive buffer can therefore retain bytes from a dead connection across reconnects.

That can contaminate the next multipart stream and produce invalid framing, delayed recovery, stale/garbled JPEGs, or repeated reconnect loops.

### Required behavior

After any stream disconnect/error/reconnect:
- parser state for the dead socket must be discarded;
- the new socket must start from an empty receive buffer;
- response headers must be parsed from the new connection only;
- multipart frame boundaries/lengths must not be mixed across sockets;
- the backend must resume caching valid JPEGs without requiring a backend process restart.

### Implementation guidance

At minimum, fix the actual scope bug if confirmed.

Prefer making receive-buffer state local to the active stream/parser rather than a process-global variable if this can be done cleanly without a large refactor. A small class/local closure/helper object is acceptable, but do not redesign the backend.

Also verify that the initial `/stream` response is actually successful before treating subsequent bytes as multipart frame data. Do not blindly parse a non-200 HTTP response as MJPEG.

Do not change unrelated schedule/stocks/weather backend behavior.

---

## Validation required

### 1. Static / code review

Before building, explain in the execution report:
- the exact ESP32-CAM race you confirmed in the old code;
- the ownership/snapshot rule used in the fix;
- the exact Python `_recv_buf` scope/reconnect defect confirmed or disproved;
- any difference between ChatGPT's hypothesis and the real code.

Do not mechanically implement a hypothesis that is not supported by the actual source.

### 2. Build checks

Run:
- ESP32-CAM PlatformIO build — must PASS.
- RLCD PlatformIO build — must still PASS, even if RLCD C++ code is untouched.
- Python syntax/import check for `rlcd-lvgl/parse_schedule.py` sufficient to catch syntax/name errors introduced by the change.

Record meaningful warnings/errors.

### 3. Real camera/backend test on M1

The ESP32-CAM serial adapter was present during RLCD-001, so perform a real transport test if the hardware remains available.

Required test sequence:

1. Flash the ESP32-CAM only if necessary to test the firmware fix. Record whether flashing was performed and result.
2. Start the Mac backend normally.
3. Confirm the backend receives valid JPEG frames from the ESP32-CAM stream.
4. Confirm `/api/camframe` repeatedly returns valid JPEG data while the upstream stream is healthy.
5. Exercise a real reconnect at least once. Examples:
   - temporarily reset/power-cycle the ESP32-CAM, or
   - otherwise deliberately break the upstream camera stream without damaging configuration.
6. Confirm the backend reconnects and resumes valid frames **without restarting the backend process**.
7. After recovery, continue the test long enough to establish that frames keep advancing rather than merely serving one stale frame.
8. Check serial/backend logs for crashes, watchdog loops, corrupted-frame errors, obvious reconnect thrashing, or resource exhaustion.

If a required physical device is unavailable, do not fabricate PASS. Mark that part BLOCKED and complete all non-hardware validation.

### 4. Regression boundaries

Verify that:
- `/status` remains available on ESP32-CAM;
- `/capture` remains functional if it is part of the current public contract;
- `/stream` remains functional for the Mac backend;
- schedule/stocks/weather backend routes are not intentionally changed;
- no camera image-quality parameters were altered just to make the test look better.

---

## Restrictions

For RLCD-002:

- Do not redesign the UI.
- Do not change Bayer dithering yet.
- Do not change RLCD thumbnail/fullscreen scaling yet.
- Do not fix RLCD camera freshness/online indicator yet unless a change is strictly necessary for transport testing.
- Do not perform broad refactors.
- Do not modify `main`.
- Do not force-push.
- Do not merge branches.
- Do not commit credentials, real meeting data, local WiFi configuration, serial dumps containing secrets, or generated build output.
- If you find a new issue outside scope, record it under `New findings` but do not fix it unless it blocks this task.

---

## Completion report

Replace the TBD fields below with real results before committing.

### Execution report

- **Status:** TBD
- **Starting HEAD:** TBD
- **Files changed:** TBD
- **Issue A confirmed root cause:** TBD
- **Issue A fix:** TBD
- **Issue B confirmed root cause:** TBD
- **Issue B fix:** TBD
- **ESP32-CAM build:** TBD
- **RLCD build regression check:** TBD
- **Python check:** TBD
- **ESP32-CAM flashed:** TBD
- **Healthy stream test:** TBD
- **`/api/camframe` repeated-frame test:** TBD
- **Forced reconnect test:** TBD
- **Post-reconnect frame advancement:** TBD
- **Serial/backend log result:** TBD
- **Regression checks:** TBD
- **New findings:** TBD
- **Blockers / not tested:** TBD

## Git checkpoint

After validation:

1. Review `git diff` carefully.
2. Confirm changes are limited to RLCD-002 scope plus this execution report.
3. Commit on `workbuddy-development`.
4. Suggested commit message:
   - `fix(camera): harden frame snapshots and stream reconnect`
5. Push to `origin/workbuddy-development`.
6. Do not merge to `main`.
7. After push, stop. Do not start RLCD-003 yourself.

## Done definition

RLCD-002 is complete when:
- served ESP32-CAM JPEGs have a correct frame-ownership/snapshot guarantee;
- the Mac MJPEG parser cannot carry stale socket receive state into a reconnect;
- both firmware builds pass;
- Python validation passes;
- real reconnect behavior is validated when hardware is available;
- the execution report is complete;
- the result is committed and pushed to `origin/workbuddy-development`.
