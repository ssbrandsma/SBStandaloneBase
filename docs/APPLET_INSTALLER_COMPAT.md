# Applet Installer with Extended Storage

Standalone Base 0.5.7 installs an in-memory adapter for the stock Applet
Installer. It does not rewrite firmware files or change `package.path`.

The stock installer selects the first physical applet directory from the Lua
search path, commonly `/mnt/storage/usr/share/jive/applets`. After Extended
Storage is enabled, a bind mount hides that tree behind
`/usr/share/jive/applets`, backed by `/mnt/sbdata/applets`. Installing into the
physical directory therefore saves files and installation bookkeeping without
making the applet visible to SqueezePlay.

`InstallerCompat.lua` checks `/proc/mounts` before install, update and removal.
When the active applet path is an `sbubifs` mount with the stock installer
present, operations use `/usr/share/jive/applets`. Otherwise the original
stock destination is retained. Repeated initialization does not stack hooks.

The adapter must load the installer through `AppletManager:loadApplet` and
patch that managed instance. A direct `require` is insufficient: Jive later
executes the applet module again with `loadfile`, replacing module-level
patches. This lifecycle bug in the initial 0.5.7 adapter left the stock ZIP
sink active and could leave the downloading popup spinning indefinitely.

Patching the first managed instance alone is also insufficient. Closing the
installer menu calls `freeApplet`, clearing the instance and its module. The
adapter therefore wraps the manager's `loadApplet` method and applies the fix
to every new installer instance. The close/free/reload/reopen lifecycle is
covered by regression tests.

Extraction and SHA transport failures wake the waiting download task. The
completion handler shows an error and skips successful-version bookkeeping
and reboot when a requested applet did not reach extraction EOF. This prevents
a failed asynchronous ZIP sink from leaving the waiting popup spinning forever.

The adapter also replaces the installer's streaming ZIP sink. It normalizes
legacy Windows separators, creates nested directories, writes binary data,
handles explicit directory records and directory-only end-of-stream, and
rejects unsafe paths and non-regular destinations. It never buffers an entire
archive. Extraction errors abort the task before the stock completion method
can record a successful installation.

Install Base 0.5.7 before Radio 0.9.9 on systems using Extended Storage. A Radio
installation already hidden in the physical directory must first be backed
up and installed into the active tree. The old hidden copy can then be moved
into that backup, avoiding stale applets reappearing when storage is disabled.

Radio 0.9.9 writes explicit forward-slash ZIP paths and directory records,
including `images/radio.png`. Both current archives and earlier backslash
archives are covered by `tests/images/test_installer_compat.lua`.
