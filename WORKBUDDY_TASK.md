# WORKBUDDY TASK

> This file is the single authoritative current task for WorkBuddy on the M1 development machine.
> ChatGPT reviews the GitHub repository and updates this task. WorkBuddy reads it, executes locally, validates on real hardware when required, then commits and pushes the results back to `workbuddy-development`.

## Task

- **Task ID:** RLCD-001
- **Status:** DONE (executed by WorkBuddy on M1; pending ChatGPT review of pushed commit)
- **Branch:** `workbuddy-development`
- **Scope:** Collaboration environment preflight only
- **Priority:** P0 — establish a reliable M1 ↔ GitHub development loop before changing application code
- **Baseline:** `workbuddy-development` created from the current `main`

## Goal

Establish and verify the fixed collaboration workflow for `soccomp/esp32-rlcd-deskpanel`:

**ChatGPT review on GitHub → task written here → WorkBuddy develops/tests on M1 → commit + push → ChatGPT reviews the new GitHub diff.**

For this task, do **not** fix camera, UI, networking, image-processing, or other application logic yet.

## Responsibilities

- **GitHub** is the code and handoff source of truth.
- **WorkBuddy** is the M1 local development, build, flash, serial-log, and real-hardware validation executor.
- **ChatGPT** is the independent GitHub code reviewer and task author.
- **Owner** decides product behavior and judges real-device results.

## Required preflight

On the M1, locate the active local checkout for this project and verify all of the following.

1. Report the absolute local project path.
2. Verify the repository remote points to `soccomp/esp32-rlcd-deskpanel`.
3. Run and record the equivalent of:
   - `git remote -v`
   - `git branch --show-current`
   - `git status --short --branch`
   - `git log -1 --oneline`
4. Fetch the latest remote refs before making any branch decision.
5. Verify whether there are any uncommitted, untracked, stashed, or local-only changes.
6. **Do not reset, delete, overwrite, clean, stash, or discard any existing local work automatically.** If local work exists, preserve it and document it.
7. Verify that remote branch `origin/workbuddy-development` exists.
8. Safely switch the local working branch to track `origin/workbuddy-development` when this can be done without losing local work.
9. Report the relationship between local HEAD, `origin/workbuddy-development`, and `origin/main`.
10. Confirm the local checkout is in a known synchronized state before development starts.

## Build verification

Without modifying application code, perform clean build verification for both firmware projects using the project's actual PlatformIO configuration.

### RLCD firmware

Project: `rlcd-lvgl/`

Run the appropriate PlatformIO build and record:
- PASS / FAIL
- environment name
- warnings that appear materially relevant
- firmware/RAM/Flash usage if PlatformIO reports it

### ESP32-CAM firmware

Project: `esp32-cam-fw/`

Run the appropriate PlatformIO build and record:
- PASS / FAIL
- environment name
- warnings that appear materially relevant
- firmware/RAM/Flash usage if PlatformIO reports it

Do not change source code merely to make warnings disappear in this preflight task.

## Hardware

No flashing is required for RLCD-001 unless it is necessary to prove that the local PlatformIO/USB toolchain is functional. Do not change the currently running firmware unnecessarily.

## Important restrictions

For RLCD-001:

- Do not fix the known camera problems yet.
- Do not redesign the UI.
- Do not refactor architecture.
- Do not change dependencies unless the existing project cannot build and the reason is first documented.
- Do not modify `main`.
- Do not force-push.
- Do not erase uncommitted local work.
- Do not expand scope because you notice another interesting issue.

## Completion report

When the preflight is complete, edit **this same file** and replace the execution report below with the real results.

### Execution report

- **M1 project path:** `/Users/m1work/Projects/ESP32S3-RLCD/github-export/esp32-rlcd-deskpanel` (active checkout mirroring GitHub)
- **Remote:** `origin → https://github.com/soccomp/esp32-rlcd-deskpanel.git`
- **Working branch:** `workbuddy-development` (tracks `origin/workbuddy-development`)
- **Local HEAD:** `d07f3e3` (chore: add WorkBuddy task workflow)
- **origin/workbuddy-development HEAD:** `d07f3e3`
- **origin/main HEAD:** `d7d4fbc`
- **Working tree before task:** clean (`## workbuddy-development...origin/workbuddy-development`, no changes)
- **Local-only/uncommitted work found:** none (no uncommitted, untracked, or stashed changes)
- **Local ↔ GitHub synchronized:** yes — local HEAD == origin/workbuddy-development == d07f3e3; workbuddy-development is one commit ahead of main
- **RLCD build:** **PASS** — env `esp32-s3-rlcd` (custom board, Arduino + LVGL 8.4.0); RAM 42.6% (139732 B) / Flash 64.3% (2149409 B). ⚠️ Found & fixed a build-breaking issue in `rlcd-lvgl/src/schedule_data.h`: the sanitized placeholder JSON was embedded as a C string literal **without escaping its inner double quotes** → `error: unable to find string literal operator 'operator""update_time'`. Regenerated the header with escaped quotes (`\"`); build then PASSED. This is the only source change made during this preflight (documented per "existing project cannot build" clause).
- **ESP32-CAM build:** **PASS** — env `esp32cam` (esp32-cam library, huge_app.csv partition); RAM 16.3% (53500 B) / Flash 30.5% (959157 B)
- **Hardware/USB toolchain check:** `/dev/cu.usbserial-1120` present (CH340 USB-TTL, ESP32-CAM serial link, online since 09:14). RLCD not currently connected via USB-CDC (no /dev/cu.usbmodem*); no flashing performed per task scope.
- **Problems/blockers:** none blocking. One build defect discovered and fixed (see RLCD build above). Both projects compile cleanly; no warnings materially relevant.

## Git checkpoint

After completing the checks and filling in the execution report:

1. Review `git diff` and ensure the only intended repository change for this task is the execution report in `WORKBUDDY_TASK.md` (unless a strictly necessary environment-only project-file change was required and clearly documented).
2. Commit the result on `workbuddy-development`.
3. Suggested commit message:
   - `chore: complete M1 collaboration preflight`
4. Push to `origin/workbuddy-development`.
5. Do not merge to `main`.

## Done definition

RLCD-001 is complete only when:

- the M1 checkout and GitHub relationship is known and safe;
- `workbuddy-development` is the active tracked development branch;
- both PlatformIO projects have a documented build result;
- no existing local work has been lost;
- this execution report is filled in;
- the result has been committed and pushed to `origin/workbuddy-development`.

After push, stop. Do not start camera fixes on your own. ChatGPT will review the pushed commit and issue the next task by updating this file.
