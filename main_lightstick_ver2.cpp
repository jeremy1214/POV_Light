#include <Arduino.h>
#include <FastLED.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <WebSocketsServer.h> 

// ==========================================
// 1. 硬體與燈條設定
// ==========================================
#define PLAYER_NUM     2   // ★★★ 燒錄前請確認這是第幾根光棍

#define DIP_M1_PIN     6   
#define DIP_M2_PIN     7   
#define BTN_PIN        8   

#define STRIP1_PIN     2
#define STRIP2_PIN     3
#define STRIP3_PIN     4
#define NUM_LEDS_PER_STRIP 30

CRGB leds1[NUM_LEDS_PER_STRIP];
CRGB leds2[NUM_LEDS_PER_STRIP];
CRGB leds3[NUM_LEDS_PER_STRIP];

void fillAll(CRGB color) {
    fill_solid(leds1, NUM_LEDS_PER_STRIP, color);
    fill_solid(leds2, NUM_LEDS_PER_STRIP, color);
    fill_solid(leds3, NUM_LEDS_PER_STRIP, color);
    FastLED.show();
}

// 動態特效算圖引擎
void renderEffect(uint8_t type, uint8_t h, uint8_t s, uint8_t b, uint8_t spd) {
    uint16_t ms = millis();
    uint8_t beat = beatsin8(spd / 2 + 10, 0, 255); 

    switch(type) {
        case 0: // Solid (單色長亮)
            fill_solid(leds1, NUM_LEDS_PER_STRIP, CHSV(h, s, b));
            break;
        case 1: // Wave (色彩波浪)
            for(int i=0; i<NUM_LEDS_PER_STRIP; i++) {
                leds1[i] = CHSV(h + (i * 10) + (ms * spd / 20), s, b);
            }
            break;
        case 2: // Symmetry (對稱光點)
            fill_solid(leds1, NUM_LEDS_PER_STRIP, CRGB::Black);
            {
                int center = NUM_LEDS_PER_STRIP / 2;
                int pos = (ms * spd / 100) % center;
                leds1[center + pos] = CHSV(h, s, b);
                leds1[center - 1 - pos] = CHSV(h, s, b);
            }
            break;
        case 3: // Breathe (呼吸)
            fill_solid(leds1, NUM_LEDS_PER_STRIP, CHSV(h, s, map(beat, 0, 255, 10, b)));
            break;
        case 4: // Tri-Color (三色跑馬燈)
            for(int i=0; i<NUM_LEDS_PER_STRIP; i++) {
                uint8_t offset = (i + (ms * spd / 50)) % 3;
                if (offset == 0) leds1[i] = CHSV(h, s, b);
                else if (offset == 1) leds1[i] = CHSV(h + 85, s, b);
                else leds1[i] = CHSV(h + 170, s, b);
            }
            break;
        case 5: // Sparkle (星光閃爍)
            fadeToBlackBy(leds1, NUM_LEDS_PER_STRIP, 40); 
            if (random8() < spd) {
                leds1[random16(NUM_LEDS_PER_STRIP)] = CHSV(h, s, b);
            }
            break;
    }

    // 鏡像同步到光棍的其他燈條
    for(int i=0; i<NUM_LEDS_PER_STRIP; i++) {
        leds2[i] = leds1[i];
        leds3[i] = leds1[i];
    }
    FastLED.show();
}

// ==========================================
// 2. 網路設定
// ==========================================
const char* WIFI_SSID   = "Lightdance";
const char* WIFI_PASS   = "wifiyee219"; // ★ 記得改密碼

IPAddress local_IP(192, 168, 1, 100 + PLAYER_NUM);
IPAddress gateway(192, 168, 1, 1);
IPAddress subnet(255, 255, 255, 0);
IPAddress primaryDNS(8, 8, 8, 8);

#define UDP_RX_PORT    12345
#define UDP_TX_PORT    12346
#define TCP_PORT       8080
#define UDP_STOP       1937010544u
#define UDP_HEARTBEAT  1751474546u
#define HEARTBEAT_MS   3000
#define WIFI_TIMEOUT   8000 

WiFiUDP udp;
WiFiServer tcpServer(TCP_PORT);
WebSocketsServer webSocket = WebSocketsServer(81); 
String deviceId = "player" + String(PLAYER_NUM);

// ==========================================
// 3. 系統變數
// ==========================================
uint8_t opMode = 0;
bool showStarted = false;
unsigned long startMs = 0;
unsigned long lastBeatMs = 0;
bool lastBtnState = HIGH;
unsigned long lastDebounceTime = 0;

struct LightFrame {
    unsigned long startTimeMs;
    uint8_t type;
    uint8_t hue;
    uint8_t sat;
    uint8_t bri;
    uint8_t spd; 
};

// 儲存光表的陣列
#define MAX_FRAMES 200
LightFrame frames[MAX_FRAMES];
int numFrames = 0;
int currentFrameIdx = -1; 

// WebSocket 即時預覽專用變數
bool isPreviewing = false;
uint8_t p_t=0, p_h=0, p_s=255, p_b=255, p_spd=128;

// WebSocket 解析用全域變數
JsonDocument wsDoc;
JsonArray wsArr1;
JsonArray wsArr2;
int wsConfigMode = 0;

// ==========================================
// 4. 連線與接收
// ==========================================
void connectWiFi() {
    WiFi.config(local_IP, gateway, subnet, primaryDNS);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    WiFi.setSleep(false); 
    
    unsigned long startAttempt = millis();
    bool isIndicatorOn = false;

    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - startAttempt > WIFI_TIMEOUT) {
            fillAll(CRGB::Red); delay(2000); ESP.restart();
        }
        if ((millis() / 500) % 2 == 0) {
            if (!isIndicatorOn) { fillAll(CRGB::Purple); isIndicatorOn = true; }
        } else {
            if (isIndicatorOn) { fillAll(CRGB::Black); isIndicatorOn = false; }
        }
        delay(100); 
    }
    fillAll(CRGB::Green); delay(2000); fillAll(CRGB::Black);
}

void respondUDP(const char* msg, IPAddress targetIP) {
    String s = deviceId + ": " + msg + ": " + String(WiFi.RSSI());
    udp.beginPacket(targetIP, UDP_TX_PORT); 
    udp.write((const uint8_t*)s.c_str(), s.length());
    udp.endPacket();
}

// Mode 01 專用的 TCP 接收函式
void receiveDataFromPC(CRGB successColor) {
    tcpServer.begin();
    fillAll(CRGB::Cyan); 

    while(true) {
        WiFiClient client = tcpServer.available();
        if (client) {
            File f = LittleFS.open("/light_data.json", "w");
            int bytesRecv = 0;
            unsigned long clientTimeout = millis();
            
            while (client.connected() && millis() - clientTimeout < 2000) {
                while (client.available()) { 
                    f.write(client.read()); 
                    bytesRecv++; 
                    clientTimeout = millis(); 
                }
            }
            f.close();
            client.stop();
            
            if (bytesRecv > 100) {
                while (true) { 
                    fillAll(successColor); delay(300);
                    fillAll(CRGB::Black); delay(300);
                }
            } else if (bytesRecv > 0) {
                for(int i=0; i<5; i++) { fillAll(CRGB::Red); delay(300); fillAll(CRGB::Black); delay(300); }
                fillAll(CRGB::Cyan); 
            } else {
                fillAll(CRGB::Cyan);
            }
        }
        delay(10);
    }
}

// Mode 11 專用的 WebSocket 事件處理器
// ==========================================
// ★ 專屬 11 模式的 WebSocket 事件處理器
// ==========================================
void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
    // ★ 新增：當網頁一連上線時，立刻變全黑
    if (type == WStype_CONNECTED) {
        fillAll(CRGB::Black); 
    }
    // ★ 新增：當網頁斷線時，變回紫燈待命 (方便你辨識)
    else if (type == WStype_DISCONNECTED) {
        fillAll(CRGB::Purple); 
    }
    else if (type == WStype_TEXT) {
        String text = (char*)payload;
        text.trim(); // ★★★ 關鍵修復：濾掉網頁傳來的隱藏換行符號 (\n)，這樣 STOP 才能完美觸發
        
        if (text == "PING") {
            webSocket.sendTXT(num, "PONG");
        } 
        else if (text.startsWith("M1:")) {
            wsDoc.clear();
            wsArr1 = wsDoc.createNestedArray("1");
            wsConfigMode = 1;
        } 
        else if (text.startsWith("M2:")) {
            wsArr2 = wsDoc.createNestedArray("2");
            wsConfigMode = 2;
        } 
        else if (text.startsWith("E:")) {
            int t=0, h=0, s=255, b=255, spd=128, durMs=0;
            sscanf(text.c_str(), "E:%d,%d,%d,%d,%d,%d", &t, &h, &s, &b, &spd, &durMs);
            
            JsonObject obj = (wsConfigMode == 1) ? wsArr1.createNestedObject() : wsArr2.createNestedObject();
            obj["type"] = t;
            obj["hue"] = h;
            obj["sat"] = s;
            obj["bri"] = b;
            obj["spd"] = spd;
            obj["dur"] = (wsConfigMode == 2) ? (durMs / 1000) : 0; 
        } 
        else if (text == "SAVE") {
            isPreviewing = false; 
            File f = LittleFS.open("/light_data.json", "w");
            serializeJson(wsDoc, f);
            f.close();
            webSocket.sendTXT(num, "SAVED");
            
            for(int i=0; i<3; i++) { fillAll(CRGB::Green); delay(200); fillAll(CRGB::Black); delay(200); }
            fillAll(CRGB::Purple); 
        } 
        else if (text == "READ") {
            File f = LittleFS.open("/light_data.json", "r");
            if (f) {
                JsonDocument readDoc;
                deserializeJson(readDoc, f);
                f.close();

                JsonArray m1 = readDoc["1"];
                webSocket.sendTXT(num, "CFG:M1:" + String(m1.size()));
                for(JsonObject item : m1) {
                    String e = "E:" + String((int)item["type"]) + "," + String((int)item["hue"]) + "," + String((int)item["sat"]) + "," + String((int)item["bri"]) + "," + String((int)item["spd"]);
                    webSocket.sendTXT(num, e);
                }

                JsonArray m2 = readDoc["2"];
                webSocket.sendTXT(num, "CFG:M2:" + String(m2.size()));
                for(JsonObject item : m2) {
                    String e = "E:" + String((int)item["type"]) + "," + String((int)item["hue"]) + "," + String((int)item["sat"]) + "," + String((int)item["bri"]) + "," + String((int)item["spd"]) + "," + String((int)item["dur"] * 1000);
                    webSocket.sendTXT(num, e);
                }
                webSocket.sendTXT(num, "CFG:END");
            }
        } 
        else if (text.startsWith("TEST:")) {
            int t=0, h=0, s=255, b=255, spd=128;
            sscanf(text.c_str(), "TEST:%d,%d,%d,%d,%d", &t, &h, &s, &b, &spd);
            p_t = t; p_h = h; p_s = s; p_b = b; p_spd = spd;
            isPreviewing = true; 
        } 
        else if (text == "STOP") {
            isPreviewing = false; 
            fillAll(CRGB::Black); // 成功觸發全黑
        }
    }
}

// ==========================================
// 5. 智慧載入光表 
// ==========================================
void loadLightData(String jsonKey) {
    File f = LittleFS.open("/light_data.json", "r");
    if (!f) return;

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, f);
    f.close();

    if (error) return;

    JsonArray arr = doc[jsonKey];
    if (arr.isNull()) return; 

    unsigned long accTimeMs = 0;
    numFrames = 0;

    for (JsonObject fObj : arr) {
        if (numFrames >= MAX_FRAMES) break;
        
        frames[numFrames].startTimeMs = accTimeMs;
        frames[numFrames].type = fObj["type"]; // ★ 已修復：載入特效類型
        frames[numFrames].hue  = fObj["hue"];
        frames[numFrames].sat  = fObj["sat"];
        frames[numFrames].bri  = fObj["bri"];
        frames[numFrames].spd  = fObj["spd"];  // ★ 已修復：載入特效速度
        
        int dur = fObj["dur"];
        accTimeMs += (dur * 1000); 
        
        numFrames++;
    }
}

// ==========================================
// 6. Setup & Loop
// ==========================================
void setup() {
    delay(1000);
    
    pinMode(DIP_M1_PIN, INPUT_PULLUP);
    pinMode(DIP_M2_PIN, INPUT_PULLUP);
    pinMode(BTN_PIN,    INPUT_PULLUP);

    FastLED.addLeds<WS2812B, STRIP1_PIN, GRB>(leds1, NUM_LEDS_PER_STRIP);
    FastLED.addLeds<WS2812B, STRIP2_PIN, GRB>(leds2, NUM_LEDS_PER_STRIP);
    FastLED.addLeds<WS2812B, STRIP3_PIN, GRB>(leds3, NUM_LEDS_PER_STRIP);
    
    FastLED.setBrightness(32); 
    fillAll(CRGB::Black);

    if (!LittleFS.begin(true)) {
        while(1) { fillAll(CRGB::Red); delay(300); fillAll(CRGB::Black); delay(300); }
    }

    int m1 = (digitalRead(DIP_M1_PIN) == LOW) ? 1 : 0;
    int m2 = (digitalRead(DIP_M2_PIN) == LOW) ? 1 : 0;
    opMode = (m1 << 1) | m2;

    if (opMode != 0) connectWiFi(); 

    switch (opMode) {
        case 0: // 00: 按鈕模式
            loadLightData("1");
            break;
        case 1: // 01: 接收模式 (TCP) -> 傳統 send_data.py
            receiveDataFromPC(CRGB::Green);
            break;
        case 2: // 10: 展演模式 -> UDP 同步音樂時間軸
            loadLightData("2");
            udp.begin(UDP_RX_PORT);
            lastBeatMs = millis();
            break;
        case 3: // 11: 網頁互動模式 (WebSocket) -> 編輯器即時預覽與上傳
            webSocket.begin();
            webSocket.onEvent(webSocketEvent);
            fillAll(CRGB::Purple); // 亮紫燈代表 WebSocket 伺服器待命中
            break;
    }
}

void loop() {
    if (opMode == 0) {
        // Mode 00: 手動按鈕切換光表
        int reading = digitalRead(BTN_PIN);
        if (reading != lastBtnState) lastDebounceTime = millis();
        
        // 1. 處理按鈕點擊，負責切換特效索引
        if ((millis() - lastDebounceTime) > 50 && reading == LOW) {
            if (numFrames > 0) {
                currentFrameIdx++; // 切換到下一個特效
                
                // 如果已經輪完最後一個，就回到 -1 (全暗狀態)
                if (currentFrameIdx >= numFrames) {
                    currentFrameIdx = -1;
                }
            } else {
                fillAll(CRGB::Red); // 如果裡面沒光表，亮紅燈警告
            }
            while(digitalRead(BTN_PIN) == LOW) delay(10); // 防彈跳，等待按鈕放開
        }
        lastBtnState = reading;

        // 2. 持續渲染畫面 (放在判斷式外面，讓特效可以持續流動)
        if (numFrames > 0) {
            if (currentFrameIdx == -1) {
                fillAll(CRGB::Black); // 如果是 -1，代表現在該全暗
            } else {
                // 如果是 0 以上，就播放對應的燈光與動畫特效
                renderEffect(
                    frames[currentFrameIdx].type, 
                    frames[currentFrameIdx].hue, 
                    frames[currentFrameIdx].sat, 
                    frames[currentFrameIdx].bri, 
                    frames[currentFrameIdx].spd
                );
            }
        }
        delay(5);
    }
    else if (opMode == 2) {
        // Mode 10: UDP 展演模式
        int cmd = 0;
        IPAddress controllerIP; 

        if (udp.parsePacket()) {
            controllerIP = udp.remoteIP(); 
            uint8_t b[4]; udp.read(b, 4);
            cmd = (b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3];
            lastBeatMs = millis();
        }

        if (cmd == UDP_STOP) {
            showStarted = false; 
            fillAll(CRGB::Black); 
            respondUDP("stopped", controllerIP); 
        } else if (cmd == UDP_HEARTBEAT) {
            respondUDP("heartbeat received", controllerIP); 
        } else if (cmd > 0) {
            showStarted = true; 
            startMs = millis() - cmd; 
            respondUDP("running", controllerIP); 
        }

        if (!showStarted) {
            if (millis() - lastBeatMs > HEARTBEAT_MS) {
                if ((millis() / 500) % 2 == 0) fillAll(CRGB::Orange); 
                else fillAll(CRGB::Black);
            } else {
                fillAll(CRGB::Black); 
            }
        } else {
            unsigned long currentShowTime = millis() - startMs;
            int activeIdx = 0;
            
            for (int i = 0; i < numFrames; i++) {
                if (currentShowTime >= frames[i].startTimeMs) {
                    activeIdx = i;
                } else {
                    break; 
                }
            }

            if (numFrames > 0) {
                renderEffect(
                    frames[activeIdx].type, 
                    frames[activeIdx].hue, 
                    frames[activeIdx].sat, 
                    frames[activeIdx].bri, 
                    frames[activeIdx].spd
                );
            }
        }
    }
    else if (opMode == 3) {
        // Mode 11: WebSocket 預覽模式
        webSocket.loop();
        
        // ★ 已修復：即時特效算圖引擎
        if (isPreviewing) {
            renderEffect(p_t, p_h, p_s, p_b, p_spd);
        }
        
        delay(5);
    }
    else {
        delay(5); // Mode 01 的迴圈卡在 setup() 的 receiveDataFromPC 裡面，不會執行到這
    }
}