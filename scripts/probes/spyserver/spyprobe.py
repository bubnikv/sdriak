"""SpyServer protocol probe (doc/todo/network-sources/spyserver/0-measurements.md).

One session per run against a public SpyServer, about a minute long:

  python spyprobe.py <host> <port> <outdir> main      questions 1-6 (and 7 when capped)
  python spyprobe.py <host> <port> <outdir> main2     FFT geometry against the IQ spectrum, dB scaling, IQ placement
  python spyprobe.py <host> <port> <outdir> scale     dB scaling and IQ placement with settled 6 s windows
  python spyprobe.py <host> <port> <outdir> shared    question 8: a second client without control
  python spyprobe.py <host> <port> <outdir> gainorder question 9: digital gain before / after STREAMING_MODE

Every message is logged with its arrival time and the number of IQ samples
received before it. TCP keeps the order, so a CLIENT_SYNC or PONG has an
exact position in the IQ stream. Raw IQ and FFT frames go to <outdir> as
.npz for spyanalyze.py; a summary is printed and written as JSON.

Stdlib plus numpy. Identifies as "sdriak-test". Keep to scripts/probes/README.md:
one session at a time, pauses between sessions, servers with free slots.
"""
import json
import os
import socket
import struct
import sys
import threading
import time

import numpy as np

PROTOCOL_VERSION = (2 << 24) | (0 << 16) | 1700
IDENT = b"sdriak-test"

CMD_HELLO, CMD_SET, CMD_PING = 0, 2, 3

STREAMING_MODE, STREAMING_ENABLED, GAIN = 0, 1, 2
IQ_FORMAT, IQ_FREQUENCY, IQ_DECIMATION, IQ_DIGITAL_GAIN = 100, 101, 102, 103
FFT_FORMAT, FFT_FREQUENCY, FFT_DECIMATION, FFT_DB_OFFSET, FFT_DB_RANGE, FFT_DISPLAY_PIXELS = 200, 201, 202, 203, 204, 205

MODE_IQ, MODE_FFT_IQ = 1, 5
FORMAT_UINT8, FORMAT_INT16 = 1, 2

MSG_DEVICE_INFO, MSG_CLIENT_SYNC, MSG_PONG = 0, 1, 2
MSG_INT16_IQ, MSG_UINT8_FFT = 101, 301

DEVICE_INFO_FIELDS = ["DeviceType", "DeviceSerial", "MaximumSampleRate", "MaximumBandwidth",
                      "DecimationStageCount", "GainStageCount", "MaximumGainIndex", "MinimumFrequency",
                      "MaximumFrequency", "Resolution", "MinimumIQDecimation", "ForcedIQFormat"]
SYNC_FIELDS = ["CanControl", "Gain", "DeviceCenterFrequency", "IQCenterFrequency", "FFTCenterFrequency",
               "MinimumIQCenterFrequency", "MaximumIQCenterFrequency",
               "MinimumFFTCenterFrequency", "MaximumFFTCenterFrequency"]
SETTING_NAMES = {0: "STREAMING_MODE", 1: "STREAMING_ENABLED", 2: "GAIN", 100: "IQ_FORMAT", 101: "IQ_FREQUENCY",
                 102: "IQ_DECIMATION", 103: "IQ_DIGITAL_GAIN", 200: "FFT_FORMAT", 201: "FFT_FREQUENCY",
                 202: "FFT_DECIMATION", 203: "FFT_DB_OFFSET", 204: "FFT_DB_RANGE", 205: "FFT_DISPLAY_PIXELS"}


def now():
    return time.monotonic()


class Spy:
    """One SpyServer connection with a receive thread."""

    def __init__(self, host, port, tag="c1"):
        self.tag = tag
        self.sock = socket.create_connection((host, port), timeout=10)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.lock = threading.Lock()
        self.t0 = now()
        self.events = []          # (t, kind, dict)
        self.iq_chunks = []       # int16 arrays (interleaved)
        self.iq_gain = []         # (sample index, flags) per IQ message
        self.iq_count = 0
        self.fft = []             # (t, iq index, seq, bins uint8)
        self.device = None
        self.sync = None
        self.syncs = []
        self.closed = None
        self.seq = {}
        self.seq_gaps = 0
        self.send(CMD_HELLO, struct.pack("<I", PROTOCOL_VERSION) + IDENT)
        self.th = threading.Thread(target=self._rx, daemon=True)
        self.th.start()

    def log(self, kind, **kw):
        with self.lock:
            self.events.append((now() - self.t0, kind, dict(kw, iq=self.iq_count)))

    def send(self, cmd, body=b""):
        self.sock.sendall(struct.pack("<II", cmd, len(body)) + body)

    def set(self, setting, value):
        self.log("set", setting=SETTING_NAMES.get(setting, setting), value=int(value))
        self.send(CMD_SET, struct.pack("<II", setting, int(value) & 0xFFFFFFFF))

    def ping(self):
        self.log("ping")
        self.send(CMD_PING)

    def _recvn(self, n):
        buf = bytearray()
        while len(buf) < n:
            chunk = self.sock.recv(n - len(buf))
            if not chunk:
                raise EOFError("closed by server")
            buf += chunk
        return bytes(buf)

    def _rx(self):
        self.sock.settimeout(30)
        try:
            while True:
                hdr = self._recvn(20)
                proto, mtype, stream, seq, size = struct.unpack("<5I", hdr)
                body = self._recvn(size) if size else b""
                kind, flags = mtype & 0xFFFF, mtype >> 16
                last = self.seq.get(stream)
                if last is not None and seq != (last + 1) & 0xFFFFFFFF:
                    self.seq_gaps += 1
                self.seq[stream] = seq
                if kind == MSG_INT16_IQ:
                    samples = np.frombuffer(body, dtype="<i2").copy()
                    with self.lock:
                        self.iq_gain.append((self.iq_count, flags))
                        self.iq_chunks.append(samples)
                        self.iq_count += len(samples) // 2
                elif kind == MSG_UINT8_FFT:
                    with self.lock:
                        self.fft.append((now() - self.t0, self.iq_count, seq, np.frombuffer(body, dtype=np.uint8).copy()))
                elif kind == MSG_DEVICE_INFO:
                    vals = struct.unpack("<%dI" % (len(body) // 4), body)
                    self.device = dict(zip(DEVICE_INFO_FIELDS, vals))
                    self.log("device_info", **self.device)
                elif kind == MSG_CLIENT_SYNC:
                    vals = struct.unpack("<%dI" % (len(body) // 4), body)
                    self.sync = dict(zip(SYNC_FIELDS, vals))
                    self.syncs.append(self.sync)
                    self.log("client_sync", **self.sync)
                elif kind == MSG_PONG:
                    self.log("pong", size=size)
                else:
                    self.log("message", type=kind, flags=flags, stream=stream, size=size)
        except Exception as e:  # noqa: BLE001 - any end of the stream is a result
            self.closed = repr(e)
            self.log("closed", reason=repr(e))

    def wait(self, pred, timeout):
        end = now() + timeout
        while now() < end:
            if pred():
                return True
            if self.closed:
                return False
            time.sleep(0.02)
        return pred()

    def iq(self):
        with self.lock:
            if not self.iq_chunks:
                return np.zeros(0, dtype=np.complex64)
            raw = np.concatenate(self.iq_chunks).astype(np.float32)
        return (raw[0::2] + 1j * raw[1::2]).astype(np.complex64)

    def fft_since(self, t):
        with self.lock:
            return [f for f in self.fft if f[0] >= t]

    def close(self):
        try:
            self.set(STREAMING_ENABLED, 0)
        except OSError:
            pass
        try:
            self.sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        self.sock.close()
        self.th.join(5)


def iq_stage(dev, target_rate=60000):
    """Smallest IQ rate at or above target_rate the server allows."""
    best = dev["MinimumIQDecimation"]
    for d in range(dev["MinimumIQDecimation"], dev["DecimationStageCount"]):
        if dev["MaximumSampleRate"] / (1 << d) >= target_rate:
            best = d
    return best


def setup(s, iq_freq, iq_dec, fft_freq, fft_dec, pixels=2048, db_offset=0, db_range=127, gain_first=False, digital_gain=None):
    s.set(IQ_FORMAT, FORMAT_INT16)
    s.set(IQ_DECIMATION, iq_dec)
    s.set(IQ_FREQUENCY, iq_freq)
    if gain_first and digital_gain is not None:
        s.set(IQ_DIGITAL_GAIN, digital_gain)
    s.set(FFT_FORMAT, FORMAT_UINT8)
    s.set(FFT_DB_OFFSET, db_offset)
    s.set(FFT_DB_RANGE, db_range)
    s.set(FFT_DISPLAY_PIXELS, pixels)
    s.set(FFT_FREQUENCY, fft_freq)
    s.set(FFT_DECIMATION, fft_dec)
    s.set(STREAMING_MODE, MODE_FFT_IQ)
    if not gain_first and digital_gain is not None:
        s.set(IQ_DIGITAL_GAIN, digital_gain)
    s.set(STREAMING_ENABLED, 1)


def strongest_carrier(frames, span, centre, guard):
    """Frequency of the strongest narrow peak across recent FFT frames,
    away from the edges and from DC."""
    stack = np.stack([f[3].astype(np.float32) for f in frames])
    mean = stack.mean(axis=0)
    n = len(mean)
    floor = np.median(mean)
    lo, hi = int(n * guard), int(n * (1 - guard))
    cand = mean.copy()
    cand[:lo] = 0
    cand[hi:] = 0
    cand[n // 2 - 3:n // 2 + 4] = 0
    k = int(np.argmax(cand))
    freq = centre - span / 2 + (k + 0.5) * span / n
    return freq, float(cand[k] - floor), k


def save(outdir, name, s, summary):
    os.makedirs(outdir, exist_ok=True)
    iq = s.iq()
    with s.lock:
        fft_t = np.array([f[0] for f in s.fft])
        fft_iq = np.array([f[1] for f in s.fft])
        fft_bins = [f[3] for f in s.fft]
        gains = np.array(s.iq_gain)
    sizes = sorted({len(b) for b in fft_bins})
    pad = max(sizes) if sizes else 0
    fft_arr = np.zeros((len(fft_bins), pad), dtype=np.uint8)
    for i, b in enumerate(fft_bins):
        fft_arr[i, :len(b)] = b
    fft_len = np.array([len(b) for b in fft_bins])
    np.savez_compressed(os.path.join(outdir, name + ".npz"), iq=iq, fft_t=fft_t, fft_iq=fft_iq, fft=fft_arr,
                        fft_len=fft_len, gains=gains)
    with open(os.path.join(outdir, name + ".json"), "w") as f:
        json.dump({"summary": summary, "events": s.events}, f, indent=1, default=str)


def t_main(host, port, outdir):
    s = Spy(host, port)
    s.wait(lambda: s.device is not None and s.sync is not None, 10)
    dev, sync = s.device, s.sync
    print("device", dev)
    print("sync  ", sync)
    if not sync or not sync["CanControl"]:
        print("no control; stopping")
        s.close()
        return
    centre = sync["DeviceCenterFrequency"]
    iq_dec = iq_stage(dev)
    iq_rate = dev["MaximumSampleRate"] / (1 << iq_dec)
    span0 = dev["MaximumBandwidth"]
    setup(s, centre, iq_dec, centre, 0)
    s.wait(lambda: len(s.fft) > 20 and s.iq_count > iq_rate, 8)

    # A carrier for the retune test: strongest peak of the full FFT span,
    # inside the IQ slide limits.
    t_found = now() - s.t0
    fc, snr, kbin = strongest_carrier(s.fft_since(t_found - 1.5), span0, centre, 0.08)
    print(f"carrier {fc:.0f} Hz, {snr:.0f} counts above the floor (bin {kbin})")
    # The carrier at +5 kHz, retunes of 2 kHz; less on a capped (narrow) IQ.
    offset = min(5000.0, 0.2 * iq_rate)
    step = int(round(min(2000.0, 0.1 * iq_rate)))
    base = int(round(fc - offset))

    # Q1-3: retune the IQ six times; PING right after each.
    s.set(IQ_FREQUENCY, base)
    s.ping()
    time.sleep(1.5)
    for i in range(6):
        s.set(IQ_FREQUENCY, base + (step if i % 2 == 0 else 0))
        s.ping()
        time.sleep(1.5)
    s.set(FFT_FREQUENCY, base)
    s.ping()
    time.sleep(1.0)
    s.set(FFT_DECIMATION, 1)
    s.ping()
    time.sleep(1.0)

    # Q5: FFT span per stage, carrier position (FFT centred 3 % of the span off the carrier).
    stages = {}
    for stage in (0, 2, 4):
        span_bw = dev["MaximumBandwidth"] / (1 << stage)
        fcent = int(round(fc - 0.03 * span_bw))
        s.set(FFT_FREQUENCY, fcent)
        s.set(FFT_DECIMATION, stage)
        t = now() - s.t0
        time.sleep(2.0)
        frames = s.fft_since(t + 0.7)
        if frames:
            stack = np.stack([f[3].astype(np.float32) for f in frames if len(f[3]) == len(frames[0][3])])
            k = int(np.argmax(stack.mean(axis=0)))
            stages[stage] = {"fft_centre": fcent, "pixels": int(len(frames[0][3])), "peak_bin": k, "frames": len(frames),
                             "fps": len(frames) / 1.3}
    print("stages", stages)

    # Q4: dB scaling. Back to stage 2 centred on the carrier.
    s.set(FFT_FREQUENCY, int(round(fc)))
    s.set(FFT_DECIMATION, 2)
    scaling = []
    for off, rng in ((0, 127), (20, 127), (0, 60), (-20, 100)):
        s.set(FFT_DB_OFFSET, off & 0xFFFFFFFF)
        s.set(FFT_DB_RANGE, rng)
        t = now() - s.t0
        time.sleep(1.8)
        frames = s.fft_since(t + 0.6)
        if frames:
            stack = np.stack([f[3].astype(np.float32) for f in frames if len(f[3]) == len(frames[0][3])])
            mean = stack.mean(axis=0)
            scaling.append({"offset": off, "range": rng, "peak_byte": float(mean.max()), "floor_byte": float(np.median(mean))})
    print("scaling", scaling)

    # Q6: requested pixel counts.
    pixels = {}
    for px in (1000, 4096, 2048):
        s.set(FFT_DISPLAY_PIXELS, px)
        t = now() - s.t0
        time.sleep(1.5)
        frames = s.fft_since(t + 0.5)
        pixels[px] = sorted({len(f[3]) for f in frames})

    # Q7: slide limits, from CLIENT_SYNC; try a step beyond them.
    limit_probe = None
    if s.sync:
        hi = s.sync["MaximumIQCenterFrequency"]
        s.set(IQ_FREQUENCY, hi + 100000)
        s.ping()
        time.sleep(1.2)
        limit_probe = {"requested": hi + 100000, "sync_after": s.sync}
    s.ping()
    time.sleep(0.5)

    summary = {"host": host, "port": port, "device": dev, "first_sync": sync, "syncs": len(s.syncs),
               "iq_decimation": iq_dec, "iq_rate": iq_rate, "carrier_hz": fc, "carrier_snr": snr, "base": base, "offset": offset, "step": step,
               "stages": stages, "scaling": scaling, "pixels": pixels, "limit_probe": limit_probe,
               "seq_gaps": s.seq_gaps, "closed": s.closed, "iq_samples": s.iq_count, "fft_frames": len(s.fft)}
    s.close()
    save(outdir, f"main_{host}_{port}", s, summary)
    print(json.dumps(summary, indent=1, default=str))


def t_main2(host, port, outdir):
    """Second session: FFT geometry against the IQ spectrum, dB scaling one
    parameter at a time, IQ placement relative to the device passband."""
    s = Spy(host, port)
    s.wait(lambda: s.device is not None and s.sync is not None, 10)
    dev, sync = s.device, s.sync
    if not sync or not sync["CanControl"]:
        print("no control; stopping")
        s.close()
        return
    centre = sync["DeviceCenterFrequency"]
    iq_dec = iq_stage(dev)
    iq_rate = dev["MaximumSampleRate"] / (1 << iq_dec)
    # IQ and FFT on the same centre, a little off the device's, so that a
    # strong station is not at DC.
    c = int(centre - 0.4 * iq_rate)
    setup(s, c, iq_dec, c, 0)
    s.wait(lambda: s.iq_count > iq_rate, 8)
    marks = []

    def window(label, seconds, **kw):
        t = now() - s.t0
        i = s.iq_count
        time.sleep(seconds)
        marks.append(dict(kw, label=label, t0=t + 0.6, t1=now() - s.t0, iq0=i + int(0.6 * iq_rate), iq1=s.iq_count))

    for stage in range(0, min(6, dev["DecimationStageCount"])):
        s.set(FFT_DECIMATION, stage)
        s.set(FFT_FREQUENCY, c)
        window("stage", 2.0, stage=stage, fft_centre=c)
    s.set(FFT_DECIMATION, 2)
    for rng in (127, 100, 80, 60):
        s.set(FFT_DB_OFFSET, 0)
        s.set(FFT_DB_RANGE, rng)
        window("scale", 1.5, offset=0, range=rng)
    for off in (10, 20, 30):
        s.set(FFT_DB_OFFSET, off)
        s.set(FFT_DB_RANGE, 100)
        window("scale", 1.5, offset=off, range=100)
    s.set(FFT_DB_OFFSET, 0)
    s.set(FFT_DB_RANGE, 127)
    # IQ placement: inside the alias-free band, past it, past the device passband.
    s.set(FFT_DECIMATION, 0)
    s.set(FFT_FREQUENCY, c)
    time.sleep(1.0)
    for frac in (0.3, 0.47, 0.75):
        f = int(c + frac * dev["MaximumSampleRate"])
        s.set(IQ_FREQUENCY, f)
        s.ping()
        window("place", 1.5, iq_freq=f, frac=frac)
    summary = {"host": host, "port": port, "device": dev, "first_sync": sync, "iq_decimation": iq_dec,
               "iq_rate": iq_rate, "centre": c, "marks": marks, "syncs": s.syncs, "seq_gaps": s.seq_gaps,
               "closed": s.closed}
    s.close()
    save(outdir, f"main2_{host}_{port}", s, summary)
    print(json.dumps({k: v for k, v in summary.items() if k != "marks"}, indent=1, default=str))


def t_scale(host, port, outdir):
    """dB scaling and IQ placement with settled windows: settings take effect
    one to three seconds late and frames arrive in bursts, so each setting is
    held 6 s and only its last 2.5 s are measured."""
    s = Spy(host, port)
    s.wait(lambda: s.device is not None and s.sync is not None, 10)
    dev, sync = s.device, s.sync
    if not sync or not sync["CanControl"]:
        print("no control; stopping")
        s.close()
        return
    centre = sync["DeviceCenterFrequency"]
    iq_dec = iq_stage(dev)
    iq_rate = dev["MaximumSampleRate"] / (1 << iq_dec)
    c = int(centre - 0.4 * iq_rate)
    setup(s, c, iq_dec, c, 2)
    s.wait(lambda: s.iq_count > iq_rate, 8)
    marks = []

    def window(label, **kw):
        time.sleep(3.5)
        t = now() - s.t0
        i = s.iq_count
        time.sleep(2.5)
        marks.append(dict(kw, label=label, t0=t, t1=now() - s.t0, iq0=i, iq1=s.iq_count))

    for off, rng in ((0, 127), (0, 100), (0, 80), (0, 60), (10, 100), (20, 100), (30, 100), (0, 127)):
        s.set(FFT_DB_OFFSET, off)
        s.set(FFT_DB_RANGE, rng)
        window("scale", offset=off, range=rng)
    s.set(FFT_DB_OFFSET, 0)
    s.set(FFT_DB_RANGE, 127)
    s.set(FFT_DECIMATION, 0)
    for frac in (0.0, 0.3, 0.4, 0.45, 0.55, 0.75):
        f = int(c + frac * dev["MaximumSampleRate"])
        s.set(IQ_FREQUENCY, f)
        s.ping()
        window("place", iq_freq=f, frac=frac)
    summary = {"host": host, "port": port, "device": dev, "first_sync": sync, "iq_decimation": iq_dec,
               "iq_rate": iq_rate, "centre": c, "marks": marks, "syncs": s.syncs, "seq_gaps": s.seq_gaps,
               "closed": s.closed}
    s.close()
    save(outdir, f"main2_scale_{host}_{port}", s, summary)
    print("done", len(marks), "windows; closed", s.closed)


def t_shared(host, port, outdir):
    a = Spy(host, port, "owner")
    a.wait(lambda: a.sync is not None and a.device is not None, 10)
    centre = a.sync["DeviceCenterFrequency"]
    iq_dec = iq_stage(a.device)
    setup(a, centre, iq_dec, centre, 0)
    a.wait(lambda: a.iq_count > 10000, 6)
    b = Spy(host, port, "guest")
    b.wait(lambda: b.sync is not None, 10)
    print("guest sync", b.sync)
    setup(b, centre + 20000, iq_dec, centre, 2)
    b.wait(lambda: b.iq_count > 10000, 6)
    lo, hi = b.sync["MinimumIQCenterFrequency"], b.sync["MaximumIQCenterFrequency"]
    tries = []
    for f in (centre - 50000, hi + 200000, lo - 200000):
        b.set(IQ_FREQUENCY, f)
        b.ping()
        time.sleep(1.0)
        tries.append({"requested": f, "sync": b.sync})
    b.set(GAIN, 0)
    b.ping()
    time.sleep(1.0)
    summary = {"owner_sync": a.syncs[-1] if a.syncs else None, "guest_first_sync": b.syncs[0] if b.syncs else None,
               "guest_syncs": b.syncs, "tries": tries, "guest_iq": b.iq_count, "guest_fft": len(b.fft),
               "owner_after_guest_gain": a.sync, "closed": [a.closed, b.closed]}
    b.close()
    a.close()
    save(outdir, f"shared_{host}_{port}_guest", b, summary)
    print(json.dumps(summary, indent=1, default=str))


def t_gainorder(host, port, outdir):
    results = {}
    for gain_first in (True, False):
        s = Spy(host, port)
        s.wait(lambda: s.sync is not None and s.device is not None, 10)
        dev = s.device
        centre = s.sync["DeviceCenterFrequency"]
        iq_dec = iq_stage(dev)
        digital = iq_dec * 3 if dev["DeviceType"] != 1 else (dev["MaximumGainIndex"] - s.sync["Gain"]) + iq_dec * 3
        setup(s, centre + 25000, iq_dec, centre, 0, gain_first=gain_first, digital_gain=digital)
        s.wait(lambda: s.iq_count > 3 * dev["MaximumSampleRate"] / (1 << iq_dec), 8)
        iq = s.iq()
        tail = iq[len(iq) // 3:]
        with s.lock:
            flags = sorted({g[1] for g in s.iq_gain})
        results["gain_first" if gain_first else "gain_after"] = {
            "digital_gain": digital, "rms": float(np.sqrt(np.mean(np.abs(tail) ** 2))) if len(tail) else None,
            "peak": float(np.max(np.abs(tail))) if len(tail) else None,
            "clipped_fraction": float(np.mean(np.abs(tail.real) >= 32767)) if len(tail) else None,
            "message_flags": flags}
        s.close()
        time.sleep(5)
    print(json.dumps(results, indent=1))
    os.makedirs(outdir, exist_ok=True)
    with open(os.path.join(outdir, f"gainorder_{host}_{port}.json"), "w") as f:
        json.dump(results, f, indent=1)


if __name__ == "__main__":
    host, port, outdir, test = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4]
    {"main": t_main, "main2": t_main2, "scale": t_scale, "shared": t_shared, "gainorder": t_gainorder}[test](host, port, outdir)
