# Web applet repository management

## Architecture and installer compatibility

The web UI manages the existing `applets` array in
`/mnt/storage/standalonebase/config.json`. There is no second catalog and no
installation database. `sbbase` exposes that array unchanged through the LMS
`jiveapplets` Comet response used by Logitech's Applet Installer. A successful
catalog transaction sends `SIGHUP` to the supervised `sbbase`; its event loop
reloads the file and retains the previous in-memory catalog if validation
fails. SqueezePlay, SlimProto, CometD and playback are not restarted.

Catalog presence is not installation state. Upload, edit and catalog removal
never extract an applet into `/usr/share/jive/applets` and never modify the
Applet Installer's records. Installation, update and uninstall remain stock
Applet Installer operations.

The first web mutation creates `.catalog-local` beside `config.json`. The Lua
supervisor then stops replacing the locally managed catalog with the upstream
GitHub fallback at startup. This marker is ownership state, not a duplicate
catalog.

## Storage and upload flow

Local ZIPs are accepted only when `/mnt/sbdata` is mounted as `sbubifs`, or
when `SB_REPOSITORY_ROOT` is explicitly set for host tests. The layout is:

```text
/mnt/sbdata/applet-repository/
  packages/<Name>-<Version>.zip
  tmp/upload-*
```

The browser sends one `multipart/form-data` part named `package`; metadata is
URL-encoded in the request query. The HTTP protocol handler detaches after the
headers and drains received bytes to a same-filesystem temporary file while
retaining only a small boundary window. The complete ZIP is never held in
RAM. Disconnects and errors close and unlink the temporary file.

Limits are 16 MiB compressed, 512 entries, 64 MiB total uncompressed content,
and a maximum compression ratio of 200:1 per entry. Before an upload starts,
the server requires at least the upload limit plus 1 MiB to be available on
extended storage. The original small overlay is never an upload fallback.

Validation parses the EOCD, central directory and local-header bounds without
extracting or executing Lua. It rejects encryption, unsupported compression,
absolute/traversal paths, special files, malformed bounds, excessive counts,
expansion limits, and packages without both `*Meta.lua` and `*Applet.lua`.
The detected internal name must match the confirmed form value.

SHA-1 is calculated over the exact ZIP bytes solely for Logitech protocol
compatibility. Local URL and SHA-1 fields are derived and cannot be changed by
metadata edits. Packages are served unchanged at:

```text
http://127.0.0.1:80/applets/packages/<Name>-<Version>.zip
```

Loopback is intentional: the Applet Installer downloads on the Radio itself.
GET and HEAD are unauthenticated for compatibility. IDs are allowlisted and
cannot contain path separators or traversal components.

## Atomic catalog updates and recovery

Updates take an exclusive `.catalog.lock`, reload the current file under the
lock, replace only the `applets` array, write a same-directory temporary file,
flush it, rename it atomically, and flush the directory. Unknown top-level and
server fields are retained byte-for-byte. Existing local ZIPs remain in place
until a replacement ZIP has passed validation and its catalog transaction has
committed. An interrupted upload leaves the prior catalog/package valid; stale
`tmp/upload-*` files may be removed while `sbwebserver` is stopped.

Removing a catalog entry does not uninstall its applet. The UI may also delete
the referenced local ZIP after the catalog commit. Externally hosted packages
are never downloaded or deleted. Previous unreferenced versions are retained
for manual rollback unless explicitly removed.

## Management security

Read APIs and package downloads are public on the existing web interface.
PUT, DELETE and upload require `X-Management-Token` (or a Bearer token), reject
foreign `Origin` values, reject state-changing GETs, and honor read-only mode.
On first start, 32 random bytes are saved with mode 0600 at:

```text
/mnt/storage/standalonebase/management.token
```

The browser keeps the entered token in `sessionStorage`. It is not placed in a
URL or logged. `SBWEBSERVER_TOKEN` supplies a deterministic token for tests.
The port-80 management service is still a LAN service; protect the LAN and do
not forward it from a router.

## Management API

| Method | Route | Purpose |
|---|---|---|
| GET | `/api/applets` | Catalog entries and storage capability |
| POST | `/api/applets/upload?...` | Stream, validate and register one ZIP |
| PUT | `/api/applets/<name>` | Edit catalog metadata |
| DELETE | `/api/applets/<name>?delete_package=1` | Remove catalog entry, optionally its local ZIP |
| GET/HEAD | `/applets/packages/<id>.zip` | Installer-compatible package download |

## Physical validation procedure (not yet executed)

Do not perform this procedure without separate approval to deploy/restart the
Radio. It does not modify UBI geometry, but it writes to initialized `sbdata`.

1. Verify Extended Storage reports Active, then install the candidate package.
2. On a PC open `http://RADIO-IP/`, select **Applets**, and confirm existing
   Standalone Radio and Spotify entries remain external entries.
3. Obtain the token over SSH:

   ```sh
   cat /mnt/storage/standalonebase/management.token
   ```

4. Upload a known newer Standalone Radio test ZIP and confirm its metadata,
   local source badge and computed SHA-1. This makes it available; it does not
   install it.
5. On the Radio open the original Applet Installer, refresh/reopen it, confirm
   the version appears, then use that installer to update it.
6. Verify playback, SlimProto connection, Now Playing, CometD and other applets.
7. Use the original installer to uninstall/reinstall the test applet as needed.
8. Reboot normally and confirm both catalog and ZIP persist.

BusyBox-compatible diagnostics:

```sh
df -k /mnt/sbdata /mnt/storage
grep -A20 '"applets"' /mnt/storage/standalonebase/config.json
ls -l /mnt/sbdata/applet-repository/packages
wget -O /tmp/package.zip http://127.0.0.1/applets/packages/NAME-VERSION.zip
sha1sum /tmp/package.zip
tail -100 /tmp/standalonebase/sbwebserver.log
tail -100 /tmp/standalonebase/sbbase.log
grep -i -E 'applet|extension|install' /var/log/messages | tail -100
```

Some stock images may lack `sha1sum`; in that case compare the catalog digest
from a PC after downloading the same loopback URL through SSH/port forwarding.
Removing the catalog entry must be tested separately from uninstall: catalog
removal alone must leave installed files untouched.
