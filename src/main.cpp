#include <Arduino.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include "config.h"
#include <Ethernet.h>
#include "network.h"
#include <WiFi.h>
#include "supabase.h"
#include "ota.h"
#include "lan_update.h"
#include "gw_net.h"
#include "gw_lora.h"

LiquidCrystal_I2C lcd(LCD_I2C_ADDR, 16, 2);

// HardwareSerial 1 untuk Sensor Ultrasonik A02YYUW
HardwareSerial SensorSerial(1);

float calibrate(float raw);

// Jarak terkalibrasi terakhir (cm) yang dikirim ke Supabase. Tidak bergantung dimensi
// tangki (itu diatur di aplikasi) dan tanpa filter median/EMA/deadband.
// Hanya dipakai bila masih segar; sensor lepas -> tidak ada yang dikirim.
float rawDistance = 0;
unsigned long rawAt = 0;
void acceptRaw(float cm) {
    if (cm < RAW_MIN_VALID_CM || cm > RAW_MAX_VALID_CM) return; // di luar jangkauan fisik sensor
    rawDistance = cm;
    rawAt = millis();
}
bool rawFresh() { return rawDistance > 0 && millis() - rawAt < RAW_MAX_AGE_MS; }

unsigned char data[4] = {};
float distance = 0; // Jarak sensor ke permukaan air (cm), 0 = belum ada data
unsigned long rxBytes = 0;        // diagnosis: total byte diterima dari sensor
unsigned long checksumErrors = 0; // diagnosis: paket dengan checksum salah

// Penyaring jarak, tiga tahap (parameter di config.h):
//  0. rentang : pembacaan di luar DISTANCE_MIN/MAX_VALID_CM dibuang
//  1. median  : pantulan nyasar sesaat (mis. 33 cm -> 148 cm untuk satu pembacaan) terbuang
//  2. EMA     : riak air diperhalus, tetap mengikuti kenaikan/penurunan sungguhan
//  3. deadband: `distance` hanya berubah bila selisihnya >= DISTANCE_DEADBAND_CM
void acceptDistance(float cal) {
    if (cal < DISTANCE_MIN_VALID_CM || cal > DISTANCE_MAX_VALID_CM) return; // di luar tangki: abaikan
    static float buf[DISTANCE_MEDIAN_WINDOW];
    static int count = 0, next = 0;
    buf[next] = cal;
    next = (next + 1) % DISTANCE_MEDIAN_WINDOW;
    if (count < DISTANCE_MEDIAN_WINDOW) count++;

    float sorted[DISTANCE_MEDIAN_WINDOW];
    for (int i = 0; i < count; i++) {
        float v = buf[i];
        int j = i;
        while (j > 0 && sorted[j - 1] > v) { sorted[j] = sorted[j - 1]; j--; }
        sorted[j] = v;
    }
    float median = sorted[count / 2];

    static float ema = 0;
    static unsigned long lastEma = 0;
    if (ema == 0) {
        ema = median;
        lastEma = millis();
    } else if (millis() - lastEma >= DISTANCE_EMA_INTERVAL_MS) {
        lastEma = millis();
        ema = DISTANCE_EMA_ALPHA * median + (1.0f - DISTANCE_EMA_ALPHA) * ema;
    }

    if (distance <= 0 || fabsf(ema - distance) >= DISTANCE_DEADBAND_CM) distance = ema;
}

enum TankState { STATE_UNKNOWN, STATE_SANGAT_KOSONG, STATE_KOSONG, STATE_NORMAL, STATE_PENUH, STATE_SANGAT_PENUH };
TankState tankState = STATE_UNKNOWN;
float levelPct = 0; // tinggi air dalam persen tinggi tangki

const char* stateName(TankState st) {
    switch (st) {
        case STATE_SANGAT_KOSONG: return "SANGAT KOSONG";
        case STATE_KOSONG:        return "KOSONG";
        case STATE_NORMAL:        return "NORMAL";
        case STATE_PENUH:         return "PENUH";
        case STATE_SANGAT_PENUH:  return "SANGAT PENUH";
        default:                  return "-";
    }
}

// Kedip LCD
unsigned long blinkStart = 0;
unsigned long lastToggle = 0;
bool blinking = false;
bool backlightOn = true;

#if SENSOR_MODE == SENSOR_MODE_DIAG
volatile unsigned long edgesRx = 0, edgesTx = 0;
void IRAM_ATTR isrRx() { edgesRx++; }
void IRAM_ATTR isrTx() { edgesTx++; }
void readUltrasonic() {
    static unsigned long last = 0;
    if (millis() - last < 5000) return;
    last = millis();
    Serial.printf("DIAG: perubahan sinyal GPIO%d=%lu (level %d), GPIO%d=%lu (level %d)\n",
                  ULTRASONIC_RX_PIN, edgesRx, digitalRead(ULTRASONIC_RX_PIN),
                  ULTRASONIC_TX_PIN, edgesTx, digitalRead(ULTRASONIC_TX_PIN));
    edgesRx = 0; edgesTx = 0;
}
#elif SENSOR_MODE == SENSOR_MODE_PWM
// Mode PWM: pulsa TRIG 20us, lebar pulsa ECHO = waktu pantul (us)
void readUltrasonic() {
    static unsigned long lastTrigger = 0;
    if (millis() - lastTrigger < 200) return;
    lastTrigger = millis();

    digitalWrite(ULTRASONIC_TX_PIN, LOW);
    delayMicroseconds(2);
    digitalWrite(ULTRASONIC_TX_PIN, HIGH);
    delayMicroseconds(20);
    digitalWrite(ULTRASONIC_TX_PIN, LOW);

    unsigned long duration = pulseIn(ULTRASONIC_RX_PIN, HIGH, 30000);
    if (duration > 0) {
        rxBytes++; // diagnosis: jumlah pulsa valid
        acceptRaw(calibrate(duration / 58.3));
        acceptDistance(calibrate(duration / 58.3)); // us -> cm (kecepatan suara 343 m/s, pulang-pergi)
    }
}
#else
void readUltrasonic() {
    // A02YYUW terus-menerus mengirim 4 byte: 0xFF, Data_H, Data_L, Checksum
    while (SensorSerial.available() > 0) {
        rxBytes++;
        if (SensorSerial.read() == 0xFF) {
            unsigned long timeout = millis();
            while (SensorSerial.available() < 3) {
                if (millis() - timeout > 50) return;
            }

            data[0] = 0xFF;
            data[1] = SensorSerial.read();
            data[2] = SensorSerial.read();
            data[3] = SensorSerial.read();

            int sum = (data[0] + data[1] + data[2]) & 0x00FF;
            if (sum == data[3]) {
                float cm = ((data[1] << 8) + data[2]) / 10.0f; // mm -> cm
                acceptRaw(calibrate(cm));          // terkalibrasi -> Supabase
                acceptDistance(calibrate(cm));     // kalibrasi + filter -> LCD & status
            } else {
                checksumErrors++;
            }
        }
    }
}

#endif

// Terapkan regresi linear hasil kalibrasi ke pembacaan mentah sensor (cm)
float calibrate(float raw) {
    float cal = CAL_SLOPE * raw + CAL_INTERCEPT;
    return cal > 0.1f ? cal : 0.1f; // 0 dipakai sebagai "belum ada data"
}

// Float switch dengan debounce: status baru dipakai setelah stabil FLOAT_DEBOUNCE_MS.
// Pin di FLOAT_ACTIVE_LEVEL = air benar-benar penuh.
bool floatFull() {
    static bool stable = false;
    static bool lastRaw = false;
    static unsigned long since = 0;
    bool raw = digitalRead(FLOAT_SWITCH_PIN) == FLOAT_ACTIVE_LEVEL;
    if (raw != lastRaw) { lastRaw = raw; since = millis(); }
    if (raw != stable && millis() - since >= FLOAT_DEBOUNCE_MS) stable = raw;
    return stable;
}

// Tentukan status dari persen level air (+ float switch sebagai cadangan sangat penuh)
TankState evaluateState() {
    // Level selalu dihitung dulu, supaya nilai yang dikirim tetap terbaru walau float aktif
    if (distance > 0) {
        // Tinggi air = (offset sensor + tinggi tangki) - jarak sensor ke permukaan air
        levelPct = constrain((SENSOR_OFFSET_CM + TANK_HEIGHT_CM - distance) * 100.0f / TANK_HEIGHT_CM, 0.0f, 100.0f);
    }
    static bool floatLatched = false;
    if (floatFull()) {
        floatLatched = true;
        return STATE_SANGAT_PENUH;
    }
    if (floatLatched) {
        // Float baru lepas: status SANGAT PENUH berasal dari float, jangan dipertahankan
        // kalau ultrasonik tidak punya data untuk menggantikannya
        floatLatched = false;
        if (distance <= 0) return STATE_UNKNOWN;
    }
    if (distance <= 0) return tankState; // belum ada data valid

    // Batas antar status: [0]=sangat kosong/kosong, [1]=kosong/normal, [2]=normal/penuh, [3]=penuh/sangat penuh
    const float bound[4] = {LEVEL_SANGAT_KOSONG_PCT, LEVEL_KOSONG_PCT, LEVEL_PENUH_PCT, LEVEL_SANGAT_PENUH_PCT};
    int idx = 0; // 0..4 = sangat kosong .. sangat penuh
    while (idx < 4 && levelPct >= bound[idx]) idx++;

    if (tankState == STATE_UNKNOWN) return (TankState)(idx + 1);

    int cur = (int)tankState - 1;
    // Histeresis: pindah status hanya jika melewati batas lebih dari margin
    if (idx > cur && levelPct < bound[idx - 1] + LEVEL_HYSTERESIS_PCT) return tankState;
    if (idx < cur && levelPct > bound[idx] - LEVEL_HYSTERESIS_PCT) return tankState;
    return (TankState)(idx + 1);
}

void startBlink() {
    if (!LCD_BLINK_ENABLED) return;
    blinking = true;
    blinkStart = millis();
    lastToggle = blinkStart;
    backlightOn = false;
    lcd.noBacklight();
}

void updateBlink() {
    if (!blinking) return;
    unsigned long now = millis();
    if (now - blinkStart >= LCD_BLINK_DURATION_MS) {
        blinking = false;
        backlightOn = true;
        lcd.backlight();
        return;
    }
    if (now - lastToggle >= LCD_BLINK_INTERVAL_MS) {
        lastToggle = now;
        backlightOn = !backlightOn;
        if (backlightOn) lcd.backlight(); else lcd.noBacklight();
    }
}

void sendNotification(TankState state) {
    // TODO: kirim notifikasi (Supabase / Telegram) lewat Ethernet
    Serial.print("NOTIFIKASI: groundtank ");
    Serial.print(stateName(state));
    if (distance <= 0) {
        Serial.println(", sensor ultrasonik tidak ada data");
        return;
    }
    Serial.print(", pembacaan sensor = ");
    Serial.print(distance, 1);
    Serial.print(" cm (");
    Serial.print(levelPct, 0);
    Serial.println("%)");
}

void showLcd() {
    char line[17];

    // Baris 1: pembacaan asli sensor (jarak ke permukaan air)
    lcd.setCursor(0, 0);
    if (distance > 0) {
        snprintf(line, sizeof(line), "Jarak:%6.1f cm ", distance);
        lcd.print(line);
    } else {
        lcd.print("Baca...         ");
    }

    // Baris 2: status tangki
    lcd.setCursor(0, 1);
    snprintf(line, sizeof(line), "%-16s", stateName(tankState));
    lcd.print(line);
}

// Tampilan LCD saat firmware dipasang lewat OTA
static void otaOnStart(const char* version) {
    gwOtaBerjalan = true; // Task_Monitor berhenti menulis LCD
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("UPDATE FIRMWARE");
    lcd.setCursor(0, 1);
    lcd.print("v");
    lcd.print(version);
}

static void otaOnProgress(int percent) {
    lcd.setCursor(11, 1);
    lcd.print(percent);
    lcd.print("%  ");
}

static void taskMonitor(void*);

void setup() {
    Serial.begin(115200);

    pinMode(FLOAT_SWITCH_PIN, INPUT_PULLUP);

    // Inisialisasi LCD I2C dengan pin dari config.h
    Wire.begin(LCD_SDA_PIN, LCD_SCL_PIN);
    lcd.init();
    lcd.backlight();
    lcd.setCursor(0, 0);
    lcd.print("GROUNDTANK");
    lcd.setCursor(0, 1);
    lcd.print("PROJECT");
    delay(1500);
    lcd.clear();

    // Inisialisasi jaringan (W5500)
    lcd.setCursor(0, 0);
    lcd.print("Init Network...");
    NetworkStatus net = network_init();
    lcd.clear();

    // Tampilkan hasil inisiasi modul LAN di LCD
    lcd.setCursor(0, 0);
    lcd.print(network_link() == LINK_WIFI ? "LAN gagal > WiFi" : "Modul LAN:");
    lcd.setCursor(0, 1);
    if (network_link() == LINK_WIFI) {
        lcd.print(WiFi.localIP());
    } else {
        switch (net) {
            case NET_CONNECTED:   lcd.print(Ethernet.localIP()); break; // maks 16 karakter
            case NET_NO_MODULE:   lcd.print("TIDAK TERDETEKSI"); break;
            case NET_NO_CABLE:    lcd.print("KABEL LAN LEPAS "); break;
            case NET_DHCP_FAILED: lcd.print("GAGAL DAPAT IP  "); break;
        }
    }
    delay(2500);
    lcd.clear();

    // Inisialisasi sensor ultrasonik A02YYUW
#if SENSOR_MODE == SENSOR_MODE_DIAG
    pinMode(ULTRASONIC_RX_PIN, INPUT_PULLDOWN);
    pinMode(ULTRASONIC_TX_PIN, INPUT_PULLDOWN);
    attachInterrupt(ULTRASONIC_RX_PIN, isrRx, CHANGE);
    attachInterrupt(ULTRASONIC_TX_PIN, isrTx, CHANGE);
#elif SENSOR_MODE == SENSOR_MODE_PWM
    pinMode(ULTRASONIC_TX_PIN, OUTPUT);
    pinMode(ULTRASONIC_RX_PIN, INPUT);
#else
    SensorSerial.begin(ULTRASONIC_BAUD, SERIAL_8N1, ULTRASONIC_RX_PIN, ULTRASONIC_TX_PIN);
#endif

    // GT-01: buat Queue_Net & Queue_Cmd, lalu jalankan Task_Monitor, Task_LoRa, Task_Net.
    gwQueueBuat();
    taskLoraMulai();
    taskNetMulai(otaOnStart, otaOnProgress);
    xTaskCreatePinnedToCore(taskMonitor, "Task_Monitor", 8192, nullptr, 2, nullptr, 1);
    Serial.println("Sistem Ground Tank Siap Berjalan");
}

// Task_Monitor (GT-01): alur Ground Tank yang sudah ada -- baca level, LCD, notifikasi.
// Semua yang memakai jaringan (Supabase, OTA, upload LAN) pindah ke Task_Net (gw_net.cpp);
// level & float dikirim lewat Queue_Net.
static void taskMonitor(void*) {
    for (;;) {
        // Baca level air
#if DUMMY_SENSOR
        // Data uji: air naik-turun pelan antara kosong dan penuh, satu siklus DUMMY_PERIOD_MS
        float phase = (millis() % DUMMY_PERIOD_MS) * 2.0f * PI / DUMMY_PERIOD_MS;
        distance = SENSOR_OFFSET_CM + TANK_HEIGHT_CM * (0.5f + 0.45f * cosf(phase));
        acceptRaw(distance);
#else
        readUltrasonic();
#endif

        // Bila status berubah: notifikasi, dan LCD berkedip 1 menit untuk status selain NORMAL
        TankState newState = evaluateState();
        if (newState != tankState) {
            tankState = newState;
            sendNotification(tankState);
            if (tankState != STATE_NORMAL) startBlink();
        }

        if (!gwOtaBerjalan) {
            showLcd();
            updateBlink();
        }

        // Kirim level ke Queue_Net tiap 1 s (tanpa pembacaan valid tidak ada yang dikirim:
        // nilai pengganti 0/-1 akan dibaca aplikasi sebagai data nyata).
        static unsigned long lastLevel = 0;
        if (millis() - lastLevel >= 1000) {
            lastLevel = millis();
            NetItem it = {};
            it.tipe = NI_LEVEL;
            it.level = rawFresh() ? rawDistance : 0;
            it.full = floatFull();
            gwNetKirim(it);
        }

        static unsigned long lastLog = 0;
        if (millis() - lastLog >= 5000) {
            lastLog = millis();
            Serial.print("Level: ");
            Serial.print(distance, 1);
            Serial.print(" cm, RX bytes: ");
            Serial.print(rxBytes);
            Serial.print(", checksum error: ");
            Serial.print(checksumErrors);
            Serial.print(", Float pin: ");
            Serial.print(digitalRead(FLOAT_SWITCH_PIN) == LOW ? "LOW" : "HIGH");
            Serial.print(" (");
            Serial.print(floatFull() ? "AKTIF" : "tidak aktif");
            Serial.print("), Status: ");
            Serial.print(stateName(tankState));
            Serial.print(", Link: ");
            NetLink lk = network_link();
            Serial.print(lk == LINK_WIFI ? "WiFi " : lk == LINK_ETHERNET ? "LAN " : "tidak ada");
            if (lk == LINK_WIFI) Serial.print(WiFi.localIP());
            else if (lk == LINK_ETHERNET) Serial.print(Ethernet.localIP());
            Serial.print(", upload 8080: ");
            Serial.print(lan_update_listening() ? "siap" : "mati");
            Serial.print(", LoRa: ");
            Serial.println(gwLoraStatusTeks());
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// loop() kosong: semua pekerjaan ada di task FreeRTOS (flowchart GT-01).
void loop() {
    vTaskDelay(pdMS_TO_TICKS(1000));
}
