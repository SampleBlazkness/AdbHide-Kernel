# Changelog

## 1.1.2
- `blacklist.conf` is the source of truth: package names are resolved to uids at boot and on demand.
- Writable `/proc/adbhide_uids` applies a new list without reloading the module.
- Netfilter: adbd port reset for target uids, mDNS `_adb` records suppressed.
- Panel shows scope, submodule state, autor and repository.

## 1.0.0
- Property-area redirect (kernel + Zygisk), `/proc/net/tcp{,6}` filtering, Settings spoof.
