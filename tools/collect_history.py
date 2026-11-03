#!/usr/bin/env python3
"""File plug history batches from the MQTT broker into permanent storage.

Run hourly by .github/workflows/history.yml. The controller publishes each
batch once, sealed with the owner's key, to the retained topic
plugctl/<tid>/h/<seq>. This script appends every new batch, still sealed, to
<dir>/<tid>/<YYYY-MM>.txt as "<seq> <base64>" lines, then clears the retained
topic. It cannot read the batches; the web app decrypts them.

If a seq is missing between batches it has, it asks the controller to resend
on plugctl/<tid>/hr, sealed with that owner's history key (HISTORY_KEYS, a JSON
object {tid: hex key}, written by tools/make_secrets.py to
secrets/history_keys.json).

Usage: HISTORY_KEYS='{...}' tools/collect_history.py <dir> [--topic-prefix plugctl]
"""
import argparse, base64, json, os, secrets, ssl, sys, time
from datetime import datetime, timezone

import paho.mqtt.client as mqtt
from cryptography.hazmat.primitives.ciphers.aead import AESGCM

BROKER, PORT = "broker.hivemq.com", 8883
MAX_GAP = 200      # a bigger jump in seq means the controller was reset, not data loss
MAX_TRIES = 6      # hourly resend requests per missing batch before giving up
MAX_REQUEST = 40   # seqs per request


def ranges_add(rs, n):
    rs.append([n, n])
    rs.sort()
    out = []
    for a, b in rs:
        if out and a <= out[-1][1] + 1:
            out[-1][1] = max(out[-1][1], b)
        else:
            out.append([a, b])
    return out


def has(rs, n):
    return any(a <= n <= b for a, b in rs)


def collect(prefix, tids, wait_s=6, max_s=40):
    got = {}  # topic -> payload
    last = [time.time()]

    def on_connect(c, _u, _f, rc, _p):
        for tid in tids:
            c.subscribe(f"{prefix}/{tid}/h/+", qos=1)

    def on_message(_c, _u, m):
        last[0] = time.time()
        if m.payload:
            got[m.topic] = bytes(m.payload)

    c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="pch-" + secrets.token_hex(4))
    c.tls_set(cert_reqs=ssl.CERT_REQUIRED)
    c.on_connect, c.on_message = on_connect, on_message
    c.connect(BROKER, PORT, keepalive=30)
    c.loop_start()
    t0 = time.time()
    while time.time() - last[0] < wait_s and time.time() - t0 < max_s:
        time.sleep(0.5)
    return c, got


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("--topic-prefix", default="plugctl")
    a = ap.parse_args()
    keys = {tid: bytes.fromhex(k) for tid, k in json.loads(os.environ["HISTORY_KEYS"]).items()}

    c, got = collect(a.topic_prefix, list(keys))
    month = datetime.now(timezone.utc).strftime("%Y-%m")
    now = int(time.time())
    pubs = []
    for tid, hkey in keys.items():
        d = os.path.join(a.dir, tid)
        os.makedirs(d, exist_ok=True)
        sp = os.path.join(d, "state.json")
        st = json.load(open(sp)) if os.path.exists(sp) else {}
        have, tries, lost = st.get("have", []), st.get("tries", {}), st.get("lost", [])

        new = []
        for topic, payload in got.items():
            parts = topic.split("/")
            if parts[1] != tid or not parts[-1].isdigit():
                continue
            pubs.append(c.publish(topic, b"", qos=1, retain=True))  # filed: clear it
            q = int(parts[-1])
            if not has(have, q):
                new.append(f"{q} {base64.b64encode(payload).decode()}\n")
                have = ranges_add(have, q)
                tries.pop(str(q), None)
        if new:
            with open(os.path.join(d, f"{month}.txt"), "a") as f:
                f.writelines(sorted(new, key=lambda s: int(s.split()[0])))

        # Gaps between batches we have are batches lost on the way: ask again.
        missing = []
        for (_, b), (a2, _) in zip(have, have[1:]):
            if a2 - b - 1 <= MAX_GAP:
                missing += [q for q in range(b + 1, a2) if q not in lost]
        ask = []
        for q in missing:
            n = tries.get(str(q), 0)
            if n >= MAX_TRIES:
                lost.append(q)
                tries.pop(str(q), None)
            elif len(ask) < MAX_REQUEST:
                ask.append(q)
                tries[str(q)] = n + 1
        hr = f"{a.topic_prefix}/{tid}/hr"
        if ask:
            iv = os.urandom(12)
            body = json.dumps({"r": ask, "t": now}, separators=(",", ":")).encode()
            pubs.append(c.publish(hr, iv + AESGCM(hkey).encrypt(iv, body, hr.encode()), qos=1, retain=True))
        elif st.get("asking"):
            pubs.append(c.publish(hr, b"", qos=1, retain=True))

        st = {"have": have, "tries": tries, "lost": sorted(set(lost)), "asking": bool(ask), "last_run": now}
        with open(sp, "w") as f:
            json.dump(st, f, separators=(",", ":"))
        print(f"{tid}: {len(new)} new batches, {len(ask)} resend requested, {len(st['lost'])} lost")

    for p in pubs:
        p.wait_for_publish(10)
    c.loop_stop()
    c.disconnect()


if __name__ == "__main__":
    sys.exit(main())
