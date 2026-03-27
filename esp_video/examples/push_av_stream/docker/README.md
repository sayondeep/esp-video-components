# Docker setup — nagare-media/ingest

[nagare-media/ingest](https://github.com/nagare-media/ingest) implements the
**DASH-IF Live Media Ingest Protocol Interface-2** over HTTP — exactly what the
ESP32 firmware pushes.

---

## Plain HTTP (default)

### Start

```bash
docker compose up nagare
```

### Configure ESP32

```
idf.py menuconfig → Example Configuration → Ingest Server
  Server type : nagare-media/ingest (plain HTTP)
  Host        : <this machine's LAN IP>
  Port        : 8080
  Track name  : video1   (or any name)
```

### Watch the live stream

The ESP32 prints the session URL at startup, e.g.:

```
I nagare_adapter: stream ready  MPD → http://192.168.0.111:8080/dash/video1_1743042000.str/manifest.mpd
```

The session name (`video1_1743042000`) is `{track}_{unix_timestamp}` — unique
per ESP32 boot, so restarting the ESP32 never overwrites previous recordings.

```bash
# Live playback
ffplay "http://192.168.0.111:8080/dash/video1_1743042000.str/manifest.mpd"

# Or open in dash.js reference player
# https://reference.dashif.org/dash.js/
```

### Play back a recording

Recordings are saved under `./recordings/` on the Docker host.  After the
stream ends a `type="static"` MPD is uploaded, enabling full seek-based
playback:

```bash
# List recorded sessions
ls recordings/

# Play back
ffplay "http://192.168.0.111:8080/dash/video1_1743042000.str/manifest.mpd"
```

---

## HTTPS via nginx TLS proxy

Adds an nginx container that terminates TLS on port **8443** and proxies
requests to nagare on port 8080 (Docker-internal only).

```
ESP32  ──HTTPS──►  nginx :8443  ──HTTP──►  nagare :8080  (internal network)
```

### 1. Generate a self-signed certificate

Run once per LAN IP.  The SAN field includes the IP so the ESP32 can verify it.

```bash
cd certs
./gen_cert.sh 192.168.0.111      # substitute your machine's LAN IP
```

This creates:
- `certs/server.crt` — certificate (the trust anchor for the ESP32)
- `certs/server.key` — private key (nginx only; never goes on the ESP32)

### 2. Copy the certificate into the firmware

```bash
cp certs/server.crt ../main/certs/nagare_server_ca.pem
```

### 3. Start nagare + nginx

```bash
docker compose --profile tls up
```

`nagare` starts on plain HTTP (as always).  The `nagare-tls` service (nginx)
only starts when the `tls` profile is active.

### 4. Configure ESP32

```
idf.py menuconfig → Example Configuration → Ingest Server
  Server type          : nagare-media/ingest (plain HTTP)
  Enable TLS for nagare: y
  Host                 : <LAN IP>
  Port                 : 8443
```

Rebuild — CMakeLists automatically embeds `nagare_server_ca.pem`.

### 5. Playback over HTTPS

```bash
# ffplay (accept self-signed cert)
ffplay -tls_verify 0 "https://192.168.0.111:8443/dash/video1_1743042000.str/manifest.mpd"

# Or supply the CA cert
ffplay --tls-ca-file certs/server.crt \
       "https://192.168.0.111:8443/dash/video1_1743042000.str/manifest.mpd"
```

---

## Directory layout

```
docker/
├── docker-compose.yml      nagare service + optional nginx TLS service (--profile tls)
├── nginx/
│   └── nagare.conf         nginx TLS proxy configuration
├── certs/
│   ├── gen_cert.sh         Self-signed cert generator (run once per LAN IP)
│   ├── server.crt          Generated certificate  ← copy to main/certs/nagare_server_ca.pem
│   └── server.key          Generated private key  ← used by nginx only
└── recordings/             Persisted stream data (created at runtime, world-writable)
```

> **Note:** `recordings/` must be world-writable so the nagare container (uid 65532)
> can write to it.  If you see HTTP 408 timeouts from the ESP32, run:
> ```bash
> chmod 777 recordings
> ```

---

## Troubleshooting

**ESP32 cannot reach the server**
- Use the machine's LAN IP — not `localhost` or `127.0.0.1`.
- Check that port 8080 (or 8443) is not blocked by a firewall.

**HTTP 408 timeout on MPD or init segment upload**
- The `recordings/` directory is not writable by the nagare container.
- Fix: `chmod 777 docker/recordings`

**Port conflict**

Edit `docker-compose.yml` and remap the host port:
```yaml
ports:
  - "8081:8080"   # if 8080 is already in use
```
Then set `Server port: 8081` in menuconfig.

**nginx fails to start — `cannot load certificate`**
- `certs/server.crt` or `certs/server.key` do not exist.
- Run `cd certs && ./gen_cert.sh <LAN_IP>` first.

**Stop and clean up**

```bash
docker compose down                  # stop containers
docker compose --profile tls down    # stop including nginx
docker compose down --rmi all        # also remove images
```
