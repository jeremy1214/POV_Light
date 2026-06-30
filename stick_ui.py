import sys
import os
import re
import math
import random
import colorsys
import time as time_module

from PySide6.QtCore import Qt, QTimer, QRectF
from PySide6.QtGui import QColor, QFont, QPainter, QBrush, QPen
from PySide6.QtWidgets import (QApplication, QMainWindow, QWidget, QVBoxLayout,
                               QHBoxLayout, QPushButton, QLabel,
                               QGridLayout, QScrollArea)

from stick_control import Controller

# ============================================================
# 色彩主題設定
# ============================================================
BG = "#0d1117"
SURFACE = "#161b22"
TEXT = "#c9d1d9"
DIM = "#8b949e"
ACCENT = "#238636"
DANGER = "#da3633"

# ============================================================
# 雙 IP 虛擬光棍 UI 元件 (搭載 Python 虛擬特效引擎)
# ============================================================
class StickWidget(QWidget):
    def __init__(self, stick_id, ip1, ip2):
        super().__init__()
        self.stick_id = stick_id
        self.ip1 = ip1
        self.ip2 = ip2
        
        self.online1 = False
        self.online2 = False
        
        self.frame1 = None
        self.frame2 = None
        self.elapsed_ms = 0
        
        self.setFixedSize(120, 340)

    def update_state(self, online1, online2, frame1, frame2, elapsed_ms):
        self.online1 = online1
        self.online2 = online2
        self.frame1 = frame1
        self.frame2 = frame2
        self.elapsed_ms = elapsed_ms
        self.update()

    # ★ Python 版的 FastLED 渲染引擎 (模擬 30 顆 LED)
    def render_leds(self, frame, ms, num_leds=30):
        if not frame:
            return [(40, 40, 45)] * num_leds
            
        t = frame.get("type", 0)
        h = frame.get("h", 0)
        s = frame.get("s", 0)
        v = frame.get("v", 0)
        spd = frame.get("spd", 128)
        
        leds = [(0, 0, 0)] * num_leds
        
        def hsv2rgb(h_8, s_8, v_8):
            r, g, b = colorsys.hsv_to_rgb(h_8/255.0, s_8/255.0, v_8/255.0)
            return (int(r*255), int(g*255), int(b*255))
            
        def beatsin8(bpm, low, high, time_ms):
            if bpm == 0: return low
            period = 60000.0 / bpm
            phase = (time_ms % period) / period * 2 * math.pi
            val = (math.sin(phase) + 1.0) / 2.0 
            return int(low + val * (high - low))
            
        beat = beatsin8(spd / 4 + 20, 0, 255, ms)
        
        if t == 0: # Solid (長亮)
            c = hsv2rgb(h, s, v)
            leds = [c] * num_leds
        elif t == 1: # Wave (波浪)
            for i in range(num_leds):
                hue = (h + (i * 8) + (ms * spd // 5)) % 256
                leds[i] = hsv2rgb(hue, s, v)
        elif t == 2: # Symmetry (對稱)
            center = num_leds // 2
            pos = (ms * spd // 20) % center
            c = hsv2rgb(h, s, v)
            leds[center + pos] = c
            leds[center - 1 - pos] = c
        elif t == 3: # Breathe (呼吸)
            val = int(0 + (beat/255.0) * (v - 0))
            c = hsv2rgb(h, s, val)
            leds = [c] * num_leds
        elif t == 4: # Tri-Color (三色)
            for i in range(num_leds):
                offset = (i + (ms * spd // 10)) % 3
                if offset == 0: hue = h
                elif offset == 1: hue = (h + 85) % 256
                else: hue = (h + 170) % 256
                leds[i] = hsv2rgb(hue, s, v)
        elif t == 5: # Sparkle (閃爍)
            for i in range(num_leds):
                random.seed((ms // 50) + i * 100) # 每 50ms 重新隨機
                if random.randint(0, 255) < (spd / 2):
                    leds[i] = hsv2rgb(h, s, v)
                    
        return leds

    def paintEvent(self, event):
        p = QPainter(self)
        p.setRenderHint(QPainter.Antialiasing)
        
        p.fillRect(self.rect(), QColor(SURFACE))
        
        p.setPen(QColor(TEXT if (self.online1 or self.online2) else DIM))
        f = QFont("Arial", 11, QFont.Bold)
        p.setFont(f)
        p.drawText(0, 10, self.width(), 20, Qt.AlignCenter, f"Stick {self.stick_id}")
        
        f.setPointSize(8)
        p.setFont(f)
        c1 = ACCENT if self.online1 else DANGER
        p.setPen(QColor(c1))
        p.drawText(0, 30, self.width()//2, 20, Qt.AlignCenter, f".{self.ip1.split('.')[-1]}")
        c2 = ACCENT if self.online2 else DANGER
        p.setPen(QColor(c2))
        p.drawText(self.width()//2, 30, self.width()//2, 20, Qt.AlignCenter, f".{self.ip2.split('.')[-1]}")

        stick_width = 30
        stick_height = 250
        x = (self.width() - stick_width) // 2
        y = 65
        
        # === 畫虛擬 LED 燈珠 ===
        led_h = (stick_height / 2.0) / 30.0
        
        # IP1 (上半部，共 30 顆 LED)
        leds1 = self.render_leds(self.frame1, self.elapsed_ms, 30) if self.online1 else [(40,40,45)]*30
        for i, c in enumerate(reversed(leds1)):
            p.fillRect(QRectF(x, y + i * led_h, stick_width, led_h), QColor(*c))
            
        # IP2 (下半部，共 30 顆 LED)
        y_bottom = y + stick_height / 2.0
        leds2 = self.render_leds(self.frame2, self.elapsed_ms, 30) if self.online2 else [(40,40,45)]*30
        for i, c in enumerate(reversed(leds2)):
            p.fillRect(QRectF(x, y_bottom + i * led_h, stick_width, led_h), QColor(*c))

        # 畫外框與中線
        p.setBrush(Qt.NoBrush)
        p.setPen(QPen(QColor(60, 60, 65), 2))
        p.drawRoundedRect(x, y, stick_width, stick_height, 15, 15)
        p.drawLine(x, int(y + stick_height/2), x + stick_width, int(y + stick_height/2))
        p.end()

# ============================================================
# 主控台視窗 (MonitorWindow)
# ============================================================
class MonitorWindow(QMainWindow):
    def __init__(self, controller, sticks_map):
        super().__init__()
        self.controller = controller
        self.sticks_map = sticks_map  
        self.stick_widgets = {}       

        self.setWindowTitle("Lightdance 智慧光棍控制中心")
        self.resize(900, 600)
        self.setStyleSheet(f"QMainWindow {{ background-color: {BG}; }}")

        central = QWidget()
        self.setCentralWidget(central)
        root = QVBoxLayout(central)
        root.setContentsMargins(20, 20, 20, 20)

        # 系統狀態提示
        info_label = QLabel(f"🔍 自動掃描完成：已載入 {len(self.sticks_map)} 根光棍，共 {len(self.sticks_map)*2} 個 IP")
        info_label.setStyleSheet(f"color: {DIM}; font-size: 14px; font-weight: bold;")
        root.addWidget(info_label)

        # --- 光棍顯示區 ---
        self.scroll = QScrollArea()
        self.scroll.setWidgetResizable(True)
        self.scroll.setStyleSheet(f"QScrollArea {{ border: none; background: transparent; }}")
        
        self.grid_container = QWidget()
        self.grid = QGridLayout(self.grid_container)
        self.grid.setAlignment(Qt.AlignTop | Qt.AlignLeft)
        self.scroll.setWidget(self.grid_container)
        root.addWidget(self.scroll, stretch=1)

        self._init_stick_widgets()

        # --- 播放控制區 ---
        ctrl_layout = QHBoxLayout()
        
        self.lbl_time = QLabel("0.00 s")
        self.lbl_time.setFont(QFont("Consolas", 18, QFont.Bold))
        self.lbl_time.setStyleSheet(f"color: {ACCENT};")
        self.lbl_time.setFixedWidth(120)
        
        self.btn_play = QPushButton("▶️ Start (開始展演)")
        self.btn_play.setMinimumHeight(50)
        self.btn_play.setStyleSheet(f"background: {ACCENT}; color: white; font-size: 16px; border-radius: 8px;")
        self.btn_play.clicked.connect(self._toggle_play)

        ctrl_layout.addWidget(self.lbl_time)
        ctrl_layout.addWidget(self.btn_play)
        root.addLayout(ctrl_layout)

        self.controller.setup_threads(self)
        self._setup_timers()

    def _init_stick_widgets(self):
        col_count = 6 
        for idx, stick_id in enumerate(sorted(self.sticks_map.keys())):
            ip1, ip2 = self.sticks_map[stick_id]
            widget = StickWidget(stick_id, ip1, ip2)
            self.stick_widgets[stick_id] = widget
            
            row, col = divmod(idx, col_count)
            self.grid.addWidget(widget, row, col)

    def _toggle_play(self):
        if self.controller.isRunning:
            self.controller.stop_function()
            self.btn_play.setText("▶️ Start (開始展演)")
            self.btn_play.setStyleSheet(f"background: {ACCENT}; color: white; font-size: 16px; border-radius: 8px;")
        else:
            self.controller.startTime = 0 
            self.controller.start_function()
            self.btn_play.setText("⏹️ Stop (停止)")
            self.btn_play.setStyleSheet(f"background: {DANGER}; color: white; font-size: 16px; border-radius: 8px;")

    def _setup_timers(self):
        self.update_timer = QTimer(self)
        self.update_timer.timeout.connect(self._update_ui)
        self.update_timer.start(33)

        self.broadcast_timer = QTimer(self)
        self.broadcast_timer.timeout.connect(self.controller.broadcast_time)
        self.broadcast_timer.start(1000)

    def _update_ui(self):
        now = time_module.time()
        
        elapsed_ms = 0
        if self.controller.isRunning and self.controller.rootTime != 0:
            elapsed_ms = int(now * 1000 - self.controller.rootTime + self.controller.startTime * 1000)
            self.lbl_time.setText(f"{elapsed_ms / 1000:.2f} s")
        else:
            self.lbl_time.setText("0.00 s")

        for stick_id, widget in self.stick_widgets.items():
            ip1, ip2 = widget.ip1, widget.ip2
            
            dev1 = self.controller.devices.get(ip1)
            dev2 = self.controller.devices.get(ip2)
            
            on1 = dev1 and dev1.last_response_time and (now - dev1.last_response_time < 2)
            on2 = dev2 and dev2.last_response_time and (now - dev2.last_response_time < 2)
            
            frame1 = None
            frame2 = None
            
            if self.controller.isRunning:
                if on1: frame1 = dev1.light_data.frame_at(elapsed_ms)
                if on2: frame2 = dev2.light_data.frame_at(elapsed_ms)
                
            widget.update_state(on1, on2, frame1, frame2, elapsed_ms)

# ============================================================
# 🚀 開機自動掃描邏輯
# ============================================================
def auto_discover_sticks():
    sticks_map = {}
    
    for filename in os.listdir('.'):
        match = re.match(r'LightStick_192\.168\.1\.(\d+)\.json', filename)
        if match:
            last_octet = int(match.group(1))
            if last_octet >= 101:
                stick_id = (last_octet - 101) // 2 + 1
                base_octet = 101 + (stick_id - 1) * 2
                ip1 = f"192.168.1.{base_octet}"
                ip2 = f"192.168.1.{base_octet + 1}"
                sticks_map[stick_id] = (ip1, ip2)
                
    return sticks_map

if __name__ == "__main__":
    app = QApplication(sys.argv)
    
    sticks_map = auto_discover_sticks()
    all_target_ips = []
    for ip1, ip2 in sticks_map.values():
        all_target_ips.extend([ip1, ip2])

    controller = Controller(extra_ips=all_target_ips)
    window = MonitorWindow(controller, sticks_map)
    window.show()
    sys.exit(app.exec())