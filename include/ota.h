#ifndef OTA_H
#define OTA_H

typedef void (*OtaStartCb)(const char* version);
typedef void (*OtaProgressCb)(int percent);

// true bila sudah waktunya cek rilis + lapor status (dan jaringan tersedia)
bool ota_due();

// Lapor versi ke devices (report_firmware), cek rilis aktif, pasang bila versinya berbeda.
// Bila pemasangan berhasil, ESP32 restart dan fungsi ini tidak kembali.
void ota_run(OtaStartCb onStart = nullptr, OtaProgressCb onProgress = nullptr);

#endif // OTA_H
