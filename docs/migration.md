# Migration

The supervisor checks its own PID files and will not start duplicates it owns. Before installation, inspect for `SBWebserver`, `HTTPSProxy`, and `StandaloneTime` applet directories, processes, ports 80/8765, and their storage directories. Preserve all settings and CSV files. Do not uninstall or kill a legacy component automatically. If a port is occupied by an unknown process, leave it running, report the conflict, and disable the corresponding StandaloneBase component until the user chooses a migration window.

Settings import should be implemented as copy-and-validate into `/mnt/storage/standalonebase`, followed by an atomic rename. The source remains untouched. Only one time authority may set the clock: disable the legacy StandaloneTime applet after native SNTP has passed hardware validation.
