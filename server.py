import socket
import json
import time
import struct
import select
import os
import threading
import subprocess

import numpy as np
import psutil
try:
    from mss import MSS as mss
except ImportError:
    from mss import mss
from PIL import Image

# ================== CẤU HÌNH ==================
# IP của ESP32 hiện trên màn hình (màn 1) và Serial Monitor khi đã vào WiFi.
# Nếu máy Linux có avahi/nss-mdns thì dùng được "cyberdeck.local".
ESP32_HOST = "10.158.24.218"
PORT = 5000

MAGIC = 0xFF
TYPE_JSON = 1
TYPE_IMG = 2

IMG_W, IMG_H = 144, 74
JSON_INTERVAL = 1.0
CAPTURE_FPS = 30         # tốc độ chụp tối đa của luồng chụp
SCREEN_DSKS = 5          # chỉ số màn hình desktop trên ESP32
# ===============================================

last_net = psutil.net_io_counters()
last_net_time = time.time()
_last_energy_uj = None
_last_energy_time = None


def get_cpu_power(cpu):
    global _last_energy_uj, _last_energy_time
    try:
        rapl_path = "/sys/class/powercap/intel-rapl/intel-rapl:0/energy_uj"
        if not os.path.exists(rapl_path):
            for root, dirs, files in os.walk("/sys/class/powercap/intel-rapl"):
                if "energy_uj" in files:
                    rapl_path = os.path.join(root, "energy_uj")
                    break
        if os.path.exists(rapl_path):
            with open(rapl_path, "r") as f:
                energy_uj = int(f.read().strip())
            now = time.time()
            if _last_energy_uj is not None and _last_energy_time is not None:
                dt = now - _last_energy_time
                de = energy_uj - _last_energy_uj
                _last_energy_uj = energy_uj
                _last_energy_time = now
                if dt > 0 and de >= 0:
                    power_w = (de / 1_000_000.0) / dt
                    if 1.0 <= power_w <= 45.0:
                        return round(power_w * 1.15, 1)
            else:
                _last_energy_uj = energy_uj
                _last_energy_time = now
    except Exception:
        pass
    return round(4.2 + (cpu / 100.0) * 16.0, 1)


def get_intel_gpu_usage():
    try:
        card_path = "/sys/class/drm"
        if os.path.exists(card_path):
            for card in os.listdir(card_path):
                if card.startswith("card"):
                    cur_p = os.path.join(card_path, card, "gt_cur_freq_mhz")
                    max_p = os.path.join(card_path, card, "gt_max_freq_mhz")
                    if os.path.exists(cur_p) and os.path.exists(max_p):
                        with open(cur_p) as f:
                            cur_freq = float(f.read().strip())
                        with open(max_p) as f:
                            max_freq = float(f.read().strip())
                        if max_freq > 0:
                            raw = (cur_freq / max_freq) * 100.0
                            return round(min(max(raw * 0.55, 2.0), 99.0), 1)
    except Exception:
        pass
    return 15.0


def collect_stats():
    global last_net, last_net_time
    cpu = psutil.cpu_percent(interval=None)

    cpu_freq = 2.0
    try:
        fi = psutil.cpu_freq()
        if fi:
            cpu_freq = round(fi.current / 1000.0, 2)
    except Exception:
        pass

    vm = psutil.virtual_memory()

    temp = 45.0
    try:
        temps = psutil.sensors_temperatures()
        if "coretemp" in temps:
            temp = temps["coretemp"][0].current
    except Exception:
        pass

    cur = psutil.net_io_counters()
    now = time.time()
    elapsed = max(now - last_net_time, 0.001)
    net_down = (cur.bytes_recv - last_net.bytes_recv) / 1024 / elapsed
    net_up = (cur.bytes_sent - last_net.bytes_sent) / 1024 / elapsed
    last_net, last_net_time = cur, now

    return {
        "cpu": round(cpu, 1),
        "cpu_freq": cpu_freq,
        "cpu_power": get_cpu_power(cpu),
        "ram": round(vm.percent, 1),
        "ram_used": round(vm.used / (1024 ** 3), 1),
        "ram_total": round(vm.total / (1024 ** 3), 1),
        "gpu": get_intel_gpu_usage(),
        "temp": round(temp, 1),
        "net_down": round(net_down, 1),
        "net_up": round(net_up, 1),
    }


# ---------- Xử lý ảnh ----------
def fit_to_box(pil_img):
    """Thu nhỏ vừa khung 144x74, giữ tỉ lệ, phần thừa để đen."""
    img = pil_img
    scale = min(IMG_W / img.width, IMG_H / img.height)
    w = max(1, round(img.width * scale))
    h = max(1, round(img.height * scale))
    # BILINEAR + reducing_gap: nhanh hơn LANCZOS nhiều, chất lượng đủ cho 144x74
    img = img.resize((w, h), Image.BILINEAR, reducing_gap=3.0)
    canvas = Image.new("RGB", (IMG_W, IMG_H), (0, 0, 0))
    canvas.paste(img, ((IMG_W - w) // 2, (IMG_H - h) // 2))
    return canvas


def image_to_rgb565_bytes(pil_img):
    img = fit_to_box(pil_img)
    a = np.asarray(img, dtype=np.uint16)
    r, g, b = a[..., 0], a[..., 1], a[..., 2]
    rgb565 = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
    return rgb565.astype(">u2").tobytes()


def get_active_geometry():
    out = subprocess.check_output(
        ["xdotool", "getactivewindow", "getwindowgeometry", "--shell"],
        text=True, timeout=1)
    g = dict(line.split("=") for line in out.split() if "=" in line)
    return int(g["X"]), int(g["Y"]), int(g["WIDTH"]), int(g["HEIGHT"])


class Grabber(threading.Thread):
    """Luồng chụp riêng: chụp cửa sổ active bằng mss, thu nhỏ, đổi RGB565.
    Chỉ tăng seq khi khung thật sự đổi nội dung."""

    def __init__(self):
        super().__init__(daemon=True)
        self.lock = threading.Lock()
        self.active = threading.Event()
        self.data = None
        self.seq = 0

    def get(self):
        with self.lock:
            return self.data, self.seq

    def run(self):
        # mss phải được tạo ngay trong luồng dùng nó (X11 handle theo từng luồng)
        sct = mss()
        full = sct.monitors[0]
        geo, geo_t = None, 0.0
        period = 1.0 / CAPTURE_FPS

        while True:
            self.active.wait()
            t0 = time.time()
            try:
                if geo is None or t0 - geo_t > 0.3:   # chỉ hỏi vị trí cửa sổ ~3 lần/giây
                    try:
                        geo = get_active_geometry()
                    except Exception:
                        geo = None
                    geo_t = t0

                region = sct.monitors[1]
                if geo:
                    x, y, w, h = geo
                    x0 = max(x, full["left"])
                    y0 = max(y, full["top"])
                    x1 = min(x + w, full["left"] + full["width"])
                    y1 = min(y + h, full["top"] + full["height"])
                    if x1 - x0 > 8 and y1 - y0 > 8:
                        region = {"left": x0, "top": y0,
                                  "width": x1 - x0, "height": y1 - y0}

                shot = sct.grab(region)
                img = Image.frombytes("RGB", shot.size, shot.bgra, "raw", "BGRX")
                data = image_to_rgb565_bytes(img)

                with self.lock:
                    if data != self.data:
                        self.data = data
                        self.seq += 1
            except Exception as e:
                print(f"[-] Lỗi chụp: {e}")
                time.sleep(0.5)

            dt = time.time() - t0
            if dt < period:
                time.sleep(period - dt)


# ---------- Giao thức ----------
def send_frame(sock, ftype, payload):
    frame = struct.pack(">BBH", MAGIC, ftype, len(payload)) + payload + bytes([sum(payload) & 0xFF])
    sock.sendall(frame)   # TCP tự lo chia gói và điều tiết, không cần sleep


def read_screen_report(sock, current):
    while True:
        r, _, _ = select.select([sock], [], [], 0)
        if not r:
            return current
        data = sock.recv(64)
        if not data:
            raise ConnectionError("ESP32 đã đóng kết nối")
        if 0 <= data[-1] <= 5:
            current = data[-1]


def main():
    print("[*] Server giám sát + Desktop Stream qua WiFi...")
    psutil.cpu_percent(interval=None)

    grabber = Grabber()
    grabber.start()

    while True:
        sock = None
        try:
            sock = socket.create_connection((ESP32_HOST, PORT), timeout=5)
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            sock.settimeout(5)
            print(f"[+] Đã kết nối tới ESP32 CyberDeck ({ESP32_HOST}:{PORT})")

            screen = 0
            prev_screen = -1
            last_seq = -1
            next_json = 0.0
            frames, t_fps = 0, time.time()

            while True:
                screen = read_screen_report(sock, screen)
                now = time.time()

                if now >= next_json:
                    payload = json.dumps(collect_stats(), separators=(",", ":")).encode("utf-8")
                    send_frame(sock, TYPE_JSON, payload)
                    next_json = now + JSON_INTERVAL

                if screen == SCREEN_DSKS:
                    grabber.active.set()
                    if prev_screen != SCREEN_DSKS:
                        last_seq = -1          # vừa chuyển sang màn 6: gửi ngay 1 khung
                    data, seq = grabber.get()
                    if data is not None and seq != last_seq:
                        send_frame(sock, TYPE_IMG, data)
                        last_seq = seq
                        frames += 1
                else:
                    grabber.active.clear()     # không ở màn 6 thì ngừng chụp, đỡ tốn CPU

                prev_screen = screen

                if now - t_fps >= 2.0:
                    if frames:
                        print(f"[i] {frames / (now - t_fps):.1f} fps")
                    frames, t_fps = 0, now

                time.sleep(0.002)

        except Exception as e:
            print(f"[-] Mất kết nối: {e}. Thử lại sau 3 giây...")
            grabber.active.clear()
            if sock:
                try:
                    sock.close()
                except Exception:
                    pass
            time.sleep(3)


if __name__ == "__main__":
    main()
