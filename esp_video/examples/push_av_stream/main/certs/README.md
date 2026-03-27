# Certificate Setup

This directory holds TLS certificate files that are embedded into the firmware
at build time.  Which files are required depends on the selected server backend
and TLS options.

---

## Matter push_av_server backend

### Standard TLS (server verification only)

The ESP32 must verify the server's certificate.  The `push_av_server` uses a
self-signed CA, so its root cert must be embedded.

```bash
# 1. Start push_av_server (generates certs on first run)
python3 server.py --working-directory ~/.pavstest --server-ip <PC_IP> --host 0.0.0.0

# 2. Copy the server root CA here
cp ~/.pavstest/certs/server/root.pem  server_root_ca.pem
```

`server_root_ca.pem` must be refreshed whenever push_av_server regenerates
its certificate hierarchy (e.g. after deleting `~/.pavstest/`).

### Mutual TLS / mTLS (server also verifies the ESP32)

Enable `CONFIG_EXAMPLE_USE_MTLS=y` in menuconfig, then provide all three files:

```bash
# 1. Generate a device keypair via the server API
curl --cacert ~/.pavstest/certs/server/root.pem \
     -X POST "https://<PC_IP>:1234/certs/esp32/keypair"

# 2. Copy server CA + device cert + device key here
cp ~/.pavstest/certs/server/root.pem   server_root_ca.pem
cp ~/.pavstest/certs/device/esp32.pem  client_cert.pem
cp ~/.pavstest/certs/device/esp32.key  client_key.pem
```

### File summary for Matter

| File | Direction | When required |
|------|-----------|---------------|
| `server_root_ca.pem` | ESP32 trusts the server | Always (standard TLS) |
| `client_cert.pem` | Server trusts the ESP32 | `CONFIG_EXAMPLE_USE_MTLS=y` only |
| `client_key.pem` | ESP32 proves its identity | `CONFIG_EXAMPLE_USE_MTLS=y` only |

---

## nagare-media/ingest backend (plain HTTP)

No certificate files are needed.  The nagare adapter uses plain HTTP by
default.

---

## nagare-media/ingest backend (HTTPS via nginx proxy)

Enable `CONFIG_EXAMPLE_NAGARE_USE_TLS=y` in menuconfig.  The ESP32 must trust
the nginx proxy's certificate.

```bash
# 1. Generate a self-signed certificate for your LAN IP (run once)
cd ../../docker/certs
./gen_cert.sh 192.168.0.111       # substitute your machine's LAN IP

# 2. Copy the certificate here as the CA trust anchor
cp server.crt nagare_server_ca.pem
```

The self-signed certificate acts as its own CA, so the certificate itself is
the trust anchor embedded in the firmware.

| File | Purpose |
|------|---------|
| `nagare_server_ca.pem` | ESP32 verifies the nginx TLS proxy |

> **Never** copy `server.key` here or embed it in firmware.
> The private key is used only by nginx on the server side.
