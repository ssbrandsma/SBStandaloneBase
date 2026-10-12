# Web Access physical PIN pairing

## Architecture

```text
physical controls
      │ trusted Jive callback (remote-dispatch guard)
      ▼
Standalone Base Jive applet ── loopback + capability ──► sbwebserver PIN manager
                                                           │
browser ── POST /api/auth/pair { six-digit PIN } ──────────┤
                                                           ▼
                                               hashed in-memory sessions
                                                           │
browser ◄── HttpOnly SameSite=Strict sb_session cookie ────┘
```

Browser management sessions and internal service authentication are separate domains. Browsers use only `sb_session`. Jive uses the pre-existing random `remote.capability`, mode 0600, over loopback for `/api/remote/actions` and `/api/internal/web-access/*`. That capability is never returned to a browser and cannot authorize browser management. The retired management token and its HTTP headers are ignored.

## Pairing workflow

1. Open the Radio's Standalone Base applet and choose **Web Access → Show Pairing Code** using the physical controls.
2. The applet asks `sbwebserver` to create a PIN through the authenticated loopback channel. The native service uses rejection sampling over `/dev/urandom`, stores one six-digit PIN in memory, and expires it after 120 seconds.
3. Open the web interface, enter the code, and select **Pair browser**. Spaces are accepted by the UI and removed before submission; leading zeroes are preserved.
4. `POST /api/auth/pair` consumes the PIN before generating an independent 256-bit token. Only its SHA-256 hash and absolute 24-hour expiration are held server-side.
5. The raw token is returned only as `sb_session`, with `HttpOnly`, `SameSite=Strict`, and `Path=/`. This build has only a direct HTTP listener, so it omits `Secure`; an HTTPS reverse proxy must add `Secure` while preserving the other attributes.

PINs are single-use. A new code invalidates the old one. Five failures invalidate the current PIN. Per-client and global one-minute limits plus a 30-second cooldown bound guessing. Authentication failures are deliberately generic.

## Physical-input and protected-screen boundary

Virtual Radio injects actions only through `RemoteBridge`. The bridge marks its synchronous dispatch as untrusted. Every Web Access callback checks this marker and does nothing when reached by an injected action. A LAN browser therefore cannot mint or revoke credentials by navigating Jive remotely.

The native service marks capture protected before it returns a PIN to Jive. While protected:

- raw framebuffer and BMP requests return `423 Protected screen`;
- the remote-active marker and any prior BMP are removed;
- the Jive bridge receives `@protected`, suppresses BMP rendering, and deletes temporary frames.

This enforcement is server-side. Pairing consumes/cancels/expires the PIN and clears protection. Unauthenticated capture and control requests are rejected independently.

This boundary has host-side tests but still requires the physical-device procedure below before being described as hardware validated.

## Browser UI and session lifecycle

Unauthenticated users see only the static pairing shell and `/api/auth/status`. Protected API calls return 401 without management data. Successful pairing restores Overview, Virtual Radio, Applets, Settings, System Info, and conditional Standalone Radio navigation. Settings shows session expiry and provides logout and revoke-all actions.

Sessions are memory-only and bounded to eight. At capacity, the session with the earliest absolute expiry is replaced. They do not slide and do not survive an `sbwebserver` restart. Logout deletes the current hash; revoke-all clears all hashes and any active PIN. Session expiry stops Virtual Radio polling and returns the browser to pairing.

## API

| Method | Route | Authentication | Purpose |
|---|---|---|---|
| GET | `/api/auth/status` | public | Current browser state only |
| POST | `/api/auth/pair` | public, rate limited, same-origin | Redeem one PIN |
| POST | `/api/auth/logout` | current cookie | End this session |
| POST | `/api/auth/revoke-all` | current cookie | End all sessions |
| POST | `/api/internal/web-access/start` | loopback capability | Create and return a PIN to Jive |
| POST | `/api/internal/web-access/cancel` | loopback capability | Invalidate the PIN |
| POST | `/api/internal/web-access/revoke-all` | loopback capability | Physical revoke-all operation |

All existing management APIs, configuration downloads, applet packages, Virtual Radio endpoints, and radio station endpoints require a browser session. State changes retain Origin checking, SameSite protection, request limits, and read-only enforcement. Authentication responses use `Cache-Control: no-store`, `X-Content-Type-Options: nosniff`, and `Referrer-Policy: no-referrer`. No permissive CORS headers are enabled.

## Migration and recovery

Existing `management.token` files may remain on disk after upgrade but are no longer read or accepted. Frontend token prompts and session-storage use are removed. Internal integrations continue using their dedicated protocols; Spotify Connect, Qobuz Connect, Now Playing, `sbproxy`, and bootstrap traffic do not depend on browser sessions.

Recovery cases:

- Lost or expired session: generate a new code on the physical Radio.
- Native service restart: generate a new code; old sessions intentionally become invalid.
- Clock correction: sessions use absolute system time and may expire early after a forward jump; pair again. A backward jump never extends beyond the stored absolute timestamp relative to the corrected clock, but reliable time synchronization remains important.
- Extended storage unavailable: pairing still works because PINs and sessions are memory-only; storage-dependent management writes remain unavailable.
- Jive menu unavailable: restart the Radio normally. Local shell access is the last-resort diagnostic path; there is no unauthenticated network reset endpoint.

## Threat model and HTTP limitation

Pairing proves access to the physical UI against ordinary unauthenticated LAN clients and prevents Virtual Radio from authorizing itself. It does not encrypt the network. On plain HTTP an observer or active attacker can intercept a PIN or session cookie. Use only on a trusted LAN. Remote access should be through a secure VPN or an HTTPS reverse proxy. Physical pairing alone is not protection against an active network attacker, device compromise, or a privileged local process able to read the internal capability.

## Performance

The implementation adds no process, daemon, persistent session database, or idle polling. Session lookup checks at most eight SHA-256 hashes. PIN/session entropy is read only when requested. Revocation and restart cause no flash writes.

## Build and deployment

Build using `scripts/build-arm.sh`, then create the distribution with the normal packaging script. Deploy using the existing deployment workflow. Deployment and SqueezePlay restart require explicit operator approval.

## Physical-device validation procedure

1. Deploy the candidate package and restart SqueezePlay.
2. Confirm the web page reveals no overview data and presents pairing instructions.
3. Attempt to navigate to **Web Access** using Virtual Radio; verify **Show Pairing Code** cannot create or reveal a PIN.
4. Use physical controls to create a code. Verify framebuffer and BMP capture immediately show `Protected screen`, including from a browser paired before the test.
5. Verify formatting, leading-zero display when encountered, the countdown, two-minute expiry, cancel, single use, five-failure invalidation, and replacement by a newer PIN.
6. Pair desktop and mobile browsers; exercise every management page, uploads, downloads, radio station editing, and Virtual Radio.
7. Test logout, browser revoke-all, and physically confirmed revoke-all.
8. Restart `sbwebserver` and verify prior cookies fail.
9. Verify Now Playing, Spotify Connect, Qobuz Connect, `sbproxy`, applet installation, storage management, startup, and shutdown.
10. Confirm no PIN, cookie, or internal capability appears in logs, diagnostics, URLs, screenshots, or exported configuration.

## Known limitations

- Sessions do not survive a service restart.
- HTTP provides no confidentiality.
- No physical-device security validation has yet been performed.
- Leaving the PIN screen through an unhandled Jive navigation path may leave capture protected until the PIN's short expiry; the explicit Cancel action clears it immediately.
