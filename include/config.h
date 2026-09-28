#ifndef CONFIG_H
#define CONFIG_H

// Konfigurasi Pin W5500 Ethernet Module
#define W5500_CS_PIN    5
#define W5500_SCK_PIN   18
#define W5500_MISO_PIN  19
#define W5500_MOSI_PIN  23

// Konfigurasi Pin LCD 16x2 I2C
#define LCD_SDA_PIN     16
#define LCD_SCL_PIN     17

// Konfigurasi Pin A02YYUW Ultrasonic Sensor (UART)
// Sensor menggunakan Serial1, ESP32 RX=22, TX=21
#define ULTRASONIC_RX_PIN 22 // Hubungkan ke pin TX Sensor (Kabel Hijau)
#define ULTRASONIC_TX_PIN 21 // Hubungkan ke pin RX Sensor (Kabel Orange)
#define ULTRASONIC_BAUD   9600

// Konfigurasi Float Switch
#define FLOAT_SWITCH_PIN 27

// Konfigurasi Ultrasonik Kedua (HC-SR04/JSN-SR04T) & Motor
#define TRIG_PIN 14
#define ECHO_PIN 13
#define MOTOR_PIN 4

#endif // CONFIG_H
