# Virtual Squeezebox Radio

## Architecture

Virtual Radio is part of the existing Standalone Base applet and
`sbwebserver`; it does not add a daemon. `/remote` loads the existing web UI
shell and its Virtual Radio page. The browser renews a short lease, polls one
shared capture, decodes the pixels into Canvas, and posts allowlisted controls.
The Standalone Base Lua applet consumes the bounded control queue and invokes
the real Jive action framework. JavaScript does not emulate any Jive menu.

The implementation is split into:

- `remote_framebuffer.c`: read-only Linux framebuffer inspection and capture;
- `remote.c`: leases, authentication, frame delivery and the input queue;
- `remote.js`: Canvas rendering, device controls and browser lifecycle;
- `RemoteBridge.lua`: Jive actions and the screenshot fallback.

At most four browser leases and 31 queued input events are retained. A single
pair of approximately 150 KiB buffers holds the current and previous physical
frame. No per-client frame buffers are allocated.

## Native framebuffer capture

The default `auto` backend opens `/dev/fb1` with `O_RDONLY`, reads fixed and
variable framebuffer information, validates RGB565 bitfields, dimensions,
stride, virtual height and all memory offsets, and maps the device with
`PROT_READ | MAP_SHARED`. It copies only the visible page at the current
`yoffset`. It reads `yoffset` again after copying and retries once if the active
page changed.

The capture code never writes framebuffer memory, changes a mode, or calls
`FBIOPAN_DISPLAY`. The browser may select active fb0/fb1 or force page 0/1.
Frames are compared with the shared previous frame and receive a new sequence
only when the bytes change. `after=<sequence>` returns HTTP 204 for an unchanged
client view. Native capture is capped at ten reads per second across all
clients.

The transport is raw little-endian RGB565. Response headers describe width,
height, stride, bit layout, physical orientation, framebuffer, page, sequence,
capture timestamp and capture duration. The default browser rotation is
counterclockwise, matching the physical portrait framebuffer-to-landscape
layout inferred from the validated Doom backend. Clockwise rotation remains
available for diagnostics.

The first implementation deliberately does not compose fb0 and fb1. A capture
can therefore be technically valid but visually incomplete.

## BMP fallback

Auto mode requests fallback only after framebuffer capture fails. BMP can also
be selected manually. While an active lease requests BMP, `RemoteBridge.lua`
uses the stock ScreenshotApplet rendering sequence without invoking the normal
screenshot action:

1. create an off-screen RGB surface at the logical screen size;
2. blit the Jive background;
3. draw the top Jive window with all layers;
4. save to `/tmp/standalonebase/remote-screen.bmp.tmp`;
5. atomically rename it to `remote-screen.bmp`.

There is no screenshot sound, toast or persistent-flash write. Only one render
can be active, and renders are limited to one every 750 ms. The webserver
serves at most 2 MiB and rejects missing or incomplete files. The browser
contains a bounded 24/32-bit uncompressed BMP decoder instead of relying on
browser BMP support. Temporary frames are removed after the final lease
expires and when the applet bridge stops.

## Jive action bridge

The browser cannot provide an action name, Lua text, shell text or Linux key
code. `sbwebserver` accepts only the documented control and phase pairs, queues
them in order, rejects duplicates by browser sequence, limits each client to
30 accepted events per second, and rejects queue overflow.

The Lua bridge reads a runtime capability from
`/mnt/storage/standalonebase/remote.capability`. The webserver creates this
random file with mode 0600; it is not compiled into Lua or exposed to the
browser. The internal action route also requires a loopback connection. Lua
polls it only while `/tmp/standalonebase/remote-active` exists.

Control mapping:

| Control | Short/repeat | Long |
|---|---|---|
| Home | `go_home_or_now_playing` | `go_home` |
| Back | `back` | `go_home` |
| Encoder clockwise/counterclockwise | `down` / `up` | repeated steps |
| Encoder press | `go` | `add` |
| Play | `play` | `create_mix` |
| Pause | `pause` | `stop` |
| Volume | `volume_up` / `volume_down` | same action, bounded repeat |
| Power | `power` | `shutdown` after browser confirmation |
| Preset 1–6 | `play_preset_N` | `set_preset_N` |

Pointer capture, cancellation, lost capture and release outside a control all
stop timers. A hold fires once after 700 ms. Volume repeat is bounded by pointer
lifecycle, the browser event rate and the server rate limit. Encoder gestures
are capped at 24 steps.

## HTTP API

All public endpoints require a valid PIN-paired `sb_session` browser cookie:

- `POST /api/remote/session` — renew a 15-second browser lease;
- `GET /api/remote/frame?client=...&source=...&after=...` — get RGB565/BMP;
- `POST /api/remote/input` — enqueue one allowlisted control event;
- `GET /api/remote/status` — capture, queue, client and bridge diagnostics.

`POST /api/remote/session` and `/input` enforce same-origin writes. Input is
disabled when web management is read-only; viewing remains authenticated. The
loopback-only `/api/remote/actions` additionally requires the runtime
capability. Request bodies are handled by Mongoose's existing bounded request
model, and all remote fields have fixed native limits.

## Session lifecycle and performance

The page renews its lease every five seconds. Closing or suspending a browser
requires no reliable unload event: the lease expires after 15 seconds. When no
lease remains, the active marker and BMP files are removed, Jive stops HTTP
action polling, and no framebuffer calls occur. Framebuffer capture happens on
demand, not in a background thread.

The default browser request rate is 2 fps, increasing to at least 5 fps for 2.5
seconds after an input event. Diagnostic 10 fps is selectable. BMP rendering is
independently capped near 1.3 fps. Actual hardware performance must be measured
without compromising playback.

## Browser UI

The responsive radio includes six presets, Home, Back, Play, Pause, volume,
power and a rotary encoder. The encoder accepts pointer rotation, mouse wheel
and click. Keyboard shortcuts are Arrow Up/Down, Enter, Escape, Home, Space,
+/-, and 1–6. Shortcuts are ignored while an input/select/textarea has focus.
The display can enter fullscreen. Source, page, rotation and diagnostics are
available beside the radio; source and rotation are remembered locally.

## Build and packaging

Run:

```sh
./scripts/build-arm.sh
ctest --test-dir build --output-on-failure
./packaging/build-package.sh
```

The ordinary Standalone Base ZIP includes `RemoteBridge.lua`, `remote.js`, the
updated web assets and the enlarged `sbwebserver`. No storage-layout changes are
made. Upgrades retain settings and generate the bridge capability only when it
does not already exist.

Deploy through the normal project deployment workflow. Do not copy individual
files onto a production Radio because the native binary, Lua bridge and web
assets must match. This implementation has not been deployed by the
implementation task.

## Troubleshooting

- **Black or incomplete screen:** switch between fb1, fb0 and Screenshot/BMP.
  The two hardware planes are not composed in this release.
- **Incorrect rotation:** open Advanced diagnostics and select clockwise or
  counterclockwise.
- **Wrong framebuffer page:** select page 0 or page 1. Return to Active page
  after diagnosis so Jive page flips are followed.
- **Stale frames:** check the sequence and received/skipped counters. HTTP 204
  is expected when pixels have not changed.
- **Jive bridge disconnected:** confirm the Standalone Base applet is loaded,
  `remote.capability` exists and the webserver port matches `settings.conf`.
- **Controls do not respond:** read-only mode disables input. Check the bridge
  heartbeat and queue depth in `/api/remote/status`.
- **High CPU use:** return to 2 fps and close unused browser tabs. Leases may
  take up to 15 seconds to expire.
- **BMP fallback errors:** ensure `/tmp/standalonebase` is writable and the
  firmware exposes `Surface:newRGB`, background blitting, window drawing and
  `saveBMP` as on stock 7.7.3.

## Validation status and limitations

Host tests cover RGB565 metadata and bounds, source-aware change sequences,
authentication, origin rejection, client caps, duplicate rejection, invalid
controls, queue overflow, internal capability authentication and read-only
input. The ARMv5 build verifies static EABI5 output.

Physical-device validation is still required for framebuffer plane choice,
page behavior, exact rotation/cropping, real Jive action behavior, off-screen
BMP rendering, capture cost, playback coexistence and long-running memory use.
Until those checks are performed, the feature must not be described as
physically validated.
