"""One OpenWebRX+ session: IQ mode, passband, offset retunes, FFT stream.

Usage: python owrxsession.py <base url, e.g. https://host/> <out.pkl> [--need-carrier]

With --need-carrier the session closes right after the config when the
current band shows no steady carrier (nothing to time a retune by).

Sequence (about 45 s, one socket, no profile changes, so no bot-ban score):
  handshake, connectionproperties (12 / 48 kHz), wait for config;
  pick the strongest steady carrier in the server FFT (offset C0; 0 if
  none stands 10 dB above the median);
  mod=iq at C0 with the mode's default passband, 10 s;
  low_cut/high_cut -24/+24 kHz, 10 s;
  offset_freq C0 + 10 kHz, C0, C0 - 10 kHz, C0, 4 s apart (the carrier
  moves to -10, 0, +10, 0 kHz in the IQ);
  close.
Every message is recorded with its monotonic arrival time; owrxanalyze.py
decodes it offline."""
import json, pickle, sys, threading, time
import numpy as np
from websockets.sync.client import connect
from owrxcommon import adpcm_plain

sys.stdout.reconfigure(encoding="utf-8")
BASE, OUTF = sys.argv[1].rstrip("/"), sys.argv[2]
NEED_CARRIER = "--need-carrier" in sys.argv[3:]
WS_URL = BASE.replace("https://", "wss://").replace("http://", "ws://") + "/ws/"
T0 = time.monotonic()
now = lambda: time.monotonic() - T0
rec = {"url": BASE, "events": [], "frames": []}


def ev(kind, **kw):
    kw.update(kind=kind, t=round(now(), 4))
    rec["events"].append(kw)
    print(f"{kw['t']:7.2f} {kind} {json.dumps({k: v for k, v in kw.items() if k not in ('kind', 't')})[:160]}")


def pick_carrier():
    """Offset of the strongest bin that is high in every FFT frame so far,
    away from DC and the edges; 0 when nothing stands out."""
    cfg = {}
    for _t, k, m in rec["frames"]:
        if k == "text" and m.startswith("{"):
            j = json.loads(m)
            if j.get("type") == "config":
                cfg.update(j["value"])
    rows = [m for _t, k, m in rec["frames"] if k == 1][-20:]
    if not rows or "samp_rate" not in cfg:
        return 0
    size = cfg.get("fft_size", 4096)
    if cfg.get("fft_compression") == "adpcm":
        r = np.array([adpcm_plain(m)[10:10 + size] / 100.0 for m in rows])
    else:
        r = np.array([np.frombuffer(m, "<f4")[:size] for m in rows])
    low = r.min(axis=0)  # steady: the weakest frame still shows it
    n = len(low)
    sr = cfg["samp_rate"]
    f = (np.arange(n) - n / 2) * sr / n
    ok = (np.abs(f) > 30000) & (np.abs(f) < sr / 2 - 40000)
    if not ok.any():
        return 0
    k = int(np.argmax(np.where(ok, low, -1e9)))
    if low[k] - np.median(low) < 10:
        return 0
    return int(round(f[k]))


def main():
    with connect(WS_URL, user_agent_header="Mozilla/5.0 (compatible; sdr-protocol-probe)", open_timeout=15,
                 max_size=8 * 1024 * 1024) as ws:
        stop = threading.Event()

        def reader():
            while not stop.is_set():
                try:
                    m = ws.recv(timeout=1.0)
                except TimeoutError:
                    continue
                except Exception as e:  # closed
                    ev("reader_end", error=repr(e))
                    return
                t = now()
                if isinstance(m, bytes):
                    rec["frames"].append((t, m[0], m[1:]))
                else:
                    rec["frames"].append((t, "text", m))

        def send(obj):
            s = obj if isinstance(obj, str) else json.dumps(obj)
            ws.send(s)
            ev("sent", msg=s)

        th = threading.Thread(target=reader, daemon=True)
        th.start()
        send("SERVER DE CLIENT client=probe type=receiver")
        send({"type": "connectionproperties", "params": {"output_rate": 12000, "hd_output_rate": 48000}})
        time.sleep(4.0)
        c0 = pick_carrier()
        rec["carrier_offset"] = c0
        ev("carrier", offset=c0)
        if NEED_CARRIER and not c0:
            stop.set()
            th.join(3.0)
            ev("no_carrier_abort")
            return
        send({"type": "dspcontrol", "action": "start"})
        send({"type": "dspcontrol", "params": {"mod": "iq", "offset_freq": c0}})
        time.sleep(10.0)
        send({"type": "dspcontrol", "params": {"low_cut": -24000, "high_cut": 24000}})
        time.sleep(10.0)
        for off in (10000, 0, -10000, 0):
            send({"type": "dspcontrol", "params": {"offset_freq": c0 + off}})
            time.sleep(4.0)
        stop.set()
        th.join(3.0)
    ev("closed")


try:
    main()
except Exception as e:
    ev("error", error=repr(e))
with open(OUTF, "wb") as f:
    pickle.dump(rec, f)
texts = [m for (_, k, m) in rec["frames"] if k == "text"]
kinds = {}
for (_, k, _m) in rec["frames"]:
    kinds[k] = kinds.get(k, 0) + 1
print("frames by type:", kinds)
for m in texts[:40]:
    print("text:", m[:300])
