#include <Arduino.h>
#include <Wire.h>
#include <FastLED.h>
#include <ESP32Servo.h>
#include <math.h>

// ============================================================
// 腳位定義 (Seeed Studio XIAO ESP32-C3)
// ============================================================

// 旋轉葉片 LED 資料腳
constexpr uint8_t PIN_B2 = D1;  // 第 2 根葉片 (GPIO3, 相位 +90°)
constexpr uint8_t PIN_B4 = D3;  // 第 4 根葉片 (GPIO5, 相位 +270°)

// 調速可變電阻
constexpr uint8_t POT_PIN = D2; // GPIO4, ADC1_CH4

// AS5600 磁性旋轉編碼器 (I2C)
constexpr uint8_t SDA_PIN = D4; // GPIO6
constexpr uint8_t SCL_PIN = D5; // GPIO7

// Spark 馬達電變 PWM 訊號腳
constexpr uint8_t SPARK_PIN = D6; // GPIO21

// ============================================================
// LED 參數
// ============================================================

constexpr uint16_t NUM_LEDS = 42;
CRGB blade2[NUM_LEDS];
CRGB blade4[NUM_LEDS];

constexpr uint8_t BRIGHTNESS = 90;

// ============================================================
// AS5600 參數
// ============================================================

constexpr uint8_t AS5600_ADDR   = 0x36;
constexpr uint8_t RAW_ANGLE_REG = 0x0C;

// 原本 D0 朝上時校正偏移量 (可透過 Serial 指令 'c' 動態校準)
constexpr float ZERO_OFFSET_DEG = 51.86f;
float runtimeZeroOffset = ZERO_OFFSET_DEG;

// 動態自轉效果參數
bool autoSpinEnabled = false;
float dynamicSpinAngle = 0.0f;
constexpr float SPIN_SPEED_DPS = 20.0f; // 動態自轉速度 (度/秒)

// 動態亮度
uint8_t currentBrightness = BRIGHTNESS;

// ============================================================
// 超前角延遲補償 (Lead-Angle Compensation)
//
// 物理原理：從 AS5600 角度取樣到 WS2812B (42 顆) 訊號發送完成並鎖存發光，
// 硬體耗時約 1.35 ~ 1.45 ms。
// 在旋轉過程中，葉片在發光前已向前轉動了 leadAngle 度。
// 補償公式：leadAngle = (RPM * 360 / 60) * (latencyUs / 1,000,000)
// ============================================================
bool leadCompEnabled = true;
float hardwareLatencyUs = 1400.0f; // 預設硬體總延遲 1400 微秒 (1.4ms)
float currentLeadAngle = 0.0f;     // 當前計算之超前角補償量 (度)

// ============================================================
// 馬達安全控制 (Safety Motor Controller)
// ============================================================

Servo spark;

constexpr int MOTOR_STOP_US = 1500; // 靜止中立脈寬 (µs)
constexpr int MOTOR_MAX_US  = 2000; // 前進全速脈寬 (µs)

// 若硬體可變電阻接線反向（轉到底反而電壓最高），請改設為 true
constexpr bool POT_INVERTED = false;

// 低端安全死區：ADC 讀數 <= 150 (旋鈕歸零或斷線掉至 0V) 視為靜止
constexpr int POT_DEAD_ZONE = 150;

// 開機安全上鎖 (Arming)：開機時必須先將旋鈕歸零才允許解鎖馬達輸出，防止暴衝
bool motorArmed = false;

int currentPWM = MOTOR_STOP_US;

// ============================================================
// 機構幾何參數 (Geometry)
// ============================================================

constexpr float OUTER_RADIUS_MM     = 300.0f;
constexpr float FIRST_LED_RADIUS_MM = 12.5f;

// ============================================================
// 十字圖案查表 (Cross Bitmask LUT: 360 角度 × uint64_t)
//
// 支援兩種經典十字樣式：
// 1. CROSS_GREEK : 正十字 (四臂等長對稱，交叉於圓心)
// 2. CROSS_LATIN : 拉丁十字架 (橫樑偏上方，經典十字架)
// ============================================================

enum CrossType {
    CROSS_GREEK = 0, // 正十字
    CROSS_LATIN = 1  // 拉丁十字架
};

CrossType currentCrossType = CROSS_GREEK; // 預設正十字

uint64_t crossBitTable[360];

// ============================================================
// 系統運作診斷數據
// ============================================================

float currentRPM = 0.0f;
uint32_t frameCount = 0;
uint32_t fps = 0;

// ============================================================
// 輔助運算函數
// ============================================================

inline float degToRad(float deg)
{
    return deg * (PI / 180.0f);
}

inline float wrapDeg(float angle)
{
    while (angle < 0.0f)   angle += 360.0f;
    while (angle >= 360.0f) angle -= 360.0f;
    return angle;
}

float logicalLEDToNormalizedRadius(uint16_t index)
{
    float ratio = (float)index / (float)(NUM_LEDS - 1);
    float radiusMM = FIRST_LED_RADIUS_MM + ratio * (OUTER_RADIUS_MM - FIRST_LED_RADIUS_MM);
    return radiusMM / OUTER_RADIUS_MM;
}

// 點到線段最短距離 (向量投影法)
float distanceToSegment(float px, float py, float x1, float y1, float x2, float y2)
{
    float vx = x2 - x1;
    float vy = y2 - y1;
    float wx = px - x1;
    float wy = py - y1;

    float len2 = vx * vx + vy * vy;
    if (len2 < 1e-6f) {
        float dx = px - x1;
        float dy = py - y1;
        return sqrtf(dx * dx + dy * dy);
    }

    float t = (wx * vx + wy * vy) / len2;
    t = constrain(t, 0.0f, 1.0f);

    float cx = x1 + t * vx;
    float cy = y1 + t * vy;
    float dx = px - cx;
    float dy = py - cy;

    return sqrtf(dx * dx + dy * dy);
}

// ============================================================
// 預先建構十字查表 (Cross Bitmask LUT)
// ============================================================

void buildCrossTable(CrossType type = CROSS_GREEK)
{
    constexpr float LINE_WIDTH = 0.065f; // 十字線寬 (約 19.5mm)

    // 垂直線段 (Vertical Beam)
    float vX1 = 0.0f, vY1 = -0.85f;
    float vX2 = 0.0f, vY2 =  0.85f;

    // 水平線段 (Horizontal Beam)
    float hX1 = -0.85f, hY1 = 0.0f;
    float hX2 =  0.85f, hY2 = 0.0f;

    if (type == CROSS_LATIN) {
        // 拉丁十字架樣式：橫樑偏上方 (y = 0.25)，橫臂稍短 (長度 0.55)
        hY1 = 0.25f;
        hY2 = 0.25f;
        hX1 = -0.55f;
        hX2 =  0.55f;
    }

    // 遍歷 360 個角度，每個角度計算 42 顆 LED 是否點亮並存入 bitmask
    for (int deg = 0; deg < 360; deg++) {
        float angle = degToRad((float)deg);
        uint64_t mask = 0;

        for (int led = 0; led < NUM_LEDS; led++) {
            float radius = logicalLEDToNormalizedRadius(led);
            float x = radius * sinf(angle);
            float y = radius * cosf(angle);

            // 判斷點 (x, y) 到垂直與水平兩條線段的最短距離
            float dVertical   = distanceToSegment(x, y, vX1, vY1, vX2, vY2);
            float dHorizontal = distanceToSegment(x, y, hX1, hY1, hX2, hY2);

            if (dVertical < LINE_WIDTH || dHorizontal < LINE_WIDTH) {
                mask |= (1ULL << led);
            }
        }
        crossBitTable[deg] = mask;
    }
}

// ============================================================
// AS5600 角度讀取
// ============================================================

bool readRawAngle(uint16_t &raw)
{
    Wire.beginTransmission(AS5600_ADDR);
    Wire.write(RAW_ANGLE_REG);
    if (Wire.endTransmission(false) != 0) {
        return false;
    }

    if (Wire.requestFrom(AS5600_ADDR, (uint8_t)2) != 2) {
        return false;
    }

    uint8_t highByte = Wire.read();
    uint8_t lowByte  = Wire.read();

    raw = ((uint16_t)(highByte & 0x0F) << 8) | lowByte;
    return true;
}

bool getRotorAngle(float &angle)
{
    uint16_t raw;
    if (!readRawAngle(raw)) {
        return false;
    }

    angle = (float)raw * (360.0f / 4096.0f);
    angle -= runtimeZeroOffset;
    angle = wrapDeg(angle);
    return true;
}

// ============================================================
// 安全馬達控制 (50Hz 降頻 + Fail-Safe 啟動鎖定)
//
// 優化重點：
// 1. 限制在 50Hz (20ms)，不占用高速 POV 迴圈時間
// 2. 避免斷線全速暴衝：旋鈕歸零 (<=150) 為停轉，順時針調大為加速
// 3. 開機 Arming 保護：開機時若旋鈕未歸零則不輸出動力
// ============================================================

void updateMotor()
{
    static uint32_t lastMotorUpdate = 0;
    uint32_t now = millis();

    // 嚴格限制在 50Hz (每 20ms 一次)，不干擾高速 POV 主迴圈
    if (now - lastMotorUpdate < 20) {
        return;
    }
    lastMotorUpdate = now;

    int pot = analogRead(POT_PIN);
    pot = constrain(pot, 0, 4095);

    if (POT_INVERTED) {
        pot = 4095 - pot;
    }

    // 開機安全上鎖 (Arming)：開機時旋鈕必須先歸零才允許解鎖
    if (!motorArmed) {
        if (pot <= POT_DEAD_ZONE) {
            motorArmed = true;
            Serial.println("[MOTOR] Arming Success: Motor Unlocked.");
        } else {
            spark.writeMicroseconds(MOTOR_STOP_US);
            return;
        }
    }

    if (pot <= POT_DEAD_ZONE) {
        currentPWM = MOTOR_STOP_US;
    } else {
        // 從死區逐漸加速至最大馬力，避免暴衝
        currentPWM = map(pot, POT_DEAD_ZONE, 4095, MOTOR_STOP_US, MOTOR_MAX_US);
        currentPWM = constrain(currentPWM, MOTOR_STOP_US, MOTOR_MAX_US);
    }

    spark.writeMicroseconds(currentPWM);
}

// ============================================================
// 單葉片快速點陣填充 (Bitmask 查表)
// ============================================================

inline void renderBlade(CRGB *leds, int angleDeg)
{
    angleDeg = (angleDeg % 360 + 360) % 360;

    uint64_t mask = crossBitTable[angleDeg];
    const CRGB goldColor(255, 220, 40);

    for (int i = 0; i < NUM_LEDS; i++) {
        leds[i] = ((mask >> i) & 1ULL) ? goldColor : CRGB::Black;
    }
}

// ============================================================
// POV 主彩現管線
// ============================================================

void renderPOV()
{
    float rotorAngle;
    if (!getRotorAngle(rotorAngle)) {
        return;
    }

    // 計算轉速 (RPM 估算)
    static float lastAngle = 0.0f;
    static uint32_t lastRevTime = 0;

    // 轉子過零點判定 (順時針跨越 360° -> 0°)
    if (lastAngle > 270.0f && rotorAngle < 90.0f) {
        uint32_t now = micros();
        uint32_t revDelta = now - lastRevTime;
        lastRevTime = now;
        if (revDelta > 0) {
            float instRPM = 60000000.0f / (float)revDelta;
            // 輕微指數平滑濾波 (Low-pass Filter)
            currentRPM = currentRPM * 0.8f + instRPM * 0.2f;
        }
    } else {
        // 馬達減速或停止時，避免 RPM 數值卡死
        if (micros() - lastRevTime > 600000) {
            currentRPM *= 0.98f;
            if (currentRPM < 15.0f) currentRPM = 0.0f;
        }
    }
    lastAngle = rotorAngle;

    // 動態超前角補償計算 (Lead-Angle Calculation)
    // 角速度 (度/微秒) = (RPM * 360) / (60 * 1,000,000) = RPM * 0.000006
    if (leadCompEnabled && currentRPM > 20.0f) {
        currentLeadAngle = currentRPM * 0.000006f * hardwareLatencyUs;
    } else {
        currentLeadAngle = 0.0f;
    }

    // 動態圖案自轉 (Dynamic Spin)
    static uint32_t lastSpinUpdate = 0;
    if (autoSpinEnabled) {
        uint32_t now = micros();
        if (lastSpinUpdate > 0) {
            float dt = (float)(now - lastSpinUpdate) / 1000000.0f;
            dynamicSpinAngle = wrapDeg(dynamicSpinAngle + SPIN_SPEED_DPS * dt);
        }
        lastSpinUpdate = now;
    } else {
        lastSpinUpdate = micros();
    }

    // 葉片 2 (Blade 2, 相位 +90° + 超前角補償)
    int angle2 = (int)(rotorAngle + currentLeadAngle + dynamicSpinAngle + 90.0f + 0.5f) % 360;

    // 葉片 4 (Blade 4, 相位 +270° + 超前角補償)
    int angle4 = (int)(rotorAngle + currentLeadAngle + dynamicSpinAngle + 270.0f + 0.5f) % 360;

    // 填充兩葉片顏色 (Bitmask 高速取值)
    renderBlade(blade2, angle2);
    renderBlade(blade4, angle4);

    // 雙通道平行硬體發送
    FastLED.show();

    frameCount++;
}

// ============================================================
// 序列埠互動指令處理 (Serial Command Interface)
// ============================================================

void processSerialCommands()
{
    while (Serial.available() > 0) {
        char cmd = (char)Serial.read();
        if (cmd == '\r' || cmd == '\n') continue;

        switch (cmd) {
            case 'c':
            case 'C': {
                // 一鍵校準零點：將目前 AS5600 角度記錄為基準零點
                uint16_t raw;
                if (readRawAngle(raw)) {
                    float currentDeg = (float)raw * (360.0f / 4096.0f);
                    runtimeZeroOffset = currentDeg;
                    Serial.println("\n--------------------------------------------------");
                    Serial.printf("[CALIB] Success! Current Raw Angle = %.2f deg.\n", currentDeg);
                    Serial.printf("[CALIB] Runtime zero offset updated to: %.2f deg.\n", runtimeZeroOffset);
                    Serial.printf("[CALIB] To make permanent, update main.cpp:\n");
                    Serial.printf("        constexpr float ZERO_OFFSET_DEG = %.2ff;\n", runtimeZeroOffset);
                    Serial.println("--------------------------------------------------\n");
                } else {
                    Serial.println("[CALIB] Error: Failed to read AS5600 via I2C!");
                }
                break;
            }
            case 'p':
            case 'P': {
                currentCrossType = (currentCrossType == CROSS_GREEK) ? CROSS_LATIN : CROSS_GREEK;
                buildCrossTable(currentCrossType);
                Serial.printf("[PATTERN] Cross pattern switched to: %s\n",
                              (currentCrossType == CROSS_GREEK) ? "GREEK CROSS (正十字)" : "LATIN CROSS (拉丁十字架)");
                break;
            }
            case 's':
            case 'S': {
                autoSpinEnabled = !autoSpinEnabled;
                Serial.printf("[EFFECT] Auto-spin %s (Speed: %.1f deg/s)\n",
                              autoSpinEnabled ? "ENABLED" : "DISABLED", SPIN_SPEED_DPS);
                break;
            }
            case 'l':
            case 'L': {
                leadCompEnabled = !leadCompEnabled;
                Serial.printf("[LEAD-COMP] Lead-Angle Compensation: %s (Latency: %.0f us, Current Lead: %.2f deg)\n",
                              leadCompEnabled ? "ENABLED" : "DISABLED", hardwareLatencyUs, currentLeadAngle);
                break;
            }
            case '[': {
                if (hardwareLatencyUs >= 100.0f) hardwareLatencyUs -= 100.0f;
                Serial.printf("[LEAD-COMP] Latency set to: %.0f us (Current Lead at %.1f RPM: %.2f deg)\n",
                              hardwareLatencyUs, currentRPM, currentRPM * 0.000006f * hardwareLatencyUs);
                break;
            }
            case ']': {
                hardwareLatencyUs += 100.0f;
                Serial.printf("[LEAD-COMP] Latency set to: %.0f us (Current Lead at %.1f RPM: %.2f deg)\n",
                              hardwareLatencyUs, currentRPM, currentRPM * 0.000006f * hardwareLatencyUs);
                break;
            }
            case '+': {
                if (currentBrightness <= 240) currentBrightness += 15;
                else currentBrightness = 255;
                FastLED.setBrightness(currentBrightness);
                Serial.printf("[BRIGHTNESS] Increased to: %u / 255\n", currentBrightness);
                break;
            }
            case '-': {
                if (currentBrightness >= 25) currentBrightness -= 15;
                else currentBrightness = 10;
                FastLED.setBrightness(currentBrightness);
                Serial.printf("[BRIGHTNESS] Decreased to: %u / 255\n", currentBrightness);
                break;
            }
            case '?':
            case 'h':
            case 'H': {
                Serial.println("\n--- [POV LIGHT CONTROL COMMANDS] ---");
                Serial.println(" 'c'     : Calibrate zero offset (align current angle as 0°)");
                Serial.println(" 'p'     : Toggle cross type (Greek Cross <-> Latin Cross)");
                Serial.println(" 'l'     : Toggle lead-angle compensation (ON/OFF)");
                Serial.println(" '[' / ']': Fine-tune hardware latency (-/+ 100 us)");
                Serial.println(" 's'     : Toggle dynamic pattern auto-spin");
                Serial.println(" '+'     : Increase LED brightness");
                Serial.println(" '-'     : Decrease LED brightness");
                Serial.println(" '?'     : Show this help menu");
                Serial.println("------------------------------------\n");
                break;
            }
            default:
                break;
        }
    }
}

// ============================================================
// 系統運作狀態回報 (每秒 1 次)
// ============================================================

void updateDiagnostics()
{
    static uint32_t lastDiag = 0;
    uint32_t now = millis();

    if (now - lastDiag >= 1000) {
        fps = frameCount;
        frameCount = 0;
        lastDiag = now;

        Serial.printf("[DIAG] RPM: %5.1f | FPS: %4u | Lead: %4.1f deg | PWM: %4d us | Armed: %s\n",
                      currentRPM, fps, currentLeadAngle, currentPWM, motorArmed ? "YES" : "WAIT_ZERO");
    }
}

// ============================================================
// Setup 初始化
// ============================================================

void setup()
{
    Serial.begin(115200);
    delay(500);

    Serial.println("\n==========================================");
    Serial.println("    ESP32-C3 POV CROSS DISPLAY (MAIN)");
    Serial.println("==========================================");

    // ADC 設置
    analogReadResolution(12);
    pinMode(POT_PIN, INPUT);

    // AS5600 I2C 初始化 (Fast-Mode 400kHz)
    Wire.begin(SDA_PIN, SCL_PIN);
    Wire.setClock(400000);

    // FastLED RMT 初始化
    FastLED.addLeds<WS2812B, PIN_B2, GRB>(blade2, NUM_LEDS);
    FastLED.addLeds<WS2812B, PIN_B4, GRB>(blade4, NUM_LEDS);
    FastLED.setBrightness(BRIGHTNESS);
    FastLED.setMaxPowerInVoltsAndMilliamps(5, 1500); // 供電安全保護：上限 5V 1500mA 防止壓降重開機

    fill_solid(blade2, NUM_LEDS, CRGB::Black);
    fill_solid(blade4, NUM_LEDS, CRGB::Black);
    FastLED.show();

    // 烘焙十字查表 (Bitmask 結構)
    Serial.print("Building Cross Bitmask LUT (360x42)... ");
    buildCrossTable(currentCrossType);
    Serial.println("Done! (2,880 Bytes)");

    // 馬達與電變初始化
    spark.setPeriodHertz(50);
    spark.attach(SPARK_PIN, 1000, 2000);
    spark.writeMicroseconds(MOTOR_STOP_US);

    Serial.println("System Ready. Please zero throttle knob to ARM motor.\n");
}

// ============================================================
// 主迴圈 (Loop)
// ============================================================

void loop()
{
    // 1. 馬達控制 (內部已自動限制在 50Hz，不佔用 CPU)
    updateMotor();

    // 2. 即時 POV 視覺暫留渲染 (全力高頻運行)
    renderPOV();

    // 3. 處理序列埠互動指令 ('c' 校準, 's' 自轉, '+/-' 亮度)
    processSerialCommands();

    // 4. 系統即時監控數據回報 (每秒 1 次)
    updateDiagnostics();
}
