#!/usr/bin/env python3
"""Stage a firmware update for the controller to fetch over a phone hotspot.

  tools/publish_fw.py                 build, encrypt, push to GitHub Pages, flag it
  tools/publish_fw.py --no-build      use the last build
  tools/publish_fw.py --notice-only   re-send the flag (e.g. after editing hotspots)
  tools/publish_fw.py --clear         withdraw the flag

The build (firmware/.pio/build/t-a7670/firmware.bin) holds the owner keys and
Wi-Fi password, so it is published encrypted with FW_KEY (secrets.h):
docs/fw/plugctl-<version>.bin = nonce(12) | AES-128-GCM ciphertext | tag(16),
AAD "plugctl-fw <version>". The controller checks the tag before installing.

The flag is a sealed, retained MQTT message on plugctl/<fw tid>/fw with the
version, size, URL and the hotspots to look for, from secrets/hotspots.json:
  [["My iPhone", "hotspot-password"], ...]
The controller gets it over the cell link (a few hundred bytes) and from then
on scans for those hotspots every 2 minutes until it has updated.

Needs: pip install paho-mqtt cryptography
"""
import argparse, glob, hashlib, json, os, re, secrets, ssl, subprocess, sys, time, urllib.request

import paho.mqtt.client as mqtt
from cryptography.hazmat.primitives.ciphers.aead import AESGCM

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "firmware/.pio/build/t-a7670/firmware.bin")
PAGES = "https://ryanwoodie.github.io/plug-control/fw/"
BROKER, PORT = "broker.hivemq.com", 8883


def fw_key():
    s = open(os.path.join(ROOT, "firmware/include/secrets.h")).read()
    m = re.search(r"#define FW_KEY \{([^}]*)\}", s)
    if not m:
        sys.exit("No FW_KEY in secrets.h: run tools/make_secrets.py")
    return bytes(int(x, 16) for x in m.group(1).split(", "))


def fw_version():
    m = re.search(r'#define FW_VERSION "([^"]+)"', open(os.path.join(ROOT, "firmware/src/main.cpp")).read())
    return m.group(1)


def publish(topic, payload):
    c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="pcf-" + secrets.token_hex(4))
    c.tls_set(cert_reqs=ssl.CERT_REQUIRED)
    c.connect(BROKER, PORT, keepalive=30)
    c.loop_start()
    c.publish(topic, payload, qos=1, retain=True).wait_for_publish(15)
    c.loop_stop()
    c.disconnect()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--no-build", action="store_true")
    ap.add_argument("--notice-only", action="store_true")
    ap.add_argument("--clear", action="store_true")
    a = ap.parse_args()

    key = fw_key()
    tid = hashlib.sha256(key + b"topic").hexdigest()[:20]
    topic = f"plugctl/{tid}/fw"
    if a.clear:
        publish(topic, b"")
        print("update flag withdrawn")
        return

    hp = os.path.join(ROOT, "secrets/hotspots.json")
    if not os.path.exists(hp):
        sys.exit(f'Write {hp} first: [["hotspot name", "password"]]')
    hotspots = json.load(open(hp))
    v = fw_version()
    name = f"plugctl-{v}.bin"
    out = os.path.join(ROOT, "docs/fw", name)

    if not a.notice_only:
        if not a.no_build:
            subprocess.run(["pio", "run"], cwd=os.path.join(ROOT, "firmware"), check=True)
        plain = open(BIN, "rb").read()
        nonce = os.urandom(12)
        os.makedirs(os.path.dirname(out), exist_ok=True)
        for old in glob.glob(os.path.join(ROOT, "docs/fw/plugctl-*.bin")):
            if old != out:
                subprocess.run(["git", "rm", "-q", old], cwd=ROOT, check=True)
        with open(out, "wb") as f:
            f.write(nonce + AESGCM(key).encrypt(nonce, plain, f"plugctl-fw {v}".encode()))
        subprocess.run(["git", "add", out], cwd=ROOT, check=True)
        subprocess.run(["git", "commit", "-q", "-m", f"Firmware {v} for hotspot update"], cwd=ROOT, check=True)
        subprocess.run(["git", "push", "-q"], cwd=ROOT, check=True)
    size = os.path.getsize(out)

    # Flag it only once GitHub Pages serves it.
    url = PAGES + name
    print(f"waiting for {url} ", end="", flush=True)
    for _ in range(60):
        try:
            with urllib.request.urlopen(urllib.request.Request(url, method="HEAD"), timeout=10) as r:
                if r.status == 200 and int(r.headers.get("Content-Length", size)) == size:
                    break
        except Exception:
            pass
        print(".", end="", flush=True)
        time.sleep(10)
    else:
        sys.exit("\nPages never served it; run again with --notice-only later")
    print(" ok")

    body = json.dumps({"v": v, "n": size, "u": url, "h": hotspots, "t": int(time.time())}, separators=(",", ":")).encode()
    nonce = os.urandom(12)
    publish(topic, nonce + AESGCM(key).encrypt(nonce, body, topic.encode()))
    print(f"firmware {v} flagged; the controller looks for {', '.join(h[0] for h in hotspots)} every 2 min")


if __name__ == "__main__":
    main()
