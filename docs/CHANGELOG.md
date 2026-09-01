<p align="right">
  <a href="CHANGELOG.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Changelog

## Unreleased

- Fixed noisy system-chime playback by opening the ES8311 DAC path as stereo,
  selecting both I2S slots, duplicating each mono PCM sample to both slots, and
  playing the chimes at full codec output volume.
- Added original ascending startup and descending software-shutdown chimes;
  holding OK in paging mode now plays the shutdown sound, turns off the display,
  and enters deep sleep.
- Stabilized CI APK signing so later builds can update an installed app
  without requiring data-destructive uninstallation.
- Saved recordings as playable PCM WAV files in the public
  `Music/AI Passport` directory, added in-app playback and deletion, replaced
  the firmware recording dot with rotating cassette reels.
- Fixed compressed Chinese/Latin title rendering on the Passport, preserved
  UTF-8 boundaries when truncating titles, and added native schedule time
  pickers plus schedule/todo deletion to the Android app.
- Fixed recording sessions that immediately reported `LINK LOST` because the
  240-byte ADPCM chunk exceeded the protocol's 238-byte audio-data limit.
- Fixed BLE discovery and connection: corrected firmware UUID byte order,
  split the oversized legacy advertising payload across advertising and scan
  response packets, and made Android match by name or service UUID, request
  runtime permissions, time out scans, and serialize GATT setup.
- Redesigned the firmware launcher, recording, days, and todo screens around
  the DayRing visual language: paper-white surfaces, monochrome typography,
  fine borders, compact status metadata, cassette reels, date badges, task
  rows, and a persistent three-section footer adapted to the 240×320 display.
- Fixed compact-screen labels that wrapped, clipped, or rendered unsupported
  separator glyphs, and centered the empty states for days and todo.
- Fixed firmware builds and prevented 60-byte schedule/todo titles from
  writing their terminator past the allocated buffer.
- Added the AI Passport sync app (replaces the demo menu on boot): a paging
  UI (Recording / Today's schedule / Todo) driven by UP/DOWN to flip pages, OK
  to enter a page, UP/DOWN for in-page navigation, and OK double-press (or
  long-press) to return to paging. Includes a Chinese font (Noto Sans CJK SC
  subset) for schedule/todo titles.
- Added an Android companion app under `android/` that implements the
  `ai-passport-sync` BLE client: scan/connect to the device, time sync, push
  today's schedule and todo, receive the live recording stream and save it to
  phone storage, and echo todo toggles both ways. A `build-android.yml`
  workflow packages the APK (unit tests + `assembleDebug`) and publishes the
  artifact on tags.
- Recording page streams the ES8311 microphone (16 kHz / 16-bit / mono) as
  IMA-ADPCM over BLE to the phone in real time (~8 KB/s); the phone stores the
  file and finalizes it on `AUDIO_END`.
- Defined the `ai-passport-sync` BLE service (GATT TX notify / RX write) with a
  documented frame protocol for time sync, today's schedule, todo, todo
  toggles, recording upload, and status — see
  `docs/software-design/passport-sync-app.md` (bilingual). The Android
  companion app implements this protocol to sync with the device.
- Added host-testable cores with host tests: `pager_core` (paging state
  machine), `adpcm_ima` (IMA ADPCM encoder), and `sync_proto` (frame codec,
  schedule/todo store, local-time conversion). `tools/validate.sh --static`
  runs every host-test binary.
- Kept the demo pages (`demo_*.c`) in the tree as reference but removed them
  from the firmware build.

- Made mini-program BLE install compatibility a template-level invariant: fixed
  protected `cardid`/Recovery partitions, retained the five-second UP-key
  Recovery boot hook, and added CI validation for merged-image structure,
  partition MD5/ranges, the 3 MB app limit, and protected payload exclusion.
- Documented a release-title convention for multi-app releases: name tags as `v<version>-<app-name>` (e.g. `v0.1.0-voice-keychain`) so the release title carries the version and the app, and confirm the title after the release is published so a release list is scannable by app.
- Added a post-release follow-up workflow: an `issue-suggestions` skill for filing user feedback as issues against the upstream project, an `experience-pr` skill for submitting reusable development experience as a documentation PR, a `docs/experiences/` directory for per-entry experience files, and supporting `project-completion`, `file-issues`, and experience-index documents.
- Simplified the tracked repository root: moved GitHub-recognized community documents into `.github/`, moved the changelog into `docs/`, updated every reference, and added a root-document allowlist to repository checks.
- Repository-wide language policy: every maintained Markdown default `.md` file is English, Simplified Chinese uses a paired `.zh_CN.md`, and both provide language switches. Static checks reject missing peers, missing switches, and Chinese prose in English defaults.
- Phase one of the AI development workflow: streamlined task-based context routing, unified local/CI validation, added PR checks and a template, and committed the dependency lock for reproducible builds.
- PR review fixes: pinned GitHub Actions to full commit SHAs, split build/release jobs by least privilege, disabled persisted sync checkout credentials, added Feature Request and Usage Question forms, clarified private security-report fallback, and corrected stale README, CI-trigger, and branch descriptions.
- Changed commit titles, PR titles, and PR bodies from Chinese-default to English; updated the Chinese punctuation rule so it no longer applies to PR descriptions.
- Reworked `build-firmware.yml` to pass `SDKCONFIG_DEFAULTS=sdkconfig.defaults`, enable `partitions.csv`, preserve the 8 MB image header, merge a flashable `FoloToy-AI-Passport-full.bin`, publish only that artifact, and use Actions cache v5.
- Integrated upstream PR #6 to resolve PR #4 conflicts: Wi-Fi, Bluetooth LE, radio lifecycle, and low-power demos; a 3 MB factory partition; build/menu/configuration updates; hardware-guide coverage; and bilingual capability tables.
- Defined English imperative Conventional Commit formatting for both commits and PR titles.
- Removed stale sync-workflow template comments and generalized an irrelevant Redis TTL rule to cache components.
- Added Chinese punctuation, credential safety, and recoverable file-deletion conventions.
- Expanded source-comment requirements for functions, state, ownership, concurrency, timing, registers, and magic values.
- Removed AI execution instructions from product READMEs so they remain human-facing product and repository overviews.
- Added `docs/development/agent-guide.md` as the focused AI workflow guide.
- Updated `AGENTS.md`, `docs/INDEX.md`, and the development index for the agent guide.
- Documented why the root README path is reserved for fork owners and how GitHub README precedence supports it.
- Created `main-update` from the upstream-aligned baseline and combined the repository-structure, firmware-CI, and upstream-sync work.
- Corrected the merged documentation index, workflow path, project tree, and CI references.
- Moved CI documentation from software design to `docs/development/`.
- Moved fork-only documentation assets from `assets/docs/` to `docs/assets/`.
- Moved the upstream English/Chinese project READMEs under `docs/` and renamed the documentation catalog to `docs/INDEX.md`.
- Initialized `AGENTS.md`, `CLAUDE.md`, and `CHANGELOG.md`.
- Standardized the initial project README language filenames.
- Added the `docs/`, `assets/`, and `skills/` directory structure.
- Moved the upstream hardware guide into `docs/hardware-design/`.
- Standardized subdirectory README capitalization and introduced fork conventions.
- Allowed fork-owned root README and supplemental documentation content on fork `main`.
- Added and documented the fork-only supplemental-document directory.
- Moved the build CI document to its dedicated CI branch before consolidation.
- Documented clean-`main` reasons, the direct-development exception, and Actions enablement for forks.
- Split the original agent rules into contribution, development, and fork documents with a compact root index.
- Updated software-design and project README references for the new documentation structure.
- Added the documentation catalog and task-triggered routing based on the earlier repository model.
- Added bilingual contribution, code-of-conduct, security, and support documents tailored to this ESP-IDF and fork workflow.
