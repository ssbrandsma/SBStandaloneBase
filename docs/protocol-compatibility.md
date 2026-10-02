# Protocol compatibility

| Area | Reference behavior | Native status |
|---|---|---|
| UDP discovery | TLV parse and requested-field response | Implemented and unit-tested |
| SlimProto framing | partial/multiple client frames, bounded at 64 KiB | Parser implemented and unit-tested |
| HELO/STAT | player registration/activity | HELO accepted; in-memory player model remains incomplete |
| `strm/t` | periodic server keepalive | Initial frame implemented; periodic scheduling pending |
| HTTP bounds | 16 KiB headers, 128 KiB body | Implemented |
| Bayeux handshake | version 1.0, 8-hex client ID, retry advice | Implemented baseline |
| Bayeux connect/disconnect | long poll and streaming | Basic responses implemented; streaming/subscription state pending |
| Jive commands | server/status/date/firmware/menu/applets | Basic server/date/menu/applets results; exact per-player/catalog data pending |
| Catalog filtering | model and firmware bounds | Pending native catalog module |

Wire equality should be evaluated against the Python fixtures. JSON member order is not semantically relevant to Bayeux, but fixtures retain exact required field values. Hardware-dependent connection promotion is not claimed.
