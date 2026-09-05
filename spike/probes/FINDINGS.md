# P1 slice-1 — iPlug2 4-output feasibility: empirical findings

Disposable probe artifacts. Method + raw results, so a reviewer can re-derive the
verdict without re-running. The iPlug2 clone / prebuilt deps are gitignored; the
probes below link the **vendored** RtAudio 6.0.1 (iPlug2 `Dependencies/IPlug/RTAudio`).

## Method

Compile the vendored RtAudio 6.0.1 source with the CoreAudio backend macro, link
the macOS CoreAudio frameworks:

```
clang++ -std=c++17 -D__MACOSX_CORE__ <probe>.cpp RtAudio.cpp -I. \
  -framework CoreAudio -framework CoreFoundation -framework AudioToolbox \
  -framework AudioUnit -framework CoreMIDI
```

Two probes: `enum_probe.cpp` (device enumeration) and `channel_open_probe.cpp`
(openStream at various channel counts, on the 2-out and 8-out devices).

## Question 1 — does the host's RtAudio enumerate on this machine? YES.

Use the **host's** API pattern (`getDeviceIds()` → `getDeviceInfo(realId)`), NOT a
0-based index:

```
RtAudio version: 6.0.1
getDeviceIds() count = 3
  id=129  Studio Display Microphone    in=1  out=0  defaultIn=y
  id=130  Studio Display Speakers      in=0  out=8  defaultOut=y
  id=131  Mac mini Speakers            in=0  out=2
```

The 8-out and 2-out devices both enumerate. This is **not** a Darwin 25
enumeration failure. An earlier claim that it was came from passing a 0-based
index to `getDeviceInfo`; RtAudio's CoreAudio internal IDs start at 129
(`currentDeviceId_ = 129`, RtAudio.cpp L652), so `getDeviceInfo(realId)` is
required.

## Question 2 — does openStream reject an over-device channel count? YES.

`channel_open_probe.cpp` (2-out Mac mini Speakers = id 131, native 2; 8-out
Studio Display = id 130, native 8):

```
2-out:  want 2 -> open OK, start OK
2-out:  want 4 -> open FAILS (error 10 INVALID_PARAMETER)
                     "the device (131) does not support the requested channel count"
2-out:  want 8 -> open FAILS
8-out:  want 2 -> open OK, start OK
8-out:  want 4 -> open OK, start OK      <- the WET+DRY (4-out) case
8-out:  want 8 -> open OK, start OK
8-out:  want 12 -> open FAILS
```

The stock iPlug2 APP host hardcodes `oParams.nChannels = MaxNChannels(ERoute::kOutput)`
(IPlugAPP_host.cpp L595, passed to openStream L618). On a 2-out device with
MaxNChannels=4, that openStream **fails** (clean error, not a silent 2-ch run).
=> the DeviceAdapter clamp (min(device-native, 4)) is **mandatory**, not optional:
without it the stock host cannot open a 2-out device.

## Caveat

- The crash seen when passing `bufferFrames=nullptr` is a probe bug (RtAudio
  6.0.1 CoreAudio opens dereference the buffer-size pointer). The host passes
  `&mBufferSize`, which is correct. Use a real buffer-size pointer.
- These tests exercise the RtAudio layer, which is what the host drives. They do
  not run the full IGraphics/UI standalone; that build is the next step if @Claude
  wants it, and would confirm the wiring end-to-end.
