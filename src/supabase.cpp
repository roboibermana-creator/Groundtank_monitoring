#include <Arduino.h>
#include <Ethernet.h>
#include <EthernetUdp.h>
#include <SSLClient.h>
#include "config.h"
#include "secrets.h"
#include "certificates.h"
#include "supabase.h"
#include "network.h"

#include <WiFi.h>
#include <WiFiUdp.h>

// Satu klien TLS per jalur: LAN (W5500) dan WiFi
static EthernetClient ethBase;
static WiFiClient wifiBase;
static SSLClient sslEth(ethBase, TAs, (size_t)TAs_NUM, SSL_ENTROPY_PIN, 1, (SSLClient::DebugLevel)GW_SSL_LOG_LEVEL);
static SSLClient sslWifi(wifiBase, TAs, (size_t)TAs_NUM, SSL_ENTROPY_PIN, 1, (SSLClient::DebugLevel)GW_SSL_LOG_LEVEL);

// Waktu NTP terakhir (unix) dan millis() saat didapat; dipakai untuk verifikasi
// sertifikat di jalur mana pun yang aktif
static bool timeSynced = false;
static uint32_t ntpUnix = 0;
static unsigned long ntpAtMillis = 0;

static SSLClient* activeSsl() {
    switch (network_link()) {
        case LINK_ETHERNET: return &sslEth;
        case LINK_WIFI:     return &sslWifi;
        default:            return nullptr;
    }
}

static bool ntpQuery(UDP& udp) {
    if (!udp.begin(8123)) return false;

    byte packet[48] = {0};
    packet[0] = 0b11100011; // LI, versi 4, mode client
    if (!udp.beginPacket(NTP_HOST, 123)) { udp.stop(); return false; }
    udp.write(packet, 48);
    udp.endPacket();

    unsigned long start = millis();
    while (udp.parsePacket() < 48) {
        if (millis() - start > 3000) { udp.stop(); return false; }
        delay(10);
    }
    udp.read(packet, 48);
    udp.stop();

    uint32_t ntp = ((uint32_t)packet[40] << 24) | ((uint32_t)packet[41] << 16) |
                   ((uint32_t)packet[42] << 8) | packet[43]; // detik sejak 1900
    uint32_t unixTime = ntp - 2208988800UL;
    if (unixTime < 1700000000UL) return false; // jawaban tidak masuk akal

    ntpUnix = unixTime;
    ntpAtMillis = millis();
    return true;
}

// Waktu NTP -> waktu verifikasi sertifikat BearSSL (hari sejak 0000-01-01 + detik)
bool supabase_sync_time() {
    bool ok = false;
    switch (network_link()) {
        case LINK_ETHERNET: { EthernetUDP udp; ok = ntpQuery(udp); break; }
        case LINK_WIFI:     { WiFiUDP udp;     ok = ntpQuery(udp); break; }
        default: return false;
    }
    if (!ok) return false;
    timeSynced = true;
    Serial.print("Waktu NTP (unix): ");
    Serial.println(ntpUnix);
    return true;
}

Client* supabase_tls_connect(const char* host, uint16_t port) {
    SSLClient* ssl = activeSsl();
    if (!ssl) return nullptr;
    if (!timeSynced && !supabase_sync_time()) return nullptr;
    uint32_t now = ntpUnix + (millis() - ntpAtMillis) / 1000UL;
    ssl->setVerificationTime(now / 86400UL + 719528UL, now % 86400UL);
    ssl->setTimeout(8000);
    if (!ssl->connect(host, port)) {
        timeSynced = false; // coba sinkron ulang waktu pada percobaan berikutnya
        return nullptr;
    }
    return ssl;
}

// IP lokal pada jalur yang aktif, untuk p_ip (ditampilkan di aplikasi)
static void localIp(char* out, size_t len) {
    IPAddress ip = network_link() == LINK_WIFI ? WiFi.localIP() : Ethernet.localIP();
    snprintf(out, len, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
}

int supabase_send(float distanceCm) {
    char value[16];
    snprintf(value, sizeof(value), "%.1f", distanceCm);
    return supabase_send(SENSOR_UUID, value);
}

int supabase_send_float(bool full) {
#ifdef FLOAT_SENSOR_UUID
    return supabase_send(FLOAT_SENSOR_UUID, full ? "1" : "0");
#else
    (void)full;
    return 204; // sensor float belum didaftarkan: lewati tanpa memicu backoff
#endif
}

int supabase_send(const char* sensorId, const char* value) {
    SSLClient* ssl = activeSsl();
    if (!ssl) return 0;
    if (!timeSynced && !supabase_sync_time()) {
        Serial.println("Supabase: waktu belum tersinkron, kirim dibatalkan");
        return 0;
    }
    SSLClient& sslClient = *ssl;
    uint32_t now = ntpUnix + (millis() - ntpAtMillis) / 1000UL;
    sslClient.setVerificationTime(now / 86400UL + 719528UL, now % 86400UL);

    // Kontrak submit_reading: nilai mentah jarak (cm) sebagai string satu desimal,
    // waktu diisi server. Token tidak pernah dicetak ke Serial.
    char ip[16];
    localIp(ip, sizeof(ip));
    char body[320];
    snprintf(body, sizeof(body),
             "{\"p_device_id\":\"%s\",\"p_sensor_id\":\"%s\",\"p_value\":\"%s\","
             "\"p_token\":\"%s\",\"p_ip\":\"%s\"}",
             DEVICE_UUID, sensorId, value, DEVICE_TOKEN, ip);

    sslClient.setTimeout(10000);
    if (!sslClient.connect(SUPABASE_HOST, 443)) {
        Serial.print("Supabase: gagal konek TLS via ");
        Serial.print(network_link() == LINK_WIFI ? "WiFi" : "LAN");
        Serial.print(", error SSL=");
        Serial.print(sslClient.getWriteError());
        Serial.print(", waktu unix=");
        Serial.println(now);
        timeSynced = false; // coba sinkron ulang waktu pada percobaan berikutnya
        return 0;
    }

    sslClient.print("POST /rest/v1/rpc/submit_reading HTTP/1.1\r\n");
    sslClient.print("Host: " SUPABASE_HOST "\r\n");
    sslClient.print("apikey: " SUPABASE_API_KEY "\r\n");
    // Authorization hanya untuk key JWT lama (eyJ...); publishable key akan ditolak
    if (strncmp(SUPABASE_API_KEY, "eyJ", 3) == 0) {
        sslClient.print("Authorization: Bearer " SUPABASE_API_KEY "\r\n");
    }
    sslClient.print("Content-Type: application/json\r\n");
    sslClient.print("Connection: close\r\n");
    sslClient.print("Content-Length: ");
    sslClient.print(strlen(body));
    sslClient.print("\r\n\r\n");
    sslClient.print(body);

    // Baca respons: baris status, header (dilewati), lalu sedikit body untuk pesan error
    String statusLine, line, resp;
    int part = 0; // 0 = baris status, 1 = header, 2 = body
    unsigned long start = millis();
    while (millis() - start < 10000 && resp.length() < 160) {
        if (sslClient.available()) {
            char c = sslClient.read();
            if (part == 2) { resp += c; continue; }
            if (c == '\r') continue;
            if (c != '\n') { line += c; continue; }
            if (part == 0) { statusLine = line; part = 1; }
            else if (line.length() == 0) part = 2;
            line = "";
        } else if (!sslClient.connected()) {
            break;
        } else {
            delay(5);
        }
    }
    sslClient.stop();
    statusLine.trim();

    int code = 0;
    int sp = statusLine.indexOf(' ');
    if (sp > 0) code = statusLine.substring(sp + 1).toInt();

    Serial.print("Supabase: ");
    Serial.print(statusLine.length() ? statusLine : String("tidak ada respons"));
    if (code != 204 && resp.length()) {
        Serial.print(" -> ");
        Serial.print(resp);
    }
    Serial.println();
    return code;
}

int supabase_rest(const char* method, const char* path, const char* body,
                  char* resp, size_t respLen) {
    if (resp && respLen) resp[0] = 0;
    Client* c = supabase_tls_connect(SUPABASE_HOST, 443);
    if (!c) return 0;

    size_t bodyLen = body ? strlen(body) : 0;
    c->print(method); c->print(" "); c->print(path); c->print(" HTTP/1.1\r\n");
    c->print("Host: " SUPABASE_HOST "\r\n");
    c->print("apikey: " SUPABASE_API_KEY "\r\n");
    if (strncmp(SUPABASE_API_KEY, "eyJ", 3) == 0) {
        c->print("Authorization: Bearer " SUPABASE_API_KEY "\r\n");
    }
    c->print("Content-Type: application/json\r\n");
    c->print("Prefer: return=minimal\r\n");
    c->print("Connection: close\r\n");
    if (body) { c->print("Content-Length: "); c->print(bodyLen); c->print("\r\n"); }
    c->print("\r\n");
    if (body) c->print(body);

    // Baris status, header (dilewati), lalu badan
    String line;
    int part = 0, code = 0; // 0 = status, 1 = header, 2 = badan
    size_t n = 0;
    unsigned long start = millis();
    while (millis() - start < 10000) {
        if (c->available()) {
            char ch = c->read();
            if (part == 2) {
                if (resp && n + 1 < respLen) { resp[n++] = ch; resp[n] = 0; }
                continue;
            }
            if (ch == '\r') continue;
            if (ch != '\n') { line += ch; continue; }
            if (part == 0) {
                int sp = line.indexOf(' ');
                if (sp > 0) code = line.substring(sp + 1).toInt();
                part = 1;
            } else if (line.length() == 0) {
                part = 2;
            }
            line = "";
        } else if (!c->connected()) {
            break;
        } else {
            delay(5);
        }
    }
    c->stop();
    return code;
}
