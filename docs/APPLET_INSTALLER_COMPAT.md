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
