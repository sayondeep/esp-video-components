#!/usr/bin/env bash
# Generate a self-signed TLS certificate for nagare-media/ingest (LAN use).
#
# The certificate is valid for the host's LAN IP *and* localhost so it can be
# reached from both the ESP32 (by IP) and a browser on the same machine.
#
# Usage:
#   ./gen_cert.sh <LAN_IP>
#   e.g. ./gen_cert.sh 192.168.0.111
#
# Outputs (in the same directory as this script):
#   server.crt  — certificate (PEM).  Copy to main/certs/nagare_server_ca.pem
#                 before building the ESP32 firmware with TLS enabled.
#   server.key  — private key (PEM).  Used by nginx only; never put on the ESP32.
#
# The certificate is self-signed, so it acts as its own CA.
# The ESP32 embeds server.crt as the trusted CA certificate.

set -euo pipefail

IP="${1:-}"
if [[ -z "$IP" ]]; then
    echo "Usage: $0 <LAN_IP>"
    echo "  e.g. $0 192.168.0.111"
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CERT="$SCRIPT_DIR/server.crt"
KEY="$SCRIPT_DIR/server.key"

echo "Generating self-signed certificate for IP: $IP ..."

openssl req -x509 \
    -newkey rsa:4096 \
    -keyout "$KEY" \
    -out    "$CERT" \
    -days   3650 \
    -nodes \
    -subj   "/CN=nagare-ingest/O=ESP32-Test/C=CN" \
    -addext "subjectAltName=IP:${IP},IP:127.0.0.1,DNS:localhost,DNS:nagare-ingest"

echo ""
echo "Done.  Files written:"
echo "  $CERT"
echo "  $KEY"
echo ""
echo "Next steps:"
echo "  1. Copy the certificate into the ESP32 firmware cert directory:"
echo "       cp $CERT $(dirname "$SCRIPT_DIR")/main/certs/nagare_server_ca.pem"
echo ""
echo "  2. Start the stack with TLS:"
echo "       docker compose --profile tls up"
echo ""
echo "  3. In menuconfig:"
echo "       Example Configuration → Ingest Server"
echo "         Server type : nagare-media/ingest (plain HTTP)"
echo "         Enable TLS  : y"
echo "         Port        : 8443"
