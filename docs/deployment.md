# Deployment

Build all ARM binaries, run host and QEMU checks, then create the ZIP. Publish neither the ZIP nor `extensions.xml` automatically. Install through the existing remote Applet Installer only after backing up `/mnt/storage` and confirming serial/SSH recovery access.

Installation copies the applet to `/usr/share/jive/applets/StandaloneBase`. On initialization it creates `/mnt/storage/standalonebase` and `/tmp/standalonebase`, starts the three services, migrates the configured bootstrap endpoint to the local service, and reports observed process state in **Standalone Base**.

The package build embeds the tracked root `config.json`, which is the public
applet catalog published on GitHub's `master` branch. It contains public ZIP
download URLs, versions and checksums, plus device-local server defaults;
it must not contain SSH credentials, hosting paths or container settings.
On first boot the packaged
catalog is copied to `/mnt/storage/standalonebase/config.json`; `sbbase` reads
that file and returns its entries for the Applet Installer `jiveapplets`
request. Catalog updates must be supplied through the authenticated management
interface or another explicitly configured distribution channel. Publishing
the GitHub file does not by itself add an automatic GitHub fetch to the service.

Update the tracked `config.json` when releasing applets. A private alternative
may remain under `.local/`; select it explicitly with PowerShell `-ConfigPath`
or shell `SB_PACKAGE_CONFIG`. Private deployment configuration remains ignored.
The remote bootstrap catalog remains Base-only; the embedded local catalog
contains Standalone Base, Standalone Radio, Spotify Connect, and Qobuz Connect.
After loading the refreshed catalog, the local Applet Installer can offer Base
updates without switching the selected LMS back to the bootstrap server. The
standard installer performs its normal reboot after installation. This catalog
entry does not add a new self-update mechanism or automatically refresh an
already installed Radio's persistent configuration.

For a Base release, build and publish the ZIP first, then update the public
Base entry to its final version, URL and SHA-1. Do not repack that same ZIP to
embed its own final checksum: that would change the ZIP and invalidate the
checksum. Its packaged catalog may refer to the preceding Base release;
the refreshed public catalog advertises the new release.

On initialization, the applet identifies the bootstrap service by its stable UUID or name and migrates its configured endpoint to `127.0.0.1`. It also replaces the bootstrap server UUID and name with the Standalone Base identity. Before changing a file, it preserves its original contents once in a sibling `.pre-standalonebase` backup. The migration is idempotent and does not modify unrelated configured servers.

SqueezePlay 9.x loads applet settings before third-party applets initialize and can otherwise save the stale bootstrap address over the migrated file. Standalone Base therefore also migrates the live `ChooseMusicSource` and `SlimDiscovery` settings, updates the active discovery poll list, and waits for the correctly identified loopback server before switching a player that is still attached to the bootstrap server. This compatibility path never switches a player away from an unrelated LMS selection.

StandaloneBase also runs `patch-squeezeplay-artwork.sh` against the active
`/usr/share/jive/jive/slim/SlimServer.lua`. It first treats explicit native
loopback support or complete removal of `/public/imageproxy` as **not needed**.
It patches only when the old `jnt:getSNHostname()` image-proxy expression and
both expected private-address anchors occur exactly once. It then saves the
original once as `SlimServer.lua.pre-standalonebase` and adds only direct-fetch
exceptions for `127.0.0.1` and `localhost`. Alternative or ambiguous proxy
implementations are left untouched and reported as unsupported in
`/tmp/standalonebase/artwork-patch.log`. A newly
applied patch requires SqueezePlay to restart because the Lua module is loaded
before the applet initializes.
# Deploying a dist package to a Radio

From the repository root on Windows:

```powershell
.\deploy_dist.cmd .\dist\StandaloneBase-0.4.0.zip root@RADIO_IP
```

The command validates that the archive is a flat StandaloneBase release ZIP,
uploads it over SSH, verifies the packaged module and initialization image on
the Radio, atomically updates the applet through the merged `/usr` UnionFS path
and updates the persistent runtime configuration, then restarts SqueezePlay.
It deliberately never writes an applet directly through `/mnt/storage/usr`:
on SqueezeOS that can create a recursive `/mnt/storage/mnt/storage/usr` copy in
the small writable UBIFS branch. It never invokes Extended Storage
initialization. Use `--no-restart` to leave SqueezePlay running or `--dry-run`
to perform only local package validation. Unknown SSH host keys are rejected;
use `--accept-new-host-key` for a new Radio after independently confirming its
identity.
