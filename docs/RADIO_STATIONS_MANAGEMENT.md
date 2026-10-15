# Standalone Radio station management

## Architecture and availability

Station management is implemented by the existing native `sbwebserver`; it does not add a process and does not modify the Standalone Radio Jive applet. The navigation entry and every `/api/radio` endpoint use the same cached applet discovery result. Discovery checks the active Jive applet tree plus known extended-storage trees and distinguishes installed, absent, and unavailable states. `SB_APPLET_ROOTS` is a semicolon/colon-separated host-test override. The cache expires after five seconds, so installation and removal are picked up without configuration edits.

All management APIs require a valid PIN-paired `sb_session` browser cookie. Writes additionally enforce same-origin and read-only-mode checks. Session tokens are HttpOnly and are never exposed to frontend JavaScript.

## Storage and schema

The persistent layout is:

```text
/mnt/sbdata/config/radio/stations.json
/mnt/sbdata/config/radio/logos/
```

No fallback is used when SBUBIFS extended storage is unavailable. Reads return an empty schema when the file does not exist and do not create it. `SB_RADIO_CONFIG_ROOT` overrides the root for host tests only.

Schema version 1 contains `schemaVersion`, monotonically increasing `revision`, and up to 250 station objects. Station fields are `id`, `name`, `description`, `streamUrl`, `logoUrl`, `logoFile`, `codec`, `enabled`, and `order`. IDs are stable restricted identifiers generated at creation. URLs must use HTTP or HTTPS. Codec hints are `auto`, `mp3`, `aac`, `ogg`, or `flac`. Names, descriptions, and URLs use the documented 256/1024/8192/4096 byte limits; the complete document is limited to 256 KiB and must be valid UTF-8. Orders and IDs must be unique.

When extended storage is not active, the same files are stored in `/mnt/storage/standalonebase/radio`. This small internal-storage fallback keeps station management writable on the factory 8 MB volume; extended storage remains preferred when mounted.

On a fresh installation the `config/radio` directory may not exist yet. Storage availability is determined from the writable mounted parent in that case, and the directory hierarchy is created by the first station write.

When both artwork fields are set, `logoFile` takes precedence. Uploaded files are server-named and stored separately; `/api/radio/logos/<filename>` is the stable authenticated web URL. A future applet can resolve the file directly below the documented logos directory. PNG and JPEG are preferred for Jive compatibility; WebP is accepted for browser/future use but may not render in older Jive builds.

## API

| Method | Endpoint | Purpose |
|---|---|---|
| GET/POST | `/api/radio/stations` | Read configuration or add a station |
| PUT/DELETE | `/api/radio/stations/:id` | Update or remove a station |
| POST | `/api/radio/stations/reorder` | Submit the complete ordered ID list |
| POST | `/api/radio/logos` | Upload multipart field `logo` (maximum 1 MiB) |
| GET/DELETE | `/api/radio/logos/:filename` | Read or remove a safe server filename |
| GET | `/api/radio/export?format=json|csv` | Export configuration |
| POST | `/api/radio/import` | Import `{expectedRevision, mode, config}`; mode is `replace` or `merge` |
| POST | `/api/radio/stream-test` | Returns 501; intentionally disabled |

Every mutation includes `expectedRevision`. A stale value returns HTTP 409 and does not write or increment the revision. Merge import matches by stable ID, replaces matching entries, and retains unrelated entries. JSON/CSV exports reference logo filenames but do not contain logo bytes. The browser parses CSV into the same validated JSON import path and shows a count/mode preview before committing.

The stream diagnostic is disabled because the current server has no safe asynchronous DNS/HTTP layer capable of applying redirect-time private-address checks without blocking Mongoose. This avoids creating an SSRF proxy or destabilizing the event loop.

## Durability and security

The complete proposed configuration is validated before persistence. Writes are serialized with a lock, written to a same-directory mode-0600 temporary file, flushed and `fsync`ed, renamed atomically, and followed by a directory sync. A failure leaves the old file intact. Logo publication uses the same temporary-file/flush/rename pattern. Uploaded content is checked by magic bytes; PNG dimensions are capped at 4096×4096, names are generated from random bytes, and path components are strictly validated. Requests are bounded and no stream URL is fetched when saved.

The management table supports add/edit/delete, enable/disable, move up/down, artwork upload, import, and export. Deletion never automatically removes artwork because another station may reference it. Empty configuration is a normal state.

## Future applet integration

The future Standalone Radio applet should read `stations.json` when its station menu opens, retain the last `revision`, and rebuild its menu only when the revision changes. No watcher or background daemon is needed. Applet playback integration is deliberately outside this implementation.

## Build and deployment

Build `sbwebserver` through the normal ARM package build so `radio_stations.c` is linked. Host builds can use the environment overrides above. Package creation includes the existing web assets automatically. No deployment or Radio restart is performed by this change.
