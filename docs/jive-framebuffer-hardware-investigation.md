# Squeezebox Radio framebuffer hardware investigation

The full source analysis, read-only measurements, capability comparison and unexecuted overlay-test proposal are in the userbuild workspace:

[jive-framebuffer-hardware-investigation.md](C:/Projects/squeeze_userbuild/docs/jive-framebuffer-hardware-investigation.md)

Confirmed: i.MX25 LCDC BG/FG hardware planes, separate memory, FG disabled, and existing BG page flipping. No foreground activation or display changes were performed. Review the full report's restoration limitations before considering a test; explicit approval is still required.
