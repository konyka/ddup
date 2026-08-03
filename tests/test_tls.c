/* test_tls.c - TLS tests (registered in CTest only when DDUP_HAS_TLS=1). */
#include <string.h>

#include "pal/pal_tls.h"
#include "test.h"

#ifndef DDUP_TEST_CERT_DIR
#define DDUP_TEST_CERT_DIR "tests/certs"
#endif

static void test_ctx_load(void)
{
    pal_tls_ctx *ctx = pal_tls_ctx_new(DDUP_TEST_CERT_DIR "/cert.pem",
                                       DDUP_TEST_CERT_DIR "/key.pem");
    DD_CHECK(ctx != NULL);
    pal_tls_ctx_free(ctx);
}

static void test_ctx_bad_files(void)
{
    DD_CHECK(pal_tls_ctx_new("no-such-cert.pem", "no-such-key.pem") == NULL);
}

int main(void)
{
    DD_RUN(test_ctx_load);
    DD_RUN(test_ctx_bad_files);
    return DD_TEST_SUMMARY();
}
