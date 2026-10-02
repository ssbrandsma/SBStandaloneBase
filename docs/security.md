# Security

Remote catalogs are executable-content metadata. Production configuration therefore requires a detached signature over the exact catalog bytes and an embedded public key. The current baseline does not yet contain an Ed25519 verifier, so remote refresh must remain disabled; the built-in empty catalog is the only trusted catalog. Unsigned fallback is prohibited.

The proxy intentionally disables upstream certificate and hostname validation to support the legacy TLS stack. It binds to loopback only and must not carry passwords, OAuth tokens, cookies, or other authenticated traffic. Encryption without authentication does not establish server identity.

Protocol buffers, request bodies, connections, and catalog sizes are bounded. Configuration-changing web endpoints need authentication before exposure beyond a trusted LAN. Logs must not contain secrets. Native services run without shell command endpoints or general filesystem APIs.
