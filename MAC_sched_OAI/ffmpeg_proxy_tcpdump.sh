#!/bin/bash

# 1. Pedir credenciales de sudo al principio
echo "Validando permisos de administrador..."
sudo -v

echo "========================================"
echo "Iniciando entorno de pruebas"
echo "========================================"

# 2. Configurar el Namespace
echo "[1/4] Configurando interfaz en el namespace ue_ns..."
sudo ip link set oaitun_ue1 netns ue_ns
sudo ip netns exec ue_ns ip link set oaitun_ue1 up
sudo ip netns exec ue_ns ip addr add 10.45.0.2/24 dev oaitun_ue1
sudo ip netns exec ue_ns ip route add default dev oaitun_ue1

# 3. Lanzar el Proxy Python en SEGUNDO plano
echo "[2/4] Iniciando Proxy RTP (Marcado DSCP)..."
# python3 rtp_proxy.py &
python3 rtp_proxy_4k_fast.py &
PROXY_PID=$!

# 4. Lanzar VLC en SEGUNDO plano (silenciado)
echo "[3/4] Iniciando transmisión de video (VLC)..."
# cvlc videoSD640.mp4 --sout '#rtp{dst=127.0.0.1,port=5000}' --loop > /dev/null 2>&1 &
ffmpeg -re -i video_referencia_1080p_10s.mp4 -an -c:v copy -f rtp rtp://127.0.0.1:50000 > /dev/null 2>&1 &
# cvlc video_lg.mp4 --sout '#rtp{dst=127.0.0.1,port=5000}' --mtu 1300 --sout-mux-caching=50 --loop > /dev/null 2>&1 &
# cvlc video_lg.mp4 --sout '#rtp{dst=127.0.0.1,port=5000}' --loop > /dev/null 2>&1 &
VLC_PID=$!

# 5. Función de limpieza (Se ejecuta al pulsar Ctrl+C o al cerrar)
cleanup() {
    echo ""
    echo "Deteniendo procesos en segundo plano..."
    kill $PROXY_PID 2>/dev/null
    kill $VLC_PID 2>/dev/null
    echo "Entorno limpio."
    exit 0
}

# Atrapamos la señal de interrupción para limpiar antes de salir
trap cleanup SIGINT EXIT

echo "[4/4] Iniciando captura tcpdump..."
echo "========================================"
echo "Todo en marcha. Presiona [Ctrl+C] para detener todo."
echo "========================================"

# 6. Lanzar tcpdump en PRIMER plano (sin el '&' al final)
# sudo ip netns exec ue_ns tcpdump -n -i oaitun_ue1 -v udp port 50002
sudo ip netns exec ue1 tcpdump -i oaitun_ue1 -n udp port 50002