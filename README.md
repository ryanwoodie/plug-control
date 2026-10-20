# Plug Control

Remote control for Shelly smart plugs over a 1NCE IoT SIM, built to last
10 years on 500 MB of data.

```
 Shelly plugs ──Wi-Fi (private, no internet)──> LilyGo T-A7670G ──LTE / 1NCE──> MQTT broker <──wss── web app (GitHub Pages)
```

- **Plugs never touch the internet.** The LilyGo runs its own WPA2 network
  (`PlugCtl-xxxxxxxx`) with no route out; plugs get fixed IPs
  (192.168.4.200+) and their own AP, Bluetooth and cloud are switched off.
- **Schedules run on the controller**, using network/GPS time, so they keep
  working without cell coverage. Timers and schedule end times are also set
  on the plug itself (`toggle_after`), so a plug still turns off if the
  controller loses power.
- **One MQTT session** on the SIM. Idle cost is a TCP keep-alive probe every 5 min;
  status is sent only when something changes, plus every 6 h.
- **End-to-end encrypted.** Each owner's messages are AES-128-GCM with a key
  derived from their password (PBKDF2-SHA256, 100k rounds). The topic name
  is derived from the key too, so this public repo and the public broker hold
  nothing usable, and owners cannot see or control each other's plugs.
- **Data guard.** The firmware estimates SIM usage and goes quiet for the rest
  of the day/month past 150 KB/day or 3 MB/month (`firmware/include/board.h`).

## Features (web app)

On / Off / Timer, repeating schedules (days + start + duration), one-time
runs, plug rename, optional auto-off for manual "On", site time zone, live
watts/volts/kWh, cell signal, SIM data estimate, and GPS location.

There is also an on-site page at `http://192.168.4.1` for anyone joined to
the controller's Wi-Fi, which works with no cell service.

## Expected data use

The 1NCE path drops idle TCP connections after 5.5–6 minutes (measured:
330 s idle survived, 360 s did not). The modem's own TCP keep-alive
(`AT+CTCPKA`, every 5 min) holds the connection open with a bare probe and
ACK, which is cheaper than an MQTT ping; MQTT pings run hourly as a backstop.

| Item | Per day |
| --- | --- |
| TCP keep-alive probes (288/day, ~100 B each) | ~29 KB |
| Hourly MQTT ping | ~4 KB |
| Heartbeat status (2 owners, every 6 h) | ~3.5 KB |
| Each command or settings change + reply | ~1 KB |
| Reconnect after a dropped session (persistent session, no re-download) | <1 KB |

Roughly 35–40 KB/day idle, about 14 MB a year or ~140 MB over 10 years. The
1NCE portal (SIM quota) shows the real figure.

## Setup

1. Generate keys and passwords (writes `firmware/include/secrets.h` and
   `secrets/credentials.txt`, both git-ignored):

   ```bash
   tools/make_secrets.py --owner C-GMEH --owner C-FTBV \
     --plug "ACEBE6F17B6C:C-GMEH:C-GMEH plug" --plug "ACEBE6F17C50:C-FTBV:C-FTBV plug"
   ```

   Add `--password C-GMEH=...` to choose a password instead of a random one.
   Changing passwords or plug ownership means re-running this and reflashing.
   The board's Wi-Fi credentials are kept, so plugs stay connected; saved
   schedules reset because they belonged to the old logins. Afterwards run
   `tools/clear_topics.py secrets/secrets.prev.h` to delete the old logins'
   messages from the broker.

2. Flash: `cd firmware && pio run -t upload`.

3. Plug in factory-fresh (or factory-reset) Shelly plugs listed in
   `--plug`. The controller finds their setup Wi-Fi and moves them onto its
   own network within a few minutes. Only listed MACs are touched.

4. Open the web app and sign in with a username/password from
   `secrets/credentials.txt`.

To factory-reset a Shelly Plug US Gen4, hold its button for about 10 s.

## Layout

- `firmware/`: PlatformIO project for the LilyGo T-A7670G (ESP32)
  - `src/main.cpp`: plugs, schedules, commands, status, GPS, on-site page
  - `src/link.cpp`: modem, MQTT session, data meter and caps
  - `src/shelly.cpp`: Shelly Gen2+ RPC and provisioning
  - `src/crypto.cpp`: AES-GCM message sealing
  - `lib/`: LilyGo's TinyGSM fork and PubSubClient
- `docs/`: the web app (served by GitHub Pages)
- `tools/make_secrets.py`: key and password generator

## Changing the broker

`broker.hivemq.com` is a free public broker. To use a private one, change
`MQTT_HOST`/`MQTT_PORT` in `firmware/include/board.h` and `BROKER_WS` in
`docs/app.js`.
