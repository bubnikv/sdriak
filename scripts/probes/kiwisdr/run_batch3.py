"""Third batch: r1 refusal, r2 real-Kiwi switch/RTT/ADPCM (Kiwi 2 + rx3.wf3), r3 UberSDR latency.
Sequential, one request at a time; /status or /users gate; 15 s between sessions."""
import http.client, json, os, random, re, ssl, subprocess, sys, time, urllib.parse

sys.stdout.reconfigure(encoding="utf-8")
SEED = int(time.time())
rng = random.Random(SEED)
print("seed", SEED, flush=True)
LIST = os.environ.get("KIWI_LIST", "../kiwilist/linkfanel.js")
txt = open(LIST, encoding="utf-8").read()
rx = json.loads(re.sub(r",\s*([\]}])", r"\1", txt[txt.index("["): txt.rindex("]") + 1]))
reach = {x["url"]: x["final"] for x in json.load(open(os.environ.get("KIWI_REACH", "../kiwilist/probe2.json"))) if x["final"]}


def target(e):
    u = urllib.parse.urlparse(e["url"])
    sch, port = reach[e["url"]]
    return sch, u.hostname, port


def get(sch, host, port, path):
    c = (http.client.HTTPSConnection(host, port, timeout=12, context=ssl.create_default_context()) if sch == "https"
         else http.client.HTTPConnection(host, port, timeout=12))
    c.request("GET", path, headers={"User-Agent": "sdriak-test"})
    return c.getresponse().read(65536).decode("utf-8", "replace")


def status(t):
    return dict(l.split("=", 1) for l in get(*t, "/status").splitlines() if "=" in l)


def eu(e):
    try:
        lat, lon = map(float, e.get("gps", "(0,0)").strip("()").split(","))
        return 35 < lat < 62 and -11 < lon < 30
    except Exception:
        return False


def real_kiwi(e):
    return int(e.get("users_max", "99") or 99) <= 8


def run(test, t):
    print(f"--- run {test} {t}", flush=True)
    subprocess.run([sys.executable, "kiwi_tests.py", test, t[0], t[1], str(t[2])], timeout=400)
    time.sleep(15)


def find(cands, accept, label, max_checks=8):
    rng.shuffle(cands)
    for e in cands[:max_checks]:
        t = target(e)
        try:
            ok, info = accept(t, e)
        except Exception as ex:
            ok, info = False, f"error {type(ex).__name__}"
        print(f"  [{label}] check {t[1]}:{t[2]} {e['mode']} -> {info} {'PICK' if ok else ''}", flush=True)
        time.sleep(3)
        if ok:
            return t
    return None


def free_slots(t, need):
    st = status(t)
    u, m = int(st.get("users", 99)), int(st.get("users_max", 0))
    return (m - u) >= need, f"{u}/{m}"


# ---------------- r1: waterfall channels all busy, slots left
def wf_busy(t, e):
    wf_chans = int(re.search(r"wf(\d+)", e["mode"]).group(1))
    us = json.loads(get(*t, "/users"))
    occ = {x["i"] for x in us if "n" in x}
    m = len(us)
    busy = all(i in occ for i in range(wf_chans))
    return busy and (m - len(occ)) >= 2, f"occupied {sorted(occ)} of {m}, wf_chans {wf_chans}"


c1 = [e for e in rx if e["url"] in reach and real_kiwi(e) and e["mode"] in ("rx8.wf2", "rx8.wf3", "rx8_wf2")
      and int(e.get("users", 0)) >= 2]
print(f"\n=== R1 candidates {len(c1)}", flush=True)
t1 = find(c1, wf_busy, "r1", 10)
if t1:
    run("a1", t1)

# ---------------- r2: real Kiwi 2 in Europe, then an rx3.wf3 (20 250 S/s) in Europe
c2 = [e for e in rx if e["url"] in reach and real_kiwi(e) and e.get("sdr_hw", "").startswith("KiwiSDR 2") and eu(e)]
print(f"\n=== R2 Kiwi 2 EU candidates {len(c2)}", flush=True)
t2 = find(c2, lambda t, e: free_slots(t, 2), "r2-kiwi2")
if t2:
    run("r2", t2)
c2b = [e for e in rx if e["url"] in reach and real_kiwi(e) and e["mode"] == "rx3.wf3" and eu(e)]
print(f"\n=== R2b rx3.wf3 EU candidates {len(c2b)}", flush=True)
t2b = find(c2b, lambda t, e: free_slots(t, 2), "r2-rx3")
if t2b:
    run("r2", t2b)

# ---------------- r3: UberSDR
c3 = [e for e in rx if e["url"] in reach and int(e.get("users_max", "0") or 0) > 14]
print(f"\n=== R3 UberSDR candidates {len(c3)}", flush=True)
t3 = find(c3, lambda t, e: free_slots(t, 5), "r3")
if t3:
    run("r3", t3)
print("\nDONE", flush=True)
