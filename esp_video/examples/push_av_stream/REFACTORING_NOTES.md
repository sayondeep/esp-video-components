# Refactoring Notes: cmaf_push.c → matter_adapter.c

## Overview

The existing `cmaf_push.c` has been wrapped into a modular adapter (`matter_adapter.c`) that implements the `ingest_transport` interface. This allows the codebase to support multiple media servers while maintaining backward compatibility.

## Changes Made

### 1. New Files Created

- **`main/ingest_transport.h`** - Abstract interface for ingest transport
- **`main/ingest_transport.c`** - Transport dispatcher implementation
- **`main/ingest_adapters/matter_adapter.h`** - Matter adapter header
- **`main/ingest_adapters/matter_adapter.c`** - Matter adapter implementation (wraps cmaf_push)

### 2. Modified Files

- **`main/cmaf_push.h`** - Added `cmaf_push_get_stream_id()` function
- **`main/cmaf_push.c`** - Implemented `cmaf_push_get_stream_id()` function
- **`main/CMakeLists.txt`** - Added new source files

### 3. Files Unchanged (Backward Compatible)

- **`main/cmaf_push.c`** - Original implementation preserved
- **`main/cmaf_push.h`** - Original API preserved
- **`main/push_av_stream_main.c`** - Still uses `cmaf_push_*` functions directly

## Architecture

```
┌─────────────────────────────────────┐
│   push_av_stream_main.c            │
│   (uses cmaf_push_* directly)      │  ← Still works!
└──────────────┬──────────────────────┘
               │
       ┌───────┴────────┐
       │                 │
┌──────▼──────┐  ┌──────▼──────────┐
│ cmaf_push.c │  │ ingest_transport │
│ (original)  │  │ (new modular)    │
└─────────────┘  └──────┬───────────┘
                        │
                ┌───────┴────────┐
                │                 │
        ┌───────▼──────┐  ┌───────▼──────────┐
        │ matter_      │  │ (future: other   │
        │ adapter.c    │  │  adapters)       │
        └──────┬───────┘  └──────────────────┘
               │
        ┌──────▼──────┐
        │ cmaf_push.c │  ← Wrapped by adapter
        └─────────────┘
```

## How It Works

### Matter Adapter Implementation

The `matter_adapter.c` wraps the existing `cmaf_push_*` functions:

1. **Initialization**: Converts `ingest_transport_config_t` → `cmaf_push_config_t` and calls `cmaf_push_init()`
2. **Stream Creation**: Calls `cmaf_push_create_stream()` and extracts stream_id
3. **Session Management**: Delegates to `cmaf_push_start_session()` and `cmaf_push_end_session()`
4. **Segment Upload**: Delegates to `cmaf_push_upload_init_segment()` and `cmaf_push_upload_media_segment()`

### Transport Dispatcher

The `ingest_transport.c` dispatches to the appropriate adapter based on `server_type`:

- `INGEST_SERVER_MATTER` → `matter_adapter`
- `INGEST_SERVER_MISTSERVER` → (TODO: implement)
- `INGEST_SERVER_AWS` → (TODO: implement)
- etc.

## Backward Compatibility

**The existing code continues to work unchanged!**

- `push_av_stream_main.c` still uses `cmaf_push_*` functions directly
- No changes required to existing code
- The adapter is an additional layer, not a replacement

## Migration Path (Optional)

To use the modular interface in `push_av_stream_main.c`:

1. Replace `cmaf_push_config_t` with `ingest_transport_config_t`
2. Replace `cmaf_push_init()` with `ingest_transport_init()`
3. Replace `cmaf_push_*` calls with `ingest_transport_*` calls
4. Set `server_type = INGEST_SERVER_MATTER` in config

Example:
```c
// Old way (still works)
cmaf_push_config_t cfg = { ... };
cmaf_push_handle_t push;
cmaf_push_init(&cfg, &push);
cmaf_push_create_stream(push);

// New way (modular)
ingest_transport_config_t cfg = {
    .server_type = INGEST_SERVER_MATTER,
    ...
};
ingest_transport_handle_t transport;
ingest_transport_init(&cfg, &transport);
ingest_transport_create_stream(transport, &stream_info);
```

## Benefits

1. **Modularity**: Easy to add new server adapters
2. **Backward Compatible**: Existing code unchanged
3. **Testability**: Can test with different servers
4. **Maintainability**: Server-specific code isolated
5. **Extensibility**: Add new servers without changing core

## Next Steps

1. ✅ Matter adapter implemented
2. ⏳ Implement MistServer adapter (for Docker testing)
3. ⏳ Update `push_av_stream_main.c` to use modular interface (optional)
4. ⏳ Add Kconfig options for server selection
5. ⏳ Test with different servers

## Testing

To test the Matter adapter:

```c
ingest_transport_config_t config = {
    .server_type = INGEST_SERVER_MATTER,
    .server_host = CONFIG_EXAMPLE_SERVER_HOST,
    .server_port = CONFIG_EXAMPLE_SERVER_PORT,
    .track_name = CONFIG_EXAMPLE_TRACK_NAME,
    .auth = {
        .method = CONFIG_EXAMPLE_USE_MTLS ? INGEST_AUTH_MTLS : INGEST_AUTH_NONE,
        .credentials = {
            .mtls = {
                .server_ca_pem = server_root_ca_pem_start,
                .client_cert_pem = client_cert_pem_start,
                .client_key_pem = client_key_pem_start,
            }
        }
    }
};

ingest_transport_handle_t transport;
ingest_transport_init(&config, &transport);
// ... use transport ...
```
