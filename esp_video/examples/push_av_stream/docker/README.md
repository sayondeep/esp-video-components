# Docker Media Servers for push_av_stream Testing

This directory contains Docker configurations for running various media servers locally to test your ESP32-P4 push_av_stream solution.

## Quick Start

### Option 1: SRS (Simple Realtime Server) - Recommended

SRS supports CMAF/DASH ingest via HTTP PUT and is production-ready.

```bash
# Start SRS
docker-compose up -d srs

# Check logs
docker-compose logs -f srs

# Access SRS console
# http://localhost:1985/api/v1/summaries
```

**SRS Configuration for CMAF Push:**
- Streams are created automatically on first PUT
- URL pattern: `http://localhost:8080/live/stream_name/segment.m4s`
- MPD: Auto-generated at `http://localhost:8080/live/stream_name/index.mpd`

### Option 2: MediaMTX (Lightweight)

MediaMTX is a zero-dependency media server, perfect for quick testing.

```bash
# Start MediaMTX
docker-compose up -d mediamtx

# Access web UI
# http://localhost:8888
```

**MediaMTX Configuration:**
- Supports HTTP PUT for segment upload
- Auto-generates HLS/DASH playlists
- Web UI for monitoring

### Option 3: Nginx with RTMP Module

For HTTP PUT-based CMAF ingest with HLS/DASH output.

```bash
# Build and start
docker-compose up -d nginx-rtmp

# Access streams
# http://localhost:8080/hls/stream_name/index.m3u8
# http://localhost:8080/dash/stream_name/index.mpd
```

### Option 4: Python CMAF Server (Matter-compatible)

A containerized version similar to push_av_server.

```bash
# Build and start
docker-compose up -d cmaf-server

# Access web UI
# https://localhost:1234/ui/streams
```

## Testing Your ESP32

### 1. Find Your Docker Host IP

```bash
# On Linux
ip addr show docker0 | grep inet

# Or use host.docker.internal (if available)
# Or use your actual host IP (e.g., 192.168.1.100)
```

### 2. Configure ESP32

In `menuconfig`:
- Set `SERVER_HOST` to your Docker host IP (not localhost!)
- Set `SERVER_PORT` to the appropriate port (8080 for SRS, 1234 for cmaf-server)
- Configure TLS certificates if using HTTPS

### 3. Test Upload

Flash your ESP32 and monitor the logs. Segments should appear in the server.

## Server-Specific Configuration

### SRS Configuration

Edit `docker/srs/conf/docker.conf`:

```conf
listen              1935;
max_connections     1000;
srs_log_tank        file;
srs_log_file        ./objs/srs.log;

http_api {
    enabled         on;
    listen          1985;
}

http_server {
    enabled         on;
    listen          8080;
    dir             ./objs/nginx/html;
}

# Enable DASH/HLS
dash {
    enabled         on;
    dash_fragment   1;
    dash_update_period 1;
    dash_timeshift  10;
    dash_path       ./objs/nginx/html;
    dash_mpd_file   [app]/[stream].mpd;
}

# Enable HTTP PUT for CMAF ingest
http_remux {
    enabled         on;
    mount           [vhost]/[app]/[stream].m4s;
    rtmp_to_http    off;  # Allow direct PUT
}
```

### MediaMTX Configuration

Edit `docker/mediamtx/mediamtx.yml`:

```yaml
paths:
  esp32-stream:
    source: publisher
    sourceProtocol: http
    sourceOnDemand: yes
    sourceOnDemandStartTimeout: 10s
    sourceOnDemandCloseAfter: 10s
    # Accept HTTP PUT for segments
    sourceOnDemand: no
    source: http
    sourceOnDemandStartTimeout: 10s
```

### Nginx RTMP Configuration

The nginx configuration in `docker/nginx-rtmp/nginx.conf` includes:
- RTMP server for ingest
- HTTP PUT endpoint for CMAF segments
- HLS/DASH output generation

## Troubleshooting

### Cannot Connect from ESP32

1. **Check Docker network**: Ensure ESP32 can reach Docker host IP
2. **Check firewall**: Docker ports must be accessible
3. **Use host network mode** (Linux only):
   ```yaml
   network_mode: "host"
   ```

### TLS Certificate Issues

For HTTPS servers, you'll need to:
1. Generate self-signed certificates
2. Copy CA cert to ESP32 `main/certs/server_root_ca.pem`
3. Rebuild ESP32 firmware

See `docker/cmaf-server/README.md` for certificate generation.

### Port Conflicts

If ports are already in use, modify `docker-compose.yml`:
```yaml
ports:
  - "8081:8080"  # Map host port 8081 to container port 8080
```

## Monitoring

### SRS API
```bash
curl http://localhost:1985/api/v1/summaries
curl http://localhost:1985/api/v1/streams
```

### MediaMTX API
```bash
curl http://localhost:8888/v3/paths/list
```

### View Streams

- **SRS**: http://localhost:8080/live/stream_name/index.mpd
- **MediaMTX**: http://localhost:8888/stream_name/index.mpd
- **Nginx**: http://localhost:8080/dash/stream_name/index.mpd

## Cleanup

```bash
# Stop all services
docker-compose down

# Remove volumes (deletes uploaded segments)
docker-compose down -v

# Remove images
docker-compose down --rmi all
```

## Next Steps

1. Choose a server (SRS recommended for production-like testing)
2. Start the Docker container
3. Configure ESP32 with server IP and port
4. Test upload and playback
5. Integrate with your modular adapter design
