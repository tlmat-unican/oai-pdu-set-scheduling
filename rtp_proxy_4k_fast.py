import argparse
import gc
import signal
import socket
import sys
from pathlib import Path


DEFAULT_LISTEN_IP = "127.0.0.1"
DEFAULT_LISTEN_PORT = 50000
# # DEFAULT_DEST_IP = "127.0.0.1"
DEFAULT_DEST_IP = "10.45.0.2"
DEFAULT_DEST_PORT = 50002
DEFAULT_LOG_FILE = "frame_stats.log"
DEFAULT_FRAME_CSV = "ffprobe_data_30M.csv"
DEFAULT_VIDEO_CODEC = "H264"


def load_frame_data(csv_path):
    frames = []
    path = Path(csv_path)
    if not path.exists():
        raise FileNotFoundError(f"No se encontro el archivo de frames: {csv_path}")

    with path.open("r", encoding="utf-8") as f:
        for line_no, raw in enumerate(f, start=1):
            line = raw.strip()
            if not line:
                continue

            parts = [p.strip() for p in line.split(",")]
            if len(parts) < 2:
                raise ValueError(f"Linea invalida en {csv_path}:{line_no}: '{line}'")

            try:
                size_bytes = int(parts[0])
            except ValueError as exc:
                raise ValueError(
                    f"Tamano invalido en {csv_path}:{line_no}: '{parts[0]}'"
                ) from exc

            frame_type = parts[1].upper()
            if frame_type not in {"I", "P", "B"}:
                raise ValueError(f"Tipo invalido en {csv_path}:{line_no}: '{parts[1]}'")

            frames.append((size_bytes, frame_type))

    if not frames:
        raise ValueError(f"No hay frames en {csv_path}")

    return frames


def size_to_level(size_bytes):
    size_kb = size_bytes / 1024.0

    if size_kb < 10:
        return 0
    if size_kb < 20:
        return 1
    if size_kb < 30:
        return 2
    if size_kb < 45:
        return 3
    if size_kb < 60:
        return 4
    if size_kb < 80:
        return 5
    if size_kb < 130:
        return 6
    if size_kb < 180:
        return 7
    if size_kb < 230:
        return 8
    if size_kb < 280:
        return 9
    if size_kb < 330:
        return 10
    if size_kb < 380:
        return 11
    if size_kb < 430:
        return 12
    if size_kb < 500:
        return 13
    if size_kb <= 750:
        return 14
    return 15


def build_base_dscp(frame_type, size_level):
    type_bit = 1 if frame_type == "I" else 0
    return (type_bit << 5) | (size_level & 0x0F)


def prepare_frame_meta(frame_data):
    meta = []
    for size_bytes, frame_type in frame_data:
        size_level = size_to_level(size_bytes)
        base_dscp = build_base_dscp(frame_type, size_level)
        meta.append((base_dscp, frame_type))
    return meta


def create_dscp_socket_pool(dest_ip, dest_port, sndbuf_bytes):
    pool = [None] * 64
    for dscp in range(64):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        if sndbuf_bytes > 0:
            s.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, sndbuf_bytes)
        s.setsockopt(socket.IPPROTO_IP, socket.IP_TOS, dscp << 2)
        s.connect((dest_ip, dest_port))
        pool[dscp] = s
    return pool


def write_stats(log_file, codec, stats, source_csv, source_frames):
    with open(log_file, "w", encoding="utf-8") as f:
        f.write(
            "codec,i_frames,p_frames,b_frames,last_frames,total_pkts,send_errors,"
            "source_csv,source_frames,csv_wraps\n"
        )
        f.write(
            f"{codec},{stats['I']},{stats['P']},{stats['B']},{stats['last']},"
            f"{stats['pkts']},{stats['send_errors']},{source_csv},{source_frames},"
            f"{stats['wrapped']}\n"
        )


def parse_args(argv):
    parser = argparse.ArgumentParser(
        description="RTP DSCP proxy optimizado para flujo 4K"
    )
    parser.add_argument("--listen-ip", default=DEFAULT_LISTEN_IP)
    parser.add_argument("--listen-port", type=int, default=DEFAULT_LISTEN_PORT)
    parser.add_argument("--dest-ip", default=DEFAULT_DEST_IP)
    parser.add_argument("--dest-port", type=int, default=DEFAULT_DEST_PORT)
    parser.add_argument("--frame-csv", default=DEFAULT_FRAME_CSV)
    parser.add_argument("--log-file", default=DEFAULT_LOG_FILE)
    parser.add_argument("--codec", default=DEFAULT_VIDEO_CODEC, choices=["H264", "H265"])
    parser.add_argument("--rcvbuf", type=int, default=16 * 1024 * 1024)
    parser.add_argument("--sndbuf", type=int, default=8 * 1024 * 1024)
    parser.add_argument("--timeout", type=float, default=0.2)
    parser.add_argument("--disable-gc", action="store_true")
    return parser.parse_args(argv)


def main(argv=None):
    args = parse_args(argv if argv is not None else sys.argv[1:])

    def _handle_sigterm(_signum, _frame):
        raise KeyboardInterrupt

    signal.signal(signal.SIGTERM, _handle_sigterm)

    frame_data = load_frame_data(args.frame_csv)
    frame_meta = prepare_frame_meta(frame_data)

    sock_in = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock_in.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, args.rcvbuf)
    sock_in.bind((args.listen_ip, args.listen_port))
    sock_in.settimeout(args.timeout)

    sock_pool = create_dscp_socket_pool(args.dest_ip, args.dest_port, args.sndbuf)

    if args.disable_gc:
        gc.disable()

    stats = {"I": 0, "P": 0, "B": 0, "last": 0, "pkts": 0, "wrapped": 0, "send_errors": 0}
    frame_index = 0

    packet_buf = bytearray(65535)
    packet_view = memoryview(packet_buf)

    recv_into = sock_in.recvfrom_into
    frame_meta_local = frame_meta
    frame_meta_len = len(frame_meta_local)
    sock_pool_local = sock_pool

    print("[*] Proxy DSCP FAST activo")
    print(f"[*] Escuchando en {args.listen_ip}:{args.listen_port}")
    print(f"[*] Reenviando a {args.dest_ip}:{args.dest_port}")
    print(f"[*] Cargados {frame_meta_len} frames desde {args.frame_csv}")

    try:
        while True:
            try:
                pkt_len, _addr = recv_into(packet_buf)
            except socket.timeout:
                continue

            if pkt_len < 12:
                continue

            if frame_index >= frame_meta_len:
                frame_index = 0
                stats["wrapped"] += 1

            base_dscp, frame_type = frame_meta_local[frame_index]
            marker_set = (packet_view[1] & 0x80) != 0
            dscp = base_dscp | (0x10 if marker_set else 0)

            try:
                sock_pool_local[dscp].send(packet_view[:pkt_len])
            except OSError:
                stats["send_errors"] += 1
                continue

            stats["pkts"] += 1

            if marker_set:
                stats["last"] += 1
                stats[frame_type] += 1
                frame_index += 1

    except KeyboardInterrupt:
        print("\n[!] Interrupcion detectada. Guardando resultados...")
    finally:
        try:
            write_stats(args.log_file, args.codec, stats, args.frame_csv, frame_meta_len)
            print(f"[*] Datos exportados a {args.log_file}")
            print(
                f"[*] Resumen: {stats['I']} I | {stats['P']} P | {stats['B']} B | "
                f"{stats['last']} Last | {stats['pkts']} Pkts | wraps={stats['wrapped']} | "
                f"send_errors={stats['send_errors']}"
            )
        except Exception as exc:
            print(f"[!] Error al guardar log: {exc}")

        sock_in.close()
        for s in sock_pool:
            if s is not None:
                s.close()

        if args.disable_gc:
            gc.enable()


if __name__ == "__main__":
    main()