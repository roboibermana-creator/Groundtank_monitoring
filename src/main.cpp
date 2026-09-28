#include <Arduino.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include "config.h"
#include "network.h"

// Alamat I2C LCD umumnya 0x27, namun beberapa modul menggunakan 0x3F
LiquidCrystal_I2C lcd(0x27, 16, 2);

// Menggunakan HardwareSerial 1 untuk Sensor Ultrasonik A02YYUW
HardwareSerial SensorSerial(1);

unsigned char data[4] = {};
float distance = 0; // Jarak sensor 1 (A02YYUW)
float distance2 = 0; // Jarak sensor 2 (HC-SR04)

void readUltrasonic() {
    // A02YYUW secara terus-menerus mengirim 4 byte data setiap ~50ms
    while (SensorSerial.available() > 0) {
        if (SensorSerial.read() == 0xFF) {
            // Header ditemukan, tunggu sisa data 3 byte berikutnya (dengan timeout)
            unsigned long timeout = millis();
            while (SensorSerial.available() < 3) {
                if (millis() - timeout > 50) return; // Timeout jika data tidak lengkap
            }
            
            data[0] = 0xFF;
            data[1] = SensorSerial.read(); // Data_H
            data[2] = SensorSerial.read(); // Data_L
            data[3] = SensorSerial.read(); // Checksum
            
            // Validasi checksum data
            int sum = (data[0] + data[1] + data[2]) & 0x00FF;
            if (sum == data[3]) {
                distance = (data[1] << 8) + data[2]; // Menghitung jarak dalam mm
                distance = distance / 10.0;          // Konversi dari mm ke cm
            }
        }
    }
}

void readUltrasonic2() {
    // Membaca sensor HC-SR04 / Trig-Echo
    digitalWrite(TRIG_PIN, LOW);
    delayMicroseconds(2);
    digitalWrite(TRIG_PIN, HIGH);
    delayMicroseconds(20);
    digitalWrite(TRIG_PIN, LOW);
    
    // Timeout 26000us (26ms) cukup untuk membaca jarak hingga ~4-5 meter
    long duration = pulseIn(ECHO_PIN, HIGH, 26000); 
    
    if (duration > 0) {
        distance2 = duration / 58.0; // Konversi durasi ke centimeter
    }
}

void setup() {
    // Inisialisasi Serial Monitor
    Serial.begin(115200);
    
    // Inisialisasi pin Float Switch dengan pull-up internal
    pinMode(FLOAT_SWITCH_PIN, INPUT_PULLUP);

    // Inisialisasi pin Ultrasonik Kedua (HC-SR04) dan Motor
    pinMode(TRIG_PIN, OUTPUT);
    pinMode(ECHO_PIN, INPUT_PULLUP); // Bisa menggunakan INPUT biasa tergantung sensor
    pinMode(MOTOR_PIN, OUTPUT);
    digitalWrite(MOTOR_PIN, LOW); // Matikan motor pada saat awal

    // Inisialisasi I2C untuk LCD menggunakan pin custom yang didefinisikan dari config.h
    Wire.begin(LCD_SDA_PIN, LCD_SCL_PIN);
    lcd.init();
    lcd.backlight();
    lcd.setCursor(0, 0);
    lcd.print("Sistem Tangki");
    lcd.setCursor(0, 1);
    lcd.print("Memulai...");
    delay(1500);
    lcd.clear();

    // Inisialisasi Jaringan (Ethernet W5500)
    lcd.setCursor(0, 0);
    lcd.print("Init Network...");
    network_init();
    lcd.clear();

    // Inisialisasi koneksi Serial untuk sensor ultrasonik A02YYUW
    SensorSerial.begin(ULTRASONIC_BAUD, SERIAL_8N1, ULTRASONIC_RX_PIN, ULTRASONIC_TX_PIN);
    Serial.println("Sistem Ground Tank Siap Berjalan");
}

void loop() {
    // Jalankan fungsi loop untuk memelihara koneksi jaringan DHCP
    network_loop();
    
    // 1. Baca data secara terus-menerus (Realtime) dari sensor ultrasonik pertama (A02YYUW)
    readUltrasonic();

    // 2. Baca data dari sensor ultrasonik kedua (HC-SR04)
    readUltrasonic2();

    // 3. Baca status float switch sebagai BACKUP
    // LOW biasanya berarti pelampung terangkat maksimal (menyentuh ujung atas)
    bool isFloatFull = (digitalRead(FLOAT_SWITCH_PIN) == LOW);

    // 4. Logika Penentuan Status & Motor
    String statusTinggi = "AMAN";
    if (isFloatFull) {
        statusTinggi = "PENUH (Float)";
        digitalWrite(MOTOR_PIN, LOW); // Matikan motor jika air penuh
    } else if ((distance > 0 && distance < 15.0) || (distance2 > 0 && distance2 < 15.0)) { 
        // Contoh: Jika salah satu ultrasonik membaca < 15 cm, anggap penuh
        statusTinggi = "PENUH (Ultra)";
        digitalWrite(MOTOR_PIN, LOW); // Matikan motor jika air penuh
    } else {
        statusTinggi = "AMAN";
        // Contoh: Jika air aman (belum penuh), hidupkan motor (bisa ditambah kondisi batas bawah)
        // digitalWrite(MOTOR_PIN, HIGH);
    }

    // 5. Update Tampilan LCD
    lcd.setCursor(0, 0);
    lcd.print("Jarak: ");
    if (distance > 0 || distance2 > 0) {
        // Tampilkan salah satu nilai ultrasonik, misal menggunakan ultrasonik pertama
        lcd.print(distance > 0 ? distance : distance2, 1);
        lcd.print(" cm    ");
    } else {
        lcd.print("Membaca... ");
    }

    lcd.setCursor(0, 1);
    lcd.print("Status: ");
    lcd.print(statusTinggi);
    lcd.print("    "); // Spasi untuk menimpa teks sebelumnya

    // 6. Placeholder Pengiriman Data ke Supabase / Notifikasi Telegram
    static unsigned long lastSendTime = 0;
    const unsigned long sendInterval = 5000; // Kirim data setiap 5 detik

    if (millis() - lastSendTime >= sendInterval) {
        lastSendTime = millis();
        
        // TODO: Eksekusi fungsi HTTP POST ke Supabase di sini
        
        Serial.print("Data Realtime: U1 = ");
        Serial.print(distance);
        Serial.print(" cm, U2 = ");
        Serial.print(distance2);
        Serial.print(" cm, Status = ");
        Serial.println(statusTinggi);
    }

    // Delay super kecil hanya untuk stabilitas mikrokontroler
    delay(50);
}
