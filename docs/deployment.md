# Deployment

Build all ARM binaries, run host and QEMU checks, then create the ZIP. Publish neither the ZIP nor `extensions.xml` automatically. Install through the existing remote Applet Installer only after backing up `/mnt/storage` and confirming serial/SSH recovery access.

Installation copies the applet to `/usr/share/jive/applets/StandaloneBase`. On initialization it creates `/mnt/storage/standalonebase` and `/tmp/standalonebase`, starts the three services, migrates the configured bootstrap endpoint to the local service, and reports observed process state in **Standalone Base**.

The packaged `config.json` provides an offline catalog containing Standalone Radio and Standalone Spotify. At startup the applet starts its loopback HTTPS proxy, retrieves `https://raw.githubusercontent.com/ssbrandsma/SBStandaloneBase/master/config.json`, validates that both expected applet identifiers are present, and atomically replaces `/mnt/storage/standalonebase/config.json`. If retrieval or validation fails, the last valid persistent file—or the packaged fallback on first boot—is retained. `sbbase` reads that file and returns its two entries for the Applet Installer `jiveapplets` request.

On initialization, the applet migrates the former bootstrap endpoint `49.12.198.91` to the local endpoint `127.0.0.1` in the SqueezePlay server-selection settings. Before changing a file, it preserves its original contents once in a sibling `.pre-standalonebase` backup. The exact-address replacement is idempotent and does not modify other configured servers.
