#ifndef LAN_UPDATE_H
#define LAN_UPDATE_H

#include "ota.h"

// Terima firmware dari PC lewat jaringan (LAN W5500 atau WiFi): POST /update dengan header
// "X-Auth: <LAN_UPDATE_PASSWORD>" dan badan = file .bin. Panggil tiap loop().
// Bila pemasangan berhasil, ESP32 restart dan fungsi ini tidak kembali.
void lan_update_loop(OtaStartCb onStart = nullptr, OtaProgressCb onProgress = nullptr);

// true bila server upload sudah mendengarkan di jalur aktif
bool lan_update_listening();

#endif // LAN_UPDATE_H
