"""Probe the public KiwiSDR list: plain-HTTP vs HTTPS reachability of /VER (no user slot used)."""
import concurrent.futures as cf, http.client, json, re, ssl, sys, urllib.parse

src, out = sys.argv[1], sys.argv[2]
txt = open(src, encoding="utf-8").read()
arr = txt[txt.index("["): txt.rindex("]") + 1]
arr = re.sub(r",\s*([\]}])", r"\1", arr)          # trailing commas
rx = json.loads(arr)
UA = {"User-Agent": "sdriak-test"}


def get(scheme, host, port, path):
    if scheme == "https":
        c = http.client.HTTPSConnection(host, port, timeout=6, context=ssl._create_unverified_context())
    else:
        c = http.client.HTTPConnection(host, port, timeout=6)
    try:
        c.request("GET", path, headers=UA)
        r = c.getresponse()
        body = r.read(4096)
        return r.status, r.getheader("Location"), body
    finally:
        c.close()


def fetch(url):
    hops = []
    for _ in range(4):
        u = urllib.parse.urlparse(url)
        port = u.port or (443 if u.scheme == "https" else 80)
        try:
            st, loc, body = get(u.scheme, u.hostname, port, "/VER")
        except Exception as ex:
            return hops + [f"{u.scheme}:{port} err:{type(ex).__name__}"], None
        if st in (301, 302, 307, 308) and loc:
            hops.append(f"{u.scheme}:{port} {st}")
            url = urllib.parse.urljoin(url, loc).replace("/VER", "")
            continue
        ok = st == 200 and body.strip().startswith(b"{")
        hops.append(f"{u.scheme}:{port} {'ok' if ok else st}")
        return hops, (u.scheme, port) if ok else None
    return hops, None


def probe(e):
    res = {"url": e.get("url"), "name": e.get("name", "")[:70], "mode": e.get("mode"), "ver": e.get("sw_version"),
           "users": e.get("users"), "users_max": e.get("users_max"), "gps": e.get("gps")}
    hops, final = fetch(e["url"].rstrip("/"))
    res["hops"] = hops
    res["final"] = final
    if final is None and e["url"].startswith("http://"):
        h2, f2 = fetch("https://" + e["url"][7:].rstrip("/"))
        res["hops_https"] = h2
        res["final"] = f2
    return res


active = [e for e in rx if e.get("status") == "active" and e.get("offline") == "no" and e.get("url")]
with cf.ThreadPoolExecutor(48) as ex:
    results = list(ex.map(probe, active))
json.dump(results, open(out, "w"), indent=1)
cat = {}
for r in results:
    k = "unreachable" if r["final"] is None else ("plain " if r["final"][0] == "http" else "tls ") + str(r["final"][1])
    cat[k] = cat.get(k, 0) + 1
print("listed", len(rx), "active", len(active))
print(json.dumps(dict(sorted(cat.items(), key=lambda x: -x[1])), indent=1))
