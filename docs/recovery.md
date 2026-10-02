# Recovery

If SqueezePlay remains usable, uninstall or disable StandaloneBase and restart SqueezePlay. The applet stops only PIDs recorded in `/tmp/standalonebase`; it does not remove legacy applets or persistent data.

If local activation was attempted and connection fails, restore the recorded previous external server address (normally the installer/bootstrap server), stop `sbbase`, and restart SqueezePlay. Do not delete `/mnt/storage/standalonebase`; it contains diagnostics and the rollback record. From SSH/serial, terminate only verified StandaloneBase PIDs and move the applet directory aside. A corrupt config should be renamed, not erased, then regenerated from `config.example.json`.
