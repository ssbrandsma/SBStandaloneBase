# GitHub applet catalog refresh

Introduced in Standalone Base 0.5.0.

Standalone Base now merges public release metadata from:

```text
https://raw.githubusercontent.com/ssbrandsma/SBStandaloneBase/master/config.json
```

The active local file remains `/mnt/storage/standalonebase/config.json`.
Publishing a newer applet entry on GitHub can therefore make that release
available through the Radio's **local Applet Installer**, including updates
to Standalone Base itself. It does not install updates automatically, change
the selected LMS, or restart Jive.

## Data flow

```text
GitHub master/config.json
        |
        | HTTPS using the existing sbproxy TLS policy
        v
127.0.0.1:8765/https/raw.githubusercontent.com/.../master/config.json
        |
        | asynchronous Mongoose client in sbwebserver
        v
validate release entries -> lock/read current local catalog -> merge
        |
        | atomic rename, only if something was added/upgraded
        v
/mnt/storage/standalonebase/config.json
        |
        | SIGHUP using the existing repository notification path
        v
sbbase reload -> jiveapplets -> local Applet Installer update listing
```

`catalog_sync.c` manages network requests and schedules; `repository.c` owns
validation, locking and the transactional merge. No new daemon, TLS library,
permanent thread, Lua downloader or image-renderer dependency is introduced.

## Schedule and manual check

- First check: ten seconds after the webserver starts, allowing sbproxy to start.
- Successful check: repeat after six hours of monotonic time.
- Failure: retry after 1, 5, 15, then at most every 60 minutes.
- Manual check: **Applets → GitHub release catalog → Check for updates**.
- Only one refresh runs at once; other webserver requests remain responsive
  while the proxy/network request is pending.

The management UI makes bounded one-second status checks only while the user
is waiting for an explicit refresh. It stops after completion, page removal,
authentication failure, or thirty attempts. There is no continuous browser
polling for catalog updates.

Read-only management mode prevents automatic and manual writes, including
when read-only mode becomes active during a download. Administrators can set
`SB_CATALOG_SYNC_DISABLED=1` in the service environment to disable synchronization.
Disabling refresh does not prevent the existing local catalog being served.

## Merge rules

Identity is `(name, target)`. All changes are validated before committing:

1. Add upstream entries absent from the local catalog, including StandaloneBase.
2. Replace release metadata for an older, external local entry only when the
   upstream dotted-numeric version is newer (`0.9.10` is newer than `0.9.9`).
3. Preserve equal and newer local releases. Equal-version changes to a URL or
   checksum do not silently replace an immutable release.
4. Preserve custom/prerelease version strings when there is no unambiguous
   numeric upgrade comparison.
5. Preserve uploaded ZIP entries with the repository's loopback package URL.
6. Preserve explicitly pinned entries with `"catalog_pin": true`.
7. Preserve custom entries not present upstream; upstream removal is not local
   deletion.
8. Preserve unknown local per-applet fields when upgrading release metadata.
9. Preserve every local top-level field outside the applets array. Remote
   `server`, local identity, ports, settings and `standalone_base_version` are
   **not** imported. Installed/runtime version is not changed by advertising
   a newer downloadable Base version.

Recognized upstream applet fields are name, title, version, target,
min_target_version, url, sha, desc, changes, creator and email. Unknown remote
entry fields, duplicate identities/known fields, malformed entries, truncated
strings, embedded NULs, credential-bearing URLs and invalid checksums are
rejected rather than imported partially. Pins are an advanced local catalog
setting, not a new web-editor field; existing repository edit/upload operations
have their own serialization rules.

The merge uses the same `.catalog.lock` as repository management operations.
It reads the current file **after acquiring the lock**, so a completed local
edit is not replaced using a stale pre-download snapshot. External writers
should follow this locking convention too.

## Failure behavior and bounds

- Fetch only the fixed GitHub URL, through loopback sbproxy. A successful
  `/health` capability check must advertise `X-SB-Public-Fetch: 1` first.
- Requests opt into sbproxy's existing public-destination/redirect restrictions.
  No user-provided catalog URL or alternate TLS transport is added.
- At most 16 KiB of remote JSON, 32 total entries, a 15,000-byte serialized
  applet array and a 65,535-byte merged local file. These bounds fit sbbase's
  existing catalog/installer response limits.
- Each asynchronous proxy request has a ten-second deadline; the proxy retains
  its existing upstream timeout and redirect limits.
- A failed HTTP request, capability check, parse, capacity check or atomic
  file operation retains the last working local catalog. There is no empty
  fallback or partial import.
- Changes are written to a temporary sibling, file-synced and atomically
  renamed; the existing mode is retained and directory sync is attempted.
- Identical/equal-version refreshes do not replace the file or write it to
  NAND. There is no persistent refresh-history database or timestamp file.
- The existing native catalog service is notified only after an actual change.

The catalog is a trusted release channel, not a new signed update system.
HTTPS follows the existing proxy compatibility policy; SHA-1 is the existing
Applet Installer integrity format, not proof of authenticity by itself.
ZIP download URLs retain their published transport; adding catalog refresh
does not turn an HTTP ZIP URL into encrypted transport.

## Management API

Both routes require the existing paired browser session:

| Route | Behavior |
|---|---|
| `GET /api/catalog/status` | State, generic message, busy flag, last merge count, last successful wall-clock timestamp. No network side effect. |
| `POST /api/catalog/refresh` | Queue a check; existing write authorization, Origin/CSRF and read-only checks apply. Returns 202, or 409 if already busy. |

Status is held in RAM, not flash. Wall-clock timestamps are informational;
scheduling and deadlines use monotonic milliseconds. Logout/session expiry
does not cancel an already authorized background check, but prevents further
browser status/refresh access.

## Rollout

This feature needs a newly built/deployed native webserver and matching web
assets; changing GitHub alone cannot teach an older installation to refresh.
No released 0.4.4 ZIP has been repacked or had its checksum changed for this
feature. Once the new implementation runs, allow the startup check or use
the manual button, then reopen the Radio's Applet Installer to refresh its
listing. Normal installer version comparisons decide whether an update is
shown. The standard installer handles installation/reboot; the synchronizer
never installs or reboots on its own.

The remote bootstrap catalog remains Base-only. The local catalog contains
Base and the other applets, so an installed Radio need not switch its selected
LMS back to the bootstrap server merely to discover a Base update.

## Validation

Automated native tests cover:

- Add/upgrade, numeric ordering, no downgrade, equality/no-write, uploaded ZIP
  and explicit pins, custom entries/fields, and local settings preservation.
- Malformed, duplicate, oversized and capacity-exhausted input; all-or-nothing
  failure; file permissions, concurrent merges and catalog reload notification.
- Fake-proxy health guard and fixed upstream request, startup checking, manual
  checking, unchanged refreshes, upstream failures and request timeout.
- Unauthenticated and cross-origin refresh rejection, single-flight behavior,
  read-only/disabled operation and a read-only transition during download.
- HTTP responsiveness while a catalog request is held open.

The same merge and HTTP tests run on host and under ARMv5 QEMU. The merger is
also tested with AddressSanitizer/UndefinedBehaviorSanitizer. Full ARMv5 builds
use the existing SDK without additional target libraries.

A workstation end-to-end smoke check also used production ARM binaries under
QEMU: the real GitHub catalog was fetched through ARM sbproxy, merged by the
ARM webserver into a fixture containing an older Radio release and no Base
entry, then read successfully by ARM sbbase. All four releases were present,
and the local identity and installed-version fields were unchanged. This did
not modify or restart the physical Radio.

Physical-device self-updating and GUI interaction still require a deployment
and a supervised device test. Do not infer those results from QEMU tests.
