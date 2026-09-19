# Volumetric frame prototype 09

This build preserves diagnostic logs across injection sessions. It does not
change rendering or implement a performance fix.

Each game keeps its current log at
`%APPDATA%/UnrealVRMod/<game executable>/log.txt`. On the next backend
initialization, the previous log is archived under that game's `logs/`
directory before a fresh `log.txt` is opened. Archives are uniquely named;
there is no automatic deletion. This also preserves the last log from older
builds. If archiving fails, logging appends instead of erasing the previous log.
Existing scripts that read `log.txt` continue to work.

Archive names use the time they were archived, for example
`logs/log-20260918-212732-123.txt`, with a numeric suffix on collisions. The
timestamps inside the file identify the original session. Archives are created
on the next injection, so the newest session remains in `log.txt` after exit.

Use the injector in `volumetric-frame-prototype-09` for this behavior. Older
prototype packages still overwrite their current logs. A running game must be
restarted and injected with the new package to load this change. Performance
Diagnostics remains a per-game setting; enable it before comparative tests.

## Deep Rock Galactic follow-up

The user test captured in
`build/diagnostics/drg-user-repeat-2026-09-18-212207/` has Native Stereo Fix off
and constant combined source/output dimensions of 5376×2880. Logged engine
counter progression was:

| Applied settings | Interval | Counter increments/s |
| --- | --- | ---: |
| Neither optimization | 21:19:12.837–21:19:28.886 | 71.90 |
| Crop and pixel reduction | 21:19:30.893–21:19:44.966 | 55.00 |
| Crop only | 21:19:46.976–21:20:03.047 | 71.99 |

The pixel checkbox was already enabled when cropping was enabled, so both
became active together. This repeats the reduction penalty, though less
severely than the earlier 72→21→72 run. These are sampled engine counters,
not GPU milliseconds or independently measured headset FPS. The near-72
baseline prevents this run from establishing a crop-only benefit.

The user also reports desktop spectator warping with head movement. The
existing D3D12 spectator path samples the game texture using the full-eye
rectangle; it does not use the portal resolve's source/output rectangle
mapping. That can explain desktop distortion separately from headset
correctness and performance. No spectator fix is included here.

Dynamic per-eye view sizes remain a performance hypothesis, not an established
root cause. Investigate stable/equal view dimensions and measure engine and
submission costs separately; also evaluate alternatives to this rendering
approach. Resizing and virtual viewing-distance features remain deferred.

## Validation

Release x64 build passes. The standalone production-helper regression verifies
consecutive sessions, preservation of an existing archive on a forced name
collision, and append fallback when the archive directory cannot be created.
The parent reran the regression successfully. No game was launched for this
logging-only change while the user was testing other games.

From an x64 Visual Studio developer command prompt:

```bat
cl /nologo /EHsc /std:c++latest tests\session_logging.cpp /Febuild\session-logging-test.exe /Fobuild\session-logging-test.obj
build\session-logging-test.exe
```
