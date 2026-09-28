#include "network.h"
#include "config.h"
#include <SPI.h>
#include <Ethernet.h>

// MAC address untuk modul ethernet (harus unik dalam jaringan lokal)
byte mac[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED };

bool isNetworkConnected = false;

void network_init() {
    // Inisialisasi pin CS untuk W5500
    Ethernet.init(W5500_CS_PIN);
    
    Serial.println("Memulai Ethernet menggunakan DHCP...");
    
    // Mulai Ethernet dan minta IP via DHCP
    if (Ethernet.begin(mac) == 0) {
        Serial.println("Gagal mendapatkan IP dari DHCP");
        isNetworkConnected = false;
        
        // Cek apakah perangkat keras W5500 terdeteksi
        if (Ethernet.hardwareStatus() == EthernetNoHardware) {
            Serial.println("Modul W5500 tidak ditemukan. Cek kabel SPI!");
        } else if (Ethernet.linkStatus() == LinkOFF) {
            Serial.println("Kabel LAN Ethernet tidak terhubung.");
        }
    } else {
        isNetworkConnected = true;
        Serial.print("Ethernet berhasil terhubung. IP Address: ");
        Serial.println(Ethernet.localIP());
    }
}

void network_loop() {
    // Hanya periksa DHCP jika sebelumnya berhasil terhubung
    if (!isNetworkConnected) return;

    // Memelihara koneksi DHCP agar sewa IP tidak kadaluarsa
    switch (Ethernet.maintain()) {
        case 1: Serial.println("DHCP renew failed"); break;
        case 2: Serial.println("DHCP renew success"); break;
        case 3: Serial.println("DHCP rebind failed"); break;
        case 4: Serial.println("DHCP rebind success"); break;
        default: break; // Tidak ada aksi yang diperlukan
    }
}

