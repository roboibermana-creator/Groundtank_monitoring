#ifndef CONFIG_H
#define CONFIG_H

// Pin untuk ESP32-S3 (DevKitC-1). Pin 22 tidak ada di S3, 26-32 dipakai flash,
// 19/20 USB, 0/3/45/46 strapping, sehingga wiring dipindah ke pin aman di bawah.

// Konfigurasi Pin W5500 Ethernet Module (SPI2/FSPI default S3)
#define W5500_CS_PIN    10  // SCS
#define W5500_MOSI_PIN  11  // MOSI
#define W5500_SCK_PIN   12  // SCLK
#define W5500_MISO_PIN  13  // MISO
#define W5500_RST_PIN   5   // RST
#define W5500_INT_PIN   6   // INT (belum dipakai di firmware)

// Konfigurasi Pin LCD 16x2 I2C
#define LCD_SDA_PIN     8
#define LCD_SCL_PIN     9
#define LCD_I2C_ADDR    0x27 // beberapa modul memakai 0x3F

// Konfigurasi Pin A02YYUW Ultrasonic Sensor
// Pilih mode sesuai versi sensor: SENSOR_MODE_UART atau SENSOR_MODE_PWM
#define SENSOR_MODE_UART 0
#define SENSOR_MODE_PWM  1
#define SENSOR_MODE_DIAG 2 // hitung perubahan sinyal di pin 16 dan 17
#define SENSOR_MODE      SENSOR_MODE_UART

// Hasil diagnosis: kabel yang mengirim data sensor ada di GPIO 17 -> RX ESP32-S3
// Mode PWM: ECHO = GPIO 17, TRIG = GPIO 16. Mode UART: GPIO 16 tidak dipakai (set -1)
#define ULTRASONIC_RX_PIN 17
#define ULTRASONIC_TX_PIN -1
#define ULTRASONIC_BAUD   9600

// Konfigurasi Float Switch HIGH (cadangan ultrasonik, ke GND, pull-up internal)
// Dipasang pada ketinggian "penuh"; aktif (LOW) => SANGAT PENUH walau ultrasonik error
#define FLOAT_SWITCH_PIN 4
// Level pin saat air penuh. HIGH = kontak float terbuka saat penuh (normally closed ke GND).
// Awas: dengan HIGH, kabel float yang lepas/putus juga terbaca "penuh".
#define FLOAT_ACTIVE_LEVEL HIGH
// Float dipasang di atas bibir tangki: aktif hanya kalau air benar-benar penuh (peringatan murni).
// Sinyal harus stabil selama waktu ini (ms) sebelum dianggap berubah, supaya riak air tidak membuatnya berkedip.
#define FLOAT_DEBOUNCE_MS 2000

// DATA DUMMY untuk uji aplikasi tanpa sensor. 1 = jarak disimulasikan (bukan data asli!)
// WAJIB kembali ke 0 sebelum dipasang di tangki.
#define DUMMY_SENSOR     0
#define DUMMY_PERIOD_MS  600000UL // satu siklus kosong -> penuh -> kosong (10 menit)

// Kalibrasi sensor (regresi linear): jarak_asli = SLOPE * pembacaan_sensor + INTERCEPT (cm)
// Hasil kalibrasi vs meteran, 16 titik 25-400 cm, R^2 = 1,0000
#define CAL_SLOPE      0.9992
#define CAL_INTERCEPT  (-0.1743)

// Tinggi tangki (cm), dari dasar sampai bibir atas tangki
#define TANK_HEIGHT_CM         180.0
// Jarak sensor ke bibir atas tangki (cm). Sensor dipasang di atas tangki,
// jadi saat tangki penuh pembacaan sensor sekitar nilai ini, bukan 0.
#define SENSOR_OFFSET_CM       30.0

// Pembacaan di luar rentang ini dibuang (0.1 = tak ada pantulan, ratusan cm = pantulan nyasar).
// Rentang wajar: SENSOR_OFFSET_CM (penuh) sampai SENSOR_OFFSET_CM + TANK_HEIGHT_CM (kosong), plus toleransi.
#define DISTANCE_MIN_VALID_CM  (SENSOR_OFFSET_CM - 10.0f)
#define DISTANCE_MAX_VALID_CM  (SENSOR_OFFSET_CM + TANK_HEIGHT_CM + 10.0f)

// Data ke Supabase = jarak TERKALIBRASI (CAL_SLOPE/INTERCEPT), tanpa filter di bawah
// dan tanpa batas tinggi tangki (dimensi tangki diatur di aplikasi).
// Hanya dibuang bila di luar jangkauan fisik A02YYUW (zona mati ~3 cm, maks 450 cm)
// atau sudah basi (sensor berhenti mengirim).
#define RAW_MIN_VALID_CM  3.0f
#define RAW_MAX_VALID_CM  450.0f
#define RAW_MAX_AGE_MS    5000UL

// Penyaring jarak ultrasonik (hanya untuk LCD & status lokal) (redam riak air saat tangki diisi):
//   median N pembacaan terakhir -> EMA tiap interval -> deadband sebelum dipakai/dikirim
#define DISTANCE_MEDIAN_WINDOW 15      // buang lonjakan sesaat
#define DISTANCE_EMA_ALPHA     0.1f    // makin kecil makin tenang (0.05 - 0.2), tapi makin lambat mengikuti
#define DISTANCE_EMA_INTERVAL_MS 1000UL // EMA diperbarui sekali per interval -> waktu respons ~10 dtk
#define DISTANCE_DEADBAND_CM   1.0f    // jarak baru dipakai jika berubah >= nilai ini (1 cm ~ 150 L)

// Ambang level air (persen tinggi air terhadap TANK_HEIGHT_CM)
//   0 - 5%   : SANGAT KOSONG
//   5 - 20%  : KOSONG
//  20 - 80%  : NORMAL
//  80 - 90%  : PENUH
//  90 - 100% : SANGAT PENUH
#define LEVEL_SANGAT_KOSONG_PCT 5.0
#define LEVEL_KOSONG_PCT        20.0
#define LEVEL_PENUH_PCT         80.0
#define LEVEL_SANGAT_PENUH_PCT  90.0
#define LEVEL_HYSTERESIS_PCT    2.0 // cegah status bolak-balik di sekitar batas

// LCD berkedip saat status berubah menjadi KOSONG / PENUH (1 = aktif, 0 = mati)
#define LCD_BLINK_ENABLED      0
#define LCD_BLINK_DURATION_MS  60000UL
#define LCD_BLINK_INTERVAL_MS  500UL

// Supabase (kunci publishable, sama seperti yang dipakai dashboard)
#define SUPABASE_HOST            "vupbropaxsqireeullbm.supabase.co"
#define SUPABASE_API_KEY         "sb_publishable_mvifCe-YeWbL4Wyp3jf1fA_Xwz2P26Z"
// Data dikirim lewat RPC submit_reading; ID perangkat/sensor & token ada di secrets.h
// Kontrak: wajib <= 60 dtk (aplikasi: "Tidak aktif" bila > 2 menit), disarankan 30 dtk, min 5 dtk
#define SUPABASE_SEND_INTERVAL_MS 30000UL
// Gagal jaringan/5xx: ulang dengan backoff 5 s -> 10 s -> 20 s ... maks 5 menit
#define SUPABASE_BACKOFF_MIN_MS  5000UL
#define SUPABASE_BACKOFF_MAX_MS  300000UL

// OTA (supabase/ota.sql): ESP32 membaca rilis aktif terbaru dari tabel firmware_releases dan
// memasang bila versinya BERBEDA dari FW_VERSION (jadi rilis lama = rollback).
// Naikkan FW_VERSION setiap membuat firmware yang akan dirilis (format a.b.c).
#define FW_VERSION               "1.0.5"
#define FW_PROJECT               "GroundTank" // = devices.project & firmware_releases.project
#define LAN_UPDATE_PORT          8080         // upload langsung dari PC lewat kabel LAN (lan_update.cpp)
#define OTA_FIRST_CHECK_MS      20000UL      // cek pertama setelah boot
#define OTA_CHECK_INTERVAL_MS    1800000UL    // lalu tiap 30 menit (sekaligus lapor versi ke devices)
#define SSL_ENTROPY_PIN         1        // pin ADC mengambang untuk data acak TLS
#define NTP_HOST                 "pool.ntp.org"

// WiFi cadangan bila modul LAN tidak tersambung. Bila keduanya ada di sekitar,
// ESP32 memilih yang sinyalnya terkuat. Ganti dengan WiFi yang sebenarnya.
#define WIFI_SSID_1              "SPRM-CORP"
#define WIFI_PASS_1              "e$kr1mN@N@s3000"
#define WIFI_SSID_2              "groundtank"
#define WIFI_PASS_2              "111222333"


// ---------------------------------------------------------------------------
// LoRa P2P (gateway <-> node HydroSense; flowchart GT-02/GT-03): modul EBYTE
// E220-900T30D (UART). Parameter radio HARUS sama dengan node (Config.h node).
// Pilih set pin dengan LORA_PIN_SET (kabel modul harus sesuai set yang dipilih):
//   Set 1: TXD modul -> GPIO15 (RX ESP), RXD modul -> GPIO14 (TX ESP), M0 -> 16, M1 -> 18, AUX -> 21
//   Set 2: TXD modul -> GPIO39 (RX ESP), RXD modul -> GPIO38 (TX ESP), M0 -> 41, M1 -> 42, AUX -> 40
//          (pin yang sama dengan node HydroSense, sudah terbukti jalan di sana; AUX 40 karena
//           GPIO10 dipakai CS W5500 di gateway).
// Pin gateway yang TERPAKAI di file ini: 1 (entropi TLS), 4 (float), 5/6/10-13 (W5500),
// 8/9 (LCD), 17 (ultrasonik). VCC modul 5 V terpisah (>= 1 A), GND bersama.
// ANTENA WAJIB TERPASANG sebelum modul dinyalakan.
// ---------------------------------------------------------------------------
#define LORA_ENABLED       1    // 0 = tanpa radio (build/uji); 1 = modul terpasang
#define LORA_PIN_SET       2    // 1 atau 2 (lihat di atas)
#if LORA_PIN_SET == 1
#define LORA_TX_PIN        14
#define LORA_RX_PIN        15
#define LORA_M0_PIN        16
#define LORA_M1_PIN        18
#define LORA_AUX_PIN       21
#else
#define LORA_TX_PIN        38
#define LORA_RX_PIN        39
#define LORA_M0_PIN        41
#define LORA_M1_PIN        42
#define LORA_AUX_PIN       40
#endif
#define LORA_CHANNEL       71    // frekuensi = 850.125 + kanal MHz -> 921.125 MHz
#define LORA_AIR_RATE_CODE 4     // 4 = 9,6 kbps
#define LORA_AIR_RATE_BPS  9600
#define LORA_POWER_CODE    3     // T30D: 0=30, 1=27, 2=24, 3=21 dBm
#define LORA_ADDR_SELF     0x0000 // alamat gateway
#define LORA_ADDR_NODE     0x0001 // alamat node
#define LORA_NODE_ID       1     // satu-satunya node yang dilayani (id di dalam paket)
#define LORA_ACK_RETRY     3     // GT-03: retry < 3
#define GW_CMD_TTL_S       120   // masa berlaku perintah ke node (detik)
#define GW_CMD_POLL_MS     3000UL // GT-05: polling perintah dashboard
#define GW_CMD_REFRESH_MS  60000UL // kirim ulang perintah manual aktif sebelum TTL habis

// Tingkat log TLS (SSLClient) di Serial Monitor: 0 mati, 1 error, 2 peringatan, 3 info.
// 3 membanjiri Serial Monitor tiap koneksi (handshake dicetak langkah demi langkah) dan
// menutupi pesan [LORA]/[NET]. Naikkan ke 3 hanya saat mendiagnosa masalah TLS.
#define GW_SSL_LOG_LEVEL   2

// Baris alat node di tabel devices (project 'hydrosense'): tempat gateway melaporkan status
// tautan LoRa (kolom lora_*, migrasi 0012) untuk indikator "LoRa" di dashboard.
#define HS_DEVICE_NAME     "hydrosense-inlet-01"
#define LORA_REPORT_MS     10000UL  // laporan status tautan + alat ke Supabase

#endif // CONFIG_H
