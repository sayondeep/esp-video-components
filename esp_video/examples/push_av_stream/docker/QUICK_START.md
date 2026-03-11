# Quick Start Guide: Docker Media Servers

## Fastest Option: SRS (Recommended)

SRS is the easiest to set up and works well with CMAF/DASH.

### 1. Start SRS

```bash
cd docker
docker-compose up -d srs
```

### 2. Check SRS is Running

```bash
# Check logs
docker-compose logs srs

# Check API
curl http://localhost:1985/api/v1/summaries
```

### 3. Find Your Host IP

```bash
# Linux
hostname -I | awk '{print $1}'

# macOS
ipconfig getifaddr en0

# Or check Docker network
docker network inspect push_av_stream_media-network | grep Gateway
```

### 4. Configure ESP32

In `menuconfig`:
- `SERVER_HOST`: Your host IP (e.g., `192.168.1.100`)
- `SERVER_PORT`: `8080`
- `TRACK_NAME`: `video1`
- `USE_MTLS`: `n` (SRS doesn't require mTLS by default)

### 5. Test Upload

After flashing ESP32, segments should be available at:
- **MPD**: http://YOUR_HOST_IP:8080/live/video1/index.mpd
- **Segments**: http://YOUR_HOST_IP:8080/live/video1/segment_1001.m4s

### 6. Playback

```bash
# Using ffplay
ffplay http://YOUR_HOST_IP:8080/live/video1/index.mpd

# Using VLC
vlc http://YOUR_HOST_IP:8080/live/video1/index.mpd
```

## Alternative: MediaMTX (Lightweight)

### 1. Start MediaMTX

```bash
docker-compose up -d mediamtx
```

### 2. Access Web UI

Open http://localhost:8888 in your browser

### 3. Configure ESP32

- `SERVER_HOST`: Your host IP
- `SERVER_PORT`: `8888` (or configure MediaMTX to use 8080)
- `TRACK_NAME`: `esp32-stream`

### 4. Upload and Playback

MediaMTX auto-generates playlists. Check the web UI for stream URLs.

## Troubleshooting

### ESP32 Cannot Connect

1. **Check IP address**: Use your actual host IP, not `localhost` or `127.0.0.1`
2. **Check firewall**:
   ```bash
   sudo ufw allow 8080/tcp
   ```
3. **Check Docker port mapping**: Verify ports are exposed in `docker-compose.yml`

### No Segments Appearing

1. **Check ESP32 logs**: Look for upload errors
2. **Check server logs**:
   ```bash
   docker-compose logs -f srs
   ```
3. **Test with curl**:
   ```bash
   curl -X PUT http://localhost:8080/live/test/segment_1001.m4s \
        --data-binary @test_segment.m4s
   ```

### Playback Issues

1. **Check MPD exists**:
   ```bash
   curl http://localhost:8080/live/video1/index.mpd
   ```
2. **Check segments exist**:
   ```bash
   ls -la /path/to/srs/objs/nginx/html/live/video1/
   ```
3. **Use correct player**: VLC or ffplay work best for DASH

## Next Steps

Once basic upload works:
1. Test with HTTPS (requires certificates)
2. Test with authentication (API keys, mTLS)
3. Integrate with your modular adapter design
4. Test with different servers to verify compatibility
