/*
 * Shared proxy/probe TLS 1.2 cipher policy. Keep the previous SDK suites first,
 * then allow only the two AES-GCM RSA exceptions
 * required by legacy broadcasters. TLS 1.3 uses its separate cipher policy.
 * Static RSA lacks forward secrecy; never add CBC, RC4, DES or NULL exceptions.
 */
#ifndef SBPROXY_TLS_POLICY_H
#define SBPROXY_TLS_POLICY_H

/* Explicit names avoid wolfSSL/OpenSSL differences in cipher-group exclusion
 * syntax. Unknown names are ignored by the backend. TLS 1.3 is not overridden.
 * Revisit this list when updating the SDK; preserve the existing PFS suites. */
#define SBPROXY_TLS12_CIPHERS \
	"DHE-RSA-AES128-SHA:DHE-RSA-AES256-SHA:" \
	"ECDHE-RSA-AES128-SHA:ECDHE-RSA-AES256-SHA:" \
	"ECDHE-ECDSA-AES128-SHA:ECDHE-ECDSA-AES256-SHA:" \
	"DHE-RSA-AES128-SHA256:DHE-RSA-AES256-SHA256:" \
	"DHE-RSA-AES128-GCM-SHA256:DHE-RSA-AES256-GCM-SHA384:" \
	"ECDHE-RSA-AES128-GCM-SHA256:ECDHE-RSA-AES256-GCM-SHA384:" \
	"ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-ECDSA-AES256-GCM-SHA384:" \
	"ECDHE-RSA-AES128-SHA256:ECDHE-ECDSA-AES128-SHA256:" \
	"ECDHE-RSA-AES256-SHA384:ECDHE-ECDSA-AES256-SHA384:" \
	"ECDHE-RSA-CHACHA20-POLY1305:ECDHE-ECDSA-CHACHA20-POLY1305:" \
	"DHE-RSA-CHACHA20-POLY1305:ECDHE-RSA-CHACHA20-POLY1305-OLD:" \
	"ECDHE-ECDSA-CHACHA20-POLY1305-OLD:DHE-RSA-CHACHA20-POLY1305-OLD:" \
	"AES128-GCM-SHA256:AES256-GCM-SHA384"

#endif
