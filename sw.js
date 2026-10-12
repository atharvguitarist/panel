/* Panel control page — service worker.
   Purpose: (1) satisfy the browser's install criteria so Android can offer a
   one-tap "Add to home screen"; (2) keep the page shell openable offline.
   Strategy: network first, cache as fallback. The page therefore always
   updates when online and never serves a stale copy over a fresh one.
   Commands themselves are never cached or replayed. */
var CACHE = "panel-shell-v1";
var SHELL = ["./", "./index.html", "./icon-192.png", "./icon-512.png",
             "./icon-maskable-512.png", "./apple-touch-icon.png"];

self.addEventListener("install", function (e) {
  e.waitUntil(caches.open(CACHE).then(function (c) { return c.addAll(SHELL); }).then(function () { return self.skipWaiting(); }));
});

self.addEventListener("activate", function (e) {
  e.waitUntil(caches.keys().then(function (keys) {
    return Promise.all(keys.filter(function (k) { return k !== CACHE; }).map(function (k) { return caches.delete(k); }));
  }).then(function () { return self.clients.claim(); }));
});

self.addEventListener("fetch", function (e) {
  var req = e.request;
  if (req.method !== "GET" || new URL(req.url).origin !== self.location.origin) return; // ntfy posts pass straight through
  e.respondWith(
    fetch(req).then(function (res) {
      var copy = res.clone();
      caches.open(CACHE).then(function (c) { c.put(req, copy); });
      return res;
    }).catch(function () {
      return caches.match(req, { ignoreSearch: true }).then(function (hit) {
        return hit || caches.match("./index.html");
      });
    })
  );
});
