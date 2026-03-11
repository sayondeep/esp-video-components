# Reorganization Summary: Adapters Directory

## Changes Made

### 1. Moved Matter-Specific Files

- **Moved**: `cmaf_push.c` → `ingest_adapters/cmaf_push.c`
- **Moved**: `cmaf_push.h` → `ingest_adapters/cmaf_push.h`
- **Reason**: Group Matter-specific implementation with its adapter

### 2. Updated Includes

- **`matter_adapter.c`**: Changed `#include "cmaf_push.h"` → `#include "ingest_adapters/cmaf_push.h"`
- **`push_av_stream_main.c`**: Changed `#include "cmaf_push.h"` → `#include "ingest_adapters/cmaf_push.h"`

### 3. Created New Adapters

- **`srs_adapter.c/h`**: SRS (Simple Realtime Server) adapter
  - URL pattern: `/live/stream_name/segment_0001.m4s`
  - HTTP-based (no TLS required)
  - Auto-generates MPD

- **`mistserver_adapter.h`**: Header for MistServer adapter (was missing)

### 4. Fixed MistServer Adapter

- Fixed handle structure to match Matter adapter pattern
- Added proper error checking
- Exported `mistserver_adapter_get_ops()` function

### 5. Updated Build System

- **`CMakeLists.txt`**: Updated source paths
  - `cmaf_push.c` → `ingest_adapters/cmaf_push.c`
  - Added `ingest_adapters/mistserver_adapter.c`
  - Added `ingest_adapters/srs_adapter.c`

### 6. Updated Transport Dispatcher

- **`ingest_transport.c`**: Added support for MistServer and SRS adapters
  - `INGEST_SERVER_MISTSERVER` → `mistserver_adapter`
  - `INGEST_SERVER_WOWZA` → `srs_adapter` (reusing Wowza type for SRS)

## New Directory Structure

```
main/
├── ingest_adapters/
│   ├── cmaf_push.c/h          # Matter-specific implementation
│   ├── matter_adapter.c/h     # Matter adapter (wraps cmaf_push)
│   ├── mistserver_adapter.c/h # MistServer adapter
│   ├── srs_adapter.c/h        # SRS adapter
│   └── README.md              # Adapter documentation
├── ingest_transport.c/h       # Generic transport interface
├── cmaf_mux.c/h               # Generic CMAF muxer (unchanged)
├── mpd_gen.c/h                # Generic MPD generator (unchanged)
└── push_av_stream_main.c      # Main application
```

## Benefits

1. **Better Organization**: Matter-specific code grouped together
2. **Clear Separation**: Each server has its own adapter
3. **Easy to Extend**: Add new servers by creating new adapters
4. **Backward Compatible**: Existing code still works (includes updated)

## Usage

### Matter Server (Original)
```c
#include "ingest_adapters/cmaf_push.h"  // Direct use (backward compatible)
// or
// Use ingest_transport with INGEST_SERVER_MATTER
```

### MistServer
```c
ingest_transport_config_t config = {
    .server_type = INGEST_SERVER_MISTSERVER,
    .server_host = "192.168.1.100",
    .server_port = 443,
    .track_name = "video1",
    .auth = { .method = INGEST_AUTH_API_KEY, ... }
};
```

### SRS
```c
ingest_transport_config_t config = {
    .server_type = INGEST_SERVER_WOWZA,  // Reusing Wowza type for SRS
    .server_host = "192.168.1.100",
    .server_port = 8080,
    .track_name = "video1",
    .auth = { .method = INGEST_AUTH_NONE }
};
```

## Next Steps

1. ✅ Matter adapter - Complete
2. ✅ MistServer adapter - Complete
3. ✅ SRS adapter - Complete
4. ⏳ Test with Docker servers
5. ⏳ Add Kconfig options for server selection
6. ⏳ Update main application to use modular interface (optional)
