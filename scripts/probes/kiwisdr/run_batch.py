"""Gentle sequential runner for kiwi_tests.py: /status gate before every session, 15 s between sessions."""
import http.client, json, random, ssl, subprocess, sys, time

sys.stdout.reconfigure(encoding="utf-8")
SEED = int(time.time())
rng = random.Random(SEED)
print("seed", SEED, flush=True)


def status(scheme, host, port):
    c = (http.client.HTTPSConnection(host, port, timeout=10, context=ssl.create_default_context()) if scheme == "https"
         else http.client.HTTPConnection(host, port, timeout=10))
    c.request("GET", "/status", headers={"User-Agent": "sdriak-test"})
    return dict(l.split("=", 1) for l in c.getresponse().read(8192).decode("utf-8", "replace").splitlines() if "=" in l)


def gate(scheme, host, port, need_free):
    try:
        st = status(scheme, host, port)
        u, m = int(st.get("users", 99)), int(st.get("users_max", 0))
        return (m - u) >= need_free, f"{u}/{m}"
    except Exception as e:
        return False, f"status error {type(e).__name__}"


def run(test, scheme, host, port, need_free=2):
    ok, occ = gate(scheme, host, port, need_free)
    print(f"\n=== {test} {scheme}://{host}:{port}  users {occ}  {'RUN' if ok else 'SKIP'}", flush=True)
    if ok:
        subprocess.run([sys.executable, "kiwi_tests.py", test, scheme, host, str(port)], timeout=300)
    time.sleep(15)


AVAILABLE = [("https", "kiwi.770net.de", 8073), ("http", "82.139.82.247", 8073), ("http", "g8ure.ddns.net", 8076),
             ("http", "162.199.177.108", 8073), ("http", "sdr-amradioantennas.com", 8078)]

# TLS handshake only: no slot
for h in [("https", "kiwi.770net.de", 8073), ("https", "kiwi.x26.de", 443), ("https", "swl-lippi-2.stbach.de", 443)]:
    print(f"\n=== B4 {h}", flush=True)
    subprocess.run([sys.executable, "kiwi_tests.py", "b4", *map(str, h)], timeout=60)
    time.sleep(4)

# A2: krec SND on a full receiver (refusal expected, no slot taken)
for h in [("http", "86.254.7.95", 8073), ("http", "178.63.122.223", 8073)]:
    ok, occ = gate(*h, 0)
    full = occ.split("/")[0] == occ.split("/")[-1]
    print(f"\n=== A2 {h} users {occ} {'RUN' if full else 'not full, try next'}", flush=True)
    if full:
        subprocess.run([sys.executable, "kiwi_tests.py", "a2", *map(str, h)], timeout=60)
        time.sleep(15)
        break
    time.sleep(4)

run("a1", "http", "sdr-amradioantennas.com", 8078)
run("a3", "http", "162.199.177.108", 8073)
run("b2", "http", "g8ure.ddns.net", 8076)
run("b3c1", "http", "82.139.82.247", 8073)
pick = rng.choice(AVAILABLE)
print("A5 random pick:", pick, flush=True)
run("a5", *pick)
run("b1", "http", "ea8bfk.ddns.net", 8073, need_free=5)
print("\nDONE", flush=True)
