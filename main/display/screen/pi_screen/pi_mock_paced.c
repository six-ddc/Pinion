/* SPDX-License-Identifier: MIT
 * pi-c — MetalioClaw5 vendor copy, forked from pi-c/tests/mock_transport.c
 * with a chunk_delay_ms pacing addition. See pi_mock_paced.h. */
#include "pi_mock_paced.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

typedef struct {
    const char *p;
    size_t len, off;
    int status;
    size_t fail_after; /* 0 = no injected failure */
    int fail_code;
} mock_conn_t;

static void free_headers(pi_mock_t *m) {
    for (size_t i = 0; i < m->last_header_count; i++) {
        free(m->last_headers[i]);
        m->last_headers[i] = NULL;
    }
    m->last_header_count = 0;
}

static void *m_open(void *ctx, const char *url, const char *const *headers, size_t header_count,
                    const char *body, size_t body_len) {
    pi_mock_t *m = (pi_mock_t *)ctx;
    if (m->next >= m->count) return NULL;
    snprintf(m->last_url, sizeof(m->last_url), "%s", url);
    free(m->last_body);
    m->last_body = (char *)malloc(body_len + 1);
    if (m->last_body) {
        memcpy(m->last_body, body, body_len);
        m->last_body[body_len] = '\0';
    }
    free_headers(m);
    for (size_t i = 0; i < header_count && i < PI_MOCK_MAX_HEADERS; i++) {
        m->last_headers[m->last_header_count] = headers[i] ? strdup(headers[i]) : NULL;
        if (m->last_headers[m->last_header_count]) m->last_header_count++;
    }
    mock_conn_t *c = (mock_conn_t *)malloc(sizeof(*c));
    if (!c) return NULL;
    const pi_mock_response_t *r = &m->responses[m->next++];
    c->p = r->body;
    c->len = strlen(r->body);
    c->off = 0;
    c->status = r->status;
    c->fail_after = m->honor_fail_fields ? r->fail_after_bytes : 0;
    c->fail_code = (m->honor_fail_fields && r->fail_code) ? r->fail_code : -1;
    return c;
}

static int m_read(void *ctx, void *conn, char *buf, size_t cap) {
    pi_mock_t *m = (pi_mock_t *)ctx;
    mock_conn_t *c = (mock_conn_t *)conn;
    if (m->chunk_delay_ms) {
        vTaskDelay(pdMS_TO_TICKS(m->chunk_delay_ms));
    }
    if (c->fail_after && c->off >= c->fail_after) return c->fail_code; /* injected drop */
    size_t left = c->len - c->off;
    if (left == 0) return 0;
    size_t take = left;
    if (m->chunk && take > m->chunk) take = m->chunk;
    if (take > cap) take = cap;
    if (c->fail_after && take > c->fail_after - c->off) take = c->fail_after - c->off;
    memcpy(buf, c->p + c->off, take);
    c->off += take;
    return (int)take;
}

static int m_status(void *ctx, void *conn) {
    (void)ctx;
    return ((mock_conn_t *)conn)->status;
}

static void m_close(void *ctx, void *conn) {
    (void)ctx;
    free(conn);
}

void pi_mock_init(pi_mock_t *m, const pi_mock_response_t *responses, size_t count, size_t chunk) {
    memset(m, 0, sizeof(*m));
    m->responses = responses;
    m->count = count;
    m->chunk = chunk;
    m->t.open_post = m_open;
    m->t.read = m_read;
    m->t.status = m_status;
    m->t.close = m_close;
    m->t.ctx = m;
    m->chunk_delay_ms = 30;
}

pi_transport_t *pi_mock_transport(pi_mock_t *m) { return &m->t; }

const char *pi_mock_find_header(const pi_mock_t *m, const char *name) {
    size_t nlen = strlen(name);
    for (size_t i = 0; i < m->last_header_count; i++) {
        const char *h = m->last_headers[i];
        if (strncasecmp(h, name, nlen) == 0 && h[nlen] == ':') {
            const char *v = h + nlen + 1;
            while (*v == ' ') v++;
            return v;
        }
    }
    return NULL;
}

void pi_mock_deinit(pi_mock_t *m) {
    free(m->last_body);
    m->last_body = NULL;
    free_headers(m);
}
