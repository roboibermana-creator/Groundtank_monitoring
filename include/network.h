#ifndef NETWORK_H
#define NETWORK_H

enum NetworkStatus {
    NET_CONNECTED,   // IP didapat dari DHCP
    NET_NO_MODULE,   // modul W5500 tidak terdeteksi (cek kabel SPI)
    NET_NO_CABLE,    // modul ada, kabel LAN lepas
    NET_DHCP_FAILED  // modul dan kabel ada, tapi gagal dapat IP
};

// Jalur internet yang sedang dipakai. LAN diutamakan, WiFi hanya cadangan.
enum NetLink {
    LINK_NONE,
    LINK_ETHERNET,
    LINK_WIFI
};

NetworkStatus network_init();   // status modul LAN (WiFi dicoba bila LAN gagal)
NetworkStatus network_status(); // status modul LAN terakhir
NetLink network_link();         // jalur yang aktif sekarang
void network_loop();

#endif // NETWORK_H
