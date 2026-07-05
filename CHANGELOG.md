# Changelog

All notable changes to slim2diretta are documented in this file.

## v1.4.13 (2026-07-05)

### Added

- **Freeze diagnostic watchdog (rapid-seek freeze investigation)** — v1.4.12's non-blocking `recv()` fix helped (the audio thread now reaches the HTTP-connect stage that earlier builds froze before) but did **not** fully close the rapid-seek freeze. Two fresh logs show playback still wedges right after `[HTTP] Stream connected`, before the format is decoded — and, decisively, the 10 s format-detection stall timeout never fires, which proves the thread is **blocked** (not spinning) *before* it reaches that loop. The remaining stuck call is one of a small set of candidates that asynchronous logging cannot disambiguate, and the affected user runs GentooPlayer (no SSH, no gdb), so a normal backtrace isn't available. This release adds a self-contained diagnostic: a low-priority watchdog thread times how long each Slimproto stream-command handler has been running and, if one stalls beyond 4 s (a handler never legitimately runs that long), dumps **every thread's backtrace straight into the log** — the same output stream the normal logs use — so the exact frozen frame is captured with no external tooling. Built with `-rdynamic` so application and library frames are named directly in the dump. The watchdog has **no effect on the audio path** and only acts on an actual stall; it can be turned off at configure time with `-DENABLE_FREEZE_WATCHDOG=OFF`. There is no functional change to playback in this release — it exists to capture the final piece of the freeze diagnosis.

## v1.4.12 (2026-07-03)

### Fixed

- **Playback freeze after rapid seeks — the actual root cause (blocking `recv()` racing `disconnect()`)** — v1.4.11's SDK-control serialization was a real fix but did **not** stop PEETR's freeze (confirmed on v1.4.11). Two fresh logs re-pointed the diagnosis: `[DirettaSync] ========== OPEN ==========` is logged synchronously (`std::endl`, always flushed), yet the frozen audio thread never logged it → it was stuck **before `open()`**, in the HTTP-read phase, not in SDK control. Root cause: `HttpStreamClient::readRaw()` did a **blocking** `recv(m_socket, …, 0)`. It is only ever reached via `readWithTimeout()`, which already `poll()`ed the socket — but `m_socket` has no lock, so on a rapid seek the Slimproto thread's `STRM_STOP` handler calls `httpStream->disconnect()` (closes the fd) and the next `connect()` can reuse that fd number, all **between** the `poll()` and the `recv()`. The blocking `recv()` then wedges on the swapped/empty socket forever, so the audio thread never returns to check `audioTestRunning`, and the unbounded `audioTestThread.join()` in the seek handler froze the whole Slimproto loop (log ends on `[Seek] Waiting for audio thread to finish...`), requiring a service restart. This is an HTTP-socket race entirely separate from the v1.4.11 SDK-control race — both were real, but only this one caused the freeze — which is exactly why the trigger is seeks **< ~2 s apart** (the only way to hit the close/reuse window). Fix: `readRaw()` now uses `recv(…, MSG_DONTWAIT)` and treats `EAGAIN`/`EWOULDBLOCK` as "no data this tick, retry". Since `readRaw()` is reached only after a successful `poll()`, this never loses data on the normal path; it simply guarantees the call can no longer block, so the audio thread always returns promptly to observe a stop. Header reads (`getResponseHeaders()`) use a separate recv path and are unchanged. (A poll-gated read should be non-blocking regardless — the blocking `recv()` was a latent bug.)

## v1.4.11 (2026-07-02)

### Fixed

- **Playback freeze after rapid seeks — the real fix (serialize SDK control)** — the v1.4.9 format-detection timeout did **not** resolve PEETR's freeze (confirmed on v1.4.10, LMS 9.1.1 + Qobuz). Two real logs pinned the true cause. It is **not** a stalled stream — it is a **concurrency race with no serialization**: on a manual seek the `STRM_STOP` handler runs in the Slimproto thread and calls `httpStream->disconnect()` + `stopPlayback(true)` (SDK `stop()`) **while the just-started audio thread is concurrently inside `DirettaSync::open()`** doing SDK `stop()`/`clear()`/`play()`. `open()` and `stopPlayback()` shared no lock, so two threads drove the SDK control state at once → a deadlocked/stuck SDK call → the audio thread never returned → the unbounded `audioTestThread.join()` in the seek handler then froze the whole Slimproto loop (log ends on `[Seek] Waiting for audio thread to finish...`), requiring a service restart. The trigger is exactly what PEETR reported: seeks **less than ~2 s apart** (fast enough to interleave the two threads); slower seeks never froze. Fix: a `std::recursive_mutex m_controlMutex` in `DirettaSync` held by every playback-control entry point (`open`, `stopPlayback`, `pausePlayback`, `resumePlayback`, `startPlayback`, `release`), so the SDK's control state is never mutated by two threads at once — a concurrent `stopPlayback()` now waits for an in-progress `open()` instead of racing it. Recursive so internal calls between these paths don't self-lock; the control mutex is the outermost lock and the SDK worker / ring-access barriers never take it, so worker joins and reconfigure barriers always complete (no new deadlock). This lives in the shared `diretta/` layer, so it also hardens any other front-end (e.g. DirettaRendererUPnP) against the same class of concurrent-control race. The v1.4.9 format-detection timeout is retained as an independent safety net.

## v1.4.10 (2026-07-01)

### Fixed

- **CPU affinity: cores silently rejected as invalid on AMD Ryzen with HT/SMT disabled** (reported by Kiran on a Ryzen 7730U running AudioLinux). `std::thread::hardware_concurrency()` returns the *count* of online CPUs (8 on a Ryzen 7730U with HT off), which slim2diretta incorrectly used as the maximum valid CPU ID. With SMT disabled, Linux takes odd-numbered logical CPUs offline and keeps even-numbered ones active (0, 2, 4, ..., 14) — so cores 10, 12, 14 are perfectly valid but were rejected as `>= 8`. The validation silently cleared `config.cpuAudio/Decode/Other`, meaning per-thread `pthread_setaffinity_np` calls were never made; only the AudioLinux cpuset slice provided coarse confinement. New `getOnlineCpus()` helper reads `/sys/devices/system/cpu/online` (e.g. `"0,2,4,6,8,10,12,14"`) and validates against actual set membership instead. Falls back to `0..N-1` if the file is unreadable (containers, older kernels). Error message now shows the real online CPU list. Same fix applied to DirettaRendererUPnP v2.5.8.

## v1.4.9 (2026-06-25)

### Fixed

- **Playback freeze after repeated rapid seeks (required a service restart)** — reported by PEETR (Roon/LMS + GentooPlayer rpi5, FLAC 96 kHz). After fast-forwarding / rewinding a few times, playback would stop and only a `slim2diretta` service restart recovered it. Root cause: the audio thread's HTTP-read / format-detection loop had **no overall timeout**. On a rapid seek, LMS occasionally leaves the new HTTP stream connected but delivers no data (`isConnected()` stays true, `readWithTimeout()` returns 0), so `httpEof` never trips and no format is ever detected — the loop spins forever waiting for data that never arrives. `open()` is never reached (no audio), and any tracks queued by subsequent seeks pile up unconsumed, so playback appears frozen. The log signature is telling: it stops right after `[HTTP] Stream connected (status 200)`, before `[FLAC] Format`. Fix: a stall safety net in the format-detection phase — if no decodable format is produced within 10 s of connecting, the thread logs a warning, sends `STMn` (so LMS knows the stream failed), disconnects the dead HTTP stream, clears any pending track, and exits cleanly. The next `strm-s` then cold-starts fresh, so the player self-recovers instead of needing a manual restart. The window is large enough never to false-trigger on normal playback (a working stream yields its format in well under a second) and only fires on a genuinely dead stream. Applies to the PCM/FLAC path (where it was reported); the native-DSD path has the same latent gap and can get the same guard later if needed.

## v1.4.8 (2026-06-24)

### Fixed

- **Boot hang: Target stuck in stale idle-mode when powered on before slim2diretta** — port of DirettaRendererUPnP PR #79 (by hoorna/Alfred). When a Diretta Target has been idle for more than a few minutes before slim2diretta starts, it can enter a stuck idle-mode: it accepts the SDK connection but never reaches a streaming-capable state — LEDs blink fast indefinitely and no LMS renderer can claim it. Unlike DirettaRendererUPnP (which had a boot warmup since commit `0d1279b`), slim2diretta performed no boot pre-connect at all, so the first `open()` on the first play request would find the Target stuck. The fix adds the same boot warmup pattern: `open(PCM 44.1kHz/24-bit/2ch)` → `stopPlayback()` → `sleep(6 s)` → `release()`, giving the Target time to exit its stuck state before the first LMS play command arrives. The first real play then does a fresh cold connect. Trade-off: boot is ~6 s longer, and the first play pays a small cold-connect cost; both are acceptable vs. a renderer that cannot claim the Target at all.
- **Version string in binary was stuck at 1.4.6** — the v1.4.7 version bump updated `CMakeLists.txt` but not `src/main.cpp`; the `--version` flag and startup log therefore still showed `1.4.6`. Fixed as part of this bump to 1.4.8.

## v1.4.7 (2026-06-17)

### Fixed

- **Web UI: duplicate `KEY=VALUE` lines accumulating in the config on every save** — symmetric port of DirettaRendererUPnP PR #75 (by hoorna/Alfred); the two projects share a byte-identical `webui/config_parser.py`. `ShellVarConfig.save()` matched assignment lines with `^#?\s*([A-Z_][A-Z0-9_]*)=`, so a commented-out *example* line such as `#NICE_LEVEL=-10` was treated as an active setting and rewritten as active; and with no `key not in written_keys` guard, every occurrence of a key was rewritten rather than just the first — so repeated web-UI saves of the shell-var (process-priority) settings could grow duplicate active lines. The fix matches only active (uncommented) assignments, updates the first occurrence per key, drops later active duplicates, and preserves commented example lines untouched. New `webui/test_config_parser.py` adds stdlib-`unittest` regression coverage (5/5 pass, no new deps). slim2diretta's `install.sh` does not do DirettaRendererUPnP's per-key migration `sed` (it copies the shipped default wholesale with a `.bak` backup), so the second half of PR #75 (the bounded migration `sed`) does not apply here — only the `config_parser.py` fix was needed.

### Fixed

- **DoP: extend the no-interruption fix to pause/resume** — v1.4.5 fixed the crackle on manual seek / fast-forward / stop (confirmed by daniellyk8: "the Roon fast forward cracking is gone now"), but **pause then play still cracked**. Same root cause, different code path: `pausePlayback()` called the SDK `stop()` and `resumePlayback()` called `play()`, so a pause broke the continuous DoP marker stream exactly like the seek used to → the DAC dropped DoP lock → crack on resume. Now both are DoP-aware (gated behind `isDoP`, like `stopPlayback()` in v1.4.5):
  - **`pausePlayback()`** no longer stops the SDK for DoP — it just sets the paused flag and keeps the SDK running. `getNewStream()` gained a DoP pause branch that emits continuous DoP silence (no ring pop) for the whole pause, so the DAC holds DoP lock.
  - **`resumePlayback()`** no longer calls `play()` for DoP — the SDK was never stopped. It discards the stale paused buffer under the reconfigure barrier and lets it re-prefill; the marker stream stays continuous through the gap (DoP silence), so there is no crack.
  - PCM / native DSD keep the existing `stop()`/`play()` pause-resume path. With this, all manual transport actions under Roon (seek, FF, stop, pause) preserve DoP lock; LMS remains clean.

## v1.4.5 (2026-06-09)

### Fixed

- **DoP: the real fix for the manual-seek crackle (Roon), and a v1.4.4 regression** — testing of v1.4.4 (daniellyk8) showed LMS was now clean but Roon still cracked **and** playback could hang after a manual fast-forward (ring stuck at the 3035 ms prefill, needing a fresh track to recover). Root cause analysis: on a manual seek / FF / stop, Roon sends `strm-q` (→ `stopPlayback()`) then `strm-s`. `stopPlayback()` called the SDK `stop()`, which **breaks the continuous DoP marker stream before the quick-resume even runs** — so no quick-resume change could save it. That upstream stop, not the silence content, was the crack. (Automatic gapless never calls `stopPlayback()`, which is why it was always clean; LMS uses the gapless path, so LMS was unaffected. The v1.4.4 hang was its own bug: the DoP quick-resume skipped `play()`, so when the SDK *had* been stopped upstream nothing restarted it.) Fixes, all gated behind `isDoP`:
  - **`stopPlayback()` is now DoP-aware**: for DoP it no longer stops the SDK. It discards buffered audio under the reconfigure barrier (so a later resume can't replay stale data) while keeping the SDK running, so `getNewStream()` keeps emitting continuous DoP silence and the marker stream **never breaks across a seek**. A genuine stop (no `strm-s` follows) is handled by the existing 5 s idle `release()` → `close()`, which stops the SDK cleanly (its shutdown silence is DoP-valid). The DAC therefore never drops DoP lock on manual track changes → no crack.
  - **Quick-resume hang fixed**: the DoP fast path now restarts the SDK with `play()` only if it was actually stopped (`m_playing == false`), so it can never hang, while preserving the uninterrupted path when the SDK is still running.
  - Retains the v1.4.4 **continuous marker phase** (`writeDopMarkers()`), which already eliminated the residual tick under LMS.
- PCM and native DSD playback paths are unchanged.

## v1.4.4 (2026-06-08)

### Fixed

- **DoP: eliminate the crackle on manual track change / fast-forward / rewind** — follow-up to v1.4.3, which made transition silence valid DoP but did **not** stop the crack (confirmed by daniellyk8 on the v1.4.3 binary). Since the silence was then byte-identical to real DoP audio yet still cracked, the cause was an **interruption of the continuous DoP stream**, not byte content. A DoP DAC auto-detects DoP from the *continuous* alternating `0x05`/`0xFA` marker stream; any break makes it drop DoP lock and re-acquire → pop. (Native DSD is immune: the Diretta layer signals "DSD" out-of-band, so the DAC never auto-detects.) Confirmed by the user: the crack happens **only** on manual stop/skip/fast-forward (which restart the stream), **never** on automatic gapless album playback (which keeps streaming) — and was much milder under LMS than Roon. Two fixes, both strictly gated behind `isDoP` (no change to PCM / native DSD):
  1. **Transition without interrupting the stream.** The same-format quick-resume path used `stop()` → `clear()` → `play()` on every manual transition, halting frame delivery and breaking the marker stream. For DoP, the SDK is now kept running and the ring is swapped under the reconfigure barrier instead: while reconfiguring, `getNewStream()` emits continuous DoP silence without touching the ring, so `clear()` stays race-free **and** the marker stream never breaks. PCM and native DSD keep the proven `stop()/play()` sequence.
  2. **Continuous marker phase.** Every output frame's marker byte (the 24-bit MSB) is now rewritten to a strictly alternating `0x05`/`0xFA` sequence — for **both** silence and popped audio — via a persistent parity shared across all `getNewStream` paths. This removes the one-frame marker phase break that occurred at every silence↔audio junction (the residual faint tick heard under LMS). The marker carries no audio (DSD is in the low 16 bits) and the ring stays frame-aligned (whole stereo frames from track start), so rewriting the parity of real audio is safe and bit-transparent.

## v1.4.3 (2026-06-06)

### Fixed

- **DoP: valid DoP silence at track transitions (no more crackle)** — reported by daniellyk8 (Roon + slim2diretta → licensed Diretta target, on Audiolinux). With a DoP source (Roon sends DSD as DoP over Slimproto, format code `p`), a crack/pop was intermittently audible *just before* the next track started on track change, stop, or fast-forward. Root cause: DoP is carried through the **24-bit PCM** path (`isDSD=false`, matching DirettaRendererUPnP), but the silence buffers injected at every transition — the quick-resume flush, shutdown silence, the 5 s idle release, underrun/rebuffering — were filled with plain `0x00`. For a DAC locked in DoP that is **not** valid silence: the `0x00` marker byte breaks DoP framing (markers must alternate `0x05`/`0xFA`) and `0x00` is not DSD idle (`0x69`), so the DAC briefly drops DoP lock and emits a full-scale crack. It was intermittent because the crack only occurs when a silence buffer actually reaches the DAC while it is still in DoP lock (timing-dependent). The fix makes the silence path **DoP-aware**: when the stream is DoP, `getNewStream()` now fills silence with a valid DoP idle pattern (24-bit LE packed, payload `0x69`, marker alternating `0x05`/`0xFA` per frame — exactly the form `detectDoP()` validates) instead of `0x00`. Plain PCM and native DSD paths are unchanged (PCM still uses `0x00`, native DSD still uses `0x69`), so there is no behavioural change for non-DoP playback. The DoP idle pattern is generated worker-thread-locally from the cached channel count (no shared buffer, no extra locking on the real-time path) and rebuilt only when the format changes.

## v1.4.2 (2026-06-05)

### Fixed

- **Web UI: comma-separated values are canonicalised at save time** — symmetric mirror of DirettaRendererUPnP v2.5.3 (the two web UIs share an identical Python codebase). A user typing `enp1s0, enp2s0` in the web UI used to persist that literal string with the space — natural for free-text input but cosmetically inconsistent with the documented `enp1s0,enp2s0` form. The shell launcher already trims defensively per-element (`tr -d ' '`), so this was not a functional bug; IRQ pinning and CPU affinity worked the same. The fix eliminates the cosmetic divergence on disk and protects any future consumer of `/etc/default/slim2diretta` that reads the file without trimming. Applied to the five known comma-list fields: `cpu-audio`, `cpu-decode`, `cpu-other`, `IRQ_INTERFACE`, `IRQ_CPUS` (full profile); `cpu-audio`, `cpu-decode`, `cpu-other` (minimal profile — IRQ tuning belongs to the downstream distro on that flavour). Idempotent: values that are already canonical pass through unchanged, so configs only get rewritten when the user explicitly opens and saves the form.

### Changed

- **Profile-driven normalization machinery in the web UI save handler** — setting JSON declarations gain an optional `"normalize"` field. The currently supported rule is `comma_list` (`re.sub(r'\s*,\s*', ',', value.strip())`), but the structure is in place for any future per-field input canonicalisation. Adding a future comma-list field to either profile is one line of JSON; no Python change needed. `save_settings()` builds the rule map from the active profile once, then normalises values before splitting between the CLI-opts (`CliOptsConfig.save`) and shell-vars (`ShellVarConfig.save`) paths — so both codepaths get clean values without each having to know about the rule.

## v1.4.1 (2026-06-04)

### Changed

- **`install.sh`: `--allowerasing` on the optional codec / FFmpeg `dnf install`**. Small UX improvement, no functional change to the slim2diretta binary itself. Fedora ships `ffmpeg-free` / `ffmpeg-free-devel` by default; users who have switched to `ffmpeg` / `ffmpeg-devel` from RPM Fusion (or vice-versa) used to hit a "conflicting requests" error when re-running `install.sh` with the codec install option enabled. `dnf` can now retire the conflicting package on its own, making the optional-codec step idempotent across FFmpeg flavours. Mirrors the equivalent change in DirettaRendererUPnP v2.5.2.

---

## v1.4.0 (2026-05-23)

### Added

- **`mlockall` at startup**: slim2diretta now calls `mlockall(MCL_CURRENT | MCL_FUTURE)` early in `main()` (just past the immediate-exit cases `--version` / `--list-targets`, before any DirettaSync setup or thread creation). All of the process's pages — code, heap, stack, and every page allocated thereafter — are locked into RAM for the lifetime of the process. No page of slim2diretta can be swapped out, evicted from the page cache, or trigger a major/minor page fault that would otherwise stall the audio thread despite SCHED_FIFO + CPU pinning + isolcpus. This is the same memory-locking discipline JACK and PipeWire perform in RT mode, and it closes the last non-deterministic source of stalls (memory pressure / cache reclaim) for a CONFIG_PREEMPT_RT + isolated-CPU host. Requires `CAP_IPC_LOCK` (running as root via the systemd unit suffices) and `LimitMEMLOCK=infinity` in `slim2diretta.service` — both already in place since v1.0. On `EPERM` (e.g. CLI run without privileges), a `LOG_WARN` is emitted and the binary continues; no behavioural change otherwise. RSS becomes a hard floor for the process — on this binary that's a few MiB and entirely negligible on any host running slim2diretta. The "Memory locked in RAM (mlockall MCL_CURRENT|MCL_FUTURE)" line is visible in the journal on every successful startup.

## v1.3.3 (2026-05-08)

### Added

- **`--cpu-decode` option** (mirrored from DirettaRendererUPnP v2.4.2 PR #68 by Daniel/Koala887): a third CPU-affinity granularity that pins the audio/decode thread (HTTP receive → decode → push to ring buffer) to its own dedicated core, separate from the Diretta SDK worker (`--cpu-audio`) and from the lighter main + slimproto threads (`--cpu-other`). When `--cpu-decode` is set, the audio/decode thread is also raised to `SCHED_FIFO` real-time priority (using `RT_PRIORITY`), since the dedicated core makes that safe. Falls back to `--cpu-other` when `--cpu-decode` is empty (no behavioural change for existing setups). Also exposed in the web UI (full and minimal profiles) under "CPU Affinity" as "Decode Core(s)". This brings slim2diretta in line with the same three-tier model now available in DirettaRendererUPnP.

### Fixed

- **Install script: stop service before replacing binary** (mirrored from DirettaRendererUPnP PR #69 by Daniel/Koala887): `install.sh` now detects whether `slim2diretta.service` is currently running, stops it before copying the new binary into `$INSTALL_BIN`, and restarts it once the install completes. Same root cause as the DRUP fix — `cp` cannot overwrite a file held open by systemd, so reinstalling on top of a running service silently left the old binary in place until the next reboot.

## v1.3.2 (2026-05-05)

### Changed

- **Diretta Host SDK 149 support**: Added `DirettaHostSDK_149` paths to the CMake auto-detection list (149 is preferred over 148, then 147 as fallback) and reworked the SDK version detection to parse `Host/Release.hpp` instead of hard-coding "148" / "147". The build now reports the actual `ReleaseNo` from the SDK headers (e.g. `SDK 149 headers detected`) and will keep working transparently with future SDK releases that preserve the same header layout. **No source code change was required** — the API differences in 149 (new `MACtype` enum, breaking signatures on `Connection` / `Find` 2-arg constructor / `sendMulti`, additive `clearExtFormat()`) do not impact the patterns used by slim2diretta, which constructs `DIRETTA::Find` via the unchanged 1-arg overload and never instantiates `Connection` directly.

## v1.3.1 (2026-05-03)

### Added

- **Minimal web UI profile** (`webui/profiles/slim2diretta_minimal.json`): An alternative profile alongside the existing `slim2diretta.json`, intended for downstream distributions that manage system-level tuning through their own framework (GentooPlayer, AudioLinux, etc.). The minimal profile drops everything that's wrapper-level system tuning — the entire "Advanced System & Network Tuning" group (SMT toggle, NIC link tuning, IRQ affinity) and the wrapper-level Process Priority shell vars except `RT_PRIORITY` (which is application-level via `--rt-priority` and remains exposed). It keeps everything that's strictly slim2diretta application configuration: target, server, name, decoder, verbose, CPU affinity, buffer sizes, RT priority, and Diretta SDK options. Distributions can simply point their packaging at the `_minimal.json` profile instead of the default one. The full profile remains the default for self-install on a generic Linux distribution.
- **2.5 GbE option in `TARGET_SPEED`**: The "Advanced System & Network Tuning" web UI dropdown now includes a `2500 Mbit (2.5 GbE)` choice alongside the existing 10 / 100 / 1000 options, for hosts equipped with 2.5 GbE NICs (Realtek RTL8125, Intel I225/I226, etc.). `ethtool` will refuse the value if the underlying NIC doesn't support it, and the launcher already logs a warning in that case — no functional change to the wrapper itself. The `slim2diretta.default` comment is updated accordingly.
- **Minimal tarball release script** (`scripts/make-minimal-tarball.sh`): At release time, this script produces a `*-minimal.tar.gz` source archive from the current HEAD (or any tag passed as argument) where `webui/profiles/slim2diretta.json` is the minimal profile content and `slim2diretta_minimal.json` is removed. Intended to be uploaded as an additional asset on each GitHub Release alongside the standard tarball, so downstream distributors who ship by consuming the source archive (GentooPlayer, AudioLinux, etc.) can pick the minimal flavor without any packaging-side modification. By default the script also strips the " (Minimal)" suffix from `product_name` for a clean web UI label; set `STRIP_SUFFIX=0` to keep it.

## v1.3.0 (2026-04-30)

### Added

- **Multi-core CPU affinity**: `--cpu-audio` and `--cpu-other` now accept a comma-separated list of cores (e.g., `--cpu-audio 3,4`). When multiple cores are provided, the kernel scheduler may distribute the thread between them. Single-core syntax (e.g., `--cpu-audio 3`) remains fully supported. The SDK itself still receives only the first core of the list (SDK limitation). Aligned with DirettaRendererUPnP v2.3.0.
- **Configurable buffer settings**: New CLI options to tune the Diretta ring buffer and prefill duration, exposed in the web UI under a new "Buffer Configuration" section:
  - `--pcm-buffer-seconds <s>` — PCM buffer size in seconds (default 0.5s)
  - `--dsd-buffer-seconds <s>` — DSD buffer size in seconds (default 0.8s)
  - `--pcm-prefill-ms <ms>` — PCM prefill duration in ms (default 80ms)
  - `--dsd-prefill-ms <ms>` — DSD prefill duration in ms (default 200ms)
- **Advanced System & Network Tuning** (mirrored from DirettaRendererUPnP v2.4.0): three wrapper-level audiophile-tuning features grouped in a new web UI section, all exposed as shell vars in `/etc/default/slim2diretta` and applied by `start-slim2diretta.sh` before launching the binary:
  - **Target NIC link tuning** — `TARGET_INTERFACE` / `TARGET_SPEED` (10/100/1000) / `TARGET_DUPLEX` force the host NIC speed/duplex via `ethtool`. Some audiophile users perceive a sound-quality difference when constraining the link. The wrapper logs a warning and skips link tuning if `ethtool` is missing. Web UI and `.default` comments include a bandwidth-vs-format reminder so users don't pick a link speed too narrow for hi-res PCM or DSD512+. `ethtool` is added to the base dependency list installed by `install.sh` (dnf/apt/pacman).
  - **IRQ affinity** — `IRQ_INTERFACE` / `IRQ_CPUS` pin all hardware IRQs (including MSI-X queues) of one or more NICs to a specific CPU list at service start. `IRQ_INTERFACE` accepts a single name (e.g. `enp1s0`) or a comma-separated list (e.g. `enp1s0,enp2s0`) to cover hosts with separate NICs for the upstream LMS source and the Diretta target. Pairs naturally with `--cpu-audio` to keep network IRQ activity off the audio worker core, a known source of jitter on busy LANs. Kernel-managed IRQs that refuse runtime reassignment are reported as "skipped" in the launcher log without failure.
  - **SMT (Hyper-Threading) toggle** — `SMT` accepts `on` / `off` / `forceoff` / empty. The wrapper writes the chosen value to `/sys/devices/system/cpu/smt/control` before launching slim2diretta, so any subsequent `--cpu-audio` / `--cpu-other` pinning sees the right topology. System-wide setting; non-persistent across kernel reboots (wrapper re-applies on each service start). BIOS lock or kernel-restricted control is detected and reported as a warning rather than a failure.

### Changed

- Config fields `cpuAudio` and `cpuOther` are now strings (previously `int`). Existing single-core values keep working unchanged.

## v1.2.8 (2026-04-18)

### Added

- **Web UI Stop button**: Added a Stop button alongside the existing Save & Restart and Restart Only buttons. Useful for users running slim2diretta on their own Linux distributions to stop the service directly from the web UI — e.g., to release the Diretta target for another player or before maintenance. Includes a confirmation dialog.

---

## v1.2.7 (2026-04-16)

### Added

- **Main thread CPU affinity**: The main thread is now also pinned to the `--cpu-other` core when CPU affinity is configured. Previously it was the only thread left unpinned, still managed by the OS scheduler. (PR #9, contributed by sheviks)

---

## v1.2.6 (2026-04-14)

### Added

- **Resilient Diretta target discovery at startup**: Instead of exiting when the target is not found, the player now retries every 2s and logs every 5s until the target becomes available (or the process is cancelled with Ctrl+C). Makes the player robust to targets that start later, are temporarily unavailable, or are restarted — especially important on systems without systemd auto-restart (e.g., GentooPlayer with OpenRC). (Suggested by Filippo, GentooPlayer)

- **Clang + LTO build option**: New `ENABLE_LTO` CMake option enables `-flto` on compile and link flags. Recommended combination for best audio quality — multiple testers report a clearly preferable sound with clang+LTO builds. Three ways to enable:
  ```bash
  # Single-switch shortcut (clang + LTO + lld linker):
  env LLVM=1 ./install.sh -b
  #   or: LLVM=1 cmake ..

  # Manual CMake flags:
  CC=clang CXX=clang++ cmake -DENABLE_LTO=ON -DUSE_LLD=ON ..

  # Interactive prompt (auto-detects clang):
  ./install.sh
  ```
  The `LLVM=1` shortcut mirrors the convention used by the Linux kernel and DirettaRendererUPnP's Makefile. It automatically sets `CC=clang`, `CXX=clang++`, enables LTO, and uses the `lld` linker via `-fuse-ld=lld`. (Inspired by PR #8 from sheviks)

- **Verbose build output**: Set `VERBOSE=1` or `V=1` to make `install.sh` pass `VERBOSE=1` to make, showing the full compiler command lines. Useful for debugging build issues. (Inspired by PR #8 from sheviks)

- **CPU affinity (thread pinning)**: Two new CLI options and Web UI fields pin critical threads to specific cores, reducing jitter on systems with CPU isolation.
  - `--cpu-audio <core>`: pins the Diretta SDK worker thread (hot path). Automatically adds the `OCCUPIED` flag (bit 16) to the SDK thread mode and also pins the worker manually via `pthread_setaffinity_np` (belt-and-suspenders, because SDK pinning doesn't always work — e.g., on RPi 4).
  - `--cpu-other <core>`: pins the audio/decode thread (HTTP reader + decoder + ring buffer push) and the Slimproto TCP receive thread.
  - Exposed in the Web UI under a new "CPU Affinity" section.
  - Default: no pinning (-1). Aligns with the existing implementation in DirettaRendererUPnP.

- **Graceful fallback when DAC doesn't support native DSD**: Previously slim2diretta crashed with an unhandled exception when the Diretta target reported no DSD support. It now logs an error and skips the track instead of crashing. Users should configure LMS/Roon to send DoP (DSD over PCM) when native DSD is unavailable. (Reported by lalekuku)

## v1.2.5 (2026-04-11)

### Fixed

- **100% CPU spin in drain loop when target is auto-released**: When the audio thread was draining the decode cache after HTTP EOF and the Diretta target got auto-released (5s inactivity), `sendAudio` returned 0 forever and the drain loop spun at 100% CPU. The audio thread never terminated, blocking subsequent track transitions indefinitely. Fixed with two safeguards: bail out of the drain loop when the target is no longer open, and add a 5ms sleep on `framesWritten==0` as a defensive measure for any other transient zero-write conditions. (Reported by cmr75)

### Changed

- **Larger PCM buffer for CDN resilience**: Multiple users (katywu, Hoorna, Progman, Ikyo) reported intermittent buffer underruns when playing Qobuz streams via LMS or Roon, even for standard CD quality (16/44). Unlike DirettaRendererUPnP, slim2diretta cannot distinguish remote from local streams (both look like local Slimproto streams), so the larger buffer is applied to all PCM playback.
  - `PCM_BUFFER_SECONDS`: 0.5s → 3.0s (6x larger buffer)
  - `PCM_PREFILL_MS`: 50ms → 500ms
  - `PREFILL_MS_COMPRESSED`: 200ms → 800ms
  - `PREFILL_MS_UNCOMPRESSED`: 100ms → 500ms
  - `REBUFFER_THRESHOLD_PCT`: 20% → 50% (more resilient recovery after underrun)
  - Trade-off: ~500ms slower initial track start and longer recovery after CDN hiccups. Acceptable for streaming where latency doesn't matter like it does for local playback.

## v1.2.4 (2026-03-31)

### Fixed

- **Tracks skipping during gapless format transitions**: When `open()` failed during a gapless PCM format change (e.g., 44.1kHz→96kHz), slim2diretta sent STMn (error) followed by STMu (track ended), causing LMS to skip to the next track prematurely. STMu is now suppressed after an open failure. (Reported by Jeep972)

- **Worker thread join timeout**: Replaced bare `m_workerThread.join()` calls (which could block indefinitely) with `joinWorkerWithTimeout(1000ms)` in all format transition paths. Prevents hangs when the SDK worker thread is unresponsive during format changes.

## v1.2.3 (2026-03-21)

### Fixed

- **Roon: next track not starting after gapless transition**: When the decode cache drained and no next track arrived within the 2-second gapless wait, the ring buffer would underrun. Roon interprets underruns as errors and refuses to start the next track. Now stops playback gracefully with silence buffers before the ring buffer drains, so Roon receives a clean end-of-track signal (STMu) instead of an underrun. (Reported by PatrickW)

## v1.2.2 (2026-03-20)

### Performance

- **RT-safe async logging**: Hot-path logging in `sendAudio` and `getNewStream` callbacks now uses `snprintf` on a stack-local buffer (`DIRETTA_LOG_ASYNC_FMT`) instead of `std::ostringstream` which heap-allocates on every call. Zero heap allocation on the audio critical path. (Inspired by leeeanh's RT jitter reduction work)

- **PcmDecoder read-offset**: Replace `O(n)` `vector::erase` on every `readDecoded()` call with a read-offset (`m_dataPos`). Compaction only occurs when the offset exceeds 64 KB. Eliminates `memmove` from the PCM decode hot path. (Inspired by leeeanh's RT jitter reduction work)

### Fixed

- **install.sh: SDK path broken after `cd build`**: `find` could return relative paths (e.g., `./DirettaHostSDK_148`) which became invalid after `cd build`. Now resolved to absolute paths via `realpath`. (Reported by sheviks, [#6](https://github.com/cometdom/slim2Diretta/issues/6))

- **install.sh: "Failed to stop inactive.service"**: Option 4 (Update binary) tried to stop `inactive.service` when the service was already stopped, because `systemctl is-active` returns the text "inactive". Now uses `--quiet` flag to check exit code only.

- **install.sh: uninstall did not remove web UI**: The uninstall function now also stops/disables `slim2diretta-webui.service` and removes `/opt/slim2diretta/webui/` when installed.

- **No sound on 16-bit FLAC with 24-bit DACs**: The bit depth formula mapped 16-bit sources to 32-bit Diretta connections (`== 24` only matched 24-bit). DACs that report 32-bit support but physically only handle 24-bit produced no sound. Changed to `<= 24` so both 16-bit and 24-bit sources open at 24-bit. Only true 32-bit content opens at 32-bit.

## v1.2.1 (2026-03-18)

### Changed

- **Resilient LMS autodiscovery**: When no LMS server is specified (`-s`) and the server is not immediately discoverable, slim2diretta now retries indefinitely (every 2 seconds) instead of exiting with an error. This is consistent with the Diretta target discovery behavior and prevents startup failures when LMS is temporarily offline or still booting. Press Ctrl+C to cancel. (Suggested by Filippo/GentooPlayer)

### Fixed

- **32-bit WAV playback broken with FFmpeg decoder**: `FfmpegDecoder` bit depth detection fell back to 24-bit when FFmpeg did not populate `bits_per_raw_sample` for raw PCM codecs, causing `m_s32Shift = 8` which corrupted the audio data. For raw PCM, `m_rawBitDepth` (from the Slimproto strm command) is now used as the authoritative source, making 32-bit WAV files play correctly via FFmpeg. (Reported by Mani)

- **White noise on 24-bit DACs with FFmpeg decoder**: Two root causes fixed: (1) `audioFmt.bitDepth` was hardcoded to 32, causing the Diretta connection to open at 32-bit even for 24-bit sources — `main.cpp` now passes the actual source bit depth, and `configureSinkPCM` in DirettaSync now only offers 32-bit negotiation when the source is actually 32-bit; (2) FFmpeg's S32/S32P output is sign-extended (LSB-aligned), whereas all other decoders produce MSB-aligned int32_t — `FfmpegDecoder` now applies a left-shift (`32 - bitsPerRawSample`) to MSB-align samples before writing to the ring buffer. libFLAC was unaffected because its MSB-aligned output survived ALSA's 32→24-bit truncation correctly. (Reported by progman)

- **High-rate WAV files wrong sample rate with FFmpeg decoder**: WAV files at non-standard rates (e.g., 705600 Hz) were played at 44100 Hz when using `--decoder ffmpeg`. The FFmpeg raw PCM path relied on the Slimproto `rate` field which cannot encode rates above ~192kHz, while the native PcmDecoder correctly reads the WAV header. PCM/WAV/AIFF format (`format=p`) now always uses the native PcmDecoder regardless of `--decoder` setting. FFmpeg is only used for compressed formats (FLAC, MP3, AAC, OGG). (Reported by abase)

## v1.2.0 (2026-03-15)

### Added

- **Resilient target discovery**: When the Diretta target is not available at startup, the player now retries every 2 seconds (with status logged every 5 seconds) instead of exiting immediately. This is especially important on systems without systemd auto-restart (e.g., GentooPlayer with OpenRC). (Suggested by Filippo/GentooPlayer)

- **Graceful DSD fallback**: DACs that do not support native DSD via Diretta no longer cause a crash. The error is logged and the track is skipped, with a suggestion to configure LMS for DoP output. (Reported by lalekuku)

- **FFmpeg decoder backend** (`--decoder ffmpeg`): Alternative audio decoder using FFmpeg's libavcodec. Users can switch between the native backend (libFLAC/libmpg123/libvorbis) and FFmpeg for a different sonic signature. The native backend offers a brighter, more detailed sound; FFmpeg provides a warmer, more enveloping presentation with a wider soundstage. Both are lossless and theoretically bit-identical — the perceived difference likely stems from internal processing patterns (memory allocation, frame buffering, decode timing).
  - Parser-based architecture (no avformat needed) — lightweight, only libavcodec + libavutil
  - Supports FLAC, MP3, AAC, OGG, ALAC, PCM via a single unified decoder
  - DSD remains handled natively (raw bitstream, not decoded)
  - Selectable via CLI (`--decoder ffmpeg`) or Web UI (Decoder Backend dropdown)
  - Build-time optional: auto-detected, graceful fallback to native if FFmpeg not installed

- **Startup codec listing**: Build capabilities now show all available codecs and FFmpeg availability at startup

### Fixed

- **DSD64 DoP playback**: Fixed continuous ~485 Hz whistle tone when playing DSD64 via Roon (DoP). DoP frames are now passed through as 24-bit PCM to the Diretta Target, which handles DoP marker detection and DAC forwarding natively. Previously, `convertDopToNative()` destroyed the DoP markers causing frame misalignment. This matches the working behavior of squeeze2upnp→DirettaRendererUPnP. (Credit: hoorna, PR #4)

- **DSD128 DoP underruns**: Fixed systematic buffer underruns when Roon downsamples DSD128 to DSD64 DoP (176.4 kHz carrier). Ring buffer increased from 0.5s to 6.0s (12MB) for rates ≥176.4kHz, adaptive rebuffer threshold set to 50% (vs 20% normal) providing ~4.2s headroom after underrun, prebuffer increased to 3000ms for high-rate streams. The high-rate threshold was lowered from 192kHz to 176kHz to capture DSD64 DoP's 176.4kHz carrier. Roon's DSD128→DoP transcoding has a cold-start phase where throughput is ~35% of real-time; second play is fine. (Reported by hoorna)

- **FFmpeg 24-bit PCM decoding**: Fixed continuous decode errors (`Invalid PCM packet, data has size 2 but at least a size of 6 was expected`) when playing 24-bit content via FFmpeg backend. Three-part fix: (1) align raw PCM packets to `block_align` — for stereo 24-bit, 8192 % 6 = 2 remainder rejected by FFmpeg; (2) disable FFmpeg's PCM parser which splits data without respecting `block_align`; (3) explicitly set `block_align = channels × bytes_per_sample` before opening the codec, as FFmpeg leaves it at 0 when no demuxer is used. 16-bit was unaffected (8192 % 4 = 0). (Reported by progman)

- **Audio data loss in push loop**: Fixed `sendAudio` return value being ignored, causing `decodeCachePos` to advance past data that wasn't actually written to the ring buffer. Multi-chunk push (4×2048 frames) is now limited to high sample rates (>176kHz) where it's needed; normal rates use single 1024-frame push like v1.2.0.

- **FFmpeg gapless click**: Fixed audible click at gapless track transitions when using FFmpeg decoder. The FFmpeg parser (`av_parser_parse2`) buffers partial codec frames internally; at stream EOF, this buffered data was never flushed, losing the last few samples of each track. Now flushes the parser with `(NULL, 0)` before flushing the decoder, recovering the final frame.

### Build Dependencies

New optional dependency for FFmpeg backend:

| Distribution | Command |
|-------------|---------|
| **Fedora** | `sudo dnf install ffmpeg-free-devel` |
| **Ubuntu/Debian** | `sudo apt install libavcodec-dev libavutil-dev` |
| **Arch** | `sudo pacman -S ffmpeg` |

To disable FFmpeg backend: `cmake -DENABLE_FFMPEG=OFF ..`

---

## v1.1.1 (2026-03-11) — internal release, not published

### Fixed

- **Seek causes track skip with Roon**: When seeking within a track, Roon (Squeezebox mode) would skip to the next track instead of seeking. The audio thread was sending STMu (track ended) on forced stop (`strm-q`), which Roon interpreted as the new stream ending. Now STMu is only sent on natural end-of-track (HTTP EOF). LMS tolerated the spurious STMu; Roon's stricter state machine did not.

- **High sample rate buffer underruns (>192kHz)**: Adaptive buffer sizing for sample rates above 192kHz (352.8kHz, 384kHz, 768kHz, 1536kHz). LMS streams at ~1x real-time at these rates, leaving no margin with the previous 0.5s ring buffer. New behavior:
  - Ring buffer: 0.5s → 2.0s for rates >192kHz
  - Prebuffer: 500ms → 1500ms for rates >192kHz
  - SDK prefill: 1000-1500ms for rates >192kHz (vs 100-200ms)
  - MAX_BUFFER raised to 32MB (accommodates 1536kHz/32bit/2ch @ 2s)
  - Decode cache raised to 9.2M samples (~3s at 1536kHz stereo)
  - No change for rates ≤192kHz (identical behavior to v1.1.0)

- **FLAC metadata log spam**: FLAC format information was logged repeatedly (dozens of times) for tracks with large metadata blocks (album art). The STREAMINFO callback was re-triggered on each metadata retry. Now logged only once per track.

- **Player name with spaces ignored (webui)**: When the player name contained spaces (e.g. "Devialet Target"), the startup script lost the quoting and the second word became an unknown option. Fixed with `eval exec` to preserve quoted arguments from `SLIM2DIRETTA_OPTS`.

### Added

- **Build capabilities log at startup**: Displays architecture (x86_64/aarch64/arm) and SIMD support (AVX2/NEON/scalar) for easier remote diagnostics

---

## v1.1.0 (2026-03-06)

### Added

- **Gapless playback**: Seamless track transitions without audio gaps
  - PCM/FLAC: gapless chaining with format change detection
  - DSD (DSF/DFF): gapless chaining with automatic format negotiation
  - Audio thread stays alive between tracks — no Diretta reconnection needed

- **Seek support**: In-track seeking via LMS progress bar
  - FLAC: seek to any position (format detection from frame header when STREAMINFO absent)
  - DSD: seek to any position
  - Correct thread lifecycle management (seek vs gapless path detection)

- **Web Configuration UI (diretta-webui)**: Browser-based settings interface
  - Accessible at `http://<ip>:8081` — no SSH needed to configure slim2diretta
  - Edit all settings: LMS server, player name, verbose mode
  - Advanced Diretta SDK settings: thread-mode, transfer-mode, cycle-time, info-cycle, target-profile-limit, MTU
  - Save & Restart: applies settings and restarts the systemd service in one click
  - Zero dependencies beyond Python 3 (stdlib only)
  - Separate systemd service (`slim2diretta-webui.service`) — transparent for audio quality
  - Installable via `install.sh` option 7 or `./install.sh --webui`
  - Port 8081 to avoid conflict with DirettaRendererUPnP web UI (port 8080)

### Fixed

- **DSF padding silence**: Replace zero-padding in last DSF block with DSD silence (0x69) to eliminate click at track transitions
- **FLAC seek without header**: Fallback format detection from FLAC frame header when LMS sends seek streams without STREAMINFO metadata
- **Config parser**: Handle missing `/etc/default/slim2diretta` file (create on first save instead of crash)
- **Config parser**: Skip duplicate uncommented `SLIM2DIRETTA_OPTS=` lines on save

---

## v1.0.0 (2026-02-28)

### Added

- **ICY metadata handling**: Transparent stripping of ICY metadata from internet radio streams
  - Automatic detection of `icy-metaint` header in HTTP responses
  - Metadata blocks filtered out before audio data reaches decoders
  - Supports both `HTTP/1.x` and `ICY` protocol responses

- **Advanced Diretta SDK options**: Fine-tuning of the Diretta transport layer
  - `--transfer-mode <mode>`: Transfer scheduling mode (`auto`, `varmax`, `varauto`, `fixauto`, `random`)
  - `--info-cycle <us>`: Info packet cycle in microseconds (default: 100000)
  - `--cycle-min-time <us>`: Minimum cycle time for RANDOM mode
  - `--target-profile-limit <us>`: Target profile limit (0=SelfProfile, default: 200)
  - RANDOM transfer mode support with configurable min cycle time
  - TargetProfile / SelfProfile dual-path transfer configuration

- **Auto-release of Diretta target**: Coexistence with other Diretta applications
  - Automatically releases the Diretta target after 5 seconds of idle
  - Transparent re-acquisition on next play command from LMS/Roon
  - Allows DirettaRendererUPnP or other Diretta hosts to use the same target

- **Extended sample rate support**: PCM up to 1536kHz, DSD up to DSD1024
  - `MaxSampleRate` reported to LMS raised from 768kHz to 1536kHz
  - Extended Slimproto sample rate table: 705.6kHz, 768kHz, 1411.2kHz, 1536kHz
  - DSD1024 (45.2MHz) support (already handled by decoder, now documented)

- **SDK improvements**:
  - Changed from `MSMODE_MS3` to `MSMODE_AUTO` for better device compatibility
  - Correct info cycle parameter passed to `DIRETTA::Sync::open()` (was using cycle time)

---

## v0.2.0 - Test Version (2026-02-27)

### Added

- **MP3 decoding** via libmpg123 (LGPL-2.1)
  - Full streaming support for internet radio
  - Automatic ID3v2 tag handling
  - Error recovery with auto-resync (robust for radio streams)

- **Ogg Vorbis decoding** via libvorbisfile (BSD-3-Clause)
  - Streaming with custom non-seekable callbacks
  - Chained stream support (format changes between tracks)
  - OV_HOLE gap handling (normal for radio streams)

- **AAC decoding** via fdk-aac (BSD-like license)
  - ADTS transport for internet radio streams
  - HE-AAC v2 support (SBR + Parametric Stereo)
  - Automatic sample rate detection (handles SBR upsampling)
  - Transport sync error recovery

- **Optional codec system**: All new codecs are compile-time optional via CMake
  - `ENABLE_MP3=ON/OFF` (default: ON, auto-disabled if libmpg123 not found)
  - `ENABLE_OGG=ON/OFF` (default: ON, auto-disabled if libvorbis not found)
  - `ENABLE_AAC=ON/OFF` (default: ON, auto-disabled if fdk-aac not found)

- **LMS capabilities**: Player now advertises mp3, ogg, aac support to LMS
  - LMS sends native format streams instead of transcoding
  - Internet radio stations play directly

### Build Dependencies

New optional dependencies (install for full codec support):

| Distribution | Command |
|-------------|---------|
| **Fedora** | `sudo dnf install mpg123-devel libvorbis-devel fdk-aac-free-devel` |
| **Ubuntu/Debian** | `sudo apt install libmpg123-dev libvorbis-dev libfdk-aac-dev` |
| **Arch** | `sudo pacman -S mpg123 libvorbis libfdk-aac` |

---

## v0.1.0 - Test Version (2026-02-27)

Initial test release for validation by beta testers.

### Features

- **Slimproto protocol**: Clean-room implementation from public documentation (no GPL code)
  - Player registration (HELO), stream control (strm), volume (audg), device settings (setd)
  - LMS auto-discovery via UDP broadcast
  - Heartbeat keep-alive, elapsed time reporting
  - Player name reporting to LMS/Roon

- **Audio formats**:
  - FLAC decoding via libFLAC
  - PCM/WAV/AIFF container parsing with raw PCM fallback (Roon)
  - Native DSD: DSF (LSB-first) and DFF/DSDIFF (MSB-first) container parsing
  - DSD rates: DSD64, DSD128, DSD256, DSD512
  - DoP (DSD over PCM): automatic detection and passthrough as 24-bit PCM to Diretta Target
  - WAVE_FORMAT_EXTENSIBLE support for WAV headers

- **Diretta output** (shared DirettaSync v2.0):
  - Automatic sink format negotiation (PCM and DSD)
  - DSD bit-order and byte-swap conversion (LSB/MSB, LE/BE)
  - Lock-free SPSC ring buffer with SIMD optimizations
  - Adaptive packet sizing with jumbo frame support
  - Quick resume for consecutive same-format tracks

- **Operational**:
  - Systemd service template (`slim2diretta@<target>`) for multi-instance
  - Interactive installation script (`install.sh`)
  - Prebuffer with flow control (500ms target, adaptive for high DSD rates)
  - Pause/unpause with silence injection for clean transitions
  - SIGUSR1 runtime statistics dump

### Known Limitations (v0.1.0)

- Linux only (requires root for RT threads)
- No MP3, AAC, OGG, ALAC support (FLAC and PCM/DSD only)
- No volume control (bit-perfect: forced to 100%)
- No automated tests (manual testing with LMS + DAC)
