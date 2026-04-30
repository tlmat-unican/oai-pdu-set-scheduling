#!/bin/bash

# Configuration and build directories
OAI_DIR="$HOME/pdu-set-oai/MAC_sched_OAI/openairinterface5g"
CONF_BASE="$OAI_DIR/targets/PROJECTS/GENERIC-NR-5GC/CONF"
BUILD_DIR="$OAI_DIR/cmake_targets/ran_build/build"

CONF_GNB_106PRB="00_106PRB_gNB_RFSIM.conf"
CONF_GNB_25PRB="00_25PRB_gNB_RFSIM.conf"

# Function for stopping processes aggressively
cleanup() {
    echo -e "\n\n[!] Stopping all OAI processes..."
    
    # 1. Try graceful closure with SIGINT
    sudo pkill -INT nr-softmodem
    sudo pkill -INT nr-uesoftmodem
    
    echo "Waiting for processes to close..."
    sleep 4
    
    # 2. Forzar cierre total con SIGKILL si siguen activos
    if pgrep -x "nr-softmodem" > /dev/null || pgrep -x "nr-uesoftmodem" > /dev/null; then
        echo "Forcing closure (SIGKILL)..."
        sudo pkill -9 nr-softmodem
        sudo pkill -9 nr-uesoftmodem
    fi
    
    echo "[!] Environment cleaned up correctly."
    exit
}

# Capture Ctrl+C
trap cleanup SIGINT SIGTERM

# Change to working directory
if [ ! -d "$BUILD_DIR" ]; then
    echo "Error: Directory $BUILD_DIR does not exist"
    exit 1
fi
cd "$BUILD_DIR"

echo "Starting gNB and UEs..."

# Run gNB
sudo ./nr-softmodem -O "$CONF_BASE/$CONF_GNB_25PRB" --gNBs.[0].min_rxtxtime 6 --rfsim --gNBs.[0].um_on_default_drb 1 > /dev/null 2>&1 &
# sudo ./nr-softmodem -O "$CONF_BASE/$CONF_GNB_106PRB" --gNBs.[0].min_rxtxtime 6 --rfsim --gNBs.[0].um_on_default_drb 1 > /dev/null 2>&1 &
sleep 1

# Run UE1
sudo ip netns exec ue1 ./nr-uesoftmodem -O "$CONF_BASE/ue.conf" -r 25 --numerology 0 --band 66 -C 2152680000 --CO -400000000 --ssb 48 --rfsim --rfsimulator.serveraddr 10.201.1.100 > /dev/null 2>&1 &
# sudo ip netns exec ue1 ./nr-uesoftmodem -O "$CONF_BASE/ue.conf" -r 106 --numerology 1 --band 78 -C 3619200000 --rfsim --rfsimulator.serveraddr 10.201.1.100 > /dev/null 2>&1 &
sleep 1

# Run UE2
sudo ip netns exec ue2 ./nr-uesoftmodem -O "$CONF_BASE/ue_2.conf" -r 25 --numerology 0 --band 66 -C 2152680000 --CO -400000000 --ssb 48 --rfsim --rfsimulator.serveraddr > /dev/null 2>&1 &
# sudo ip netns exec ue2 ./nr-uesoftmodem -O "$CONF_BASE/ue_2.conf" -r 106 --numerology 1 --band 78 -C 3619200000 --rfsim --rfsimulator.serveraddr 10.202.1.100 > /dev/null 2>&1 &
sleep 1

# Run UE3
sudo ip netns exec ue3 ./nr-uesoftmodem -O "$CONF_BASE/ue_3.conf" -r 25 --numerology 0 --band 66 -C 2152680000 --CO -400000000 --ssb 48 --rfsim --rfsimulator.serveraddr 10.203.1.100 > /dev/null 2>&1 &
# sudo ip netns exec ue3 ./nr-uesoftmodem -O "$CONF_BASE/ue_3.conf" -r 106 --numerology 1 --band 78 -C 3619200000 --rfsim --rfsimulator.serveraddr 10.203.1.100 > /dev/null 2>&1 &

echo -e "\n[+] All processes launched. Press Ctrl+C in this terminal to close them all safely."

# Keep the script active waiting for signals
wait