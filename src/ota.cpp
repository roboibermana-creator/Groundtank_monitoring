#include <Arduino.h>
#include <Client.h>
#include <Ethernet.h>
#include <WiFi.h>
#include <Update.h>
#include <mbedtls/sha256.h>
#include "config.h"
#include "secrets.h"
#include "network.h"
#include "supabase.h"
#include "ota.h"

namespace {

// --- HTTP/1.1 minimal di atas klien TLS (mendukung Content-Length dan chunked) ---

struct Http {
    Client* c = nullptr;
    bool chunked = false;
    bool needCrlf = false;  // chunked: CRLF penutup chunk sebelumnya belum dibaca
    long remaining = -1;    // sisa byte badan/chunk; -1 = sampai koneksi ditutup
    bool finished = false;
};

void httpClose(Http& h) {
    if (h.c) { h.c->stop(); h.c = nullptr; }
}

bool readLine(Client* c, String& out) {
    out = "";
    unsigned long start = millis();
    while (millis() - start < 10000UL) {
        if (c->available()) {
            char ch = c->read();
            if (ch == '\n') return true;
            if (ch != '\r') out += ch;
            start = millis();
        } else if (!c->connected()) {
            return out.length() > 0;
        } else {
            delay(2);
        }
    }
    return false;
}

// Kirim GET dan baca header. Return kode HTTP, atau <0 bila gagal.
int httpGet(Http& h, const char* host, const char* path, bool withApiKey) {
    h = Http();
    Client* c = supabase_tls_connect(host);
    if (!c) return -1;
    h.c = c;

    c->print("GET "); c->print(path); c->print(" HTTP/1.1\r\nHost: "); c->print(host); c->print("\r\n");
    if (withApiKey) c->print("apikey: " SUPABASE_API_KEY "\r\nAccept: application/json\r\n");
    c->print("Connection: close\r\n\r\n");

    String line;
    if (!readLine(c, line)) { httpClose(h); return -2; }
    int status = line.substring(line.indexOf(' ') + 1).toInt();
    while (readLine(c, line) && line.length()) {
        line.toLowerCase();
        if (line.startsWith("content-length:")) h.remaining = line.substring(15).toInt();
        else if (line.startsWith("transfer-encoding:") && line.indexOf("chunked") >= 0) h.chunked = true;
    }
    if (h.chunked) h.remaining = 0; // belum ada chunk yang dibaca
    return status;
}

// Baca sepotong badan. Return jumlah byte, 0 = selesai, <0 = gagal/terputus.
int bodyRead(Http& h, uint8_t* buf, size_t n) {
    if (h.finished) return 0;
    Client* c = h.c;
    String line;

    if (h.chunked && h.remaining == 0) {
        if (h.needCrlf) {
            if (!readLine(c, line)) return -1;
            h.needCrlf = false;
        }
        if (!readLine(c, line)) return -1;
        long size = strtol(line.c_str(), nullptr, 16);
        if (size <= 0) { h.finished = true; return 0; }
        h.remaining = size;
    }
    if (!h.chunked && h.remaining == 0) { h.finished = true; return 0; }

    size_t want = n;
    if (h.remaining > 0 && (long)want > h.remaining) want = (size_t)h.remaining;

    unsigned long start = millis();
    while (millis() - start < 15000UL) {
        int avail = c->available();
        if (avail > 0) {
            size_t take = (size_t)avail < want ? (size_t)avail : want;
            int got = c->read(buf, take);
            if (got > 0) {
                if (h.remaining > 0) {
                    h.remaining -= got;
                    if (h.chunked && h.remaining == 0) h.needCrlf = true;
                }
                return got;
            }
        } else if (!c->connected()) {
            h.finished = true;
            return h.remaining > 0 ? -1 : 0;
        }
        delay(2);
    }
    return -1;
}

bool bodyToString(Http& h, String& out, size_t maxLen) {
    uint8_t buf[256];
    int n;
    out = "";
    while ((n = bodyRead(h, buf, sizeof(buf))) > 0) {
        if (out.length() + n > maxLen) return false;
        out.concat((const char*)buf, n);
    }
    return n == 0;
}

// --- JSON sederhana (cukup untuk satu baris hasil PostgREST) ---

String jsonStr(const String& j, const char* key) {
    String k = String("\"") + key + "\":\"";
    int i = j.indexOf(k);
    if (i < 0) return "";
    i += k.length();
    int e = j.indexOf('"', i);
    return e < 0 ? String("") : j.substring(i, e);
}

long jsonNum(const String& j, const char* key) {
    String k = String("\"") + key + "\":";
    int i = j.indexOf(k);
    return i < 0 ? 0 : j.substring(i + k.length()).toInt();
}

// --- Lapor versi firmware & hasil OTA ke devices (RPC report_firmware, token perangkat) ---

bool heartbeat(String otaStatus) {
    otaStatus.replace("\"", " ");
    otaStatus.replace("\\", " ");
    String body = String("{\"p_device_id\":\"" DEVICE_UUID "\",\"p_token\":\"" DEVICE_TOKEN
                         "\",\"p_fw_version\":\"" FW_VERSION "\",\"p_ota_status\":\"") + otaStatus + "\"}";

    Client* c = supabase_tls_connect(SUPABASE_HOST);
    if (!c) return false;
    c->print("POST /rest/v1/rpc/report_firmware HTTP/1.1\r\n");
    c->print("Host: " SUPABASE_HOST "\r\n");
    c->print("apikey: " SUPABASE_API_KEY "\r\n");
    c->print("Content-Type: application/json\r\n");
    c->print("Connection: close\r\n");
    c->print("Content-Length: ");
    c->print(body.length());
    c->print("\r\n\r\n");
    c->print(body);

    String line;
    bool got = readLine(c, line);
    c->stop();
    bool ok = got && line.indexOf(" 204") >= 0;
    Serial.print("OTA: lapor status -> ");
    Serial.println(got ? line : String("tidak ada respons"));
    return ok;
}

// --- Unduh + tulis ke partisi OTA. Return "" bila berhasil, selain itu pesan galat. ---

String downloadAndFlash(const String& url, const String& sha256, long sizeHint,
                        const String& version, OtaStartCb onStart, OtaProgressCb onProgress) {
    if (!url.startsWith("https://")) return "url bukan https";
    int slash = url.indexOf('/', 8);
    if (slash < 0) return "url tidak valid";
    String host = url.substring(8, slash);
    String path = url.substring(slash);

    Http h;
    int status = httpGet(h, host.c_str(), path.c_str(), false);
    if (status != 200) { httpClose(h); return "unduh http " + String(status); }

    long total = (!h.chunked && h.remaining > 0) ? h.remaining : sizeHint;
    if (total <= 0) { httpClose(h); return "ukuran tidak diketahui"; }
    if (!Update.begin(total)) { httpClose(h); return String("begin: ") + Update.errorString(); }

    if (onStart) onStart(version.c_str());
    Serial.print("OTA: memasang v"); Serial.print(version);
    Serial.print(" ("); Serial.print(total); Serial.println(" byte)");

    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);

    static uint8_t buf[1024];
    long done = 0;
    int lastPct = -1, n;
    String err = "";
    while ((n = bodyRead(h, buf, sizeof(buf))) > 0) {
        if (done + n > total) { err = "data melebihi ukuran"; break; }
        if (Update.write(buf, n) != (size_t)n) { err = String("tulis: ") + Update.errorString(); break; }
        mbedtls_sha256_update(&ctx, buf, n);
        done += n;
        int pct = (int)(done * 100 / total);
        if (pct != lastPct) { lastPct = pct; if (onProgress) onProgress(pct); }
    }
    httpClose(h);

    if (err.length() == 0 && n < 0) err = "unduhan terputus";
    if (err.length() == 0 && done != total) err = "ukuran tidak cocok";

    unsigned char digest[32];
    mbedtls_sha256_finish(&ctx, digest);
    mbedtls_sha256_free(&ctx);

    if (err.length() == 0) {
        char hex[65];
        for (int i = 0; i < 32; i++) sprintf(hex + i * 2, "%02x", digest[i]);
        if (!sha256.equalsIgnoreCase(hex)) err = "sha256 tidak cocok";
    }
    if (err.length() == 0 && !Update.end(true)) err = String("end: ") + Update.errorString();

    if (err.length() > 0) Update.abort();
    return err;
}

unsigned long nextCheckAt = OTA_FIRST_CHECK_MS;

} // namespace

bool ota_due() {
    return network_link() != LINK_NONE && (long)(millis() - nextCheckAt) >= 0;
}

void ota_run(OtaStartCb onStart, OtaProgressCb onProgress) {
    nextCheckAt = millis() + OTA_CHECK_INTERVAL_MS;
    String status = "ok";

    Http h;
    int code = httpGet(h, SUPABASE_HOST,
                       "/rest/v1/firmware_releases?project=eq." FW_PROJECT
                       "&is_active=eq.true&order=created_at.desc&limit=1"
                       "&select=version,url,sha256,size_bytes", true);
    String json;
    bool fetched = (code == 200) && bodyToString(h, json, 1024);
    httpClose(h);

    if (!fetched) {
        status = code > 0 ? "cek rilis gagal http " + String(code) : String("cek rilis gagal (koneksi)");
        Serial.println("OTA: " + status);
    } else {
        String version = jsonStr(json, "version");
        String url = jsonStr(json, "url");
        String sha = jsonStr(json, "sha256");
        if (version.length() && version != FW_VERSION) {
            Serial.println("OTA: rilis v" + version + " berbeda dari v" FW_VERSION);
            String err = downloadAndFlash(url, sha, jsonNum(json, "size_bytes"), version, onStart, onProgress);
            if (err.length() == 0) {
                Serial.println("OTA: berhasil, restart");
                delay(500);
                ESP.restart();
            }
            status = "gagal v" + version + ": " + err;
            Serial.println("OTA: " + status);
        } else {
            Serial.println("OTA: firmware sudah terbaru (v" FW_VERSION ")");
        }
    }

    // Laporkan hasilnya; kalau jaringan bermasalah, coba lagi sebentar lagi
    if (!heartbeat(status)) nextCheckAt = millis() + 60000UL;
}
