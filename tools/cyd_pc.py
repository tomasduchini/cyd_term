#!/usr/bin/env python3
"""Manda metricas de la PC a CYD-TERM por el puerto serie.

Requiere:  pip install pyserial psutil requests
Temperaturas y carga de GPU: LibreHardwareMonitor abierto (como administrador)
con Options > Remote Web Server activado (puerto 8085).

Uso:
  python tools/cyd_pc.py COM3 --presentmon C:\ruta\PresentMon.exe     enviar metricas
  python tools/cyd_pc.py --list   listar los sensores que ve LibreHardwareMonitor
"""
import argparse
import re
import time

import psutil
import requests
import serial

import collections
import ctypes
import subprocess
import threading
from ctypes import wintypes

import json
import winreg

LHM_URL = "http://localhost:8085/data.json"

# Por cada clave: (grupo, texto) que se buscan en la ruta del sensor; gana el primero que aparece.
# Si --list muestra otros nombres para tus sensores, ajustalos aca.
SENSORS = {
    "cput": [("Temperatures", "Tctl/Tdie"), ("Temperatures", "CPU Package")],
    "gput": [("Temperatures", "GPU Core")],
    "gpu":  [("Load", "GPU Core")],
}

NUM = re.compile(r"-?\d+(?:[.,]\d+)?")
# Argumentos de PresentMon 2.x. Si tu version se queja, mira "PresentMon --help" y ajusta aca
# (la 1.x usa un solo guion: -output_stdout -stop_existing_session -session_name cydterm).
PM_ARGS = ["--output_stdout", "--stop_existing_session", "--session_name", "cydterm"]
FT_COLUMNS = ["FrameTime", "MsBetweenPresents"]  # nombre de la columna segun la version
LOW_WINDOW_S = 10.0  # ventana para el 1 % low: con 1 s no hay frames suficientes

INFO_EVERY_S = 30  # cada cuanto se reenvian los modelos (por si la placa se reinicio)


def short_cpu(name):
    s = re.sub(r"\((R|TM)\)", "", name, flags=re.I)
    s = re.sub(r"\b\d+-Core\b|\bProcessor\b|\bCPU\b|@.*$|with .*$", "", s, flags=re.I)
    s = re.sub(r"^\s*(AMD|Intel)\s+", "", s)
    return " ".join(s.split())


def short_gpu(name):
    s = re.sub(r"\((R|TM)\)", "", name, flags=re.I)
    s = re.sub(r"^\s*(AMD|NVIDIA|Intel)\s+", "", s)
    s = re.sub(r"^(Radeon|GeForce)\s+", "", s)
    return " ".join(s.split())


def hw_info():
    """Modelos de CPU, GPU y RAM, ya acortados. Lo que no se pueda leer, se omite."""
    info = {}
    try:
        key = r"HARDWARE\DESCRIPTION\System\CentralProcessor\0"
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, key) as k:
            info["cpu"] = short_cpu(winreg.QueryValueEx(k, "ProcessorNameString")[0])
    except OSError:
        pass

    ps = ("$g = Get-CimInstance Win32_VideoController | Select-Object Name,AdapterRAM; "
          "$m = Get-CimInstance Win32_PhysicalMemory | "
          "Select-Object Capacity,ConfiguredClockSpeed,Speed,SMBIOSMemoryType; "
          "@{gpu=@($g); ram=@($m)} | ConvertTo-Json -Compress -Depth 3")
    try:
        out = subprocess.run(["powershell", "-NoProfile", "-Command", ps], capture_output=True,
                             text=True, timeout=20, creationflags=subprocess.CREATE_NO_WINDOW).stdout
        data = json.loads(out)
        gpus = [g for g in data.get("gpu") or [] if g.get("Name") and "Basic" not in g["Name"]]
        if gpus:
            g = max(gpus, key=lambda g: g.get("AdapterRAM") or 0)
            info["gpu"] = short_gpu(g["Name"])
        mods = data.get("ram") or []
        if mods:
            gb = round(sum(int(m.get("Capacity") or 0) for m in mods) / 2**30)
            types = {20: "DDR", 21: "DDR2", 24: "DDR3", 26: "DDR4", 34: "DDR5"}
            kind = types.get(mods[0].get("SMBIOSMemoryType"), "")
            mhz = mods[0].get("ConfiguredClockSpeed") or mods[0].get("Speed") or 0
            info["ram"] = " ".join(x for x in (f"{gb}GB", kind, str(mhz) if mhz else "") if x)
    except (OSError, ValueError, subprocess.SubprocessError):
        pass
    return info


def info_line(info):
    def clean(v):  # sin separadores del protocolo, solo ASCII, 20 caracteres
        v = re.sub(r"[;=\r\n]", " ", v).encode("ascii", "ignore").decode()
        return v.strip()[:20]
    return "PCI " + ";".join(f"{k}={clean(v)}" for k, v in info.items()) + "\n"


class FpsReader(threading.Thread):
    """Corre PresentMon y guarda los frametimes del ultimo segundo por proceso."""

    def __init__(self, exe):
        super().__init__(daemon=True)
        self.exe = exe
        self.lock = threading.Lock()
        self.frames = collections.deque()  # (t, pid, app, frametime_ms)
        self.proc = None
        self.preamble = []  # lineas previas al encabezado, para diagnostico

    def run(self):
        try:
            self.proc = subprocess.Popen([self.exe, *PM_ARGS], stdout=subprocess.PIPE,
                                         stderr=subprocess.STDOUT, text=True, bufsize=1)
        except OSError as e:
            print("No pude lanzar PresentMon:", e)
            return

        cols_idx = None
        for line in self.proc.stdout:
            cols = line.strip().split(",")
            if cols_idx is None:
                if "Application" in cols and "ProcessID" in cols:
                    idx = {name: i for i, name in enumerate(cols)}
                    ft = next((idx[c] for c in FT_COLUMNS if c in idx), None)
                    if ft is None:
                        print("PresentMon: no encuentro la columna de frametime en:", line.strip())
                        return
                    cols_idx = (idx["ProcessID"], idx["Application"], ft)
                elif len(self.preamble) < 10:
                    self.preamble.append(line.rstrip())
                continue

            pid_i, app_i, ft_i = cols_idx
            try:
                pid, app, ft = int(cols[pid_i]), cols[app_i], float(cols[ft_i])
            except (ValueError, IndexError):
                continue  # filas con "NA" u otras cosas
            now = time.monotonic()
            with self.lock:
                self.frames.append((now, pid, app, ft))
                while self.frames and now - self.frames[0][0] > LOW_WINDOW_S:
                    self.frames.popleft()

        if cols_idx is None:
            print("PresentMon termino sin datos. Salida:")
            for l in self.preamble:
                print("   ", l)

    def fps_for(self, pid):
        """(fps promedio del ultimo segundo, 1 % low de los ultimos 10 s, app) o (None, None, None)."""
        now = time.monotonic()
        with self.lock:
            mine = [(f[0], f[2], f[3]) for f in self.frames if f[1] == pid]
        last = [ft for t, _, ft in mine if now - t <= 1.0 and ft > 0]
        if len(last) < 2:
            return None, None, None
        avg = 1000.0 / (sum(last) / len(last))
        fts = sorted((ft for _, _, ft in mine if ft > 0), reverse=True)
        worst = fts[:max(1, len(fts) // 100)]  # el 1 % de frames mas lentos
        low = 1000.0 / (sum(worst) / len(worst))
        return avg, low, mine[-1][1]

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()


def foreground_pid():
    hwnd = ctypes.windll.user32.GetForegroundWindow()
    pid = wintypes.DWORD()
    ctypes.windll.user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
    return pid.value


def clean_app(name):
    name = re.sub(r"\.exe$", "", name, flags=re.I)
    return re.sub(r"[^A-Za-z0-9_.-]", "", name).lower()[:20]


def lhm_sensors():
    """Lista de (ruta, valor) de todos los sensores; vacia si LHM no responde."""
    try:
        root = requests.get(LHM_URL, timeout=1).json()
    except (requests.RequestException, ValueError):
        return []
    out = []

    def walk(node, path):
        text = node.get("Text", "")
        p = path + [text] if text else path
        kids = node.get("Children", [])
        val = node.get("Value", "")
        if not kids and val:
            out.append((p, val))
        for k in kids:
            walk(k, p)

    walk(root, [])
    return out


def number(s):
    m = NUM.search(s)
    return float(m.group().replace(",", ".")) if m else None  # LHM puede usar coma decimal


def pick(sensors, rules):
    for group, text in rules:
        for path, val in sensors:
            if group in path and text in path[-1]:
                v = number(val)
                if v is not None:
                    return v
    return None


def build_line(sensors, fps_reader=None):
    vm = psutil.virtual_memory()
    vals = {
        "cpu": psutil.cpu_percent(interval=None),
        "ram": vm.percent,
        "ramu": (vm.total - vm.available) / 2**30,
        "ramt": vm.total / 2**30,
    }
    for key, rules in SENSORS.items():
        v = pick(sensors, rules)
        if v is not None:
            vals[key] = v

    parts = [f"{k}={v:.1f}" for k, v in vals.items()]
    if fps_reader:
        fps, low, app = fps_reader.fps_for(foreground_pid())
        if fps is not None:
            parts.append(f"fps={fps:.1f}")
            parts.append(f"fps1={low:.1f}")
            parts.append(f"app={clean_app(app)}")
    return "PC " + ";".join(parts) + "\n"


def open_port(name):
    ser = serial.Serial()
    ser.port = name
    ser.baudrate = 115200
    ser.dtr = False  # sin esto, abrir el puerto resetea el ESP32
    ser.rts = False
    ser.open()
    return ser


def main():
    ap = argparse.ArgumentParser(description="Metricas de la PC para CYD-TERM")
    ap.add_argument("port", nargs="?", help="puerto serie, p. ej. COM3")
    ap.add_argument("--list", action="store_true", help="listar sensores de LibreHardwareMonitor")
    ap.add_argument("--presentmon", metavar="EXE", help="ruta a PresentMon.exe para medir FPS")
    args = ap.parse_args()

    if args.list:
        sensors = lhm_sensors()
        if not sensors:
            print("No pude leer LibreHardwareMonitor en", LHM_URL)
            return
        for path, val in sensors:
            print(f"{val:>14}  {' / '.join(path)}")
        return

    if not args.port:
        ap.error("falta el puerto, p. ej. COM3")

    psutil.cpu_percent(interval=None)  # la primera lectura siempre da 0
    info = hw_info()
    print("Hardware:", info)
    last_info = 0.0
    fps_reader = None
    if args.presentmon:
        fps_reader = FpsReader(args.presentmon)
        fps_reader.start()

    ser = open_port(args.port)
    print(f"Enviando a {args.port}. Ctrl+C para salir.")
    try:
        while True:
            if time.monotonic() - last_info > INFO_EVERY_S:
                ser.write(info_line(info).encode("ascii"))
                last_info = time.monotonic()
            line = build_line(lhm_sensors(), fps_reader)
            ser.write(line.encode("ascii"))
            ser.reset_input_buffer()  # descarta los logs que manda el ESP32
            print(line.strip())
            time.sleep(1)
    except KeyboardInterrupt:
        pass
    finally:
        if fps_reader:
            fps_reader.stop()
        ser.close()


if __name__ == "__main__":
    main()