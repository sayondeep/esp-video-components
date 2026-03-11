# push_av_stream – CMAF over HTTPS (Matter PushAVStreamTransport)

Captures H.264 video from an ESP32-P4 camera, wraps it in **fragmented MP4
(CMAF)** segments, and pushes them to a server using **HTTPS PUT** requests
that follow the Matter PushAVStreamTransport spec (DASH Interface-2 ingest).

## Protocol overview

```
ESP32-P4                                 push_av_server
   |                                           |
   |-- POST /streams?interface=dash ---------->|  allocate stream → stream_id
   |<-- 201 {"id":1, ...} --------------------|
   |                                           |
   |-- PUT …/session_1/index.mpd (dynamic) -->|  start session
   |-- PUT …/session_1/video1/video1.init ---->|  init segment (ftyp+moov)
   |-- PUT …/session_1/video1/segment_1001.m4s|  media segment (styp+moof+mdat)
   |-- PUT …/session_1/video1/segment_1002.m4s|
   |   …                                       |
   |-- PUT …/session_1/index.mpd (static) --->|  close session
```

All requests use HTTPS (TLS).  With `CONFIG_EXAMPLE_USE_MTLS=y` the device
also presents a client certificate (mutual TLS).

## Quick start

### Option 1: Docker Media Servers (Recommended for Testing)

For quick local testing, you can use Docker-based media servers. See [`docker/README.md`](docker/README.md) for details.

**Quick start with SRS (Simple Realtime Server):**

```bash
cd docker
docker-compose up -d srs

# Find your host IP
hostname -I | awk '{print $1}'  # Linux
# or
ipconfig getifaddr en0          # macOS

# Test upload (optional)
./test_upload.sh <YOUR_HOST_IP> 8080 video1
```

Then configure ESP32 with:
- `SERVER_HOST`: Your Docker host IP
- `SERVER_PORT`: `8080`
- `USE_MTLS`: `n` (disabled for SRS)

**Other Docker options:**
- MediaMTX: `docker-compose up -d mediamtx`
- Nginx-RTMP: `docker-compose up -d nginx-rtmp`
- Python CMAF Server: `docker-compose up -d cmaf-server`

See [`docker/QUICK_START.md`](docker/QUICK_START.md) for detailed instructions.

### Option 2: Matter push_av_server (Original)

### 1. Start push_av_server on your PC

```bash
cd connectedhomeip/src/tools/push_av_server

# Install dependencies (once)
pip install -r requirements.txt

cd $(pip show hypercorn | grep "Location: " | sed "s/Location: //") && patch -p0 < /home/sayon/connectedhomeip/src/tools/push_av_server/hypercorn.patch

# Apply the hypercorn patch so client-cert info is accessible (once)
patch -p1 < hypercorn.patch  # or follow README.md in that folder

# Start server (omit --strict-mode to accept uploads without strict path validation)
python3 server.py --working-directory ~/.pavstest --server-ip 192.168.0.111


python3 server.py --working-directory ~/.pavstest --server-ip 192.168.0.111 --strict-mode

python3 server.py --working-directory ~/.pavstest --server-ip 192.168.0.111 --host 0.0.0.0

python3 server.py --working-directory ~/.pavstest --server-ip 192.168.0.111 --host 192.168.0.111

```

The server listens on `https://<PC_IP>:1234` by default.

### 2. Configure the ESP project

```bash
cd esp_video/examples/push_av_stream
idf.py set-target esp32p4
idf.py menuconfig
```

Key options (`Example Configuration`):

| Option | Default | Description |
|--------|---------|-------------|
| `SERVER_HOST` | `192.168.1.100` | push_av_server IP / hostname |
| `SERVER_PORT` | `1234` | HTTPS port |
| `TRACK_NAME` | `video1` | Segment URL track component |
| `USE_MTLS` | `n` | Enable client cert authentication |
| `FRAMES_PER_SEGMENT` | `30` | ~1 s segments at 30 fps |
| `STREAM_DURATION_SEC` | `0` | 0 = run forever |

### 3. Build and flash

```bash
idf.py build flash monitor
```

### 4. Verify uploads

Browse to `https://<PC_IP>:1234/ui/streams` in a browser (accept the
self-signed cert).  Each session shows uploaded segments with validation
status.

You can also play back segments with FFmpeg once the session ends:

```bash
# Find the working directory (default: a temp dir printed at server start)
PAVS_DIR=~/.pavstest/streams/1/session_1/video1

# Concatenate init + first segment
cat $PAVS_DIR/video1.init $PAVS_DIR/segment_1001.m4s > test.mp4
ffplay test.mp4

# Or play directly via the server's segment download endpoint:
ffplay "https://<PC_IP>:1234/streams/1/session_1/video1/segment_1001.m4s" \
  -cacert ~/.pavstest/certs/server/root.pem

# Live streaming (while ESP32 is actively uploading):
# The dynamic MPD is available while streaming is active. Players that support
# DASH live playback will automatically poll for new segments.

# Option 1: Skip certificate verification (quick test)
ffplay -k "https://<PC_IP>:1234/streams/<stream_id>/session_1/index.mpd"

# Option 2: Use CA cert via environment variable
export SSL_CERT_FILE=~/.pavstest/certs/server/root.pem
ffplay "https://<PC_IP>:1234/streams/<stream_id>/session_1/index.mpd"

# Note: Live playback has ~4 second delay (suggestedPresentationDelay).
# The player will refresh the MPD every 1 second to discover new segments.

# After session ends (static MPD for full playback):
ffplay -k "https://<PC_IP>:1234/streams/<stream_id>/session_1/index.mpd"

# With VLC (open in GUI and accept the certificate when prompted):
# For live: Use while ESP32 is streaming
# For playback: Use after session ends
vlc "https://<PC_IP>:1234/streams/<stream_id>/session_1/index.mpd"

cd ~/.pavstest/streams/5/session_1/video1 && ls -1 segment_*.m4s 2>/dev/null | wc -l && echo "---" && if [ -f video1.init ]; then cat video1.init $(ls -1 segment_*.m4s | sort -V) > /tmp/stream5_full.mp4 && ls -lh /tmp/stream5_full.mp4 && echo "✓ Created /tmp/stream5_full.mp4"; else echo "✗ No init segment found"; fi

vlc /tmp/stream5_full.mp4

```

## Enabling mutual TLS (optional)

```bash
# 1. Generate a device keypair via the server API
curl --cacert ~/.pavstest/certs/server/root.pem \
     -X POST "https://<PC_IP>:1234/certs/esp32/keypair" \
     | python3 -m json.tool

# 2. Copy certs into the project
cp ~/.pavstest/certs/server/root.pem   main/certs/server_root_ca.pem
cp ~/.pavstest/certs/device/esp32.pem  main/certs/client_cert.pem
cp ~/.pavstest/certs/device/esp32.key  main/certs/client_key.pem

# 3. Enable in menuconfig: Example Configuration → TLS → Use mutual TLS
# 4. Rebuild
idf.py build flash monitor
```

### What is happening and why

TLS (Transport Layer Security) involves two independent trust checks.
Understanding both helps clarify which files are needed and when.

**1. The ESP32 verifies the server — `server_root_ca.pem` (always required)**

When the ESP32 opens an HTTPS connection it must decide whether the server it
is talking to is genuine. It does this by checking that the server's TLS
certificate was signed by a trusted Certificate Authority (CA). The
`push_av_server` is a test server that uses its own self-signed CA — it is not
in any public trust store. `server_root_ca.pem` is that CA's certificate. It is
baked into the firmware so that mbedTLS (the TLS library on the ESP32) can
verify the server's identity. Without it the TLS handshake is rejected before
any data is sent.

This file must be copied from `~/.pavstest/certs/server/root.pem` once (after
the server first generates its hierarchy) and rebuilt into the firmware. It does
not change unless you wipe `~/.pavstest/`.

**2. The server verifies the ESP32 — `client_cert.pem` + `client_key.pem` (mTLS only)**

Normal TLS is one-way: only the client checks the server. Mutual TLS (mTLS)
adds the reverse: the server also demands a certificate from the client and
refuses connections from unknown devices. This is what the Matter
PushAVStreamTransport specification requires for production deployments.

When `CONFIG_EXAMPLE_USE_MTLS=y`:
- The ESP32 presents `client_cert.pem` to the server during the TLS handshake.
- The server checks that this certificate was signed by its own device CA
  (`~/.pavstest/certs/device/root.pem`).
- `client_key.pem` is the ESP32's private key, used to prove it actually owns
  the certificate (possession proof via a cryptographic signature).

The `/certs/esp32/keypair` API call asks the server to generate and sign a
fresh keypair for a device named `esp32`. The resulting `.pem` and `.key` files
are then embedded in the firmware.

**Summary**

| File | Direction | When needed |
|------|-----------|-------------|
| `server_root_ca.pem` | ESP32 → trusts server | Always (standard TLS) |
| `client_cert.pem` | Server → trusts ESP32 | mTLS only |
| `client_key.pem` | ESP32 proves identity | mTLS only |

## File layout

```
push_av_stream/
├── main/
│   ├── push_av_stream_main.c   Main application (V4L2 + CMAF loop)
│   ├── cmaf_mux.c / .h         ISO BMFF fMP4 muxer (init + media segments)
│   ├── cmaf_push.c / .h        HTTPS CMAF push client (PUT to server)
│   ├── mpd_gen.c / .h          DASH MPD XML generator
│   ├── certs/                  TLS cert placeholders (see certs/README.md)
│   ├── CMakeLists.txt
│   ├── Kconfig.projbuild
│   └── idf_component.yml
├── CMakeLists.txt
├── partitions.csv
├── sdkconfig.defaults
└── sdkconfig.defaults.esp32p4
```

## Memory usage (ESP32-P4 with PSRAM)

| Buffer | Size | Location |
|--------|------|----------|
| CMAF segment accumulation | ~125–250 KB | PSRAM |
| Camera capture buffers | mmap'd from V4L2 driver | — |
| H.264 encoder buffer | mmap'd from V4L2 M2M | — |
| mbedTLS record buffers | ~32 KB | PSRAM |
| Init/media segment output | ~125–250 KB (transient) | heap |

Total additional RAM: **≈ 165–300 KB** (~2–4 % of 8 MB PSRAM).

## Relationship to the Matter spec

This example implements the upload side of the
**Matter PushAVStreamTransport cluster** (DASH Interface-2) without the
full Matter stack, making it directly testable with the `push_av_server`
reference tool.  To integrate with Matter:

1. The V4L2 capture + H.264 encode pipeline is reused as-is.
2. `cmaf_mux` / `cmaf_push` / `mpd_gen` are reused as-is.
3. Replace Kconfig-driven `server_host/port/certs` with values received
   from the `AllocatePushTransport` Matter command.
4. Wrap the streaming task start/stop with `ManuallyTriggerTransport` and
   motion-detection callbacks.
