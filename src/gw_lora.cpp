// Task_LoRa gateway (GT-02 + GT-03) dengan modul E220-900T30D lewat UART (LoraE220.h).
// Radio hanya dipakai task ini, jadi tanpa mutex.
#include <Arduino.h>
#include "config.h"
#include "gw_lora.h"
#include "gw_net.h"

#if LORA_ENABLED
#include "LoraE220.h"

static LoraE220 radio;
static HardwareSerial loraSerial(2); // UART2 (UART1 = sensor ultrasonik)
static bool siap = false;
static volatile uint32_t paketNodeTerakhirMs = 0; // 0 = belum pernah
static volatile int16_t rssiNodeTerakhir = 0;

bool gwLoraSiap() { return siap; }
void gwLoraLaporan(const char** status, int* rssi, long* umurDetik) {
    *status = siap ? "siap" : "mati";
    *rssi = rssiNodeTerakhir;
    *umurDetik = paketNodeTerakhirMs ? (long)((millis() - paketNodeTerakhirMs) / 1000UL) : -1;
}

const char* gwLoraStatusTeks() {
    static char buf[200];
    if (!siap) return "MATI (modul E220 tidak merespons / konfigurasi gagal; lihat baris [E220] saat boot, atau reset)";
    snprintf(buf, sizeof(buf), "siap | byte masuk=%u paket valid=%u rusak=%u",
             (unsigned)radio.rxByte(), (unsigned)radio.rxValid(), (unsigned)radio.rxRusak());
    return buf;
}

static bool radioMulai() {
    E220Config c = {};
    c.addr = LORA_ADDR_SELF;
    c.channel = LORA_CHANNEL;
    c.airRateCode = LORA_AIR_RATE_CODE;
    c.powerCode = LORA_POWER_CODE;
    c.fixedPoint = true;
    c.rssiByte = true;
    return radio.begin(loraSerial, LORA_RX_PIN, LORA_TX_PIN, LORA_M0_PIN, LORA_M1_PIN,
                       LORA_AUX_PIN, c, LORA_AIR_RATE_BPS);
}

static bool radioKirim(const LoraPaket& p) { return radio.kirim(LORA_ADDR_NODE, LORA_CHANNEL, p); }
static bool radioBaca(LoraPaket& p, int16_t* rssi) { return radio.baca(p, rssi); }

// ---------------- GT-02: uplink dari node ----------------
static uint32_t galatPaket = 0;

// Jembatan HTTP: fragmen permintaan dari node disusun di sini, lalu diserahkan ke Task_Net.
static uint8_t reqBuf[LORA_PROXY_MAKS_REQ];
static size_t reqLen = 0;
static uint8_t reqTx = 0;
static int reqNext = 0, reqTotal = 0;
static uint8_t resTx = 0;  // txId permintaan yang sedang dilayani (disalin ke tiap fragmen respons)
static uint16_t seqRes = 0;

static void prosesUplink() {
    LoraPaket p;
    int16_t rssi = 0;
    if (!radioBaca(p, &rssi)) return;
    // CRC sudah dicek di loraDecode; di sini node_id.
    if (p.nodeId != LORA_NODE_ID) { galatPaket++; return; } // "Buang paket; hitung error"
    paketNodeTerakhirMs = millis() ? millis() : 1; // paket valid dari node ini = tautan hidup
    rssiNodeTerakhir = rssi;

    if (p.tipe == LORA_DATA || p.tipe == LORA_ALARM) {
        static int32_t seqTerakhir = -1;
        bool baru = (int32_t)p.seq != seqTerakhir; // "Seq baru?"
        if (baru && p.len == sizeof(LoraDataPayload)) {
            seqTerakhir = p.seq;
            NetItem it = {};
            it.tipe = p.tipe == LORA_ALARM ? NI_NODE_EVENT : NI_NODE_DATA;
            it.kode = p.tipe;
            it.nodeId = p.nodeId;
            it.rssi = rssi;
            memcpy(&it.data, p.payload, sizeof(it.data));
            if (p.tipe == LORA_ALARM) it.evKode = it.data.alarm;
            gwNetKirim(it);
        }
        radioKirim(loraBuatAck(LORA_ACK, p.nodeId, p.seq)); // ACK selalu, juga untuk duplikat
    } else if (p.tipe == LORA_HTTP_REQ && p.len >= LORA_FRAG_HDR) {
        uint8_t tx = p.payload[0], idx = p.payload[1], total = p.payload[2];
        if (idx == 0) {
            if (gwProxyOcupado()) return; // sibuk: tidak di-ACK, node mengulang lalu menyerah cepat
            reqTx = tx;
            reqNext = 0;
            reqLen = 0;
            reqTotal = total;
        }
        radioKirim(loraBuatAck(LORA_ACK, p.nodeId, p.seq)); // fragmen basi pun di-ACK agar node berhenti mengulang
        if (tx != reqTx || idx != reqNext) return; // basi / duplikat
        size_t dl = p.len - LORA_FRAG_HDR;
        if (reqLen + dl > sizeof(reqBuf)) { reqNext = -1; return; }
        memcpy(reqBuf + reqLen, p.payload + LORA_FRAG_HDR, dl);
        reqLen += dl;
        reqNext++;
        if (reqNext == reqTotal && gwProxyTerimaPermintaan(reqBuf, reqLen)) resTx = reqTx;
    } else if (p.tipe == LORA_TEXT) {
        // Uji komunikasi: cetak pesan apa adanya, lalu ACK.
        char teks[LORA_MAX_PAYLOAD + 1];
        memcpy(teks, p.payload, p.len);
        teks[p.len] = 0;
        Serial.printf("[LORA] PESAN dari node %u (RSSI %d dBm): %s\n", p.nodeId, rssi, teks);
        radioKirim(loraBuatAck(LORA_ACK, p.nodeId, p.seq));
    } else if (p.tipe == LORA_EVENT) {
        NetItem it = {};
        it.tipe = NI_NODE_EVENT;
        it.kode = LORA_EVENT;
        it.nodeId = p.nodeId;
        it.rssi = rssi;
        it.evKode = p.len ? p.payload[0] : 0;
        if (it.evKode == 3 && p.len >= 2) { // ping tautan + status alat
            it.adaStatus = true;
            it.statusAlat = p.payload[1];
            if (p.len >= sizeof(LoraStatusPayload)) { // format baru: mode, fase, level, hujan, dst.
                memcpy(&it.st, p.payload, sizeof(LoraStatusPayload));
                it.statusPenuh = true;
            }
        }
        gwNetKirim(it);
        radioKirim(loraBuatAck(LORA_ACK, p.nodeId, p.seq));
    }
    // ACK/NAK nyasar di luar jendela tunggu downlink: diabaikan
}

// ---------------- GT-03: downlink perintah ----------------
static void laporStatus(const CmdItem& c, CmdStatus s) {
    NetItem it = {};
    it.tipe = NI_CMD_STATUS;
    it.cmdId = c.cmdId;
    it.kode = s;
    gwNetKirim(it);
}

static void kirimDownlink(const CmdItem& c) {
    if ((int32_t)(millis() - c.kedaluwarsaMs) >= 0) { // TTL masih berlaku?
        laporStatus(c, CS_EXPIRED);
        return;
    }
    LoraCmdPayload pl = {};
    pl.cmd_id = c.cmdId;
    pl.aksi = c.aksi;
    pl.nilai = c.nilai;
    // TTL yang dikirim = sisa waktu, supaya node tidak menjalankan lebih lama dari batas.
    uint32_t sisaS = (c.kedaluwarsaMs - millis()) / 1000UL;
    pl.ttl_s = (uint16_t)min<uint32_t>(sisaS, c.ttlS);
    LoraPaket p = {};
    p.nodeId = LORA_NODE_ID;
    p.seq = c.cmdId;
    if (c.jenis == 1) { // CFG: jadwal siklus otomatis
        p.tipe = LORA_CFG;
        p.len = sizeof(c.jadwal);
        memcpy(p.payload, &c.jadwal, sizeof(c.jadwal));
    } else if (c.jenis == 2) { // NOTIFY: ada perintah baru di dashboard
        LoraNotifPayload nt = {};
        nt.jenis = c.notifJenis;
        nt.id = c.notifId;
        p.tipe = LORA_NOTIFY;
        p.len = sizeof(nt);
        memcpy(p.payload, &nt, sizeof(nt));
    } else {
        p.tipe = LORA_CMD;
        p.len = sizeof(pl);
        memcpy(p.payload, &pl, sizeof(pl));
    }
    // ToA kirim + ToA ACK + waktu eksekusi di node
    unsigned long tunggu = 2UL * radio.perkiraanToaMs(LORA_HDR_LEN + p.len + 2) + 700UL + 400UL;

    for (int retry = 0; retry < LORA_ACK_RETRY; retry++) {
        if ((int32_t)(millis() - c.kedaluwarsaMs) >= 0) break; // "retry < 3 & TTL berlaku?"
        radioKirim(p);
        unsigned long batas = millis() + tunggu;
        while ((long)(millis() - batas) < 0) {
            LoraPaket r;
            if (radioBaca(r, nullptr)) {
                bool cocok = r.nodeId == LORA_NODE_ID && r.len == 2 &&
                             (uint16_t)(r.payload[0] | (r.payload[1] << 8)) == c.cmdId;
                if (cocok && r.tipe == LORA_ACK) { laporStatus(c, CS_SELESAI); return; }
                if (cocok && r.tipe == LORA_NAK) { laporStatus(c, CS_DITOLAK); return; }
            }
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
    laporStatus(c, CS_GAGAL);
}

// Kirim satu paket lalu tunggu ACK dengan seq yang sama (retry LORA_ACK_RETRY kali).
static bool kirimDenganAck(const LoraPaket& p) {
    unsigned long tunggu = 2UL * radio.perkiraanToaMs(LORA_HDR_LEN + p.len + 2) + 700UL + 300UL;
    for (int retry = 0; retry < LORA_ACK_RETRY; retry++) {
        radioKirim(p);
        unsigned long batas = millis() + tunggu;
        while ((long)(millis() - batas) < 0) {
            LoraPaket r;
            if (radioBaca(r, nullptr) && r.nodeId == LORA_NODE_ID && r.tipe == LORA_ACK && r.len == 2 &&
                (uint16_t)(r.payload[0] | (r.payload[1] << 8)) == p.seq)
                return true;
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
    return false;
}

// Respons jembatan HTTP ([kode u16 LE][badan]) dipecah menjadi fragmen, masing-masing ber-ACK.
static void kirimRespons(const uint8_t* data, size_t n) {
    int total = (int)((n + LORA_FRAG_DATA - 1) / LORA_FRAG_DATA);
    for (int i = 0; i < total; i++) {
        size_t off = (size_t)i * LORA_FRAG_DATA;
        size_t c = n - off < LORA_FRAG_DATA ? n - off : LORA_FRAG_DATA;
        LoraPaket p = {};
        p.tipe = LORA_HTTP_RES;
        p.nodeId = LORA_NODE_ID;
        if (++seqRes == 0) seqRes = 1;
        p.seq = seqRes;
        p.payload[0] = resTx;
        p.payload[1] = (uint8_t)i;
        p.payload[2] = (uint8_t)total;
        memcpy(p.payload + LORA_FRAG_HDR, data + off, c);
        p.len = (uint8_t)(LORA_FRAG_HDR + c);
        if (!kirimDenganAck(p)) {
            Serial.printf("[PROXY] respons gagal terkirim ke node (fragmen %d/%d)\n", i + 1, total);
            return;
        }
    }
}

static void taskLora(void*) {
    static uint8_t res[LORA_PROXY_MAKS_RES + 2];
    uint32_t cobaBerikut = 0;
    for (;;) {
        if (!siap) { // modul gagal saat boot: coba lagi tiap 30 detik sampai pulih
            if ((int32_t)(millis() - cobaBerikut) >= 0) {
                cobaBerikut = millis() + 30000UL;
                loraSerial.end();
                siap = radioMulai();
                Serial.printf("[LORA] percobaan ulang: %s\n", siap ? "siap" : "TIDAK siap");
            }
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        prosesUplink();
        size_t rn = 0;
        if (gwProxyAdaRespons(res, &rn)) { // jawaban Supabase untuk permintaan node
            kirimRespons(res, rn);
            gwProxyResponsTerkirim();
        }
        CmdItem c;
        if (xQueueReceive(queueCmd, &c, 0) == pdTRUE) kirimDownlink(c);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void taskLoraMulai() {
    siap = radioMulai();
    Serial.printf("[LORA] %s (E220, %.3f MHz, node %d)\n", siap ? "siap" : "TIDAK siap",
                  850.125 + LORA_CHANNEL, LORA_NODE_ID);
    xTaskCreatePinnedToCore(taskLora, "Task_LoRa", 8192, nullptr, 3, nullptr, 1);
}

#else // LORA_ENABLED == 0

bool gwLoraSiap() { return false; }
void gwLoraLaporan(const char** status, int* rssi, long* umurDetik) {
    *status = "nonaktif";
    *rssi = 0;
    *umurDetik = -1;
}
const char* gwLoraStatusTeks() { return "mati (LORA_ENABLED=0 di config.h)"; }

// Tanpa radio: perintah dari dashboard tetap dikuras supaya Queue_Cmd tidak penuh.
static void taskLoraTiruan(void*) {
    for (;;) {
        CmdItem c;
        if (xQueueReceive(queueCmd, &c, pdMS_TO_TICKS(500)) == pdTRUE) {
            NetItem it = {};
            it.tipe = NI_CMD_STATUS;
            it.cmdId = c.cmdId;
            it.kode = CS_GAGAL;
            gwNetKirim(it);
        }
    }
}

void taskLoraMulai() {
    Serial.println("[LORA] nonaktif (LORA_ENABLED=0): perintah dashboard dibuang");
    xTaskCreatePinnedToCore(taskLoraTiruan, "Task_LoRa", 4096, nullptr, 1, nullptr, 1);
}

#endif
