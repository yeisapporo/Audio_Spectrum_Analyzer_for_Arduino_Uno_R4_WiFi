#include <Arduino.h>
#include <WiFiS3.h>
#include <Arduino_LED_Matrix.h>
#include <arduinoFFT.h>
#include <FspTimer.h>

// ---------- WiFi設定 ----------
const char *SSID = "rs500m-14aaf8-1";
const char *PASSWORD = "6a6a227fb3f17";

// ---------- 音声設定 ----------
constexpr uint16_t UDP_PORT = 5005;
constexpr uint16_t PCM_SIZE = 512;
constexpr uint16_t FFT_SIZE = 256;
constexpr float SAMPLE_RATE = 8000.0f;

// 最初はfalseで受信速度を確認する
constexpr bool FFT_BOTH_HALVES = false;

// ---------- LED表示 ----------
constexpr uint8_t BANDS = 12;
constexpr uint8_t ROWS = 8;

// FFTビン境界（高域ほど幅を広げる）
const uint8_t bandEdges[BANDS + 1] = {
    1, 3, 5, 7, 10, 14, 20, 28, 39, 54, 75, 101, 128
};

// 表示レベルの調整値（dB）
constexpr float DB_MIN = 28.0f;
constexpr float DB_MAX = 66.0f;

ArduinoLEDMatrix matrix;
uint8_t frame[8][12] = {};
uint8_t levels[BANDS] = {};
// ---------- ピークホールド ----------
constexpr uint32_t PEAK_HOLD_MS = 750;
constexpr uint32_t PEAK_FALL_MS = 50;

uint8_t peakLevels[BANDS] = {};
uint32_t peakTime[BANDS] = {};
uint32_t peakFallTime[BANDS] = {};

// ---------- UDP ----------
WiFiUDP udp;
uint8_t pcm[PCM_SIZE];

// ---------- FFT ----------
float vReal[FFT_SIZE];
float vImag[FFT_SIZE];

ArduinoFFT<float> FFT(
    vReal, vImag, FFT_SIZE, SAMPLE_RATE
);

float bandValues[BANDS] = {};

// ---------- 統計 ----------
uint32_t packets = 0;
uint32_t invalidPackets = 0;
uint32_t lastReport = 0;
uint32_t lastPacket = 0;
uint32_t lastDecay = 0;
uint32_t maxFftUs = 0;

bool updatePeakHold() {
    uint32_t now = millis();
    bool changed = false;

    for (uint8_t x = 0; x < BANDS; x++) {

        // 新しいピークを検出
        if (levels[x] > peakLevels[x]) {
            peakLevels[x] = levels[x];
            peakTime[x] = now;
            peakFallTime[x] = now;
            changed = true;
        }

        // 保持時間が過ぎたら落下
        else if (
            peakLevels[x] > levels[x] &&
            now - peakTime[x] >= PEAK_HOLD_MS &&
            now - peakFallTime[x] >= PEAK_FALL_MS
        ) {
            peakLevels[x]--;
            peakFallTime[x] = now;
            changed = true;
        }
    }

    return changed;
}

// ---------- LED表示 ----------
constexpr uint8_t PWM_STEPS = 8;
// 0=消灯、1=25%、2=50%、3=75%、4=100%
constexpr uint8_t BAR_PWM_ON_STEPS = 1;

constexpr float LED_SCAN_HZ =
    BANDS * ROWS * PWM_STEPS * 100.0f;

static_assert(BAR_PWM_ON_STEPS <= PWM_STEPS, "Invalid bar PWM duty");

FspTimer displayTimer;
volatile uint8_t displayFrames[2][ROWS][BANDS] = {};
volatile uint8_t displayFrameIndex = 0;

// FFT・UDP処理中もタイマー割り込みで走査を続ける
void scanSpectrumLED(timer_callback_args_t*) {
    static uint8_t pixel = 0;
    static uint8_t pwmPhase = 0;

    uint8_t duty = displayFrames[displayFrameIndex][pixel / BANDS][pixel % BANDS];

    if (pwmPhase < duty) {
        matrix.on(pixel);
    } else {
        matrix.off(pixel);
    }

    if (++pixel == BANDS * ROWS) {
        pixel = 0;
        if (++pwmPhase == PWM_STEPS) {
            pwmPhase = 0;
        }
    }
}

bool beginSpectrumDisplay() {
    matrix.off(0);

    uint8_t timerType;
    int8_t timerChannel = FspTimer::get_available_timer(timerType);

    if (timerChannel < 0) {
        return false;
    }

    bool ret1 = displayTimer.begin(
        TIMER_MODE_PERIODIC, timerType, timerChannel,
        LED_SCAN_HZ, 50.0f, scanSpectrumLED
    );
    bool ret2 = displayTimer.setup_overflow_irq();
    bool ret3 = displayTimer.open();
    bool ret4 = displayTimer.start();

    return (ret1 && ret2 && ret3 && ret4);
}

void drawSpectrum() {
    memset(frame, 0x00, sizeof(frame));

    for (uint8_t x = 0; x < BANDS; x++) {
        // バーは設定した点灯率
        for (uint8_t y = 0; y < levels[x]; y++) {
            frame[ROWS - 1 - y][x] = BAR_PWM_ON_STEPS;
        }

        // ピークは毎走査点灯。バーとの重なりも優先
        if (peakLevels[x] > 0) {
            frame[ROWS - peakLevels[x]][x] = PWM_STEPS;
        }
    }

    uint8_t nextFrameIndex = displayFrameIndex ^ 1;

    for (uint8_t y = 0; y < ROWS; y++) {
        for (uint8_t x = 0; x < BANDS; x++) {
            displayFrames[nextFrameIndex][y][x] = frame[y][x];
        }
    }

    noInterrupts();
    displayFrameIndex = nextFrameIndex;
    interrupts();
}

// ---------- 256サンプルのFFT ----------
void analyzeFFT(uint16_t offset, float *result) {
    int maxAmplitude = 0;

    for (uint16_t i = 0; i < FFT_SIZE; i++) {
        int sample = (int)pcm[offset + i] - 128;

        vReal[i] = (float)sample;
        vImag[i] = 0.0f;

        int a = abs(sample);
        if (a > maxAmplitude) {
            maxAmplitude = a;
        }
    }

    // 無音ならスペクトル0
    for (uint8_t b = 0; b < BANDS; b++) {
        result[b] = 0.0f;
    }

    if (maxAmplitude <= 1) {
        return;
    }

    uint32_t startUs = micros();

    FFT.dcRemoval();

    FFT.windowing(FFTWindow::Blackman, FFTDirection::Forward);

    FFT.compute(FFTDirection::Forward);
    FFT.complexToMagnitude();

    uint32_t elapsed = micros() - startUs;
    if (elapsed > maxFftUs) {
        maxFftUs = elapsed;
    }

    // 各周波数バンド内の最大振幅
    for (uint8_t b = 0; b < BANDS; b++) {
        float maxValue = 0.0f;

        for (uint16_t i = bandEdges[b];
            i < bandEdges[b + 1]; i++) {

            if (vReal[i] > maxValue) {
                maxValue = vReal[i];
            }
        }

        result[b] = maxValue;
    }
}

// ---------- バンド強度 -> LED高さ ----------
void updateLevels() {
    for (uint8_t b = 0; b < BANDS; b++) {
        // 対数変換（表示用の相対レベル）
        float db = 20.0f * log10f(bandValues[b] + 1.0f);
        float scaled = (db - DB_MIN) * 8.0f / (DB_MAX - DB_MIN);
        int target = (int)(scaled + 0.5f);

        target = constrain(target, 0, 8);

        // アタックは即時、減衰はゆっくり
        if (target >= levels[b]) {
            levels[b] = target;
        } else if (levels[b] > 0) {
            levels[b]--;
        }
    }
}

// ---------- 初期化 ----------
void setup() {
    Serial.begin(115200);

    while (!Serial) {
        delay(10);
    }

// matrix.begin() を置き換える
if (!beginSpectrumDisplay()) {
    Serial.println("LED Matrix init failed");
    while (true) delay(1000);
}

// matrix.clear() を置き換える
drawSpectrum();

    matrix.clear();
    Serial.println("Connecting WiFi...");

    if (WiFi.status() == WL_NO_MODULE) {
        Serial.println("WiFi module not found");
        while (true) {
            delay(1000);
        }
    }

    WiFi.begin(SSID, PASSWORD);

    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - start > 30000) {
            Serial.println("WiFi timeout");
            while (true) {
                delay(1000);
            }
        }
        delay(500);
    }

    IPAddress ip(0, 0, 0, 0);

    for (int i = 0; i < 20; i++) {
        ip = WiFi.localIP();

        if (ip != IPAddress(0, 0, 0, 0)) {
            break;
        }
        delay(500);
    }

    if (ip == IPAddress(0, 0, 0, 0)) {
        Serial.println("IP acquisition failed");
        while (true) {
            delay(1000);
        }
    }

    Serial.print("Arduino IP: ");
    Serial.println(ip);

    if (!udp.begin(UDP_PORT)) {
        Serial.println("UDP begin failed");
        while (true) {
            delay(1000);
        }
    }

    Serial.println("Spectrum analyzer ready");

    lastReport = millis();
    lastPacket = millis();
    lastDecay = millis();
}

// ---------- メイン処理 ----------
void loop() {
    int packetSize = udp.parsePacket();

    if (packetSize > 0) {
        if (packetSize == PCM_SIZE) {
            int n = udp.read(pcm, PCM_SIZE);

            if (n == PCM_SIZE) {
                packets++;
                lastPacket = millis();

                // 前半256サンプル
                analyzeFFT(0, bandValues);

                // 必要なら後半256サンプルも解析
                if (FFT_BOTH_HALVES) {
                    float second[BANDS];

                    analyzeFFT(FFT_SIZE, second);
                    for (uint8_t b = 0; b < BANDS; b++) {
                        bandValues[b] = (bandValues[b] + second[b]) * 0.5f;
                    }
                }

                updateLevels();
                updatePeakHold();
                drawSpectrum();
            } else {
                invalidPackets++;
            }
        } else {
            invalidPackets++;
            while (udp.available() > 0) {
                udp.read();
            }
        }
    }

    // 送信が止まった場合は表示を減衰
    uint32_t now = millis();

    if (now - lastPacket > 200 && now - lastDecay >= 80) {
        bool changed = false;

        for (uint8_t b = 0; b < BANDS; b++) {
            if (levels[b] > 0) {
                levels[b]--;
                changed = true;
            }
        }

        if (changed) {
            drawSpectrum();
        }

        lastDecay = now;
    }

    // ピークホールドの下降を継続する
    if (updatePeakHold()) {
        drawSpectrum();
    }

    // 受信状態を1秒ごとに表示
    if (now - lastReport >= 100) {
        Serial.print("Packets/s: ");
        Serial.print(packets);

        Serial.print(" FFT max: ");
        Serial.print(maxFftUs);
        Serial.print(" us");

        Serial.print(" Invalid: ");
        Serial.println(invalidPackets);

        packets = 0;
        invalidPackets = 0;
        maxFftUs = 0;
        lastReport = now;
    }
}
