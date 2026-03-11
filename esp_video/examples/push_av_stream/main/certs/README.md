# Certificate Setup

The push_av_server uses a self-signed CA hierarchy. The ESP32 firmware
**always** needs the server's root CA cert (`server_root_ca.pem`) so that it
can verify the server's TLS certificate, even when mTLS is disabled.

## Quick Setup (no mTLS — default)

### 1. Start push_av_server

```bash
cd connectedhomeip/src/tools/push_av_server
python3 server.py --working-directory ~/.pavstest \
                  --server-ip <PC_IP> \
                  --host 0.0.0.0
```

This generates the server CA and cert under `~/.pavstest/certs/` on first run.

### 2. Copy the server root CA into the firmware

```bash
cp ~/.pavstest/certs/server/root.pem \
   <example>/main/certs/server_root_ca.pem
```

### 3. Build and flash

```bash
idf.py build flash monitor
```

---

## Full mTLS Setup (optional)

Set `CONFIG_EXAMPLE_USE_MTLS=y` in `idf.py menuconfig` and follow the
additional steps below.

### 4. Generate a device certificate via the server API

```bash
curl --cacert ~/.pavstest/certs/server/root.pem \
     -X POST https://<PC_IP>:1234/certs/esp32/keypair | python3 -m json.tool
```

Then copy the generated files:

```bash
cp ~/.pavstest/certs/device/esp32.pem  main/certs/client_cert.pem
cp ~/.pavstest/certs/device/esp32.key  main/certs/client_key.pem
```

### 5. Build and flash

```bash
idf.py menuconfig   # enable CONFIG_EXAMPLE_USE_MTLS
idf.py build flash monitor
```

---

## Notes

- **`server_root_ca.pem`**: Must be updated whenever the push_av_server
  regenerates its certificate hierarchy (e.g., after deleting `~/.pavstest/`).
- **`client_cert.pem` / `client_key.pem`**: Only needed with mTLS enabled.
  Leave as placeholders otherwise.
