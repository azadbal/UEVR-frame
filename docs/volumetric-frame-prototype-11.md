# Volumetric frame prototype 11

Prototype 11 keeps the prototype 10 benchmark sequence and adds runtime CPU
call timing for `xrWaitFrame`, `xrBeginFrame`, and `xrEndFrame`. The new
records have explicit setting epochs and report interval boundaries, counts,
means, maxima, slow-call counts, failures, and retries. The comparison reporter
includes only intervals wholly within a measured phase with matching requested
settings. It keeps settings epochs separate.

The build used for this instrumentation is:

```text
Backend SHA256: BEBA9D96F4184640B44B53E35A275BCD887E05CB8D530F72ECEC95704887E9D4
```

The unchanged FluidFlux 3x repeat archive is
`build/diagnostics/frame-benchmark-2026-09-18-223442-666`. It passed with
profiles restored, owned processes closed, 75 matched crop probes, and 6,798
scoped GPU timing samples. The sequence was
`baseline,fixed,reduced,fixed,reduced,baseline`, with fixed scale 0.74.

The paired engine-delta results were:

| Condition | Repeat 1 median / p95 ms | Repeat 2 median / p95 ms |
|---|---:|---:|
| Baseline | 21.808 / 24.556 | 21.984 / 24.964 |
| Fixed 0.74 | 17.031 / 19.975 | 16.981 / 19.930 |
| Live-sized reduction | 17.098 / 24.598 | 17.103 / 33.291 |

These are engine tick deltas, not GPU time or displayed headset frame time.
The fixed and live-sized conditions have comparable total active area in this
pose, but equal area does not imply equal sampling: fixed scaling reduces
horizontal sampling while increasing the active vertical extent. The result
does not qualify visual quality or image equivalence; capture acceptance is
still separate.

The new runtime records add evidence about CPU wall time inside the three
OpenXR calls. They do not measure the whole frame, GPU work, Unreal scene
rendering, locks outside those calls, swapchain work, or display time. Slow
call counts can reflect runtime or GPU waits, so they localize a timing class
without assigning a cause.

Logging is synchronous and flushes at info level while the existing OpenXR
lock is held. Call durations exclude that logging, but it can affect later
cadence; no zero-overhead or whole-engine profiling claim is made.

## FluidFlux: instrumented repeat

Archive `build/diagnostics/frame-benchmark-2026-09-18-225040-671` passed all
five phases at 3x, with eight-second warmups and 25-second measurements.
Profiles were restored, owned processes closed, and 62 submission probes and
4,974 scoped GPU samples passed validation. Backend hash is the one above.

| Phase | Engine median / p95 ms | Ticks >27.78 ms |
|---|---:|---:|
| Baseline | 24.436 / 27.378 | 14 / 1025 |
| Live-sized reduction | 19.205 / 35.659 | 246 / 1290 |
| Fixed 0.74 | 19.117 / 21.777 | 0 / 1311 |
| Live-sized repeat | 19.170 / 26.352 | 32 / 1303 |
| Baseline repeat | 24.475 / 27.594 | 23 / 1022 |

The first live-sized phase reproduces the tail regression while the instrumented
OpenXR calls remain short: maximum wait/begin/end durations were
11.618/0.593/1.166 ms. Fixed maxima were 11.924/0.344/0.490 ms. No calls in
the included intervals exceeded twice the runtime's 13.889 ms predicted period.
UEVR submission GPU means total approximately 1.99 ms for both live and fixed,
versus 1.12 ms for baseline. These measurements do not establish the source of
the long engine intervals: swapchain availability, UEVR lock waits, Unreal work,
and scheduling remain outside the scopes. Do not subtract CPU/GPU means from
engine percentiles or assume one-to-one frame correspondence.

The fixed condition again has roughly 22% shorter median engine intervals than
its same-session baseline, with a steadier tail. It is still not quality-matched
and is not a proven universal optimization.

## Deep Rock Galactic: stationary Space Rig repeat

Archive `build/diagnostics/frame-benchmark-2026-09-18-224239-926` passed
evidence validation and cleanup with the backend hash above. It used 1x
resolution (2688x2880 per eye), Virtual Desktop Fix forced on, Native Stereo
Fix off, and fixed scale 0.8. Each phase had 10 seconds of warmup and 40 seconds
of measurement, in order `crop,reduced,fixed,reduced,fixed,crop`. The pose was
stationary and captures were disabled; this run does not validate moving-head
behavior or image quality. Profiles were restored and owned processes closed.

| Condition | Repeat 1 engine median / p95 ms | Repeat 2 engine median / p95 ms | Ticks >27.78 ms, repeat 1 / 2 |
|---|---:|---:|---:|
| Projection crop only | 13.964 / 14.845 | 13.860 / 15.035 | 0 / 0 |
| Live-sized reduction | 13.892 / 15.035 | 13.885 / 15.023 | 1 / 0 |
| Fixed 0.8 | 13.894 / 14.952 | 13.876 / 15.017 | 0 / 0 |

The measured windows stayed near 72 engine ticks/s; the prior sustained
intermittent slowdown was not reproduced in these windows. The first reduced
phase still had a 36.119 ms engine tick and one measured `xrEndFrame` call of
32.172 ms. Separate warmup calls reached 98.550 ms and 38.410 ms immediately
before that measurement window; the reporter correctly excludes them. The
OpenXR call durations locate blocking inside a runtime call, not its root cause
or whole-frame GPU cost. No runtime-call failures or retries occurred in the
included report intervals.

All eight slow runtime calls in the archive were `xrEndFrame` calls; summary
counts equal the logged-event counts, so none were hidden by the slow-log cap.
The threshold was twice the 13.889 ms predicted display period.

| Period | Logged slow `xrEndFrame` durations, ms |
|---|---|
| Before benchmark phase 1 | 39.701, 44.023, 99.821, 85.730, 64.030 |
| First reduced phase, warmup | 98.550, 38.410 |
| First reduced phase, measurement | 32.172 |
| Both crop phases, both fixed phases, second reduced phase; warmup and measurement | None |

Thus transient hitches were observed, including substantial blocking inside
`xrEndFrame` during reduced warmup. This localizes those stalls to a call
boundary without proving that the runtime, rather than work it waits for,
caused them; it is distinct from reproducing the earlier sustained slowdown.

Before this run, the benchmark's per-tick gameplay-ready-file read was changed
to a cached confirmation. That harness change and the added instrumentation
limit comparisons with prototype 10; they do not establish why an earlier
slowdown disappeared. This repeat does not show that the intermittent DRG issue
is solved or that fixed sizing improves performance or preserves visual quality.

## Decision and next discriminating experiment

Keep both pixel reduction and fixed sizing experimental/default-off. The next
rendering experiment should freeze each eye's own crop dimensions at a reference
pose, rather than impose one shared scale. At that pose this preserves existing
per-axis sampling and eye asymmetry, separating size changes from the current
fixed-scale sampling tradeoff. Compare live versus frozen dimensions first;
alignment and shared-eye sizing are separate experiments. A scripted portal-size
sweep can stress resizing without requiring physical headset movement, but must
be labeled a synthetic workload and run identically for each policy.

The next timing boundary is swapchain acquire/wait/release: existing
`cpu_image_wait_reset` begins after `xrWaitSwapchainImage` returns. This is a
smaller diagnostic change than adding Unreal render hooks, and should cover
HUD/framework images as well as the world image. Runtime blocking can itself
reflect queued GPU work; it must not automatically be called a runtime bug.

## Validation and reproduction

Release x64 build, production crop/view-state regression, 25 Python benchmark
tests, and PowerShell parser checks passed. The comparison reporter is read-only:

```powershell
python tests/report_frame_comparison.py build/diagnostics/frame-benchmark-2026-09-18-225040-671 --refresh-hz 72
python tests/report_frame_comparison.py build/diagnostics/frame-benchmark-2026-09-18-224239-926 --refresh-hz 72 --control crop
```

The runner now enables DRG spectator view temporarily for agent-controlled menu
navigation. It also detects and stops only crash reporters started by the tested
DRG session at its expected executable path, without submitting reports, and
marks such a run failed even if measurements completed. The new crash-reporter
branch was parser/review checked; this DRG run exited cleanly, so that failure
branch was not exercised live. A reporter left by the prior prototype-10 session
blocked Steam relaunch and was closed without sending before this run.
