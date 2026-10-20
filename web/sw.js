// Service worker minimal: syarat install PWA + penampil notifikasi di Android.
// Data realtime tetap selalu diambil dari jaringan (tidak di-cache).
self.addEventListener("install", function () { self.skipWaiting(); });
self.addEventListener("activate", function (e) { e.waitUntil(self.clients.claim()); });
self.addEventListener("fetch", function () {});
self.addEventListener("notificationclick", function (e) {
  e.notification.close();
  e.waitUntil(self.clients.matchAll({ type: "window" }).then(function (list) {
    if (list.length) return list[0].focus();
    return self.clients.openWindow("./");
  }));
});
