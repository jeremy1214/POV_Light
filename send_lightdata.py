import socket
import json
import time
import os

# ==========================================
# 設定區
# ==========================================
# 填入所有你想傳送的光棍 IP
ESP32_IPS = [
    
    "192.168.1.102", # 未來有第二根可以直接取消註解
]

PORT = 8080

# ==========================================
# 傳輸邏輯
# ==========================================
def send_file_to_esp32(ip):
    # 動態生成專屬檔名
    filename = f"LightStick_{ip}.json"
    
    print(f"\n🚀 準備傳送至 ESP32 ({ip})...")
    
    # 1. 檢查檔案是否存在
    if not os.path.exists(filename):
        print(f"⚠️ 找不到專屬檔案: {filename}，已自動跳過。")
        return

    # 2. 讀取並打包 JSON (這段只需執行一次)
    try:
        with open(filename, 'r', encoding='utf-8') as f:
            data = json.load(f)
            payload = json.dumps(data, separators=(',', ':'))
    except Exception as e:
        print(f"❌ 讀取 {filename} 失敗: {e}")
        return

    payload_size = len(payload.encode('utf-8'))
    print(f"📦 讀取 {filename} 成功！總大小: {payload_size} bytes")

    # 3. 建立 TCP 連線並傳送 (無限重試迴圈)
    attempt = 1
    while True:
        print(f"🔄 [第 {attempt} 次嘗試] 正在連線至 {ip}:{PORT} ...", end="\r")
        
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(3.0) 
        
        try:
            sock.connect((ip, PORT))
            # 連線成功，發送資料
            sock.sendall(payload.encode('utf-8'))
            
            # 成功發送，印出提示並跳出迴圈
            print(f"\n✅ 成功傳送至 {ip}！")
            sock.close()
            break 
            
        except socket.timeout:
            print(f"\n⏳ 連線超時，光棍可能還在開機或未亮青藍色。2 秒後重試...")
        except ConnectionRefusedError:
            print(f"\n🚫 目標拒絕連線 (尚未進入接收模式)。2 秒後重試...")
        except Exception as e:
            print(f"\n⚠️ 發生錯誤: {e}。2 秒後重試...")
        
        # 如果發生例外錯誤走到這裡，關閉 socket 並等待 2 秒再試
        sock.close()
        time.sleep(2)
        attempt += 1

# ==========================================
# 執行區
# ==========================================
if __name__ == "__main__":
    for ip in ESP32_IPS:
        send_file_to_esp32(ip)
        time.sleep(0.5) # 傳完一支稍微等半秒，再傳下一支