#!/usr/bin/env python3
"""Arrival -> display latency of the camera's own WebUI player, in Chromium.

usage: browser.py CAMERA_HOST [--mode mse|mse-chase|webrtc] [--seconds 45] [--headed] [--js DIR]

Loads the camera's /a/preview*.js into a page on the camera's origin and plays
stream 0 the way the WebUI does. For every presented frame it records
requestVideoFrameCallback metadata and reports, p50/p95:

  mse        WebSocket fragment arrival -> expectedDisplayTime (matched by tfdt)
  mse-chase  the same player, with playbackRate nudged to 1.25 while more than
             30 ms is buffered ahead of currentTime (a live-edge chaser)
  webrtc     receiveTime (last RTP packet of the frame) -> expectedDisplayTime

Only the browser half is measured, so the network between the host and the
camera does not enter the numbers. The display's own scan-out and panel lag
are not included: headless Chromium has no monitor.
"""
import argparse, asyncio, json, os
import numpy as np
from playwright.async_api import async_playwright

HOOK = r"""
(() => {
  window.__arr = []; window.__pres = []; window.__stats = [];
  const tfdt = (u) => {
    for (let i = 0; i + 16 < u.length; i++)
      if (u[i+4] === 0x74 && u[i+5] === 0x66 && u[i+6] === 0x64 && u[i+7] === 0x74) {
        const dv = new DataView(u.buffer, u.byteOffset + i + 12);
        return u[i+8] ? Number(dv.getBigUint64(0)) : dv.getUint32(0);
      }
    return -1;
  };
  const WS = window.WebSocket;
  window.WebSocket = function (url, p) {
    const w = new WS(url, p);
    if (/\/ws\/video/.test(url))
      w.addEventListener('message', (e) => {
        if (typeof e.data !== 'string') window.__arr.push([performance.now(), tfdt(new Uint8Array(e.data))]);
      });
    return w;
  };
  window.WebSocket.prototype = WS.prototype;
  Object.assign(window.WebSocket, WS);
})();
"""

START = """(mode) => {
  const v = document.createElement('video');
  v.muted = true; v.autoplay = true; v.playsInline = true; v.width = 960;
  document.body.appendChild(v);
  const M = mode === 'webrtc' ? MajesticWebRTC : MajesticVideo;
  window.__player = M.attach(v, { stream: 0, iceServers: [], onStats: (s) => window.__stats.push(s),
                                  onState: (s, d) => console.log('state', s, d || '') });
  if (mode === 'mse-chase')
    setInterval(() => {
      try { const ahead = v.buffered.end(v.buffered.length - 1) - v.currentTime;
            v.playbackRate = ahead > 0.03 ? 1.25 : 1.0; } catch (x) {}
    }, 50);
  const cb = (now, md) => {
    window.__pres.push([now, md.mediaTime, md.expectedDisplayTime, md.receiveTime || 0]);
    v.requestVideoFrameCallback(cb);
  };
  v.requestVideoFrameCallback(cb);
}"""


def report(mode, r):
    pres = r['pres'][50:]  # skip start-up
    if mode == 'webrtc':
        d = np.array([p[2] - p[3] for p in pres if p[3]])
        what = 'receiveTime -> expectedDisplay'
    else:
        arr = {t: a for a, t in r['arr'] if t >= 0}
        d = []
        for scale in (1000000, 90000, 1000):
            d = np.array([p[2] - arr[round(p[1] * scale)] for p in pres if round(p[1] * scale) in arr])
            if len(d) > 20:
                break
        what = 'fragment arrival -> expectedDisplay'
    if len(d) < 20:
        print(f'{mode}: too few matched frames ({len(d)})')
        return
    print(f'{mode:9s} {what}: p50 {np.median(d):.1f}  p95 {np.percentile(d, 95):.1f} ms  (n={len(d)})')
    st = r['stats']
    if mode != 'webrtc':
        b = [s.get('bufferedMs') for s in st if s.get('bufferedMs') is not None]
        if b:
            print(f'          buffered ahead of playhead: first {b[0]:.0f} ms, last {b[-1]:.0f} ms')
        if st and st[-1].get('stalls') is not None:
            print(f'          stalls {st[-1]["stalls"]}')
    elif st and st[-1].get('jbEmitted'):
        print(f'          jitter buffer delay avg {1000 * st[-1]["jbDelay"] / st[-1]["jbEmitted"]:.1f} ms')


async def run(o):
    if o.js and not os.path.isfile(os.path.join(o.js, 'preview.js')):
        # Falling back file by file is for a checkout that lacks a script it
        # does not change; without preview.js, the player under test, a run
        # would quietly measure the camera's own.
        raise SystemExit(f'--js {o.js}: no preview.js there; refusing to measure the camera\'s player instead')
    async with async_playwright() as p:
        b = await p.chromium.launch(headless=not o.headed, args=['--autoplay-policy=no-user-gesture-required'])
        pg = await b.new_page()
        states = []
        pg.on('console', lambda m: states.append(m.text))
        await pg.goto(f'http://{o.camera}/favicon.ico')
        await pg.evaluate(HOOK)
        for js in ('preview-signal.js', 'preview.js', 'preview-webrtc.js'):
            local = os.path.join(o.js, js) if o.js else None
            if local and os.path.exists(local):
                await pg.add_script_tag(content=open(local).read())
            else:
                await pg.add_script_tag(url=f'http://{o.camera}/a/{js}')
        await pg.evaluate(START, o.mode)
        await asyncio.sleep(o.seconds)
        r = await pg.evaluate('() => ({arr: __arr, pres: __pres, stats: __stats})')
        await b.close()
    fell = [x for x in states if x.startswith(('state mjpeg', 'state fallback'))]
    if fell:
        print(f'{o.mode}: the player gave up ({fell[0][6:]}). Playwright\'s Chromium cannot decode H.265;'
              ' measure an H.264 stream, or run --headed with a browser that can.')
        return
    if o.json:
        json.dump(r, open(o.json, 'w'))
    report(o.mode, r)


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('camera')
    ap.add_argument('--mode', default='mse', choices=('mse', 'mse-chase', 'webrtc'))
    ap.add_argument('--js', help='load the preview*.js found in this directory instead of the camera\'s '
                    '(a WebUI checkout\'s www/a), to measure a player change before it ships')
    ap.add_argument('--seconds', type=int, default=45)
    ap.add_argument('--headed', action='store_true')
    ap.add_argument('--json')
    asyncio.run(run(ap.parse_args()))
