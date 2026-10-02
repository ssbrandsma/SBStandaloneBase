# Deployment

Build all ARM binaries, run host and QEMU checks, then create the ZIP. Publish neither the ZIP nor `extensions.xml` automatically. Install through the existing remote Applet Installer only after backing up `/mnt/storage` and confirming serial/SSH recovery access.

Installation copies the applet to `/usr/share/jive/applets/StandaloneBase`. On initialization it creates `/mnt/storage/standalonebase` and `/tmp/standalonebase`, starts the three services, and reports observed process state in **Standalone Base**. The first release must keep the current external server selected. Local activation is a separate, explicit hardware-validation step.
