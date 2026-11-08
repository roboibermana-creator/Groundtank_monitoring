#ifndef GW_LORA_H
#define GW_LORA_H

// Task_LoRa (GT-02 uplink dari node + GT-03 downlink perintah). Lihat gw_lora.cpp.
// Dengan LORA_ENABLED=0 task tidak dibuat dan perintah di Queue_Cmd dibuang (GAGAL).
void taskLoraMulai();
bool gwLoraSiap();
// Status tautan untuk indikator LoRa di dashboard (dilaporkan Task_Net ke Supabase):
//   status = "nonaktif" | "mati" | "siap"; rssi = RSSI (dBm) paket terakhir dari node;
//   umurDetik = detik sejak paket valid terakhir dari node (-1 bila belum pernah).
void gwLoraLaporan(const char** status, int* rssi, long* umurDetik);
// Teks status singkat untuk baris status Serial (alasan bila mati).
const char* gwLoraStatusTeks();

#endif // GW_LORA_H
