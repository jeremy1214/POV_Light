# Lightdance Smart Light Stick System

Lightdance is an end-to-end, high-performance stage lighting control system designed for synchronized live performances. The system consists of ESP32-based hardware light sticks and a powerful Python-based desktop control center, enabling sub-millisecond audio synchronization, real-time virtual previews, and seamless sequence management.

## 🌟 Key Features

### 🖥️ Python Control Center (Desktop UI)
* **Virtual LED Rendering Engine**: Features a custom-built Python rendering engine that mathematically mirrors the ESP32's `FastLED` logic. It provides a real-time, 60-fps virtual preview of dynamic effects (Wave, Breathe, Symmetry, Tri-Color, Sparkle) directly on the PC monitor without needing the physical sticks.
* **Dual-IP Architecture Mapping**: Designed for advanced hardware configurations where one physical stick houses two ESP32 controllers (Top and Bottom). The UI intelligently splits the virtual stick in half to represent both IP addresses seamlessly.
* **Auto-Discovery**: Automatically parses local `LightStick_192.168.1.xxx.json` configuration files to discover devices, map IPs to physical sticks, and load lighting sequences on startup.
* **UDP Audio Synchronization**: Broadcasts precise timeline data and heartbeat packets alongside audio playback (`2026_show.mp3`) to keep all physical devices perfectly in sync.

### 🔌 ESP32 Hardware Firmware
* **Dynamic Animation Engine**: Built on the `FastLED` framework with real-time effect calculation.
* **Intelligent Status Indicators**: Uses low-brightness (value: 48) status colors (Green, Orange, Red) to indicate Wi-Fi and connection states without blinding the performers or audience, switching to full brightness (255) only during the actual performance.
* **Multi-Mode Operation**: Selectable via physical DIP switches (M1, M2).
* **WebSocket Integration**: Supports live-previewing and on-the-fly configuration updates via a web-based editor.

---

## ⚙️ Hardware Operation Modes

The operational mode of the ESP32 is determined by the onboard DIP switches (Pins 6 & 7):

| DIP (M1, M2) | Mode Code | Description |
| :--- | :---: | :--- |
| **0, 0** | **00** | **Standalone (Button)**: Cycles through saved sequences offline via a physical button. |
| **0, 1** | **01** | **Legacy TCP**: Receives batch sequence updates via TCP port 8080. |
| **1, 0** | **10** | **Show Mode (UDP)**: The primary performance mode. Listens for UDP heartbeats and timecodes from the Python Control Center for synchronized execution. |
| **1, 1** | **11** | **Web Editor Mode**: Hosts a WebSocket server (Port 81) for real-time interaction, live previewing, and saving JSON sequences via a browser UI. |

---

## 🛠️ System Requirements & Installation

### ESP32 Firmware
Built with PlatformIO. Ensure the following dependencies are in your `platformio.ini`:
```ini
lib_deps = 
    fastled/FastLED
    bblanchon/ArduinoJson
    links2004/WebSockets