#ifndef GW_NET_H
#define GW_NET_H

// Antrean antar-task gateway (flowchart GT-01/GT-04). Aturan GT-04: HANYA Task_Net yang
// memakai TLS/HTTP/W5500; Task_Monitor dan Task_LoRa hanya menaruh/mengambil item antrean.
#include <Arduino.h>
#include <freertos/queue.h>
#include "LoraProto.h"
#include "ota.h"

enum NetItemTipe : uint8_t {
    NI_LEVEL,       // dari Task_Monitor: jarak mentah + float switch
    NI_NODE_DATA,   // dari Task_LoRa: DATA node -> tabel Hydrosense
    NI_NODE_EVENT,  // dari Task_LoRa: ALARM / EVENT node (dicatat; belum ada tabel tujuan)
    NI_CMD_STATUS,  // dari Task_LoRa: hasil pengiriman perintah (GT-03)
};

enum CmdStatus : uint8_t { CS_SELESAI, CS_GAGAL, CS_EXPIRED, CS_DITOLAK };

struct NetItem {
    uint8_t tipe;
    uint8_t nodeId;
    int16_t rssi;
    float snr;
    float level;          // NI_LEVEL: jarak mentah (cm), <= 0 = tidak segar
    bool full;            // NI_LEVEL: float switch penuh
    LoraDataPayload data; // NI_NODE_DATA
    uint8_t kode;         // NI_NODE_EVENT: tipe paket (ALARM/EVENT); NI_CMD_STATUS: CmdStatus
    uint16_t cmdId;
    uint8_t evKode;       // NI_NODE_EVENT: payload[0]
    bool adaStatus;       // NI_NODE_EVENT kode 3: payload[1] = status alat
    uint8_t statusAlat;   // bit0 pompa nyala, bit1 valve buka, bit2 float HIGH
    bool statusPenuh;     // true bila st terisi (ping status format baru, 18 byte)
    LoraStatusPayload st; // mode, fase, hitung mundur, TSS mentah, level, hujan
};

struct CmdItem {
    uint16_t cmdId;
    uint8_t aksi;   // 1 pompa, 2 valve, 3 wiper, 4 kembali otomatis
    uint8_t nilai;
    uint16_t ttlS;
    uint32_t kedaluwarsaMs; // batas millis() pengiriman (GT-03: TTL masih berlaku?)
    uint8_t jenis;          // 0 = CMD (aksi/nilai), 1 = CFG (jadwal siklus otomatis), 2 = NOTIFY
    uint8_t notifJenis;     // jenis == 2: LoraNotifJenis
    uint32_t notifId;       // jenis == 2: id baris perintah / faktor
    LoraJadwalPayload jadwal; // dipakai bila jenis == 1
};

extern QueueHandle_t queueNet; // Task_Monitor/Task_LoRa -> Task_Net
extern QueueHandle_t queueCmd; // Task_Net -> Task_LoRa

extern volatile bool gwOtaBerjalan; // true saat OTA: Task_Monitor tidak menulis LCD

void gwQueueBuat();
// Jembatan HTTP untuk node tanpa WiFi (Task_LoRa <-> Task_Net), lihat gw_net.cpp.
bool gwProxyOcupado();
bool gwProxyTerimaPermintaan(const uint8_t* m, size_t n);
bool gwProxyAdaRespons(uint8_t* out, size_t* n); // out >= LORA_PROXY_MAKS_RES + 2
void gwProxyResponsTerkirim();

void gwNetKirim(const NetItem& item); // tidak memblok; item dibuang bila antrean penuh
void taskNetMulai(OtaStartCb onStart, OtaProgressCb onProgress);

#endif // GW_NET_H
