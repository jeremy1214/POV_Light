import sys
import os
import re
import time as time_module
import argparse

from PySide6.QtCore import Qt, QTimer
from PySide6.QtGui import QColor, QFont, QPainter, QBrush, QPen
from PySide6.QtWidgets import (QApplication, QMainWindow, QWidget, QVBoxLayout,
                               QHBoxLayout, QPushButton, QLabel,
                               QGridLayout, QScrollArea)

from stick_control import Controller

# 色彩主題設定
BG = "#0d1117"
SURFACE = "#161b22"
TEXT = "#c9d1d9"
DIM = "#8b949e"
ACCENT = "#238636"
DANGER = "#da3633"

# ============================================================
# 雙 IP 虛擬光棍 UI 元件
# ============================================================
class StickWidget(QWidget):
    def __init__(self, stick_id, ip1, ip2):
        super().__init__()
        self.stick_id = stick_id
        self.ip1 = ip1
        self.ip2 = ip2
        
        # 雙主機連線狀態
        self.online1 = False
        self.online2 = False
        
        self.color = (30, 30, 30) # 預設關機顏色
        self.setFixedSize(120, 340)

    def update_state(self, online1, online2, color):
        if (self.online1 != online1 or 
            self.online2 != online2 or 
            self.color != color):
            self.online1 = online1
            self.online2 = online2
            # 只要其中一個 IP 上線，光棍就允許發光
            self.color = color if (online1 or online2) else (30, 30, 30)
            self.update()

    def paintEvent(self, event):
        p = QPainter(self)
        p.setRenderHint(QPainter.Antialiasing)
        
        # 背景底板
        p.fillRect(self.rect(), QColor(SURFACE))
        
        # 標題 (Stick 1, Stick 2...)
        p.setPen(QColor(TEXT if (self.online1 or self.online2) else DIM))
        f = QFont("Arial", 11, QFont.Bold)
        p.setFont(f)
        p.drawText(0, 10, self.width(), 20, Qt.AlignCenter, f"Stick {self.stick_id}")
        
        # 雙 IP 狀態指示燈
        f.setPointSize(8)
        p.setFont(f)
        
        # 繪製 IP1 狀態
        c1 = ACCENT if self.online1 else DANGER
        p.setPen(QColor(c1))
        p.drawText(0, 30, self.width()//2, 20, Qt.AlignCenter, f".{self.ip1.split('.')[-1]}")
        
        # 繪製 IP2 狀態
        c2 = ACCENT if self.online2 else DANGER
        p.setPen(QColor(c2))
        p.drawText(self.width()//2, 30, self.width()//2, 20, Qt.AlignCenter, f".{self.ip2.split('.')[-1]}")

        # 繪製發光的光棍本體 (圓角矩形)
        stick_width = 30
        stick_height = 250
        x = (self.width() - stick_width) // 2
        y = 65
        
        if self.online1 or self.online2:
            r, g, b = self.color
            p.setBrush(QBrush(QColor(r, g, b)))
            p.setPen(QPen(QColor(r, g, b).lighter(150), 2))
        else:
            p.setBrush(QBrush(QColor(40, 40, 45)))
            p.setPen(QPen(QColor(60, 60, 65), 2))
            
        p.drawRoundedRect(x, y, stick_width, stick_height, 15, 15)
        p.end()

# ============================================================
# 主控台視窗
# ============================================================
class MonitorWindow(QMainWindow):
    def __init__(self, controller, sticks_map):
        super().__init__()
        self.controller = controller
        self.sticks_map = sticks_map  # { stick_id: (ip1, ip2) }
        self.stick_widgets = {}       # stick_id -> StickWidget

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

        # 初始化生成畫面上所有的實體光棍元件
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

        # --- 啟動背景服務 ---
        self.controller.setup_threads(self)
        self._setup_timers()

    def _init_stick_widgets(self):
        """根據掃描結果，在畫面上依照順序排列光棍"""
        col_count = 6 # 每排顯示 6 根
        
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

        # 針對每一根實體光棍，檢查它的兩個 IP 狀態
        for stick_id, widget in self.stick_widgets.items():
            ip1, ip2 = widget.ip1, widget.ip2
            
            dev1 = self.controller.devices.get(ip1)
            dev2 = self.controller.devices.get(ip2)
            
            on1 = dev1 and dev1.last_response_time and (now - dev1.last_response_time < 2)
            on2 = dev2 and dev2.last_response_time and (now - dev2.last_response_time < 2)
            
            color = (0, 0, 0)
            # 只要有任何一端連線，就從該端提取顏色資料
            if on1:
                color = dev1.light_data.color_at(elapsed_ms)
            elif on2:
                color = dev2.light_data.color_at(elapsed_ms)
                
            widget.update_state(on1, on2, color)

# ============================================================
# 🚀 開機自動掃描邏輯
# ============================================================
def auto_discover_sticks():
    """
    掃描目前資料夾，尋找 LightStick_192.168.1.xxx.json。
    自動推算 Stick ID：
    .101 & .102 -> Stick 1
    .103 & .104 -> Stick 2
    依此類推。
    回傳：{ stick_id: (ip1, ip2) }
    """
    sticks_map = {}
    
    for filename in os.listdir('.'):
        match = re.match(r'LightStick_192\.168\.1\.(\d+)\.json', filename)
        if match:
            last_octet = int(match.group(1))
            if last_octet >= 101:
                # 數學推算：將 101/102 映射到 1，103/104 映射到 2
                stick_id = (last_octet - 101) // 2 + 1
                
                # 計算出這根光棍該有的兩個 IP
                base_octet = 101 + (stick_id - 1) * 2
                ip1 = f"192.168.1.{base_octet}"
                ip2 = f"192.168.1.{base_octet + 1}"
                
                sticks_map[stick_id] = (ip1, ip2)
                
    return sticks_map

if __name__ == "__main__":
    app = QApplication(sys.argv)
    
    # 1. 執行自動掃描
    sticks_map = auto_discover_sticks()
    
    # 2. 展開所有的 IP，餵給 Controller
    all_target_ips = []
    for ip1, ip2 in sticks_map.values():
        all_target_ips.extend([ip1, ip2])

    # 3. 啟動系統
    controller = Controller(extra_ips=all_target_ips)
    window = MonitorWindow(controller, sticks_map)
    window.show()
    sys.exit(app.exec())