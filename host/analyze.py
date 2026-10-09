#!/usr/bin/env python3
"""Score a latprobe run: decode the saved stream, find the first frame each register step changed.

usage: analyze.py RUN.txt RUN.es [--json OUT.json]

Prints, per step direction, the time from the register write to:
  * majestic's capture stamp (prft) of the first changed frame
  * that frame's last RTP packet arriving over loopback RTSP
and the capture-stamp -> loopback time on its own.
"""
import argparse, json, sys
import av
import numpy as np


def load(run):
    R, S, W, codec, info = [], [], [], 'h264', ''
    for line in open(run):
        a = line.split()
        if not a:
            continue
        if a[0] == 'C':
            codec = a[1]
        elif a[0] == 'I':
            info = ' '.join(a[1:])
        elif a[0] == 'R' and a[6] == '1':
            R.append(dict(tf=float(a[1]), t=float(a[2]), ts=int(a[3]), sz=int(a[4]), key=int(a[5])))
        elif a[0] == 'S':
            S.append(dict(t0=float(a[1]), t1=float(a[2]), v=int(a[3])))
        elif a[0] == 'W' and float(a[3]) > 0:
            W.append(dict(t=float(a[1]), sz=int(a[2]), prft=float(a[3])))
    return R, S, W, codec, info


def luma(es, codec):
    """Mean luma per access unit; NaN where one does not decode (a datagram
    lost on the way in), so indices still line up with the R lines."""
    c = av.open(es, format=codec)
    out = []
    for pkt in c.demux(video=0):
        if pkt.size == 0:
            continue
        try:
            frames = pkt.decode()
        except av.error.InvalidDataError:
            frames = []
        if not frames:
            out.append(float('nan'))
            continue
        for f in frames:
            p = f.planes[0]
            y = np.frombuffer(p, np.uint8).reshape(-1, p.line_size)[::4, :f.width:4]
            out.append(float(y.mean()))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('run')
    ap.add_argument('es')
    ap.add_argument('--json')
    ap.add_argument('--min-step', type=float, default=8.0, help='ignore steps that move mean luma less than this')
    o = ap.parse_args()

    R, S, W, codec, info = load(o.run)
    Y = luma(o.es, codec)
    n = min(len(R), len(Y))
    for i in range(n):
        R[i]['y'] = Y[i]
    R = R[:n]
    period = float(np.median(np.diff([r['ts'] for r in R]))) / 90.0
    print(f'{info}  codec {codec}')
    bad = sum(1 for r in R if r['y'] != r['y'])
    print(f'access units {len(R)} ({bad} undecodable), ws fragments {len(W)}, steps {len(S)}, frame period {period:.2f} ms')

    wt = np.array([w['t'] for w in W]) if W else None
    res = []
    for s in S:
        pre = [r for r in R if r['t'] < s['t0']][-5:]
        post = [r for r in R if r['t'] >= s['t0']][:15]
        if len(pre) < 3 or len(post) < 10:
            continue
        y0 = float(np.nanmedian([r['y'] for r in pre]))
        y1 = float(np.nanmedian([r['y'] for r in post[-4:]]))
        if abs(y1 - y0) < o.min_step:
            continue
        mid = (y0 + y1) / 2
        first = next((r for r in post if (r['y'] - mid) * (y1 - y0) > 0), None)
        if first is None:
            continue
        d = dict(dir='up' if y1 > y0 else 'down', loopback=(first['t'] - s['t0']) * 1e3,
                 first_pkt=(first['tf'] - s['t0']) * 1e3)
        if W:
            w = W[int(np.argmin(np.abs(wt - first['t'])))]
            d['stamp'] = (w['prft'] - s['t0']) * 1e3
        res.append(d)

    for dr in ('up', 'down'):
        v = [r for r in res if r['dir'] == dr]
        if not v:
            continue
        lb = np.array([r['loopback'] for r in v])
        fp = np.array([r['first_pkt'] for r in v])
        line = (f'{dr:>4} n={len(v):2d}  write->loopback  min {lb.min():5.1f}  p50 {np.median(lb):5.1f}  max {lb.max():5.1f} ms'
                f'  (first packet p50 {np.median(fp):5.1f})')
        if W:
            st = np.array([r['stamp'] for r in v])
            line += f'  | write->stamp p50 {np.median(st):5.1f} | stamp->loopback p50 {np.median(lb - st):5.1f} ms'
        print(line)
    spread = np.array([(r['t'] - r['tf']) * 1e3 for r in R])
    print(f'all frames: first -> last RTP packet of a frame  p50 {np.median(spread):.2f}  p95 {np.percentile(spread, 95):.2f} ms')
    if W:
        d = np.array([(w['t'] - w['prft']) * 1e3 for w in W])
        print(f'all frames: prft stamp -> /ws/video arrival  p50 {np.median(d):.1f}  p95 {np.percentile(d, 95):.1f} ms')
    if o.json:
        json.dump(dict(info=info, codec=codec, period_ms=period, steps=res), open(o.json, 'w'), indent=1)


if __name__ == '__main__':
    sys.exit(main())
