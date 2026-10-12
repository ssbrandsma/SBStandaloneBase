# Photorealistic Virtual Radio

The Virtual Radio has two frontend-only visual modes. Photorealistic is the
default; Classic preserves the original CSS representation. Both modes move the
same canvas between layouts and use the same session, capture loop, input
dispatcher, rotary handling, authentication, and diagnostics.

## Image asset

The source asset is
`native/sbwebserver/web/assets/virtual-radio/radio-front.webp`. Packaging installs
it as `radio-front.webp` beside the other flattened web assets.

The image is an original project-created AI product rendering generated with
OpenAI's built-in image generation tool on 2026-10-12. Public product images
were used only to understand the hardware layout; no third-party photograph is
bundled. The WebP is 1536 x 1024 pixels, lossless, with an alpha channel. It
shows a straight-on black Radio, blank LCD, six presets, Home, Back, Power,
Play, Pause, volume controls, and the rotary encoder.

To replace it, use another straight-on 3:2 image with the entire front panel
visible, no perspective distortion, and a blank LCD. Replace the source WebP,
rebuild the package, then use calibration mode to align the LCD and hotspots.
Record the replacement image's source and redistribution license here.

## Geometry

Geometry is normalized to the 1536 x 1024 image. The default LCD overlay is:

```json
{"x":0.6354,"y":0.2266,"w":0.25,"h":0.28125}
```

That produces a 384 x 288 (4:3) live area inside the generated blank display.
Hotspot defaults are stored separately from action mappings in `PHOTO_DEFAULTS`
in `remote.js`. Coordinates cover presets 1-6, Home, Back, Power, Play, Pause,
volume up/down, and the encoder. Circular controls use circular hit areas.

## Calibration

Open **Display > Hotspot calibration** and enable **Show and edit geometry**.
Calibration mode disables remote input.

- Drag a hotspot or the cyan LCD boundary to move it.
- Shift-drag to resize it.
- **Export calibration JSON** copies the values to the clipboard and displays
  them on the page.
- **Reset calibration** restores the checked-in defaults.

Edits are stored in `localStorage` under
`sb-remote-photo-calibration`. Copy reviewed values into `PHOTO_DEFAULTS` to
make them the project defaults. Calibration must be visually confirmed on the
target browsers after replacing the photograph.

## Interaction and responsive layout

Transparent hotspot elements scale with the image. Existing short press, long
press, preset save, and volume repeat behavior is reused. The encoder supports
circular or linear drag, touch, wheel, click-to-select, and keyboard input.
Pressed hotspots receive a brief neutral highlight; focus receives a restrained
outline. On narrow screens the settings card moves below the device.

The visual-mode preference is stored as `sb-remote-visual-mode`. Switching
moves the existing canvas and message overlay; it does not renew the session,
restart capture, or create another rendering loop. LCD fullscreen remains
available, and Photorealistic mode adds whole-Radio fullscreen.

## Known limitations

- The asset is an original visual approximation, not a licensed Logitech
  product photograph and not a manufacturing-accurate CAD rendering.
- The 4:3 canvas occupies the calibrated center of the generated blank opening;
  a small amount of the surrounding black display surface remains visible.
- Hotspot alignment is calibrated to the bundled 1536 x 1024 asset. Replacement
  images require recalibration.
- Browser fullscreen behavior depends on platform permissions, especially on
  mobile Safari.
