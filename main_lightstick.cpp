#include <Arduino.h>
#include <FastLED.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

// ==========================================
// 1. 硬體與燈條設定
// ==========================================
#define PLAYER_NUM     2   // ★★★ 燒錄前請確認 Player 幾

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
};

#define MAX_FRAMES 200
LightFrame frames[MAX_FRAMES];
int numFrames = 0;
int currentFrameIdx = 0; 

// ==========================================
// 4. 連線與接收 (統一存成 /light_data.json)
// ==========================================
void connectWiFi() {
    WiFi.config(local_IP, gateway, subnet, primaryDNS);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    WiFi.setSleep(false); // ★ 保持關閉省電模式，確保 UDP 穩定
    
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

// 修改這個函式，讓它能接受目標 IP 作為參數
void respondUDP(const char* msg, IPAddress targetIP) {
    String s = deviceId + ": " + msg + ": " + String(WiFi.RSSI());
    udp.beginPacket(targetIP, UDP_TX_PORT); 
    udp.write((const uint8_t*)s.c_str(), s.length());
    udp.endPacket();
}

void receiveDataFromPC(CRGB successColor) {
    tcpServer.begin();
    fillAll(CRGB::Cyan); 

    while(true) {
        WiFiClient client = tcpServer.available();
        if (client) {
            // ★ 無論 01 或 11，統一覆寫同一個檔案
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
            
            // 降低門檻至 100 bytes
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

// ==========================================
// 5. 智慧載入光表 (依賴 jsonKey 選擇陣列)
// ==========================================
void loadLightData(String jsonKey) {
    File f = LittleFS.open("/light_data.json", "r");
    if (!f) return;

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, f);
    f.close();

    if (error) return;

    // ★ 根據傳入的 jsonKey ("1" 或 "2") 抓取對應的陣列
    JsonArray arr = doc[jsonKey];
    if (arr.isNull()) return; // 找不到對應的標籤就退出

    unsigned long accTimeMs = 0;
    numFrames = 0;

    for (JsonObject fObj : arr) {
        if (numFrames >= MAX_FRAMES) break;
        
        frames[numFrames].startTimeMs = accTimeMs;
        frames[numFrames].type = fObj["type"];
        frames[numFrames].hue  = fObj["hue"];
        frames[numFrames].sat  = fObj["sat"];
        frames[numFrames].bri  = fObj["bri"];
        
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
        case 0: // 00: 按鈕模式 -> 讀取標籤 "1"
            loadLightData("1");
            break;
        case 1: // 01: 接收模式 -> 統一存檔，閃綠燈
            receiveDataFromPC(CRGB::Green);
            break;
        case 2: // 10: 展演模式 -> 讀取標籤 "2"
            loadLightData("2");
            udp.begin(UDP_RX_PORT);
            lastBeatMs = millis();
            break;
        case 3: // 11: 接收模式 -> 統一存檔，閃黃燈
            receiveDataFromPC(CRGB::Yellow);
            break;
    }
}

void loop() {
    if (opMode == 0) {
        int reading = digitalRead(BTN_PIN);
        if (reading != lastBtnState) lastDebounceTime = millis();
        if ((millis() - lastDebounceTime) > 50 && reading == LOW) {
            if (numFrames > 0) {
                currentFrameIdx = (currentFrameIdx + 1) % numFrames;
                fillAll(CHSV(frames[currentFrameIdx].hue, frames[currentFrameIdx].sat, frames[currentFrameIdx].bri));
            } else {
                fillAll(CRGB::Red); 
            }
            while(digitalRead(BTN_PIN) == LOW) delay(10); 
        }
        lastBtnState = reading;
    }
else if (opMode == 2) {
        int cmd = 0;
        IPAddress controllerIP; // ★ 新增：用來記住主控台(電腦)的 IP

        if (udp.parsePacket()) {
            controllerIP = udp.remoteIP(); // ★ 記下是誰發心跳過來的
            uint8_t b[4]; udp.read(b, 4);
            cmd = (b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3];
            lastBeatMs = millis();
        }

        if (cmd == UDP_STOP) {
            showStarted = false; fillAll(CRGB::Black); 
            respondUDP("stopped", controllerIP); // ★ 回覆給正確的 IP
        } else if (cmd == UDP_HEARTBEAT) {
            respondUDP("heartbeat received", controllerIP); // ★ 回覆給正確的 IP
        } else if (cmd > 0) {
            showStarted = true; startMs = millis() - cmd; 
            respondUDP("running", controllerIP); // ★ 回覆給正確的 IP
        }

        if (!showStarted) {
            if (millis() - lastBeatMs > HEARTBEAT_MS) {
                if ((millis() / 500) % 2 == 0) fillAll(CRGB::Orange); else fillAll(CRGB::Black);
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
                fillAll(CHSV(frames[activeIdx].hue, frames[activeIdx].sat, frames[activeIdx].bri));
            }
        }
    }
    delay(5);
}