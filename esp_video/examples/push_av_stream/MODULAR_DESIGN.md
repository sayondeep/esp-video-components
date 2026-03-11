# Modular Design for Multi-Server Support

## Production-Grade Media Servers Supporting CMAF/DASH Ingest

### 1. **MistServer** (Open Source)
- **Protocol**: CMAF over HTTP (DASH Interface-1/2)
- **Ingest**: HTTP PUT to `cmaf://` or `cmafs://` URLs
- **URL Pattern**: `cmaf://server:port/stream_name/segment.m4s`
- **Auth**: Basic Auth, API keys, or mTLS
- **Features**: Live transcoding, multi-format output (DASH, HLS, HSS)

### 2. **Wowza Streaming Engine** (Commercial)
- **Protocol**: CMAF ingest via HTTP PUT/POST
- **Ingest**: REST API for stream creation, then PUT segments
- **URL Pattern**: `http://server:port/vod/stream_name/segment.m4s`
- **Auth**: API keys, OAuth tokens
- **Features**: Cloud integration, CDN distribution

### 3. **AWS Elemental MediaLive** (Cloud)
- **Protocol**: DASH-IF Interface-2 compliant
- **Ingest**: HTTPS PUT to pre-allocated endpoints
- **URL Pattern**: `https://ingest-endpoint.amazonaws.com/stream_id/segment.m4s`
- **Auth**: AWS Signature V4, IAM roles
- **Features**: Auto-scaling, multi-region, CDN integration

### 4. **Azure Media Services** (Cloud)
- **Protocol**: CMAF/DASH ingest
- **Ingest**: REST API + HTTPS PUT
- **URL Pattern**: `https://account.mediaservices.windows.net/streamingLocators/...`
- **Auth**: Azure AD tokens, API keys
- **Features**: Live encoding, DRM, analytics

### 5. **SRS (Simple Realtime Server)** (Open Source)
- **Protocol**: RTMP → CMAF conversion, or direct CMAF push
- **Ingest**: HTTP PUT to configured paths
- **URL Pattern**: `http://server:port/live/stream_name/segment.m4s`
- **Auth**: Token-based or IP whitelist
- **Features**: Low latency, HLS/DASH output

### 6. **Nginx with nginx-rtmp-module** (Open Source)
- **Protocol**: RTMP ingest (converts to CMAF) or direct HTTP PUT
- **Ingest**: HTTP PUT to configured location
- **URL Pattern**: `http://server:port/hls/stream_name/segment.m4s`
- **Auth**: Basic Auth, JWT tokens
- **Features**: High performance, CDN-ready

## Current Implementation Analysis

### Hardcoded Dependencies

1. **URL Structure** (lines 228, 274, 297, 321, 351 in `cmaf_push.c`)
   - Hardcoded: `/streams/{id}/session_{N}/...`
   - Should be: Configurable path template

2. **Stream Creation** (line 228)
   - Hardcoded: `POST /streams?interface=dash`
   - Should be: Pluggable stream allocation method

3. **Response Parsing** (lines 243-248)
   - Hardcoded: JSON parsing `{"id": N}`
   - Should be: Abstract response handler

4. **Content Types** (lines 280, 304, 330, 357)
   - Hardcoded: `application/dash+xml`, `video/mp4`, `video/iso.segment`
   - Should be: Server-specific content types

5. **Headers** (line 106)
   - Hardcoded: `DASH-IF-Ingest: 1.1`
   - Should be: Configurable headers per server

6. **Authentication** (lines 184-186)
   - Only mTLS supported
   - Should be: Pluggable auth (mTLS, API keys, OAuth, Basic Auth)

## Modular Design Proposal

### Architecture: Adapter Pattern

```
┌─────────────────────────────────────┐
│   CMAF Muxer (cmaf_mux.c)           │  ← Unchanged
│   - Generates fMP4 segments          │
│   - Creates init segments            │
└──────────────┬──────────────────────┘
               │
┌──────────────▼──────────────────────┐
│   Ingest Transport Interface        │  ← NEW: Abstract layer
│   - create_stream()                  │
│   - upload_init_segment()            │
│   - upload_media_segment()           │
│   - start_session() / end_session()  │
└──────────────┬──────────────────────┘
               │
       ┌───────┴────────┐
       │                │
┌──────▼──────┐  ┌─────▼──────────┐  ┌──────────────┐
│ Matter      │  │ MistServer      │  │ AWS          │
│ Adapter     │  │ Adapter         │  │ Adapter      │
│ (existing)  │  │ (new)           │  │ (new)        │
└─────────────┘  └─────────────────┘  └──────────────┘
```

### Implementation Structure

```
main/
├── cmaf_mux.c/h          (unchanged - media format)
├── ingest_transport.h    (NEW - abstract interface)
├── ingest_adapters/
│   ├── matter_adapter.c/h    (refactored from cmaf_push.c)
│   ├── mistserver_adapter.c/h
│   ├── aws_adapter.c/h
│   └── generic_adapter.c/h   (configurable URL templates)
└── push_av_stream_main.c (uses ingest_transport interface)
```
