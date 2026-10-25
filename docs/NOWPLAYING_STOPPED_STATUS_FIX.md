# Stopped player status: stock NowPlaying compatibility

`np_playerstatus_json` in `native/sbbase/src/nowplaying.c` included numeric
`playlist repeat` and `playlist shuffle` in its active response but omitted them
when stopped/inactive. Stock SqueezePlay NowPlaying concatenates these values
in `_updateRepeat` and `_updateShuffle`; a transition to the incomplete empty
status could therefore cause the reported `mode` nil concatenation error.

The empty response now supplies both numeric zero values, consistent with the
existing fixed-off live-radio semantics. Playback `mode` already existed;
changing it or patching stock NowPlaying would not correct this defect.

Regression tests cover initial empty status, buffering, playing, paused, stopped,
released and expired ownership. All 49 host tests and the static ARMv5 build
passed. No physical device was deployed or restarted for this fix.

The companion Standalone Radio 0.9.15 work in the Radio repository removes
HELO-triggered ownership release and isolates stale decoder/gain commands.
Both changes should be released together. Do not overwrite the published Base
0.6.2 artifact: include this correction in the next Base release. Malformed
responses from a separate external LMS are outside this local status fix.

Source contract checks and simulation are not proof of audible operation on
firmware 7.7.3; a bounded physical 7.7.3 and 9.x switch test remains recommended.
