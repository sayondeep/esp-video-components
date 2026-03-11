# Adapter Architecture: Should Each Adapter Have Its Own Implementation File?

## Current Situation

### Matter Adapter
- **Has separate file**: `cmaf_push.c/h` (376 lines)
- **Reason**: Original implementation, complex logic (JSON parsing, DASH-IF headers, response handling)
- **Adapter**: `matter_adapter.c` wraps `cmaf_push.c`

### MistServer & SRS Adapters
- **No separate files**: Implementation directly in adapter files
- **Reason**: Simpler implementations (~250-300 lines each)
- **Code duplication**: Both have similar `do_request()` and `build_url()` helpers

## Analysis: Code Duplication

### Common Patterns Found

1. **HTTP Request Helper** (`do_request`)
   - Present in: `cmaf_push.c`, `mistserver_adapter.c`, `srs_adapter.c`
   - Differences:
     - `cmaf_push.c`: Handles response reading, DASH-IF headers, drain_response()
     - Others: Simpler, no response reading

2. **URL Building** (`build_url`)
   - Present in: `mistserver_adapter.c`, `srs_adapter.c`
   - Differences: HTTP vs HTTPS protocol

3. **HTTP Client Initialization**
   - Similar pattern in all adapters
   - Differences: TLS config, auth methods

## Recommendation: Hybrid Approach

### Option 1: Keep Current Structure (Recommended for Now)

**Pros:**
- ✅ Each adapter is self-contained
- ✅ Easy to understand and maintain
- ✅ No shared dependencies
- ✅ Can optimize per-server

**Cons:**
- ❌ Some code duplication
- ❌ More files to maintain

**When to use:**
- Adapters are simple (< 500 lines)
- Server APIs are significantly different
- Want maximum flexibility

### Option 2: Extract Common HTTP Utilities

Create `ingest_adapters/http_utils.c/h`:

```c
// http_utils.h
esp_err_t ingest_http_request(esp_http_client_handle_t http,
                               esp_http_client_method_t method,
                               const char *url,
                               const void *data, size_t data_len,
                               const char *content_type,
                               const char **custom_headers,
                               char *resp_buf, size_t resp_buf_sz,
                               size_t *resp_len);

void ingest_build_url(char *buf, size_t buf_size,
                      const char *protocol,  // "http" or "https"
                      const char *host, uint16_t port,
                      const char *path);
```

**Pros:**
- ✅ Eliminates duplication
- ✅ Consistent error handling
- ✅ Easier to add features (retry, timeout, etc.)

**Cons:**
- ❌ More abstraction layers
- ❌ May not fit all server needs
- ❌ Shared code = shared bugs

**When to use:**
- Multiple adapters share significant code
- Want centralized HTTP improvements
- Codebase is growing

### Option 3: Separate Implementation Files (Like Matter)

Create `mistserver_push.c/h`, `srs_push.c/h`, etc.

**Pros:**
- ✅ Consistent structure across adapters
- ✅ Can reuse implementation directly (like Matter)
- ✅ Clear separation of concerns

**Cons:**
- ❌ More files
- ❌ May be overkill for simple adapters
- ❌ More complex build system

**When to use:**
- Implementation is complex (> 300 lines)
- Want to reuse implementation outside adapter
- Need to support both direct use and adapter use

## Current Code Duplication Analysis

### Duplicated Code

1. **`do_request()` function** (3 versions)
   - `cmaf_push.c`: 58 lines (complex)
   - `mistserver_adapter.c`: 32 lines (simple)
   - `srs_adapter.c`: 32 lines (simple)

2. **`build_url()` function** (2 versions)
   - `mistserver_adapter.c`: HTTPS
   - `srs_adapter.c`: HTTP

3. **HTTP client initialization** (3 versions)
   - Similar patterns, different configs

### Estimated Savings

If extracting common code:
- **Current**: ~150 lines duplicated
- **After extraction**: ~50 lines shared + ~100 lines adapter-specific
- **Savings**: ~50 lines, but adds abstraction layer

## Recommendation

### Short Term (Current State)
✅ **Keep current structure** - It works, adapters are simple enough

### Medium Term (When Adding More Servers)
✅ **Extract HTTP utilities** if:
- Adding 3+ more adapters
- Duplication becomes significant (> 200 lines)
- Need centralized improvements (retry logic, better error handling)

### Long Term (If Complexity Grows)
✅ **Consider separate files** if:
- Adapter implementations exceed 500 lines
- Need to reuse implementations
- Want to support both direct and adapter use

## Best Practice Guidelines

### When to Create Separate Implementation File

Create `server_push.c/h` when:
1. **Complexity**: Implementation > 300 lines
2. **Reusability**: Need to use outside adapter context
3. **API Surface**: Need to expose additional functions
4. **Testing**: Want to test implementation independently

### When to Keep in Adapter File

Keep implementation in `server_adapter.c` when:
1. **Simplicity**: Implementation < 300 lines
2. **Server-Specific**: Logic is tightly coupled to adapter
3. **No Reuse**: Won't be used outside adapter
4. **Quick Prototype**: Rapid development/testing

## Example: Refactored Structure

```
ingest_adapters/
├── common/
│   ├── http_utils.c/h      # Shared HTTP helpers (optional)
│   └── url_builder.c/h     # URL construction (optional)
├── matter/
│   ├── cmaf_push.c/h        # Matter implementation
│   └── matter_adapter.c/h   # Matter adapter
├── mistserver/
│   └── mistserver_adapter.c/h  # Self-contained
├── srs/
│   └── srs_adapter.c/h         # Self-contained
└── README.md
```

## Conclusion

**Current approach is fine** - Each adapter is self-contained and simple enough.

**Consider extracting common code** when:
- Adding more adapters (3+)
- Duplication exceeds 200 lines
- Need centralized improvements

**Don't force consistency** - Matter has a separate file because it's complex and was the original. Other adapters don't need the same structure if they're simpler.
