if(NOT EXISTS "${SB_TLS_PREFIX}/include/wolfssl/options.h" OR
   NOT EXISTS "${SB_TLS_PREFIX}/lib/libwolfssl.a")
  message(FATAL_ERROR "Missing rebuilt wolfSSL installation")
endif()
file(APPEND "${SB_TLS_PREFIX}/include/wolfssl/options.h" "
/* Standalone Base: must match libwolfssl.a's fixed integer/RSA capacity. */
#if defined(SP_INT_BITS) && SP_INT_BITS != 4096
#error Incompatible wolfSSL integer capacity
#endif
#ifndef SP_INT_BITS
#define SP_INT_BITS 4096
#endif
")
file(WRITE "${SB_TLS_PREFIX}/sbproxy-rsa-capacity" "4096\n")
