#include <Arduino.h>
#include "network.h"
#include "config.h"
#include <SPI.h>
#include <Ethernet.h>
#include <WiFi.h>
#include <WiFiMulti.h>

// MAC address untuk modul ethernet (harus unik dalam jaringan lokal)
byte mac[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED };

bool isNetworkConnected = false; // DHCP LAN pernah berhasil
static NetworkStatus netStatus = NET_NO_MODULE;
static NetLink activeLink = LINK_NONE;

static WiFiMulti wifiMulti;
static bool wifiStarted = false;

NetworkStatus network_status() { return netStatus; }
NetLink network_link() { return activeLink; }

static bool ethernetUp() {
    return isNetworkConnected && Ethernet.linkStatus() != LinkOFF;
}

static void wifiStart() {
    if (wifiStarted) return;
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    wifiMulti.addAP(WIFI_SSID_1, WIFI_PASS_1);
    wifiMulti.addAP(WIFI_SSID_2, WIFI_PASS_2);
    wifiStarted = true;
}

static void wifiStop() {
    if (!wifiStarted) return;
    WiFi.disconnect(true, false);
    WiFi.mode(WIFI_OFF);
    wifiStarted = false;
}

// Menyambung ke WiFi terkuat dari daftar. wifiMulti.run() memblok beberapa
// detik bila sedang mencari, jadi dipanggil terbatas (lihat network_loop).
static bool wifiConnect(uint32_t timeoutMs) {
    wifiStart();
    return wifiMulti.run(timeoutMs) == WL_CONNECTED;
}

NetworkStatus network_init() {
    // Reset hardware W5500
    pinMode(W5500_RST_PIN, OUTPUT);
    digitalWrite(W5500_RST_PIN, LOW);
    delay(10);
    digitalWrite(W5500_RST_PIN, HIGH);
    delay(200);

    // SPI memakai pin custom sesuai wiring
    SPI.begin(W5500_SCK_PIN, W5500_MISO_PIN, W5500_MOSI_PIN, W5500_CS_PIN);
    Ethernet.init(W5500_CS_PIN);

    Serial.println("Memulai Ethernet menggunakan DHCP...");

    // Mulai Ethernet dan minta IP via DHCP
    if (Ethernet.begin(mac, 10000, 4000) == 0) {
        Serial.println("Gagal mendapatkan IP dari DHCP");
        isNetworkConnected = false;

        // Cek apakah perangkat keras W5500 terdeteksi
        if (Ethernet.hardwareStatus() == EthernetNoHardware) {
            Serial.println("Modul W5500 tidak ditemukan. Cek kabel SPI!");
            netStatus = NET_NO_MODULE;
        } else if (Ethernet.linkStatus() == LinkOFF) {
            Serial.println("Kabel LAN Ethernet tidak terhubung.");
            netStatus = NET_NO_CABLE;
        } else {
            netStatus = NET_DHCP_FAILED;
        }

        // LAN gagal -> cadangan WiFi
        Serial.println("LAN tidak tersedia, mencoba WiFi...");
        if (wifiConnect(10000)) {
            activeLink = LINK_WIFI;
            Serial.print("WiFi terhubung: ");
            Serial.print(WiFi.SSID());
            Serial.print("  IP: ");
            Serial.println(WiFi.localIP());
        } else {
            Serial.println("WiFi juga gagal, akan dicoba lagi di background");
            activeLink = LINK_NONE;
        }
    } else {
        isNetworkConnected = true;
        netStatus = NET_CONNECTED;
        activeLink = LINK_ETHERNET;
        Serial.print("Ethernet berhasil terhubung. IP Address: ");
        Serial.println(Ethernet.localIP());
    }
    return netStatus;
}

void network_loop() {
    static unsigned long lastEthTry = 0;
    static unsigned long lastWifiTry = 0;

    // Kabel baru dicolok / modul baru siap setelah boot: coba DHCP lagi
    if (!isNetworkConnected && netStatus != NET_NO_MODULE &&
        Ethernet.linkStatus() == LinkON && millis() - lastEthTry >= 15000UL) {
        lastEthTry = millis();
        if (Ethernet.begin(mac, 4000, 2000) != 0) {
            isNetworkConnected = true;
            netStatus = NET_CONNECTED;
            Serial.print("Ethernet pulih. IP: ");
            Serial.println(Ethernet.localIP());
        }
    }

    if (ethernetUp()) {
        // Memelihara sewa DHCP agar tidak kadaluarsa
        switch (Ethernet.maintain()) {
            case 1: Serial.println("DHCP renew failed"); break;
            case 2: Serial.println("DHCP renew success"); break;
            case 3: Serial.println("DHCP rebind failed"); break;
            case 4: Serial.println("DHCP rebind success"); break;
            default: break;
        }
        if (activeLink != LINK_ETHERNET) {
            Serial.println("Kembali memakai LAN");
            activeLink = LINK_ETHERNET;
        }
        wifiStop(); // LAN sudah ada, WiFi dimatikan
        return;
    }

    // LAN tidak ada -> pakai WiFi. Bila terputus, dicoba ulang tiap 10 detik.
    if (WiFi.status() == WL_CONNECTED && wifiStarted) {
        if (activeLink != LINK_WIFI) {
            Serial.print("Pindah ke WiFi: ");
            Serial.println(WiFi.SSID());
            activeLink = LINK_WIFI;
        }
        return;
    }
    activeLink = LINK_NONE;
    if (millis() - lastWifiTry >= 10000UL) {
        lastWifiTry = millis();
        if (wifiConnect(5000)) {
            activeLink = LINK_WIFI;
            Serial.print("WiFi terhubung: ");
            Serial.println(WiFi.SSID());
        }
    }
}
