# Ingest Adapters

This directory contains server-specific adapters that implement the `ingest_transport` interface for different media servers.

## Structure

```
ingest_adapters/
├── cmaf_push.c/h          # Matter-specific implementation (Matter push_av_server)
├── matter_adapter.c/h     # Adapter wrapping cmaf_push.c for generic interface
├── mistserver_adapter.c/h # MistServer adapter
├── srs_adapter.c/h        # SRS (Simple Realtime Server) adapter
└── README.md              # This file
```

## Adapters

### Matter Adapter (`matter_adapter.c`)

- **Server**: Matter push_av_server (`connectedhomeip/src/tools/push_av_server`)
- **Implementation**: Wraps `cmaf_push.c`
- **URL Pattern**: `/streams/{id}/session_{N}/{track}/segment_{M}.m4s`
- **Auth**: mTLS (client certificate) or none

### MistServer Adapter (`mistserver_adapter.c`)

- **Server**: MistServer
- **URL Pattern**: `/stream_name/segment_0001.m4s`
- **Auth**: API key (X-API-Key header) or Basic Auth
- **Features**: Simpler URL structure, optional stream creation

### SRS Adapter (`srs_adapter.c`)

- **Server**: SRS (Simple Realtime Server)
- **URL Pattern**: `/live/stream_name/segment_0001.m4s`
- **Auth**: Optional API key
- **Features**: HTTP-based, auto-generates MPD

## Adding a New Adapter

1. Create `your_server_adapter.c` and `your_server_adapter.h`
2. Implement all functions in `ingest_transport_ops_t`
3. Export `your_server_adapter_get_ops()` function
4. Add server type to `ingest_server_type_t` enum in `ingest_transport.h`
5. Register adapter in `ingest_transport.c` dispatcher

Example:

```c
// your_server_adapter.c
static const ingest_transport_ops_t your_adapter_ops = {
    .init = your_adapter_init,
    .create_stream = your_adapter_create_stream,
    // ... implement all functions
};

const ingest_transport_ops_t *your_server_adapter_get_ops(void)
{
    return &your_adapter_ops;
}
```

Then in `ingest_transport.c`:

```c
case INGEST_SERVER_YOUR_SERVER:
    ops = your_server_adapter_get_ops();
    break;
```

## Usage

All adapters are accessed through the generic `ingest_transport` interface:

```c
ingest_transport_config_t config = {
    .server_type = INGEST_SERVER_MATTER,  // or MISTSERVER, etc.
    .server_host = "192.168.1.100",
    .server_port = 1234,
    .track_name = "video1",
    .auth = { ... }
};

ingest_transport_handle_t transport;
ingest_transport_init(&config, &transport);
// Use transport...
```

The dispatcher automatically routes to the correct adapter based on `server_type`.
