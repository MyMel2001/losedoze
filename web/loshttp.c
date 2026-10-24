/* loshttp.c - minimal HTTP/1.0 client over POSIX sockets (http:// only).
 * Synchronous GET; returns malloc'd body (NUL-terminated) + status line.
 */
#define _GNU_SOURCE
#include "loshttp.h"
#include "k93util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <unistd.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>

#define MAXBODY (8 * 1024 * 1024)

int loshttp_get(const char *url, struct loshttp *out, char *err, size_t errlen) {
    out->body = NULL;
    out->len = 0;
    out->status = -1;
    const char *p = url;
    const char *slash = strchr(p, '/');
    char hostbuf[1024];
    strncpy(hostbuf, slash ? slash : p, sizeof hostbuf - 1);
    hostbuf[sizeof hostbuf - 1] = 0;
    int port = 80;
    const char *host = hostbuf;
    const char *colon = strrchr(hostbuf, ':');
    if (colon && colon != hostbuf) {
        port = atoi(colon + 1);
        static char hostbuf2[1024];
        strncpy(hostbuf2, hostbuf, (size_t)(colon - hostbuf));
        hostbuf2[colon - hostbuf] = 0;
        host = hostbuf2;
    }
    char path[1024];
    if (slash) { strncpy(path, slash, sizeof path - 1); path[sizeof path - 1] = 0; }
    else { strncpy(path, "/", sizeof path - 1); path[sizeof path - 1] = 0; }

    int fd = -1;
    struct addrinfo hints, *res, *rp;
    char service[8];
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(service, sizeof service, "%d", port);
    if (getaddrinfo(host, service, &hints, &res) != 0) { snprintf(err, errlen, "dns fail %s", host); return -1; }
    for (rp = res; rp; rp = rp->ai_next) {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) break;
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) break;
        close(fd); fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) { snprintf(err, errlen, "connect fail %s", host); return -1; }

    char req[2400];
    snprintf(req, sizeof req, "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: LosedozeBrowser/1.0\r\n\r\n", path, host);
    size_t rl = strlen(req);
    size_t sent = 0;
    while (sent < rl) {
        ssize_t n = write(fd, req + sent, rl - sent);
        if (n <= 0) break;
        sent += (size_t)n;
    }

    char *accum = NULL;
    size_t accap = 0;
    char chunk[65536];
    ssize_t n;
    int done = 0;
    int status = -1;
    while (!done) {
        n = read(fd, chunk, sizeof chunk);
        if (n <= 0) break;
        accum = (char *)realloc(accum, accap + (size_t)n + 1);
        accap += (size_t)n;
        memcpy(accum + accap - (size_t)n, chunk, (size_t)n);
        char *hdr_end = (char *)memmem(accum, accap, "\r\n\r\n", 4);
        if (hdr_end) {
            status = -1;
            char *nl = strstr(accum, "\r\n");
            if (nl) sscanf(nl + 2, "%*s %d", &status);
            size_t blen = accap - (size_t)(hdr_end - accum) - 4;
            if (blen > MAXBODY) blen = MAXBODY;
            char *body = (char *)malloc(blen + 1);
            memcpy(body, hdr_end + 4, blen);
            body[blen] = 0;
            out->body = body;
            out->len = blen;
            out->status = status;
            done = 1;
        }
    }
    close(fd);
    free(accum);
    return 0;
}

void loshttp_free(struct loshttp *r) {
    free(r->body);
    r->body = NULL;
    r->len = 0;
}
