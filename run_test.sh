#!/bin/bash

# ==========================================
# TEST CONFIGURATION
# ==========================================
IPERF_RATES=(0 30)             # Rates in Mbps
REPETITIONS=2              # Number of times each case is repeated
VIDEO_TIME=12              # Seconds to wait (10s for the video + 2s margin)
COOLING_TIME=4       # Seconds to empty MAC/RLC queues
IPERF_SERVER_IP="10.45.0.3"
SDP="escenario_1080p_5g.sdp"

RESULTS_FILE="vmaf_results.txt"
FFMPEG_BIN=$(echo ./ffmpeg-git-*-static/ffmpeg | awk '{print $1}')

# ==========================================
# VIDEO & INITIAL VALIDATIONS
# ==========================================
if [ -n "$1" ]; then
    VIDEO_REF="$1"
else
    read -rp "Input video reference: " VIDEO_REF
fi
if [ ! -f "$FFMPEG_BIN" ]; then
    echo "ERROR: ffmpeg not found at $FFMPEG_BIN"; exit 1
fi
if [ ! -f "$VIDEO_REF" ]; then
    echo "ERROR: Reference video not found: $VIDEO_REF"; exit 1
fi

# Clean result file
{
    echo "==============================================="
    echo " VMAF RESULTS"
    echo " Start: $(date)"
    echo "==============================================="
} > "$RESULTS_FILE"

# ==========================================
# PHASE 1: TRANSMISSION
# ==========================================
echo "Starting transmission phase..."

for i in "${!IPERF_RATES[@]}"; do
    CASE=$((i + 1))
    RATE=${IPERF_RATES[$i]}

    echo "--------------------------------------------------"
    echo "STARTING CASE $CASE: iperf at $RATE Mbps"
    echo "--------------------------------------------------"

    for rep in $(seq 1 $REPETITIONS); do
        echo "  -> Repetition $rep/$REPETITIONS  [$(date +%H:%M:%S)]"

        OUTPUT_FILE="output_1080p_5g_${CASE}_${rep}.mp4"
        IPERF_SERVER_PID=""
        IPERF_CLIENT_PID=""

        # 1. Start iperf3 server (ue2)
        sudo ip netns exec ue2 iperf3 -s < /dev/null > /dev/null 2>&1 &
        IPERF_SERVER_PID=$!

        sleep 0.1

        # 2. Receiver BEFORE transmitter
        sudo ip netns exec ue1 "$FFMPEG_BIN" -nostdin -y \
            -protocol_whitelist file,udp,rtp \
            -i "$SDP" \
            -c:v copy "$OUTPUT_FILE" < /dev/null > /dev/null 2>&1 &
        RX_PID=$!

        # Wait for ports to be listening
        sleep 0.5

        # 3. Launch transmitter with setsid to kill children (tcpdump, internal ffmpeg)
        setsid ./ffmpeg_proxy_tcpdump.sh "$VIDEO_REF" < /dev/null > /dev/null 2>&1 &
        TX_PID=$!

        sleep 0.1

        # 4. Launch iperf client in background
        if [ "$RATE" -gt 0 ]; then
            iperf3 -c "$IPERF_SERVER_IP" -u -b "${RATE}M" -t 20 < /dev/null > /dev/null 2>&1 &
            IPERF_CLIENT_PID=$!
        fi

        # 5. Wait video duration
        sleep "$VIDEO_TIME"

        # 6. Kill transmitter process group + others
        sudo kill -- -"$TX_PID" 2>/dev/null
        sudo kill "$RX_PID" 2>/dev/null
        [ -n "$IPERF_CLIENT_PID" ] && kill "$IPERF_CLIENT_PID" 2>/dev/null
        [ -n "$IPERF_SERVER_PID" ] && sudo kill "$IPERF_SERVER_PID" 2>/dev/null

        # Security kill to avoid stuck ports
        sudo pkill -9 iperf3 2>/dev/null

        # 7. Wait for processes to finish before cooling
        wait "$TX_PID" "$RX_PID" 2>/dev/null

        # 8. Verify the file exists and has real content
        if [ -f "$OUTPUT_FILE" ] && [ -s "$OUTPUT_FILE" ]; then
            echo "     [OK] $OUTPUT_FILE saved ($(du -h "$OUTPUT_FILE" | cut -f1)). Pause ${COOLING_TIME}s..."
        else
            echo "     [ERROR] $OUTPUT_FILE does not exist or is empty."
        fi

        sleep "$COOLING_TIME"
    done
done

echo ""
echo "Transmission phase completed. [$(date)]"
echo "Processing VMAF metrics in 5 seconds..."
sleep 5

# ==========================================
# FASE 2: VMAF CALCULATION
# ==========================================
echo "Starting VMAF calculation..."

for i in "${!IPERF_RATES[@]}"; do
    CASE=$((i + 1))
    RATE=${IPERF_RATES[$i]}

    {
        echo ""
        echo "Case $CASE (Iperf $RATE Mbps):"
    } >> "$RESULTS_FILE"

    for rep in $(seq 1 $REPETITIONS); do
        OUTPUT_FILE="output_1080p_5g_${CASE}_${rep}.mp4"
        JSON_SALIDA="vmaf_1080p_5g_${CASE}_${rep}.json"

        if [ ! -f "$OUTPUT_FILE" ] || [ ! -s "$OUTPUT_FILE" ]; then
            echo "  Rep $rep: ERROR - $OUTPUT_FILE no encontrado o vacío." >> "$RESULTS_FILE"
            continue
        fi

        echo "  Calculating VMAF for $OUTPUT_FILE..."

        "$FFMPEG_BIN" -nostdin -y \
            -i "$OUTPUT_FILE" -i "$VIDEO_REF" \
            -filter_complex "[0:v]settb=AVTB,setpts=PTS-STARTPTS[dist]; \
                             [1:v]settb=AVTB,setpts=PTS-STARTPTS[ref]; \
                             [dist][ref]libvmaf=log_fmt=json:log_path=$JSON_SALIDA" \
            -f null - < /dev/null > /dev/null 2>&1

        VMAF_SCORE=$(python3 -c "
import json
try:
    data = json.load(open('$JSON_SALIDA'))
    print(data.get('pooled_metrics', {}).get('vmaf', {}).get('mean', 'N/A'))
except Exception as e:
    print('Error: ' + str(e))
" 2>/dev/null)

        if [ -z "$VMAF_SCORE" ] || [ "$VMAF_SCORE" == "N/A" ]; then
            VMAF_SCORE="Error al extraer VMAF"
        fi

        echo "  Rep $rep: $VMAF_SCORE" >> "$RESULTS_FILE"
    done
done

echo "=================================================="
echo "PROCESS COMPLETED. [$(date)]"
echo "Results in: $RESULTS_FILE"
cat "$RESULTS_FILE"

# ==========================================
# PHASE 3: CLEAN UP GENERATED VIDEOS (optional)
# ==========================================
echo "Eliminating generated files..."
rm -f output_1080p_5g_*.mp4
rm -f vmaf_1080p_5g_*.json
echo "Generated files eliminated."