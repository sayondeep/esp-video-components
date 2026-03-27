# Docker: CMAF Ingest Server for push_av_stream

## nagare-media/ingest

[nagare-media/ingest](https://github.com/nagare-media/ingest) implements the
DASH-IF Live Media Ingest Protocol Interface-2 over plain HTTP — the same
protocol the ESP32 firmware uses. No TLS certificates required.

### Start

```bash
docker compose up nagare
```

### Configure ESP32

```
idf.py menuconfig
→ Example Configuration → Ingest Server
  Server type : nagare-media/ingest (plain HTTP)
  Host        : <this machine's LAN IP>
  Port        : 8080
```

### Watch the stream

After the ESP32 starts pushing, the log prints `stream created: id=N`.
Open the MPD in any DASH player (e.g. [reference.dashif.org](https://reference.dashif.org/dash.js/)):

```
http://<HOST>:8080/streams/N/session_1/index.mpd
```

Or use ffplay:

```bash
ffplay http://<HOST>:8080/streams/N/session_1/index.mpd
```

---

## Troubleshooting

**ESP32 cannot reach the server**
- Use the machine's LAN IP — not `localhost` or `127.0.0.1`.
- Check that port 8080 is not blocked by a firewall.

**Port conflict**
```yaml
# docker-compose.yml
ports:
  - "8081:8080"   # remap host port if 8080 is taken
```
Then set `Server port: 8081` in menuconfig.

**Stop and clean up**
```bash
docker compose down          # stop container
docker compose down --rmi all  # also remove image
```
