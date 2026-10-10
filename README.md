# latency-probe

Measures where the latency goes on an OpenIPC camera running majestic, without
an LED, a photodiode or clock sync between machines. It splits the delay into
two halves and measures each one on a single clock:

- **Camera half** (`probe/`): a static ARM binary that runs *on the camera*.
  It steps one sensor register over I2C — by default IMX335 analog gain — and
  timestamps the write and the arrival of every frame over loopback RTSP and
  `/ws/video`. It saves the stream, so the host can decode it and find the
  first frame the step changed. All timestamps come from the camera's own
  clock, so the network between you and the camera does not enter the result.
  A camera on the far side of a VPN measures the same as one on your desk.
- **Browser half** (`host/browser.py`): headless Chromium plays the camera
  with the camera's own WebUI player (`/a/preview.js`, `/a/preview-webrtc.js`)
  and times each frame from arrival to `expectedDisplayTime`
  (`requestVideoFrameCallback`).

The display after that (refresh, panel response) is outside both halves. Add it
for your monitor, or measure the whole chain once with a slow-motion camera
(see the methodology doc below).

## Quick start

```sh
make image                     # docker image: zig, PyAV, Chromium
make docker                    # builds probe/latprobe (static, ARMv7, musl)

# camera half: 45 s, a gain step every 0.8-1.2 s, scored on the host
./run.sh root@CAMERA baseline -d 45
./run.sh root@CAMERA lowdelay -d 45          # after setting isp.lowDelay: true

# browser half (H.264 streams; Playwright's Chromium has no H.265 decoder)
docker run --rm --network host --ipc=host -v $PWD:/w latency-probe \
    python host/browser.py CAMERA --mode webrtc
```

`run.sh` copies the probe to `/tmp` on the camera over ssh, runs it, fetches
`results/<tag>.txt` and `results/<tag>.es`, and prints:

```
bus=0 addr=0x1a reg=0x30e8 orig=0x00 hi=0x3c  codec h264
access units 2430, ws fragments 2430, steps 42, frame period 18.52 ms
  up n=21  write->loopback  min  84.6  p50  94.9  max 114.6 ms  | write->stamp p50  67.4 | stamp->loopback p50  27.1 ms
down n=21  write->loopback  min  68.9  p50  81.6  max 130.4 ms  | write->stamp p50  55.4 | stamp->loopback p50  26.1 ms
all frames: prft stamp -> /ws/video arrival  p50 25.1  p95 26.2 ms
```

- `write->loopback`: register write to the last packet of the first changed
  frame, received by a client on the camera. This is the camera half.
- `stamp`: majestic's capture timestamp for that frame (the `prft` box it puts
  on every `/ws/video` fragment). `stamp->loopback` is therefore the part
  majestic itself spends: ISP tail, VENC and packetising.
- Trust the `up` row more than `down`. Brightening is a clean single-frame
  step. Darkening sometimes coincides with a dropped frame, which spreads the
  distribution.

## Probe options

```
latprobe [-d SEC] [-b BUS] [-a ADDR] [-r REG] [-8] [-v VAL] [-s N] [-u B64] [-W] [-U PORT] [-o FILE] [-p MS]
  -d  run time, s (45)          -b  I2C bus (0)            -a  sensor address (0x1a)
  -r  register (0x30e8)         -8  8-bit register address -v  bright value (0x3c)
  -s  stream number (0)         -u  RTSP basic auth, base64 of user:pass
  -W  no /ws/video client       -o  stream file (/tmp/latprobe.es)
  -p  minimum ms between steps (800, plus a random 0-400)
  -U  take RTP over UDP on loopback PORT and PORT+1 instead of interleaved TCP
```

With `-U`, a datagram the probe misses leaves a partial frame behind and skews
the scoring, so check that the camera's UDP `RcvbufErrors` in `/proc/net/snmp`
did not move during the run. The probe forces a 4 MB receive buffer, which
needs root, because `net.core.rmem_max` on these cameras is 160 KB.

The probe reads the register at start and writes that value back on exit. For
a sensor other than IMX335, use its analog-gain register (`-r`, `-a`, `-b`) and
a value that makes the picture visibly brighter. The sensor driver's I2C table
(`*_cmos.c` in openhisilicon) lists the address and register.
Run with `aeMode: manual` so auto-exposure doesn't fight the step.

## What a register step does and doesn't measure

A gain write does not take effect on the next frame. The sensor applies it
on a frame boundary, and the HiSilicon IMX335 driver declares
`u8Cfg2ValidDelayMax = 2`, i.e. up to two frames later. A real change of light
appears in the frame being exposed when it happens. The absolute
`write->loopback` figure therefore overstates glass-to-wire by about one frame
period. Differences between two settings measured the same way are exact.
That's what the tool is for: A/B tests of sensor modes, `isp.lowDelay`,
codecs and resolutions, each over ~20 steps with randomised phase.

The probe uses `I2C_SLAVE_FORCE` on a bus the ISP also drives. A 3-byte write
between frames has been harmless on IMX335. Still, don't leave it running on a
camera that is doing real work.

## Results: gk7205v300 + IMX335, majestic master+b767dd9 (2026-10-08)

H.264, 4096 kbit, `profile: base`, exposure 8 ms manual, p50 of ~21 brightening steps.

| Sensor INI / stream | Setting | write → loopback | capture stamp → loopback |
|---|---|---|---|
| `high-fps/imx335_1920x1080_55fps.ini`, 1080p55 | default | 94.9 ms | 27.1 ms |
| same | `isp.lowDelay: true` | 77.6 ms | 17.5 ms |
| same | lowDelay + `video0.sliceUnits: 17` | 80.6 ms | 17.9 ms |
| `high-fps/imx335_1280x720_120fps.ini`, 720p120 | lowDelay | 48.0 ms | 15.7 ms |
| same sensor mode, `video0.fps: 60` | lowDelay | 63.0 ms | 11.1 ms |
| `5M_imx335.ini`, 2592x1944 (encoder caps it at 26 fps) | default | 230 ms | 60.5 ms |

Browser half, same camera at 1080p55 (headless Chromium, no monitor):

| Player | arrival → expected display, p50 |
|---|---|
| WebUI MSE (`/ws/video`) | 62.5 ms (buffer 60–82 ms ahead of the playhead) |
| same player, `playbackRate` 1.25 while > 30 ms is buffered | 19 ms |
| WebUI WebRTC | 16.8 ms (jitter buffer ~8 ms) |

What these show:

- **The WebUI's MSE player is the largest single cost.** It plays from wherever
  the buffer stood when playback began and only seeks to the live edge once the
  excess passes one second. WebRTC (the WebUI's default transport) doesn't do
  this.
- **`isp.lowDelay` saves about one frame.** majestic switches it off while
  `motionDetect` is enabled, and it rules out `storageSaver` frame dropping.
- **Slices don't make the camera send earlier.** On gk7205v300 and
  hi3516av300 the encoder releases a frame's slices together, once the whole
  frame is encoded. They can still cut latency at a receiver that decodes
  slice by slice, such as the OpenIPC/research `vdec` FPV receiver, which
  this harness does not measure: it times the last packet of each frame.
- **Fewer pixels at a higher rate win.** Every per-frame stage shortens with
  the frame period. Run the encoder at the sensor rate: 720p120 encoded at
  60 fps is slower than encoded at 120.
- **Full 5M on this SoC is slow.** VI goes offline, the encoder tops out at
  26 fps, and VI drops frames for want of buffers. Because majestic drives the
  ISP at the encoder's rate, `imx335_2592x1944_45fps.ini` (boost-1944p) falls
  back to the stock 30 fps sensor mode, and its latency gain never materialises.

## Layout

```
probe/latprobe.c   camera side, C, no dependencies beyond libc
host/analyze.py    decode the saved stream, find the step, print the split
host/browser.py    WebUI player in Chromium: mse, mse-chase, webrtc
run.sh             ssh copy, run, fetch, analyze
Dockerfile         python + PyAV + Chromium + zig (cross-compiler)
```

Methodology background, the full pipeline diagram and other measurement
methods are in
[openhisilicon/docs/sensor-pipeline-latency-metering.md](https://github.com/OpenIPC/openhisilicon/blob/main/docs/sensor-pipeline-latency-metering.md).

Written for [OpenIPC/firmware#2551](https://github.com/OpenIPC/firmware/issues/2551).
