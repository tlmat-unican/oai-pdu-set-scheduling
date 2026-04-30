#!/bin/bash

VIDEO_REF="${1}"

# 1. Ask for sudo credentials
echo "Validating admin permissions..."
sudo -v

echo "========================================"
echo "Starting test environment"
echo "========================================"

# 2. Configure the Namespace
echo "[1/4] Configuring interface in the ue_ns namespace..."
sudo ip link set oaitun_ue1 netns ue_ns
sudo ip netns exec ue_ns ip link set oaitun_ue1 up
sudo ip netns exec ue_ns ip addr add 10.45.0.2/24 dev oaitun_ue1
sudo ip netns exec ue_ns ip route add default dev oaitun_ue1

# 3. Launch the Python Proxy in the background
echo "[2/4] Starting Proxy RTP (DSCP Marked)..."
python3 rtp_proxy_4k_fast.py &
PROXY_PID=$!

# 4. Launch VLC in the background (muted)
echo "[3/4] Starting video transmission (VLC)..."
ffmpeg -re -i "$VIDEO_REF" -an -c:v copy -f rtp rtp://127.0.0.1:50000 > /dev/null 2>&1 &
VLC_PID=$!

# 5. Cleanup function (Executed when Ctrl+C is pressed or upon exit)
cleanup() {
    echo ""
    echo "Stopping background processes..."
    kill $PROXY_PID 2>/dev/null
    kill $VLC_PID 2>/dev/null
    echo "Clean environment."
    exit 0
}

# Catch the interrupt signal to clean up before exiting
trap cleanup SIGINT EXIT

echo "[4/4] Starting tcpdump capture..."
echo "========================================"
echo "Everything running. Press [Ctrl+C] to stop all."
echo "========================================"

# 6. Launch tcpdump in the foreground (without the '&' at the end)
sudo ip netns exec ue1 tcpdump -i oaitun_ue1 -n udp port 50002