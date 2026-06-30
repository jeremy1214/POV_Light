import os
import socket
import struct
import sys
import threading
import time
import json
import colorsys
import argparse

from PySide6.QtCore import QObject, QThread, QUrl, Signal
from PySide6.QtMultimedia import QAudioOutput, QMediaDevices, QMediaPlayer

# ============================================================
# 系統設定
# ============================================================
MUSIC_FILE = "2026_show.mp3"
MUSIC_OFFSET = 0.2  

PORT          = 12345
RESPONSE_PORT = 12346

UDP_STOP = 1937010544
UDP_HEARTBEAT = 1751474546

# ============================================================
# JSON 光表解析器 (專為光棍設計)
# ============================================================
# ============================================================
# JSON 光表解析器 (專為光棍設計)
# ============================================================
class StickData:
    def __init__(self, ip):
        self.ip = ip
        self.filename = f"LightStick_{self.ip}.json"
        self.frames = []
        self.last_modified = 0 
        self.load_json()

    def check_for_updates(self):
        if not os.path.exists(self.filename):
            return
        current_modified = os.path.getmtime(self.filename)
        if current_modified > self.last_modified:
            print(f"🔄 偵測到 {self.filename} 已更新，自動重新載入光表！")
            self.last_modified = current_modified
            self.load_json()

    def load_json(self):
        if not os.path.exists(self.filename):
            return
        try:
            self.frames = []
            with open(self.filename, 'r', encoding='utf-8') as f:
                data = json.load(f)
                show_data = data.get("2", [])
                acc_time = 0
                for item in show_data:
                    self.frames.append({
                        "start": acc_time,
                        "type": item.get("type", 0), # 抓取特效種類
                        "h": item.get("hue", 0),
                        "s": item.get("sat", 0),
                        "v": item.get("bri", 0),
                        "spd": item.get("spd", 128)  # 抓取特效速度
                    })
                    acc_time += item.get("dur", 0) * 1000
        except Exception as e:
            print(f"❌ 讀取 {self.filename} 失敗: {e}")

    # ★ 將原本的 color_at 改成 frame_at，回傳所有特效參數
    def frame_at(self, elapsed_ms):
        self.check_for_updates()
        if not self.frames:
            return None
        active = self.frames[0]
        for f in self.frames:
            if elapsed_ms >= f["start"]:
                active = f
            else:
                break
        return active

# ============================================================
# 裝置狀態
# ============================================================
class DeviceState:
    def __init__(self, ip, device_id):
        self.ip = ip
        self.device_id = device_id
        self.last_response_time = None
        self.status = "Disconnected"
        self.rssi = None
        self.light_data = StickData(ip) # 載入對應的 JSON

# ============================================================
# 音樂播放器 (與先前相同)
# ============================================================
class MusicPlayer(QObject):
    started = Signal()
    def __init__(self, file_path):
        super().__init__()
        self.player = QMediaPlayer()
        self.audio_output = QAudioOutput()
        self.player.setAudioOutput(self.audio_output)
        self.startTime = 0
        self._want_play = False
        self.player.setSource(QUrl.fromLocalFile(file_path))
        self.player.playbackStateChanged.connect(self._on_playback_state)

    def play_music(self):
        self._want_play = True
        target_ms = max(0, int(self.startTime * 1000))
        self.player.setPosition(target_ms)
        self.player.play()

    def stop_music(self):
        self._want_play = False
        self.player.stop()

    def set_start_time(self, t):
        self.startTime = t

    def _on_playback_state(self, state):
        if state == QMediaPlayer.PlaybackState.PlayingState and self._want_play:
            self.started.emit()

# ============================================================
# 背景網路執行緒
# ============================================================
class ResponseListener(QThread):
    response_received = Signal(str, str, str) # IP, Device ID, RSSI
    def __init__(self, sock, exit_event):
        super().__init__()
        self.sock = sock
        self.exit_event = exit_event

    def run(self):
        while not self.exit_event.is_set():
            try:
                data, addr = self.sock.recvfrom(1024)
                msg = data.decode()
                parts = [p.strip() for p in msg.split(":", 2)]
                
                device_id = parts[0] if len(parts) > 0 else "Unknown"
                rssi = parts[2] if len(parts) >= 3 else ""
                
                # 發送訊號給主執行緒
                self.response_received.emit(addr[0], device_id, rssi)
            except socket.timeout:
                continue
            except Exception:
                pass

class HeartbeatThread(QThread):
    def __init__(self, controller):
        super().__init__()
        self.controller = controller

    def run(self):
        while not self.controller.exit_event.is_set():
            if not self.controller.isRunning:
                self.controller.broadcast_message(UDP_HEARTBEAT)
            time.sleep(0.1) # 10Hz

# ============================================================
# 控制核心
# ============================================================
class Controller:
    def __init__(self, extra_ips=None):
        self.devices = {}
        self.extra_ips = set(extra_ips) if extra_ips else set()
        self.exit_event = threading.Event()
        
        self.isRunning = False
        self.rootTime = 0   
        self.startTime = 0  
        self.music_offset = MUSIC_OFFSET
        self.count = 0

        self.port = PORT
        self.response_port = RESPONSE_PORT
        self._setup_network()

        self.music_player = MusicPlayer(MUSIC_FILE) if os.path.exists(MUSIC_FILE) else None
        if self.music_player:
            self.music_player.started.connect(self._on_music_started)

    def add_target_ip(self, ip):
        if ip not in self.extra_ips:
            self.extra_ips.add(ip)
            # 強制建立一個裝置狀態，即使它還沒回傳訊號
            if ip not in self.devices:
                self.devices[ip] = DeviceState(ip, "Manual")
            print(f"📡 已將 {ip} 加入清單並載入光表")

    def _setup_network(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("", self.response_port))
        self.sock.settimeout(0.1)

        temp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        temp.connect(("8.8.8.8", 80))
        self.local_ip = temp.getsockname()[0]
        temp.close()

        octets = self.local_ip.split(".")
        self.broadcast_address = ".".join(octets[:3]) + ".255"

    def setup_threads(self, window):
        self.listener = ResponseListener(self.sock, self.exit_event)
        self.listener.response_received.connect(self._update_device_status)
        self.listener.start()
        self.heartbeat_thread = HeartbeatThread(self)
        self.heartbeat_thread.start()

    def _update_device_status(self, ip, device_id, rssi):
        if ip not in self.devices:
            print(f"🌟 自動發現新光棍: {ip} ({device_id})")
            self.devices[ip] = DeviceState(ip, device_id)
            self.extra_ips.add(ip) # 自動加入單獨發送清單
        
        d = self.devices[ip]
        d.last_response_time = time.time()
        d.status = "Connected"
        d.rssi = rssi

    def _send_data(self, data):
        self.sock.sendto(data, (self.broadcast_address, self.port))
        for ip in self.extra_ips:
            try: self.sock.sendto(data, (ip, self.port))
            except Exception: pass

    def broadcast_message(self, cmd_int):
        data = struct.pack("!I", cmd_int)
        self._send_data(data)

    def broadcast_time(self):
        if not self.isRunning: return
        now_ms = time.time() * 1000
        if now_ms - self.rootTime >= 1000 * self.count:
            self.count += 1
            number = int(now_ms - self.rootTime + self.startTime * 1000)
            if number <= 0: number = 1
            self._send_data(struct.pack("!I", number))

    def _on_music_started(self):
        self.rootTime = time.time() * 1000
        self.count = 0
        self.isRunning = True

    def start_function(self):
        self.count = 0
        if not self.music_player or not self.music_player.play_music():
            self.rootTime = time.time() * 1000
            self.isRunning = True

    def stop_function(self):
        self.isRunning = False
        self.rootTime = 0
        if self.music_player: self.music_player.stop_music()
        self.broadcast_message(UDP_STOP)