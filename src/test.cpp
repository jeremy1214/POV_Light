#include <Arduino.h>
#include <Wire.h>
#include <FastLED.h>
#include <ESP32Servo.h>
#include <math.h>

// ============================================================
// Pin
// ============================================================

// 第 2 根葉片
constexpr uint8_t PIN_B2 = D1;

// 第 4 根葉片
constexpr uint8_t PIN_B4 = D3;

// 可變電阻
constexpr uint8_t POT_PIN = D2;

// AS5600
constexpr uint8_t SDA_PIN = D4;
constexpr uint8_t SCL_PIN = D5;

// SPARK
constexpr uint8_t SPARK_PIN = D6;


// ============================================================
// LED
// ============================================================

constexpr uint16_t NUM_LEDS = 42;

CRGB blade2[NUM_LEDS];
CRGB blade4[NUM_LEDS];

constexpr uint8_t BRIGHTNESS = 90;


// ============================================================
// AS5600
// ============================================================

constexpr uint8_t AS5600_ADDR = 0x36;
constexpr uint8_t RAW_ANGLE_REG = 0x0C;

// 原本 D0 朝上時校正
constexpr float ZERO_OFFSET_DEG = 51.86f;


// ============================================================
// Motor
// ============================================================

Servo spark;

constexpr int MOTOR_STOP_US = 1500;
constexpr int MOTOR_MAX_US  = 2000;

constexpr int POT_DEAD_ZONE = 100;

int currentPWM = MOTOR_STOP_US;


// ============================================================
// Geometry
// ============================================================

constexpr float OUTER_RADIUS_MM = 300.0f;
constexpr float FIRST_LED_RADIUS_MM = 12.5f;


// ============================================================
// 六芒星查表
//
// 360 angle × 42 LED
// ============================================================

bool hexagramTable[360][NUM_LEDS];


// ============================================================
// Helper
// ============================================================

float degToRad(float deg)
{
    return deg * PI / 180.0f;
}


float wrapDeg(float angle)
{
    while (angle < 0.0f)
        angle += 360.0f;

    while (angle >= 360.0f)
        angle -= 360.0f;

    return angle;
}


float logicalLEDToNormalizedRadius(uint16_t index)
{
    float ratio =
        (float)index
        /
        (NUM_LEDS - 1);


    float radiusMM =
        FIRST_LED_RADIUS_MM
        +
        ratio
        *
        (
            OUTER_RADIUS_MM
            -
            FIRST_LED_RADIUS_MM
        );


    return
        radiusMM
        /
        OUTER_RADIUS_MM;
}


// ============================================================
// AS5600
// ============================================================

bool readRawAngle(uint16_t &raw)
{
    Wire.beginTransmission(AS5600_ADDR);

    Wire.write(
        RAW_ANGLE_REG
    );


    if (
        Wire.endTransmission(false)
        !=
        0
    )
    {
        return false;
    }


    uint8_t count =
        Wire.requestFrom(
            AS5600_ADDR,
            (uint8_t)2
        );


    if (count != 2)
    {
        return false;
    }


    uint8_t highByte =
        Wire.read();

    uint8_t lowByte =
        Wire.read();


    raw =
        ((uint16_t)(highByte & 0x0F) << 8)
        |
        lowByte;


    return true;
}


// ============================================================
// Rotor angle
// ============================================================

bool getRotorAngle(float &angle)
{
    uint16_t raw;


    if (!readRawAngle(raw))
    {
        return false;
    }


    angle =
        raw
        *
        360.0f
        /
        4096.0f;


    angle -=
        ZERO_OFFSET_DEG;


    angle =
        wrapDeg(angle);


    return true;
}


// ============================================================
// Motor
// ============================================================

void updateMotor()
{
    int pot =
        analogRead(POT_PIN);


    pot =
        constrain(
            pot,
            0,
            4095
        );


    if (
        pot >=
        4095 - POT_DEAD_ZONE
    )
    {
        currentPWM =
            MOTOR_STOP_US;
    }
    else
    {
        currentPWM =
            map(
                pot,

                0,
                4095,

                MOTOR_MAX_US,
                MOTOR_STOP_US
            );


        currentPWM =
            constrain(
                currentPWM,
                MOTOR_STOP_US,
                MOTOR_MAX_US
            );
    }


    spark.writeMicroseconds(
        currentPWM
    );
}


// ============================================================
// Point -> segment distance
// ============================================================

float distanceToSegment(
    float px,
    float py,

    float x1,
    float y1,

    float x2,
    float y2
)
{
    float vx =
        x2 - x1;

    float vy =
        y2 - y1;


    float wx =
        px - x1;

    float wy =
        py - y1;


    float len2 =
        vx * vx
        +
        vy * vy;


    if (len2 < 0.000001f)
    {
        float dx =
            px - x1;

        float dy =
            py - y1;


        return sqrtf(
            dx * dx
            +
            dy * dy
        );
    }


    float t =
        (
            wx * vx
            +
            wy * vy
        )
        /
        len2;


    t =
        constrain(
            t,
            0.0f,
            1.0f
        );


    float cx =
        x1
        +
        t * vx;

    float cy =
        y1
        +
        t * vy;


    float dx =
        px - cx;

    float dy =
        py - cy;


    return sqrtf(
        dx * dx
        +
        dy * dy
    );
}


// ============================================================
// 建立六芒星
// ============================================================

void buildHexagramTable()
{
    constexpr float OUTER_R =
        0.86f;

    // 可調線寬
    constexpr float LINE_WIDTH =
        0.055f;


    float upX[3];
    float upY[3];

    float dnX[3];
    float dnY[3];


    // ========================================================
    // 上三角形
    // ========================================================

    const float upAngles[3] =
    {
        0.0f,
        120.0f,
        240.0f
    };


    // ========================================================
    // 下三角形
    // ========================================================

    const float dnAngles[3] =
    {
        180.0f,
        300.0f,
        60.0f
    };


    for (int i = 0; i < 3; i++)
    {
        float a =
            degToRad(
                upAngles[i]
            );


        upX[i] =
            OUTER_R
            *
            sinf(a);


        upY[i] =
            OUTER_R
            *
            cosf(a);
    }


    for (int i = 0; i < 3; i++)
    {
        float a =
            degToRad(
                dnAngles[i]
            );


        dnX[i] =
            OUTER_R
            *
            sinf(a);


        dnY[i] =
            OUTER_R
            *
            cosf(a);
    }


    // ========================================================
    // 建立 360 × 42 查表
    // ========================================================

    for (
        int deg = 0;
        deg < 360;
        deg++
    )
    {
        float angle =
            degToRad(
                (float)deg
            );


        for (
            int led = 0;
            led < NUM_LEDS;
            led++
        )
        {
            float radius =
                logicalLEDToNormalizedRadius(
                    led
                );


            float x =
                radius
                *
                sinf(angle);


            float y =
                radius
                *
                cosf(angle);


            bool on =
                false;


            // =================================================
            // 上三角形
            // =================================================

            for (
                int edge = 0;
                edge < 3;
                edge++
            )
            {
                int next =
                    (edge + 1)
                    %
                    3;


                float d =
                    distanceToSegment(
                        x,
                        y,

                        upX[edge],
                        upY[edge],

                        upX[next],
                        upY[next]
                    );


                if (
                    d <
                    LINE_WIDTH
                )
                {
                    on = true;
                    break;
                }
            }


            // =================================================
            // 下三角形
            // =================================================

            if (!on)
            {
                for (
                    int edge = 0;
                    edge < 3;
                    edge++
                )
                {
                    int next =
                        (edge + 1)
                        %
                        3;


                    float d =
                        distanceToSegment(
                            x,
                            y,

                            dnX[edge],
                            dnY[edge],

                            dnX[next],
                            dnY[next]
                        );


                    if (
                        d <
                        LINE_WIDTH
                    )
                    {
                        on = true;
                        break;
                    }
                }
            }


            hexagramTable[deg][led] =
                on;
        }
    }
}


// ============================================================
// Render one blade
// ============================================================

void renderBlade(
    CRGB *leds,
    int angleDeg
)
{
    angleDeg %= 360;

    if (angleDeg < 0)
    {
        angleDeg += 360;
    }


    for (
        int i = 0;
        i < NUM_LEDS;
        i++
    )
    {
        if (
            hexagramTable[angleDeg][i]
        )
        {
            // 金黃色
            leds[i] =
                CRGB(
                    255,
                    220,
                    40
                );
        }
        else
        {
            leds[i] =
                CRGB::Black;
        }
    }
}


// ============================================================
// POV
// ============================================================

void renderPOV()
{
    float rotorAngle;


    if (!getRotorAngle(rotorAngle))
    {
        return;
    }


    // ========================================================
    // 第 2 根葉片
    //
    // 原本：
    // D0 = 0°
    // D1 = +90°
    //
    // 所以 Blade 2 = rotor + 90°
    // ========================================================

    int angle2 =
        (int)(
            rotorAngle
            +
            90.0f
            +
            0.5f
        );


    angle2 %= 360;


    // ========================================================
    // 第 4 根葉片
    //
    // D3 = +270°
    // ========================================================

    int angle4 =
        (int)(
            rotorAngle
            +
            270.0f
            +
            0.5f
        );


    angle4 %= 360;


    // ========================================================
    // 先把兩條的下一幀都算好
    // ========================================================

    renderBlade(
        blade2,
        angle2
    );


    renderBlade(
        blade4,
        angle4
    );


    // ========================================================
    // 重點
    //
    // 只有一次 show()
    //
    // FastLED 交給 ESP32 RMT backend
    // 處理兩條 controller
    // ========================================================

    FastLED.show();
}


// ============================================================
// Setup
// ============================================================

void setup()
{
    Serial.begin(
        115200
    );


    delay(
        500
    );


    // ========================================================
    // ADC
    // ========================================================

    analogReadResolution(
        12
    );


    pinMode(
        POT_PIN,
        INPUT
    );


    // ========================================================
    // AS5600
    // ========================================================

    Wire.begin(
        SDA_PIN,
        SCL_PIN
    );


    Wire.setClock(
        400000
    );


    // ========================================================
    // FastLED
    //
    // ESP32-C3 有兩個 RMT TX channel
    // 現在剛好兩條 WS2812
    // ========================================================

    FastLED.addLeds<
        WS2812B,
        PIN_B2,
        GRB
    >(
        blade2,
        NUM_LEDS
    );


    FastLED.addLeds<
        WS2812B,
        PIN_B4,
        GRB
    >(
        blade4,
        NUM_LEDS
    );


    FastLED.setBrightness(
        BRIGHTNESS
    );


    fill_solid(
        blade2,
        NUM_LEDS,
        CRGB::Black
    );


    fill_solid(
        blade4,
        NUM_LEDS,
        CRGB::Black
    );


    FastLED.show();


    // ========================================================
    // 六芒星 lookup
    // ========================================================

    buildHexagramTable();


    // ========================================================
    // Motor
    // ========================================================

    spark.setPeriodHertz(
        50
    );


    spark.attach(
        SPARK_PIN,
        1000,
        2000
    );


    spark.writeMicroseconds(
        MOTOR_STOP_US
    );


    Serial.println();
    Serial.println(
        "2-BLADE PARALLEL HEXAGRAM"
    );


    Serial.println(
        "Blade 2 = D1"
    );


    Serial.println(
        "Blade 4 = D3"
    );
}


// ============================================================
// Loop
// ============================================================

void loop()
{
    updateMotor();

    renderPOV();
}