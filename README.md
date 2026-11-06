# test.cpp 程式碼深度分析報告 (POV 六芒星燈光顯示系統)

---

## 1. 專案與檔案背景概述 (System Overview)

`test.cpp` 是一套基於 **ESP32-C3** 的 **旋轉視覺暫留 (POV, Persistence of Vision)** 顯示與馬達控制系統。其核心目標是透過馬達帶動兩根安裝有 WS2812B 可定址 LED 的旋轉葉片，利用磁性旋轉編碼器 (AS5600) 即時回傳旋轉角度，在旋轉過程中精確點亮對應位置的 LED，從而在半空中透過人類視覺暫留效應「投影」出一個懸浮的幾何圖形——**金色六芒星 (Hexagram / 大衛之星)**。

```
                    ┌─────────────────────────┐
                    │      ESP32-C3 MCU       │
                    └───────────┬─────────────┘
                                │
         ┌──────────────────────┼──────────────────────┐
         ▼                      ▼                      ▼
┌──────────────────┐  ┌──────────────────┐  ┌──────────────────┐
│  I2C (400 kHz)   │  │   2x RMT (GPIO)  │  │   PWM & ADC      │
│  AS5600 編碼器   │  │   WS2812B 燈條   │  │   Spark 馬達電變  │
│  (即時角度回授)  │  │  (雙葉片各42顆)  │  │  (可變電阻調速)  │
└──────────────────┘  └──────────────────┘  └──────────────────┘
```

---

## 2. 硬體架構與腳位映射 (Hardware & Pinout)

依據 `platformio.ini` 與程式碼定義，目標開發板為 **Seeed Studio XIAO ESP32-C3**。

| 腳位定義 | 實體 GPIO | 連接設備 / 模組 | 訊號類型 / 協定 | 功能說明 |
| :--- | :--- | :--- | :--- | :--- |
| `PIN_B2` (`D1`) | GPIO3 | 葉片 2 LED 燈條 | WS2812B (RMT) | 第 2 根葉片，機械相位偏移 $+90^\circ$ |
| `PIN_B4` (`D3`) | GPIO5 | 葉片 4 LED 燈條 | WS2812B (RMT) | 第 4 根葉片，機械相位偏移 $+270^\circ$ (與 B2 相對 $180^\circ$) |
| `POT_PIN` (`D2`)| GPIO4 | 可變電阻 (Potentiometer) | ADC1_CH4 (12-bit) | 馬達轉速手動調速輸入 |
| `SDA_PIN` (`D4`)| GPIO6 | AS5600 磁編碼器 | I2C (SDA) | 傳輸轉子當前 12-bit 絕對角度 |
| `SCL_PIN` (`D5`)| GPIO7 | AS5600 磁編碼器 | I2C (SCL, 400kHz) | I2C 通訊時脈 |
| `SPARK_PIN` (`D6`)| GPIO21 | REV Spark 電變 / 伺服馬達 | PWM (50Hz Servo) | 輸出 1000~2000 $\mu s$ 脈寬控制轉速 |

---

## 3. 核心模組與演算法剖析 (Detailed Code Analysis)

### 3.1 幾何模型與半徑正規化 (`Geometry`)
* **機構規格**：
  * 外徑半徑：`OUTER_RADIUS_MM = 300.0 mm`
  * 首顆 LED 半徑：`FIRST_LED_RADIUS_MM = 12.5 mm`
  * 單葉片 LED 數量：`NUM_LEDS = 42`
* **正規化半徑映射**：
  函數 `logicalLEDToNormalizedRadius(index)` 將第 $0 \sim 41$ 顆 LED 線性對應至實體半徑 ($12.5 \sim 300.0\text{ mm}$)，再除以外徑 $300.0\text{ mm}$，得出範圍在 $[0.0417, 1.0]$ 之間的正規化半徑值 $r$。

### 3.2 點到線段距離演算法 (`distanceToSegment`)
為了在旋轉座標中判斷某個點 $(x, y)$ 是否位於六芒星的線段上，程式實作了標準的向量投影點線距演算法：
$$\vec{v} = P_2 - P_1,\quad \vec{w} = P - P_1$$
投影比例係數：
$$t = \text{clamp}\left(\frac{\vec{w} \cdot \vec{v}}{\|\vec{v}\|^2}, 0, 1\right)$$
投影點座標 $C = P_1 + t\vec{v}$，最後回傳 $\|P - C\|$。此方法正確處理了線段兩側端點與垂直投影區域。

### 3.3 靜態查表建構 (`buildHexagramTable`) —— 空間換取時間
* **圖形定義**：六芒星由兩個正三角形交疊而成，頂點分佈在半徑 $R = 0.86$ 的圓周上：
  * **正立三角形**：頂點角度為 $0^\circ, 120^\circ, 240^\circ$
  * **倒立三角形**：頂點角度為 $180^\circ, 300^\circ, 60^\circ$
  * **線條寬度**：`LINE_WIDTH = 0.055`
* **查表矩陣 (`hexagramTable[360][42]`)**：
  * 雙層迴圈在 `setup()` 時執行：遍歷 360 個整數角度及 42 顆 LED。
  * 利用極座標轉直角座標：$x = r \sin(\theta), y = r \cos(\theta)$ (以 $12$ 點鐘方向為 $0^\circ$、順時針旋轉)。
  * 逐一計算與六條邊的距離，若小於 `LINE_WIDTH` 則標記為 `true`。
* **優點**：此舉在開機時完成所有耗時的浮點三角函數運算 (`sinf`, `cosf`, `sqrtf`)，讓旋轉動態彩現時只需做陣列取值，大幅降低 CPU 即時運算負擔。

### 3.4 角度讀取與零點校正 (`AS5600`)
* 透過 I2C 讀取 AS5600 暫存器 `0x0C` (RAW ANGLE 高低 2 位元組，12-bit 解析度，共 4096 階)。
* 轉換為角度：$\text{deg} = \text{raw} \times \frac{360.0}{4096.0}$。
* 套用零點校正偏移：`angle -= ZERO_OFFSET_DEG (51.86°)`，並呼叫 `wrapDeg()` 限制在 $[0, 360)$ 範圍內。

### 3.5 雙葉片相位補償與彩現管線 (`renderPOV`)
* 機械結構為 4 葉片架構中的第 2 與第 4 葉片：
  * 葉片 2 (Blade 2)：相較基準軸偏移 $+90^\circ$
  * 葉片 4 (Blade 4)：相較基準軸偏移 $+270^\circ$
* 實作即時角度對應：
  ```cpp
  int angle2 = (int)(rotorAngle + 90.0f + 0.5f) % 360;
  int angle4 = (int)(rotorAngle + 270.0f + 0.5f) % 360;
  ```
* 依據查表內容填入金黃色 (`CRGB(255, 220, 40)`) 或黑色 (`CRGB::Black`)。
* 呼叫單次 `FastLED.show()`，由 ESP32-C3 的硬體 RMT 控制器同時對兩個通道輸出訊號。

### 3.6 馬達轉速控制 (`updateMotor`)
* 讀取類比輸入 `POT_PIN` (12-bit，範圍 $0 \sim 4095$)。
* 設有死區 `POT_DEAD_ZONE = 100`：當旋鈕讀數大於等於 $4095 - 100 = 3995$ 時，輸出靜止訊號 $1500\,\mu s$。
* 否則將 $0 \sim 4095$ 映射為 $2000\,\mu s \sim 1500\,\mu s$ (逆向映射)。

---

## 4. 關鍵效能與物理時序極限分析 (Timing & Bottleneck Analysis)

POV 系統能否清晰成像，完全取決於**旋轉速度**與**單幀更新耗時**之間的物理限制：

```
┌─────────────────────────────────────────────────────────────┐
│ 單次 Loop 耗時構成                                          │
├─────────────────┬───────────────────┬───────────────────────┤
│ I2C 角度讀取    │ WS2812B 訊號傳輸  │ ADC 類比轉換          │
│ 約 100~150 µs   │ 約 1,260 µs       │ 約 30~50 µs           │
└─────────────────┴───────────────────┴───────────────────────┘
  總耗時 t_cycle ≈ 1.45 ~ 1.50 ms ──► 最高更新率約 660 ~ 700 Hz
```

1. **WS2812B 協議的硬性瓶頸**：
   * WS2812B 為單線歸零碼 (NRZ)，每個 bit 傳輸耗時 $1.25\,\mu s$。
   * 每顆 LED 需 24 bits = $30\,\mu s$。
   * 單條 42 顆 LED：$42 \times 30\,\mu s = 1,260\,\mu s = 1.26\text{ ms}$。
   * 加上重置信號 (Latch Reset) 至少需要 $50 \sim 300\,\mu s$。
   * 傳輸 42 顆 LED 至少需要 **$\approx 1.3\text{ ms}$**。

2. **旋轉角速度與解析度衝擊**：
   * 假設馬達轉速為 **600 RPM** ($10\text{ 轉/秒}$，每轉耗時 $100\text{ ms}$)：
     * 每轉理論時間：$100\text{ ms}$。
     * 單幀循環時間約 $1.5\text{ ms}$，因此每轉最多只能更新：
       $$\frac{100\text{ ms}}{1.5\text{ ms}} \approx 66 \text{ 次/轉}$$
     * 每次更新葉片已轉動了約 **$5.4^\circ$**！
   * 結論：**查表雖然切了 360 個角度（$1^\circ$ 解析度），但在轉速達到 600 RPM 以上時，受限於 WS2812B 通訊速度，實際顯示角解析度約只有 $5^\circ \sim 6^\circ$**，會產生輕微的角向拉伸或條紋感。

---

## 5. 優勢與亮點 (Strengths)

1. **查表優化設計優秀**：
   * 在記憶體充裕的 ESP32-C3 上，將複雜的浮點點線幾何計算預先烘焙成 $360 \times 42$ 的布林陣列，在即時中斷/輪詢時只需 $O(1)$ 的記憶體查找，非常適合即時嵌入式系統。
2. **多通道同步輸出**：
   * 充分利用 FastLED 在 ESP32 上的 RMT 平行驅動能力，兩個葉片只需呼叫一次 `FastLED.show()` 即可由硬體並行發送，無額外 CPU 序列等待。
3. **幾何座標正規化完備**：
   * 考慮到了葉片中心到第一顆燈的實際機械物理間距 (`FIRST_LED_RADIUS_MM = 12.5mm`)，避免圖案在中心扭曲變形。

---

## 6. 潛在風險與重大問題分析 (Issues & Critical Risks)

### ⚠️ 風險 1：馬達控制安全邏輯存在安全漏洞 (Severe Safety Hazard)
在 `updateMotor()` 中：
```cpp
if (pot >= 4095 - POT_DEAD_ZONE) {
    currentPWM = MOTOR_STOP_US; // 1500us (停轉)
} else {
    currentPWM = map(pot, 0, 4095, MOTOR_MAX_US, MOTOR_STOP_US); // 0 -> 2000us (全速!)
}
```
* **問題**：
  * 當 `pot == 0` 時，映射出的 PWM 為 `2000us` (最大轉速)！
  * **斷線風險**：若調速旋鈕訊號線鬆脫、虛焊或接觸不良，ESP32 ADC 容易拉低至 0V (讀數為 0)。
  * **後果**：系統一旦斷線或開機未旋轉旋鈕，馬達會**直接以最大馬力 (Full Throttle) 暴衝**！對於裝有 600mm 直徑高速旋轉葉片的機構而言，這是極高危險的機械與人身安全隱患。

### ⚠️ 風險 2：主迴圈頻繁讀取 ADC 拖慢 POV 更新速度
* 在 `loop()` 裡，每次執行 `renderPOV()` 都會先呼叫 `updateMotor()`。
* ESP32 的 `analogRead()` 每次轉換需消耗 $30 \sim 50\,\mu s$ 且常有底層雜訊。
* 電變 (ESC) 的伺服脈衝訊號週期通常只有 **50Hz (每 20ms 一次)**。在 600Hz 以上的高速 POV 迴圈中，每隔 1.5ms 就去讀一次 ADC 並呼叫 `spark.writeMicroseconds` 完全是多餘的開銷，反而稀釋了 POV 的及時性。

### ⚠️ 風險 3：記憶體結構型態有 8 倍的優化空間
* 目前宣告：`bool hexagramTable[360][NUM_LEDS];`
* 在 C++ 中，`bool` 實質佔用 1 byte (8 bits)。因此該表格消耗：
  $$360 \times 42 \times 1\text{ byte} = 15,120\text{ bytes} \approx 14.8\text{ KB}$$
* 雖然 ESP32-C3 有 400KB SRAM，但若改用 `uint64_t` 位元遮罩 (因為 $42 \le 64$ 位元)：
  $$360 \times 8\text{ bytes} = 2,880\text{ bytes} \approx 2.8\text{ KB}$$
  可減少 **81%** 的快取與記憶體佔用，且位元操作速度更敏捷。

### ⚠️ 風險 4：I2C 讀取失敗時無重試或日誌機制
* `readRawAngle()` 若在高速旋轉引發電磁干擾 (EMI) 或晃動導致 I2C 通訊失敗，會直接回傳 `false`，`renderPOV()` 則直接 `return` 跳過該幀。
* 雖然不會死機，但若旋轉時頻繁丟失封包，葉片會短暫保持上一幀內容，在高速下可能形成殘影或錯位。

---

## 7. 重構與優化建議 (Recommended Improvements)

### 7.1 馬達調速安全性修復與非同步降頻
建議將馬達調速獨立於 POV 迴圈之外，限制在 20ms (50Hz) 更新一次，並加入「低檔安全啟動 (Fail-Safe)」與正確的方向映射：

```cpp
void updateMotorSafe()
{
    static unsigned long lastMotorUpdate = 0;
    unsigned long now = millis();
    
    // 降頻至 50Hz (20ms)，避免消耗 POV 循環資源
    if (now - lastMotorUpdate < 20) {
        return;
    }
    lastMotorUpdate = now;

    int pot = analogRead(POT_PIN);
    pot = constrain(pot, 0, 4095);

    // 安全邏輯：將低端設為死區 (停轉)，高端設為加速
    // 即使旋鈕斷線拔除 (通常掉到 0V)，也會安全停轉
    constexpr int POT_MIN_DEAD_ZONE = 150; 

    if (pot <= POT_MIN_DEAD_ZONE) {
        currentPWM = MOTOR_STOP_US; // 1500us
    } else {
        // 從停止漸進至全速
        currentPWM = map(pot, POT_MIN_DEAD_ZONE, 4095, MOTOR_STOP_US, MOTOR_MAX_US);
        currentPWM = constrain(currentPWM, MOTOR_STOP_US, MOTOR_MAX_US);
    }

    spark.writeMicroseconds(currentPWM);
}
```

### 7.2 記憶體位元遮罩化 (Bitmask LUT)
```cpp
// 360 個角度，每個角度用一個 64 位元整數儲存 42 顆 LED 的開關狀態
uint64_t hexagramBitTable[360];

// 建表時：
if (on) {
    hexagramBitTable[deg] |= (1ULL << led);
}

// 彩現時：
uint64_t pattern = hexagramBitTable[angleDeg];
for (int i = 0; i < NUM_LEDS; i++) {
    if ((pattern >> i) & 1ULL) {
        leds[i] = CRGB(255, 220, 40);
    } else {
        leds[i] = CRGB::Black;
    }
}
```

### 7.3 硬體層級建議 (次世代 POV 升級)
若後續欲提高顯示細緻度與轉速：
* 將 **WS2812B** 更換為 **APA102 / SK9822 / HD107s** (具備獨立 Data 與 Clock 腳位之 SPI 燈條)。
* SPI 傳輸頻率可達 20MHz 以上，42 顆燈的更新時間可從 $1260\,\mu s$ 縮減至 **$50\,\mu s$ 以內**，顯示角解析度將提升 20 倍以上，徹底消除高速旋轉下的扇形拉扯效應。
