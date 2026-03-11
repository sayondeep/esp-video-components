#!/bin/bash
# Test script to verify Docker media server is accepting CMAF uploads

set -e

SERVER_HOST="${1:-localhost}"
SERVER_PORT="${2:-8080}"
STREAM_NAME="${3:-test}"

echo "Testing CMAF upload to ${SERVER_HOST}:${SERVER_PORT}"
echo "Stream name: ${STREAM_NAME}"
echo ""

# Create a minimal test init segment (ftyp + moov)
# This is a simplified version - in real usage, ESP32 generates proper segments
echo "Creating test init segment..."
cat > /tmp/test_init.mp4 << 'EOF'
ftypisom
EOF

# Create a minimal test media segment
echo "Creating test media segment..."
cat > /tmp/test_segment.m4s << 'EOF'
stypmsdh
EOF

# Test 1: Upload init segment
echo "Test 1: Uploading init segment..."
HTTP_CODE=$(curl -s -o /dev/null -w "%{http_code}" \
    -X PUT \
    "http://${SERVER_HOST}:${SERVER_PORT}/${STREAM_NAME}/init.mp4" \
    --data-binary @/tmp/test_init.mp4 \
    -H "Content-Type: video/mp4")

if [ "$HTTP_CODE" -ge 200 ] && [ "$HTTP_CODE" -lt 300 ]; then
    echo "✓ Init segment uploaded successfully (HTTP $HTTP_CODE)"
else
    echo "✗ Init segment upload failed (HTTP $HTTP_CODE)"
    exit 1
fi

# Test 2: Upload media segment
echo "Test 2: Uploading media segment..."
HTTP_CODE=$(curl -s -o /dev/null -w "%{http_code}" \
    -X PUT \
    "http://${SERVER_HOST}:${SERVER_PORT}/${STREAM_NAME}/segment_1001.m4s" \
    --data-binary @/tmp/test_segment.m4s \
    -H "Content-Type: video/iso.segment")

if [ "$HTTP_CODE" -ge 200 ] && [ "$HTTP_CODE" -lt 300 ]; then
    echo "✓ Media segment uploaded successfully (HTTP $HTTP_CODE)"
else
    echo "✗ Media segment upload failed (HTTP $HTTP_CODE)"
    exit 1
fi

# Test 3: Check if MPD exists (for SRS)
echo "Test 3: Checking for MPD..."
HTTP_CODE=$(curl -s -o /dev/null -w "%{http_code}" \
    "http://${SERVER_HOST}:${SERVER_PORT}/${STREAM_NAME}/index.mpd")

if [ "$HTTP_CODE" -eq 200 ]; then
    echo "✓ MPD found (HTTP $HTTP_CODE)"
    echo "  Playback URL: http://${SERVER_HOST}:${SERVER_PORT}/${STREAM_NAME}/index.mpd"
else
    echo "⚠ MPD not found (HTTP $HTTP_CODE) - may need more segments"
fi

# Cleanup
rm -f /tmp/test_init.mp4 /tmp/test_segment.m4s

echo ""
echo "✓ All tests passed!"
echo ""
echo "Next steps:"
echo "1. Configure ESP32 with SERVER_HOST=${SERVER_HOST} and SERVER_PORT=${SERVER_PORT}"
echo "2. Flash and run the ESP32 example"
echo "3. Playback: ffplay http://${SERVER_HOST}:${SERVER_PORT}/${STREAM_NAME}/index.mpd"
