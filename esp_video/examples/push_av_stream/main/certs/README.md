# Certificate Setup (Matter backend only)

Certificates are only needed when using `INGEST_SERVER_MATTER`
(the Matter push_av_server). The nagare-media/ingest backend uses plain HTTP
and requires no certificates.

## No-mTLS setup (default for Matter)

### 1. Start push_av_server

```bash
cd connectedhomeip/src/tools/push_av_server
python3 server.py --working-directory ~/.pavstest \
                  --server-ip <PC_IP> --host 0.0.0.0 --no-strict
```

### 2. Copy the server root CA

```bash
cp ~/.pavstest/certs/server/root.pem main/certs/server_root_ca.pem
```

### 3. Build and flash

```bash
idf.py build flash monitor
```

---

## mTLS setup (optional)

Enable `CONFIG_EXAMPLE_USE_MTLS` in `idf.py menuconfig`, then:

### 4. Generate a device certificate

```bash
curl --cacert ~/.pavstest/certs/server/root.pem \
     -X POST https://<PC_IP>:1234/certs/esp32/keypair | python3 -m json.tool
```

### 5. Copy the device cert and key

```bash
cp ~/.pavstest/certs/device/esp32.pem  main/certs/client_cert.pem
cp ~/.pavstest/certs/device/esp32.key  main/certs/client_key.pem
```

### 6. Build and flash

```bash
idf.py build flash monitor
```

---

**Note:** `server_root_ca.pem` must be refreshed whenever push_av_server
regenerates its certificate hierarchy (e.g. after deleting `~/.pavstest/`).
