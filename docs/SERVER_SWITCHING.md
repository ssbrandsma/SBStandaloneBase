# Switching from Standalone Base to another LMS

When a connected Radio selects another music source, Jive's
`LocalPlayer.connectToServer()` sends `["connect", "<IPv4>"]` to the
**current** server through `/slim/request`. It does not directly reconnect
SlimProto while the current player connection is healthy.

Previously sbbase acknowledged this command without performing it. HTTP
discovery and applet listing on the destination worked, but the player
remained on local port 3483 and Jive eventually reported a connection timeout.
Jive's background requests also omit the optional CometD `id`. The dispatcher
now finds message boundaries structurally instead of iterating over IDs, so
these commands execute as well as foreground requests and batched requests.

`native/sbbase/src/main.c` now validates the request and finds the SlimProto
connection whose HELO MAC matches the requested player. It sends a `serv`
frame with four IPv4 bytes in network order, using the existing framed sender.
Jive's existing `SlimProto.lua` serv handler performs the actual disconnect
and reconnect. No Jive framework patch, DNS request, or new service is needed.

The old connection stays open until the player processes the frame and leaves.
Malformed addresses, missing arguments, extra arguments, unknown players,
disconnected players, multicast/reserved addresses, and 0/8 special targets
return an unsuccessful command response. Literal loopback and LAN addresses
are supported. Hostnames, IPv6, custom ports, and legacy SqueezeNetwork
special destination codes are not supported by this command.

Success means the frame was sent, not that the destination accepted the
connection. Jive retains responsibility for displaying destination failures.
The command uses the existing loopback-only internal LMS API; browser
management authentication and its remote-control trust boundaries are unchanged.

## Validation

`tests/comet/test_server_switch.py` launches sbbase with a fake SlimProto
player and tests exact serv framing, several destinations, rejected requests,
continued status requests, and disconnect handling. It is registered as
`comet_server_switch` in CTest and can also run directly:

```sh
python3 tests/comet/test_server_switch.py /path/to/sbbase
```

Physical acceptance test after deploying the fix: start connected to local
Standalone Base, select another reachable LMS/bootstrap server, and confirm
Jive logs `slimproto serv command` followed by the destination connection and
a successful source change. Also check switching back and an unreachable
destination. This physical acceptance test has not yet been performed.
