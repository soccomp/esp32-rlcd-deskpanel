# RLCD Phase 2 — Architecture Convergence, Correctness Fixes and Full Validation

## Objective

Converge the current RLCD project into one clear production architecture. This task is based on the current `workbuddy-development` branch and focuses on correctness, stability, maintainability and reproducibility.

Do not rewrite the project. Do not add unrelated features.

## Current Target Architecture

The official runtime path should be:

```
ESP32-CAM
   |
   | USB-TTL / UART
   v
Mac M1
   |
   +-- camusb_bridge
   |       |
   |       +-- JPEG frames
   |       +-- PAGE commands / ACK
   |
   +-- finger_page_control
   |       |
   |       +-- MediaPipe gesture recognition
   |
   v
RLCD ESP32-S3
```

USB full-link is the production path. Old WiFi camera/control paths should not remain active in production mode.

---

# Phase A — Verify Runtime Reality

Before modifying code:

- Record current git branch and SHA.
- Identify all launchd services.
- Confirm actual running scripts and working directories.
- Do not trust old documentation over running code.

Required services to verify:

- camusb_bridge
- finger_page_control
- schedule-api

---

# P0 — Restore Repository Completeness

`parse_schedule.py` must be restored or confirmed.

Goal:

> A fresh checkout of GitHub must be able to reproduce the current M1 runtime environment.

Actions:

- Find the currently running script.
- Compare with git history.
- Add the correct version back into the repository.
- Update documentation if paths changed.

---

# P0 — Unify Camera and Gesture Transport

Current target:

```
finger_page_control
        |
        v
camusb_bridge :8770
        |
        v
USB CDC
        |
        v
RLCD
```

Remove production dependency on:

- ESP32-CAM HTTP capture path
- RLCD WiFi PAGE command server

Keep old WiFi mode only if explicitly selected as debug fallback.

Validate:

- PAGE:HOME
- PAGE:GUITAR
- PAGE:CAMERA
- ACK:PAGE:X
- retry/pending/confirmed_page logic

---

# P0 — Weather gzip validation

Audit `weather_client.cpp` gzip decompression.

Verify the meaning of the output length returned by the decompression API.

Validate:

- compressed size
- decompressed size
- JSON parse
- weather rendering
- failure fallback

Do not rely only on Mac fallback for success.

---

# P1 — Stock fallback parser

Verify Tencent and Sina parsers.

Tencent format:

```
~ delimiter
```

Sina format:

```
, delimiter
```

Implement separate delimiter handling and add fixtures/tests.

Validate:

- Tencent success
- Tencent failure -> Sina success
- no unnecessary backend fallback

---

# P1 — WiFi reconnect state machine

Fix reconnect handling.

First connection and recovered connection should call the same recovery function.

On reconnect:

- refresh required data
- restore network services
- update state correctly

Avoid hidden state bugs caused by `g_wifi_prev` handling.

---

# P1 — Replace unconditional WiFi hard restart

Current workaround of periodic WiFi radio restart should not be the final architecture.

Replace with health-based recovery:

```
HEALTHY
  -> retry
  -> reassociate
  -> hard restart only after confirmed failure
```

If the workaround must remain, document the real hardware evidence.

---

# P1 — Camera and AI health indicators

The display should expose system health without requiring camera preview.

Separate:

## CAM

Meaning:

- camera frames arriving
- frame freshness
- decode health

## AI

Meaning:

- finger recognition process alive
- last processed frame age
- MediaPipe service health

Do not display fake FPS values.

---

# P1 — Update tests for three-page UI

Current pages:

```
HOME
GUITAR
CAMERA
```

Remove outdated test assumptions such as PAGE:MEETING.

Re-run:

- gesture tests
- ACK/retry tests
- protocol tests

---

# P2 — Build reproducibility

Verify:

- LVGL dependency
- patch scripts
- PlatformIO versions

Mandatory patch failure should fail the build, not silently continue.

Pin tested platform versions.

---

# P2 — Market data scheduling

Move stock refresh from relative timers to market time slots.

Avoid drift such as:

```
11:29 update
13:09 update
```

Use exchange clock slots.

---

# P2 — Cache freshness

Cached data should expose age/state:

- LIVE
- CACHED
- STALE

Especially for stock data.

---

# Final Validation

Required:

## Firmware

- RLCD clean build PASS
- ESP32-CAM clean build PASS

## Python

- bridge compile PASS
- gesture controller compile PASS
- backend compile PASS

## Hardware

Gesture:

```
1 -> HOME
2 -> GUITAR
3 -> CAMERA
```

Repeat three rounds.

Verify:

- detection
- command delivery
- ACK
- confirmed page

## Recovery

Verify:

- camera reconnect
- bridge restart recovery
- M1 service restart recovery

---

# Completion Report

Return only:

- final commit SHA
- build results
- test results
- runtime validation results
- remaining blockers
- whether GitHub can now be the RLCD Source of Truth

Do not provide a development diary.
