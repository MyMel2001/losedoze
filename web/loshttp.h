/* loshttp.h - minimal HTTP/1.0 client for the Losedoze web browser.
 * http:// only (no TLS: the guest has no mbedTLS); one GET, no redirects.
 */
#ifndef LOSHTTP_H
#define LOSHTTP_H
#include <stddef.h>

struct loshttp {
    char *body;
    size_t len;
    int status;
};

int  loshttp_get(const char *url, struct loshttp *out, char *err, size_t errlen);
void loshttp_free(struct loshttp *r);

#endif
