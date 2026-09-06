#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Pure policy shared with host tests. Elapsed subtraction tolerates wrap. */
#define KOYODA_WIFI_CONNECT_TIMEOUT_MS 30000U
typedef enum {
    WIFI_RETRY_WAIT_START, WIFI_RETRY_CONNECTING,
    WIFI_RETRY_CONNECTED, WIFI_RETRY_WAIT
} wifi_retry_phase_t;
typedef enum {
    WIFI_RETRY_NONE, WIFI_RETRY_CONNECT,
    WIFI_RETRY_TIMEOUT, WIFI_RETRY_START_TIMEOUT
} wifi_retry_action_t;
typedef struct {
    wifi_retry_phase_t phase;
    uint32_t since_ms, delay_ms;
    unsigned failures;
} koyoda_wifi_retry_t;

static inline koyoda_wifi_retry_t wifi_retry_init(uint32_t now)
{
    return (koyoda_wifi_retry_t){.phase = WIFI_RETRY_WAIT_START, .since_ms = now};
}
static inline void wifi_retry_begin(koyoda_wifi_retry_t *r, uint32_t now)
{
    r->phase = WIFI_RETRY_CONNECTING;
    r->since_ms = now;
}
static inline uint32_t wifi_retry_schedule(koyoda_wifi_retry_t *r, uint32_t now)
{
    static const uint32_t delays[] = {5000U, 10000U, 20000U, 30000U};
    if (r->phase == WIFI_RETRY_WAIT) return 0;
    r->phase = WIFI_RETRY_WAIT;
    r->since_ms = now;
    r->delay_ms = delays[r->failures];
    if (r->failures < 3) r->failures++;
    return r->delay_ms;
}
static inline bool wifi_retry_connected(koyoda_wifi_retry_t *r)
{
    if (r->phase != WIFI_RETRY_CONNECTING && r->phase != WIFI_RETRY_CONNECTED) return false;
    r->phase = WIFI_RETRY_CONNECTED;
    r->failures = 0;
    return true;
}
static inline wifi_retry_action_t wifi_retry_poll(koyoda_wifi_retry_t *r, uint32_t now)
{
    uint32_t elapsed = now - r->since_ms;
    if (r->phase == WIFI_RETRY_WAIT && elapsed >= r->delay_ms) {
        wifi_retry_begin(r, now);
        return WIFI_RETRY_CONNECT;
    }
    if (r->phase == WIFI_RETRY_CONNECTING && elapsed >= KOYODA_WIFI_CONNECT_TIMEOUT_MS)
        return WIFI_RETRY_TIMEOUT;
    if (r->phase == WIFI_RETRY_WAIT_START && elapsed >= KOYODA_WIFI_CONNECT_TIMEOUT_MS)
        return WIFI_RETRY_START_TIMEOUT;
    return WIFI_RETRY_NONE;
}
