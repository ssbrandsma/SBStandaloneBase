# Configuration backup and restore

## Browser workflow

Pair the browser normally, open **Settings → Backup & Restore**, and select
**Download configuration backup**. No SSH is needed. Keep the JSON file private:
station URLs and Radio preferences can contain service identifiers or private
URL parameters. Backups are not encrypted or signed.

To restore, choose a backup and select **Check backup**. This validates the full
archive without changing live configuration. Review the listed sections, then
select **Restore and restart SqueezePlay** and confirm the interruption. Playback
stops; SqueezePlay restarts, not Linux. The browser checks the operation's result
rather than interpreting HTTP acceptance as successful restoration. Review and
explicitly enable restored alarms afterwards; their global switch is forced off.

Read-only mode permits download but blocks validation/staging and restore.
Listener port, enabled/autostart state, read-only policy and pairing credentials
are retained from the destination, so restoring cannot silently remove the
current management restrictions or change its authentication domain.

## Included sections

| Logical section | Configuration restored |
|---|---|
| `settings` | Display name and artwork fallback preferences; listener/security policy preserved |
| `alarms` | Configured alarms and options, with global enable and active/snooze state cleared |
| `stations` | Standalone Radio custom station definitions; revision advanced to reject stale editors |
| `radioPreferences` | Standalone Radio applet preferences, including presets and favorites |

Only existing sections are included. Missing sections in a backup do not delete
destination configuration. Storage paths are selected by the existing modules:
Base configuration beside its active catalog, stations on active extended storage
or the normal internal fallback, and Radio preferences in Jive's settings directory.
The feature works without initializing extended storage.

This is **not a system image**. It excludes installed binaries/applets, applet
repository/catalog lists, firmware, firmware-update authorization, Wi-Fi settings,
selected external LMS, storage-layout/boot configuration, browser sessions,
internal capabilities, legacy management credentials, caches and uploaded custom
station-logo files. Reinstall missing applets and re-upload custom logos separately.
It does not copy Spotify/Qobuz account credentials or their applet settings.

## Implementation and safety

`http_server.c` applies normal browser-session protection. `backup.c` adds origin,
content-type, read-only and request-size checks. No authentication bypass or
additional permanent process is introduced.

Endpoints:

- `GET /api/backup`: attachment containing a version-1 JSON archive.
- `POST /api/backup/validate`: complete JSON backup; validate without applying.
- `POST /api/backup/restore`: complete JSON backup; queue the confirmed restore.
- `GET /api/backup/status`: busy/completion state, without backup contents.

The envelope has exactly `format:"StandaloneBaseBackup"`, `version:1` and
`files`. Each allowlisted entry has base64 `data` and its SHA-256. Checksums detect
accidental damage, not a deliberately modified backup. Unknown/duplicate structural
keys, client-supplied paths, incompatible versions, bad checksums and invalid
component data are rejected. There is no TAR/ZIP extraction or archive traversal.

Limits: 768 KiB HTTP body, four entries, 128 KiB decoded per file. Oversized or
lengthless/chunked uploads are rejected at headers, before buffering the body.
Station data uses the existing station-schema/URL validator. Alarms use the
existing bounded alarm parser and are normalized to disabled, inactive state.
Radio's Lua settings file must match a strict serialized-data grammar: only
`settings = { ... }`, scalar data and bounded nested tables. Functions, calls,
expressions, comments/extra statements, invalid table keys and executable code
are rejected. Native code never evaluates the uploaded Lua.

Validated data is staged in a private `restore.*` directory beside Base's
configuration. Restore holds an exclusive lock; other management configuration
writes are blocked until completion. A double-forked, short-lived invocation of
the existing sbwebserver executable runs its local-only `--apply-backup` helper.
It uses the firmware's SqueezePlay stop/start script directly, without shell
interpolation. Stopping Jive before replacing files prevents cached preferences
from overwriting the restored files during shutdown.

All candidate files are prepared before committing. Same-directory temporary
files and atomic renames replace each target; ordinary commit failure rolls
earlier replacements back. Symlink targets, existing recovery files and unsafe
target types fail closed. Files are mode 0600. Only the applet's verified sbbase
PID is stopped before Jive starts it again with restored alarm configuration.
The webserver/proxy and existing browser sessions remain running.

There is **no all-files atomicity guarantee across sudden power loss**, and no
claim of protection against failing flash. Keep power connected and retain the
downloaded backup. An interrupted transaction can leave `.restore-old` and
`.restore-new` recovery files and a stale lock; it deliberately does not guess
which state should be discarded. Space is needed for the staged archive,
candidate files and old configuration. Insufficient space fails without formatting
or initializing any storage.

When alarms are restored, the destination's derived AlarmSnooze RTC cache is
cleared in the same replacement/rollback sequence. It is not imported from the
archive. This prevents an old cached occurrence sounding before the first
disabled server status reaches the restarted Jive applet. An incomplete rollback
retains the restore lock and reports failure for deliberate local recovery.

## Recovery and diagnostics

The browser reports accepted, busy, succeeded or failed/unknown completion.
Inspect `/tmp/standalonebase/backup-restore.log` for helper failures and the
configuration root's `backup-result.json` for the last result. A busy state that
survives a reboot needs local inspection of `.backup-restore.lock` and the
referenced private staging directory. Do not blindly clear the lock, delete
`.restore-old` files, or initialize extended storage: they may be the only old
copy after an interrupted restore. Stop SqueezePlay, preserve both generations,
inspect which fixed targets were committed and recover deliberately from the
saved archive/old copies before restarting. Local shell access is a last-resort
recovery path, not required for ordinary backups/restores.

## Tests and build

The normal CMake/CTest suite includes `configuration_backup`. Tests use temporary
host directories and paired-browser sessions: round-trip export, checksums,
strict validation, credential isolation, CSRF, oversized headers, read-only
preservation, successful file application, staging failure and a fault-injected
last-file rename failure with verification of complete rollback.

Optional browser tests: `python tests/backup/test_backup_ui.py <repo-root>` with
Playwright/Chromium installed; `SB_TEST_BROWSER_CHANNEL=msedge` uses installed
Edge. APIs are mocked; no Radio is contacted. They exercise validation, download,
confirmation, completion polling and read-only controls.

Build using the existing CMake or `scripts/build-arm.sh` workflow. Both package
builders include `backup.js`. There is no TLS dependency added to the webserver;
existing native alarm parsing and Mongoose hashing/base64 are reused. Idle CPU
and flash activity are unchanged; bounded reads/staging occur only on user action.

Physical acceptance is pending. On a recoverable test Radio, first download and
retain a real backup, verify its four sections and validate without restoring.
With explicit approval and safe playback/alarm state, test restore, preset and
station preservation, alarms remaining off, unchanged browser authentication,
and success/error reporting. Repeat on internal-only storage. Host tests do not
prove watchdog/startup behavior or flash-failure recovery on every firmware.

Validation recorded 2026-10-26: the configured host suite passed all 55 tests
(`BUILD_SBPROXY=OFF`), including late-commit fault injection and RTC-cache
clearing. The optional browser tests passed in headless Edge using mocked APIs.
Native sleep tests also passed under QEMU ARM926. These new features have not
been deployed to the physical Radio or used to restore its settings.
The final ARMv5 build passed, retaining only the existing image-render/NanoSVG
warnings. No release ZIP was published and no deployment was performed.
