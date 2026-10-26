# Recovering a stale selected player on the physical Radio

On 192.168.1.228, explicit selection of either Base or Desktop LMS failed even
with playback stopped. The decisive Jive message was `No method to connect
Player {00:04:20:2a:56:81}`. Sysfs identified the physical Radio as
`00:04:20:29:16:7f`. Saved SlimDiscovery settings selected the former player but
identified the library as the local Base UUID and loopback address.

Stock `Player:connectToServer` cannot switch a generic player with neither a
server connection nor the `needsServer` provisioning state. Meanwhile the
physical LocalPlayer retained a healthy SlimProto connection to Base. This is
a stale UI/control selection, not evidence that Base itself is unreachable.

The Radio station-start fix alone is insufficient when playback stays stopped.
Base now performs a narrowly guarded initial-selection recovery through its
existing supervisor, without adding polling or changing server-switch policy.

`ServerMigration.localPlayerForOrphanSelection` returns a replacement only when:

- Saved discovery identifies Base's UUID, loopback address and Baby model.
- Saved player identity matches the selected orphan, not another selection.
- The selected player is non-local, has no server, and is not awaiting device
  provisioning (`needsServer`).
- The actual LocalPlayer is attached to Base's identified server.

`connectToLocalServer` uses that decision once during initialization and selects
the actual LocalPlayer through the stock `Player:setCurrentPlayer` path. Native
SlimDiscovery notifications persist the corrected identity. A subsequent user
selection is not pulled back by supervision; external-server settings and valid
remote players are left alone. The Radio does not have to start a station for
this recovery to run.

Regression coverage includes the actual startup function and pure decision
helper, repeated calls, a subsequent external selection, absent/local/already
selected players, valid remote connections, provisioning and mismatched saved
identities. The focused migration test passes.
All 50 configured host regression tests passed (150.73 seconds); the expanded
focused selection tests also passed separately after their final additions.

Only `ServerMigration.lua` and `StandaloneBaseApplet.lua` were deployed with
originals backed up, preserving the installed version label and unrelated local
changes. SqueezePlay was restarted; the OS boot ID did not change. Without a
manual discovery-file edit, the resulting saved `playerId` and `currentPlayer`
were both verified as `00:04:20:29:16:7f`. The user subsequently confirmed that
both Standalone Base and DESKTOP-OVMHVD2 library selections work while playback
is stopped. This confirms physical library switching on the test Radio; audible
desktop playback and other firmware versions were not separately validated.
