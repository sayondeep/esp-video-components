# push_av_stream — CMAF live ingest over HTTP(S)

Captures H.264 video from an ESP32-P4 camera, wraps it in **CMAF fMP4
segments**, and pushes them to an ingest server using **HTTP(S) PUT** requests
following the **DASH-IF Live Media Ingest Protocol Interface-2**.

Two server backends are supported:

| Backend | Transport | Use case |
|---------|-----------|----------|
| **nagare-media/ingest** | HTTP (plain) or HTTPS via nginx | Local testing, live streaming |
| **Matter push_av_server** | HTTPS (mTLS optional) | Matter PushAVStreamTransport integration |

---

## Protocol overview

```
ESP32-P4                                    ingest server
   |                                              |
   |-- PUT .../manifest.mpd (type="dynamic") ---->|  open session
   |-- PUT .../video1/video1.init --------------->|  ftyp + moov
   |-- PUT .../video1/segment_1001.m4s ---------->|  styp + moof + mdat
   |-- PUT .../video1/segment_1002.m4s ---------->|
   |   …                                          |
   |-- PUT .../manifest.mpd (type="static") ----->|  close session
```

Segment paths are relative to the MPD location.  Each new ESP32 boot writes to
a uniquely-named session directory (`{track}_{unix_timestamp}.str/`) so
sessions never collide.

---

## Quick start — nagare (recommended for testing)

### 1. Start the server

```bash
cd esp_video/examples/push_av_stream/docker
docker compose up nagare
```

### 2. Configure the ESP32

```bash
idf.py menuconfig
# Example Configuration → Ingest Server
#   Server type : nagare-media/ingest (plain HTTP)
#   Host        : <this machine's LAN IP>
#   Port        : 8080
#   Track name  : video1
```

### 3. Build and flash

```bash
idf.py build flash monitor
```

### 4. Watch the stream

The ESP32 log prints the session URL at startup:

```
I nagare_adapter: stream ready  MPD → http://192.168.0.111:8080/dash/video1_1743042000.str/manifest.mpd
```

Play it with ffplay:

```bash
ffplay "http://192.168.0.111:8080/dash/video1_1743042000.str/manifest.mpd"
```

Or open the URL in [dash.js reference player](https://reference.dashif.org/dash.js/).

### 5. Play back a recording

Each session is saved under `docker/recordings/`.  After the stream ends,
the static MPD enables full seek-based playback:

```bash
# List sessions
ls docker/recordings/

# Play a recorded session
ffplay "http://192.168.0.111:8080/dash/video1_1743042000.str/manifest.mpd"
```

---

## Quick start — nagare with HTTPS

Adds an nginx TLS proxy in front of nagare so the ESP32 connects over HTTPS.

### 1. Generate a self-signed certificate (once per LAN IP)

```bash
cd docker/certs
./gen_cert.sh 192.168.0.111      # substitute your LAN IP

# Copy the cert into the firmware cert directory
cp server.crt ../../main/certs/nagare_server_ca.pem
```

### 2. Start nagare + nginx

```bash
cd docker
docker compose --profile tls up
```

### 3. Configure the ESP32

```bash
idf.py menuconfig
# Example Configuration → Ingest Server
#   Server type          : nagare-media/ingest (plain HTTP)
#   Enable TLS for nagare: y
#   Host                 : <LAN IP>
#   Port                 : 8443
```

### 4. Build and flash

```bash
idf.py build flash monitor
```

The ESP32 log now shows `https://…` URLs and verifies the server certificate
against the embedded `nagare_server_ca.pem`.

---

## Quick start — Matter push_av_server

### 1. Start push_av_server

```bash
cd connectedhomeip/src/tools/push_av_server
pip install -r requirements.txt          # once
python3 server.py \
    --working-directory ~/.pavstest \
    --server-ip 192.168.0.111 \
    --host 0.0.0.0
```

The server listens on `https://<PC_IP>:1234`.

### 2. Copy the server root CA

```bash
cp ~/.pavstest/certs/server/root.pem \
   esp_video/examples/push_av_stream/main/certs/server_root_ca.pem
```

### 3. Configure the ESP32

```bash
idf.py menuconfig
# Example Configuration → Ingest Server
#   Server type : Matter push_av_server (HTTPS)
#   Host        : 192.168.0.111
#   Port        : 1234
```

### 4. Build and flash

```bash
idf.py build flash monitor
```

### 5. Verify uploads

Browse to `https://<PC_IP>:1234/ui/streams` (accept the self-signed cert).

Play back a session after it ends:

```bash
ffplay -k "https://192.168.0.111:1234/streams/1/session_1/manifest.mpd"
```

---

## Enabling mutual TLS for Matter (optional)

```bash
# 1. Generate a device keypair via the server API
curl --cacert ~/.pavstest/certs/server/root.pem \
     -X POST "https://192.168.0.111:1234/certs/esp32/keypair"

# 2. Copy certs
cp ~/.pavstest/certs/device/esp32.pem  main/certs/client_cert.pem
cp ~/.pavstest/certs/device/esp32.key  main/certs/client_key.pem

# 3. Enable in menuconfig
#    Example Configuration → TLS / mTLS (Matter only) → Enable mutual TLS: y

# 4. Rebuild
idf.py build flash monitor
```

See [`main/certs/README.md`](main/certs/README.md) for a full explanation of
what each certificate file is for.

---

## menuconfig reference

`idf.py menuconfig` → **Example Configuration**

### Ingest Server

| Option | Default | Description |
|--------|---------|-------------|
| `EXAMPLE_SERVER_TYPE` | Matter | `MATTER` or `NAGARE` |
| `EXAMPLE_NAGARE_USE_TLS` | n | Enable HTTPS for nagare (nginx proxy required) |
| `EXAMPLE_SERVER_HOST` | `192.168.1.100` | Server IP / hostname |
| `EXAMPLE_SERVER_PORT` | 1234 / 8080 / 8443 | Auto-defaults by server+TLS choice |
| `EXAMPLE_TRACK_NAME` | `video1` | CMAF track name; used in URLs |

### Stream Settings

| Option | Default | Description |
|--------|---------|-------------|
| `EXAMPLE_FRAMES_PER_SEGMENT` | 30 | Frames per CMAF segment (~1 s at 30 fps) |
| `EXAMPLE_STREAM_DURATION_SEC` | 0 | 0 = stream forever |
| `EXAMPLE_SEGMENT_NAMING` | Number | `Number` (`segment_1001.m4s`) or `Timestamp` (`segment_0.m4s`) |

### H.264 Encoder

| Option | Default | Description |
|--------|---------|-------------|
| `EXAMPLE_H264_I_PERIOD` | 30 | I-frame interval (should equal `FRAMES_PER_SEGMENT`) |
| `EXAMPLE_H264_BITRATE` | 1000000 | Target bitrate (bps) |
| `EXAMPLE_H264_MIN_QP` | 25 | Min quantiser (quality ceiling) |
| `EXAMPLE_H264_MAX_QP` | 26 | Max quantiser (quality floor) |

---

## File layout

```
push_av_stream/
├── main/
│   ├── push_av_stream_main.c        Application entry point; camera + encode + upload loop
│   ├── cmaf_mux.c / .h              ISO BMFF fMP4 muxer (init segment + media segments)
│   ├── mpd_gen.c / .h               DASH MPD XML generator (dynamic + static)
│   ├── ingest_transport.c / .h      Server-agnostic upload interface
│   ├── ingest_adapters/
│   │   ├── nagare_adapter.c         Adapter for nagare-media/ingest (HTTP/HTTPS)
│   │   └── matter_adapter.c         Adapter for Matter push_av_server (HTTPS / mTLS)
│   ├── certs/                       TLS certificate placeholders (see certs/README.md)
│   ├── CMakeLists.txt
│   ├── Kconfig.projbuild
│   └── idf_component.yml
├── docker/
│   ├── docker-compose.yml           nagare service + optional nginx TLS proxy
│   ├── nginx/nagare.conf            nginx TLS proxy configuration
│   ├── certs/gen_cert.sh            Self-signed cert generator for nagare TLS
│   ├── recordings/                  Persisted stream recordings (created at runtime)
│   └── README.md
├── push_av_server/                  Matter push_av_server reference implementation
├── CMakeLists.txt
├── partitions.csv
├── sdkconfig.defaults
└── sdkconfig.defaults.esp32p4
```

---

## Memory usage (ESP32-P4 with PSRAM)

| Buffer | Size | Location |
|--------|------|----------|
| CMAF segment accumulation | ~125–250 KB | PSRAM |
| Camera capture buffers | mmap'd from V4L2 driver | — |
| H.264 encoder buffer | mmap'd from V4L2 M2M | — |
| mbedTLS record buffers | ~32 KB | PSRAM |
| Upload queue (2 slots) | ~250–500 KB | PSRAM |

Total additional RAM: **≈ 200–350 KB** (~3–5 % of 8 MB PSRAM).

---

## Relationship to the Matter spec

This example implements the upload side of the
**Matter PushAVStreamTransport cluster** (DASH Interface-2) without requiring
the full Matter stack.  To integrate with a real Matter deployment:

1. Reuse the V4L2 capture + H.264 encode pipeline as-is.
2. Reuse `cmaf_mux`, `mpd_gen`, and `ingest_transport` as-is.
3. Replace the Kconfig-driven `server_host/port/certs` with values received
   from the `AllocatePushTransport` Matter command.
4. Wrap streaming task start/stop with `ManuallyTriggerTransport` and
   motion-detection callbacks.
