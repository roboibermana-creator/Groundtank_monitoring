#include <Arduino.h>
#include <Ethernet.h>
#include <WiFi.h>
#include <Update.h>
#include "config.h"
#include "secrets.h"
#include "network.h"
#include "lan_update.h"

namespace {

// Server::begin(uint16_t) di core ESP32 bersifat pure virtual, tapi library Ethernet
// hanya punya begin(): jembatani supaya kelasnya bisa dibuat.
class LanServer : public EthernetServer {
public:
    explicit LanServer(uint16_t port) : EthernetServer(port) {}
    void begin(uint16_t = 0) override { EthernetServer::begin(); }
};

LanServer ethServer(LAN_UPDATE_PORT);
WiFiServer wifiServer(LAN_UPDATE_PORT);
NetLink listeningOn = LINK_NONE;

void reply(Client& c, int code, const char* text) {
    c.print("HTTP/1.1 ");
    c.print(code);
    c.print(" ");
    c.print(text);
    c.print("\r\nContent-Type: text/plain\r\nConnection: close\r\nContent-Length: ");
    c.print(strlen(text));
    c.print("\r\n\r\n");
    c.print(text);
    c.flush();
    delay(50);
    c.stop();
}

bool readLine(Client& c, String& out) {
    out = "";
    unsigned long start = millis();
    while (millis() - start < 5000UL) {
        if (c.available()) {
            char ch = c.read();
            if (ch == '\n') return true;
            if (ch != '\r' && out.length() < 200) out += ch;
            start = millis();
        } else if (!c.connected()) {
            return false;
        } else {
            delay(1);
        }
    }
    return false;
}

// Banding waktu-konstan supaya password tidak bisa ditebak dari lama responsnya
bool sameSecret(const String& a, const char* b) {
    size_t n = strlen(b);
    uint8_t diff = (a.length() != n);
    for (size_t i = 0; i < n; i++) diff |= (i < a.length() ? a[i] : 0) ^ b[i];
    return diff == 0;
}

void handle(Client& c, OtaStartCb onStart, OtaProgressCb onProgress) {
    String line;
    if (!readLine(c, line)) { c.stop(); return; }
    bool isUpdate = line.startsWith("POST /update ");

    String auth;
    long total = -1;
    while (readLine(c, line) && line.length()) {
        String low = line;
        low.toLowerCase();
        if (low.startsWith("x-auth:")) { auth = line.substring(7); auth.trim(); }
        else if (low.startsWith("content-length:")) total = line.substring(15).toInt();
    }

    if (!isUpdate) { reply(c, 404, "tidak ada"); return; }
    if (!sameSecret(auth, LAN_UPDATE_PASSWORD)) {
        Serial.println("LAN update: password salah, ditolak");
        reply(c, 401, "password salah");
        return;
    }
    if (total < 100000 || total > 3000000) { reply(c, 400, "ukuran tidak wajar"); return; }
    if (!Update.begin(total)) {
        reply(c, 500, "tidak ada ruang update");
        return;
    }

    Serial.print("LAN update: menerima ");
    Serial.print(total);
    Serial.println(" byte");
    if (onStart) onStart("LAN");

    uint8_t buf[1024];
    long got = 0;
    int lastPct = -1;
    bool ok = true;
    unsigned long idle = millis();
    while (got < total && ok) {
        int avail = c.available();
        if (avail > 0) {
            size_t take = min((size_t)avail, min(sizeof(buf), (size_t)(total - got)));
            int n = c.read(buf, take);
            if (n > 0) {
                if (Update.write(buf, n) != (size_t)n) ok = false;
                got += n;
                idle = millis();
                int pct = (int)(got * 100L / total);
                if (pct != lastPct) { lastPct = pct; if (onProgress) onProgress(pct); }
            }
        } else if (!c.connected() || millis() - idle > 15000UL) {
            ok = false;
        } else {
            delay(1);
        }
    }

    if (ok && Update.end(true)) {
        Serial.println("LAN update: berhasil, restart");
        reply(c, 200, "ok, restart");
        delay(500);
        ESP.restart();
        return;
    }
    Serial.print("LAN update gagal: ");
    Serial.println(Update.errorString());
    Update.abort();
    reply(c, 500, "gagal");
}

} // namespace

bool lan_update_listening() { return listeningOn != LINK_NONE; }

void lan_update_loop(OtaStartCb onStart, OtaProgressCb onProgress) {
    NetLink link = network_link();
    if (link == LINK_NONE) { listeningOn = LINK_NONE; return; }
    if (link != listeningOn) {
        if (link == LINK_ETHERNET) ethServer.begin();
        else wifiServer.begin();
        listeningOn = link;
        Serial.print("Upload firmware siap di port ");
        Serial.print(LAN_UPDATE_PORT);
        Serial.println(link == LINK_ETHERNET ? " (LAN)" : " (WiFi)");
    }
    if (link == LINK_ETHERNET) {
        EthernetClient c = ethServer.available();
        if (c) handle(c, onStart, onProgress);
    } else {
        WiFiClient c = wifiServer.accept();
        if (c) handle(c, onStart, onProgress);
    }
}
