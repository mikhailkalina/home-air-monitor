// SPDX-License-Identifier: Apache-2.0

#include "port_telemetry_contract.h"

#include <stddef.h>
#include <string.h>

// The step in which the runner lets time pass while it waits.
#define STEP_MS 100u

// Calls timed for the non-blocking checks.
#define TIMED_CALLS 16u

// Messages one wait collects at most.
#define MAX_RX 8u

// Static rather than on the stack: a received message is some 400 bytes, and
// the board's test runner has a task's stack, not a PC's.
static telemetry_message_t s_rx[MAX_RX];
static uint8_t s_big[PORT_TELEMETRY_PAYLOAD_MAX];

typedef struct {
    char live_filter[PORT_TELEMETRY_TOPIC_MAX];
    char live[PORT_TELEMETRY_TOPIC_MAX];
    char w_filter[PORT_TELEMETRY_TOPIC_MAX];
    char w_match[PORT_TELEMETRY_TOPIC_MAX];
    char w_other[PORT_TELEMETRY_TOPIC_MAX];
    char w_deeper[PORT_TELEMETRY_TOPIC_MAX];
    char keep_filter[PORT_TELEMETRY_TOPIC_MAX];
    char keep[PORT_TELEMETRY_TOPIC_MAX];
    char will[PORT_TELEMETRY_TOPIC_MAX];
    char q_filter[PORT_TELEMETRY_TOPIC_MAX];
    char q[PORT_TELEMETRY_TOPIC_MAX];
    char plus[PORT_TELEMETRY_TOPIC_MAX];
    char hash[PORT_TELEMETRY_TOPIC_MAX];
    char bad_plus[PORT_TELEMETRY_TOPIC_MAX];
    char bad_hash[PORT_TELEMETRY_TOPIC_MAX];
} topics_t;

static topics_t s_topics;

static void check(contract_report_fn report, void *ctx, unsigned *failures, const char *name,
                  bool ok)
{
    report(ctx, name, ok);
    if (!ok) {
        (*failures)++;
    }
}

static uint64_t now(const port_telemetry_contract_env_t *env)
{
    return env->clock->now_ms(env->clock);
}

// `root` + `suffix` into `dst`, which holds PORT_TELEMETRY_TOPIC_MAX.
static void join(char *dst, const char *root, const char *suffix)
{
    const size_t r = strlen(root);
    const size_t s = strlen(suffix);
    memcpy(dst, root, r);
    memcpy(dst + r, suffix, s + 1u);
}

static bool root_ok(const char *root)
{
    if (root == NULL) {
        return false;
    }
    const size_t n = strlen(root);
    return n > 0u && n <= PORT_TELEMETRY_CONTRACT_ROOT_MAX && root[n - 1u] != '/' &&
           port_telemetry_topic_ok(root);
}

static void build_topics(const char *root)
{
    topics_t *t = &s_topics;
    join(t->live_filter, root, "/live/#");
    join(t->live, root, "/live/a");
    join(t->w_filter, root, "/w/+/x");
    join(t->w_match, root, "/w/a/x");
    join(t->w_other, root, "/w/a/y");
    join(t->w_deeper, root, "/w/a/b/x");
    join(t->keep_filter, root, "/keep/#");
    join(t->keep, root, "/keep/a");
    join(t->will, root, "/will");
    join(t->q_filter, root, "/q/#");
    join(t->q, root, "/q/n");
    join(t->plus, root, "/+");
    join(t->hash, root, "/#");
    join(t->bad_plus, root, "/a+");
    join(t->bad_hash, root, "/#/x");
}

static hal_status_t pub(port_telemetry_t *t, const char *topic, const char *text, bool retain)
{
    return t->publish(t, topic, (const uint8_t *)text, strlen(text), 1u, retain);
}

static bool is(const telemetry_message_t *m, const char *topic, const char *text)
{
    const size_t n = strlen(text);
    return strcmp(m->topic, topic) == 0 && m->len == n && !m->truncated &&
           (n == 0u || memcmp(m->payload, text, n) == 0);
}

// Everything waiting now, into s_rx; how many there were.
static size_t drain(port_telemetry_t *t)
{
    size_t n = 0u;
    telemetry_message_t m;
    // Bounded: an implementation that never runs dry fails the counts rather
    // than hanging the runner.
    for (size_t guard = 0u; guard < 4u * PORT_TELEMETRY_CONTRACT_MAX_QUEUE && t->poll(t, &m);
         ++guard) {
        if (n < MAX_RX) {
            s_rx[n] = m;
        }
        ++n;
    }
    return n;
}

// Lets time pass until `want` messages have come, or the delivery time is
// up, then a little longer for any that should not come; how many came.
static size_t collect(port_telemetry_t *t, const port_telemetry_contract_env_t *env, size_t want)
{
    size_t n = 0u;
    telemetry_message_t m;
    for (uint64_t waited = 0u;; waited += STEP_MS) {
        while (t->poll(t, &m)) {
            if (n < MAX_RX) {
                s_rx[n] = m;
            }
            ++n;
        }
        if ((want > 0u && n >= want) || waited >= PORT_TELEMETRY_CONTRACT_DELIVERY_MS) {
            break;
        }
        env->advance(env->ctx, STEP_MS);
    }
    env->advance(env->ctx, STEP_MS);
    while (t->poll(t, &m)) {
        if (n < MAX_RX) {
            s_rx[n] = m;
        }
        ++n;
    }
    return n;
}

static bool wait_connected(port_telemetry_t *t, const port_telemetry_contract_env_t *env, bool want,
                           uint64_t within_ms)
{
    for (uint64_t waited = 0u;; waited += STEP_MS) {
        if (t->is_connected(t) == want) {
            return true;
        }
        if (waited >= within_ms) {
            return false;
        }
        env->advance(env->ctx, STEP_MS);
    }
}

static telemetry_session_t session_of(const port_telemetry_contract_env_t *env)
{
    static const uint8_t gone[] = "gone";
    telemetry_session_t s;
    memset(&s, 0, sizeof(s));
    s.client_id = env->client_id;
    s.keepalive_s = (uint16_t)PORT_TELEMETRY_CONTRACT_KEEPALIVE_S;
    s.will_topic = s_topics.will;
    s.will_payload = gone;
    s.will_len = sizeof(gone) - 1u;
    s.will_qos = 1u;
    s.will_retain = true;
    return s;
}

static bool connect_and_wait(port_telemetry_t *t, const port_telemetry_contract_env_t *env)
{
    const telemetry_session_t s = session_of(env);
    return t->connect(t, &s) == HAL_OK &&
           wait_connected(t, env, true, PORT_TELEMETRY_CONTRACT_CONNECT_MS);
}

static void not_exercised(contract_report_fn report, void *ctx, unsigned *failures,
                          const char *const *checks, size_t n)
{
    for (size_t i = 0u; i < n; ++i) {
        check(report, ctx, failures, checks[i], true);
    }
}

// --- the parts -------------------------------------------------------------------

static void without_a_session(port_telemetry_t *t, const port_telemetry_contract_env_t *env,
                              contract_report_fn report, void *ctx, unsigned *failures)
{
    check(report, ctx, failures, "before connect: not connected", !t->is_connected(t));
    check(report, ctx, failures, "before connect: publish() is HAL_ERR_NOT_READY",
          pub(t, s_topics.live, "x", false) == HAL_ERR_NOT_READY);
    check(report, ctx, failures, "before connect: subscribe() is HAL_ERR_NOT_READY",
          t->subscribe(t, s_topics.live_filter, 1u) == HAL_ERR_NOT_READY);
    check(report, ctx, failures, "before connect: nothing to poll", drain(t) == 0u);

    const uint64_t before = now(env);
    for (unsigned i = 0u; i < TIMED_CALLS; ++i) {
        (void)pub(t, s_topics.live, "x", false);
        (void)t->is_connected(t);
        telemetry_message_t m;
        (void)t->poll(t, &m);
    }
    check(report, ctx, failures, "without a session, nothing waits",
          now(env) - before <= (uint64_t)PORT_TELEMETRY_CONTRACT_MAX_CALL_MS);
}

static void arguments(port_telemetry_t *t, contract_report_fn report, void *ctx, unsigned *failures)
{
    const uint8_t x = 'x';
    check(report, ctx, failures, "publish(): a NULL or empty topic is HAL_ERR_INVALID_ARG",
          t->publish(t, NULL, &x, 1u, 1u, false) == HAL_ERR_INVALID_ARG &&
              t->publish(t, "", &x, 1u, 1u, false) == HAL_ERR_INVALID_ARG);
    check(report, ctx, failures, "publish(): a wildcard in the topic is HAL_ERR_INVALID_ARG",
          t->publish(t, s_topics.plus, &x, 1u, 1u, false) == HAL_ERR_INVALID_ARG &&
              t->publish(t, s_topics.hash, &x, 1u, 1u, false) == HAL_ERR_INVALID_ARG);
    check(report, ctx, failures, "publish(): QoS above 2 is HAL_ERR_INVALID_ARG",
          t->publish(t, s_topics.live, &x, 1u, 3u, false) == HAL_ERR_INVALID_ARG);
    check(report, ctx, failures, "publish(): no data with a length is HAL_ERR_INVALID_ARG",
          t->publish(t, s_topics.live, NULL, 1u, 1u, false) == HAL_ERR_INVALID_ARG);
    memset(s_big, 'b', sizeof(s_big));
    check(report, ctx, failures,
          "publish(): PORT_TELEMETRY_PAYLOAD_MAX bytes is HAL_ERR_INVALID_ARG",
          t->publish(t, s_topics.live, s_big, sizeof(s_big), 1u, false) == HAL_ERR_INVALID_ARG);
    check(report, ctx, failures, "subscribe(): a malformed filter is HAL_ERR_INVALID_ARG",
          t->subscribe(t, s_topics.bad_plus, 1u) == HAL_ERR_INVALID_ARG &&
              t->subscribe(t, s_topics.bad_hash, 1u) == HAL_ERR_INVALID_ARG &&
              t->subscribe(t, "", 1u) == HAL_ERR_INVALID_ARG &&
              t->subscribe(t, NULL, 1u) == HAL_ERR_INVALID_ARG);
    check(report, ctx, failures, "subscribe(): QoS above 2 is HAL_ERR_INVALID_ARG",
          t->subscribe(t, s_topics.live_filter, 3u) == HAL_ERR_INVALID_ARG);
}

static void delivery(port_telemetry_t *t, const port_telemetry_contract_env_t *env,
                     contract_report_fn report, void *ctx, unsigned *failures)
{
    check(report, ctx, failures, "subscribe() in a session is HAL_OK",
          t->subscribe(t, s_topics.live_filter, 1u) == HAL_OK);
    (void)collect(t, env, 0u);  // nothing is kept under live/, but a broker may hold leftovers

    uint8_t buf[3] = {'o', 'n', 'e'};
    const hal_status_t st = t->publish(t, s_topics.live, buf, sizeof(buf), 1u, false);
    memset(buf, 'X', sizeof(buf));  // the caller's buffer, reused at once
    size_t n = collect(t, env, 1u);
    check(report, ctx, failures, "publish() in a session is HAL_OK", st == HAL_OK);
    check(report, ctx, failures, "a message published reaches a subscription, once", n == 1u);
    check(report, ctx, failures, "delivered to its own topic, not marked retained",
          n >= 1u && strcmp(s_rx[0].topic, s_topics.live) == 0 && !s_rx[0].retained);
    check(report, ctx, failures, "publish() copies the payload before it returns",
          n >= 1u && is(&s_rx[0], s_topics.live, "one"));

    (void)pub(t, s_topics.live, "1", false);
    (void)pub(t, s_topics.live, "2", false);
    (void)pub(t, s_topics.live, "3", false);
    n = collect(t, env, 3u);
    check(report, ctx, failures, "messages arrive in the order published",
          n == 3u && is(&s_rx[0], s_topics.live, "1") && is(&s_rx[1], s_topics.live, "2") &&
              is(&s_rx[2], s_topics.live, "3"));

    const hal_status_t empty = t->publish(t, s_topics.live, NULL, 0u, 1u, false);
    n = collect(t, env, 1u);
    check(report, ctx, failures, "an empty payload is a message",
          empty == HAL_OK && n == 1u && is(&s_rx[0], s_topics.live, ""));

    const uint64_t before = now(env);
    for (unsigned i = 0u; i < TIMED_CALLS; ++i) {
        (void)pub(t, s_topics.live, "t", false);
        telemetry_message_t m;
        (void)t->poll(t, &m);
    }
    check(report, ctx, failures, "publish() and poll() never wait",
          now(env) - before <= (uint64_t)PORT_TELEMETRY_CONTRACT_MAX_CALL_MS);
    (void)collect(t, env, 0u);

    check(report, ctx, failures, "subscribe() with '+' is HAL_OK",
          t->subscribe(t, s_topics.w_filter, 1u) == HAL_OK);
    (void)pub(t, s_topics.w_other, "other", false);
    (void)pub(t, s_topics.w_deeper, "deeper", false);
    (void)pub(t, s_topics.w_match, "match", false);
    n = collect(t, env, 1u);
    check(report, ctx, failures, "'+' matches one whole level, no more",
          n == 1u && is(&s_rx[0], s_topics.w_match, "match"));
}

static void retained(port_telemetry_t *t, const port_telemetry_contract_env_t *env,
                     contract_report_fn report, void *ctx, unsigned *failures)
{
    (void)pub(t, s_topics.keep, "kept", true);
    check(report, ctx, failures, "a retained message with no subscription reaches nobody",
          collect(t, env, 0u) == 0u);

    (void)t->subscribe(t, s_topics.keep_filter, 1u);
    size_t n = collect(t, env, 1u);
    check(report, ctx, failures, "a retained message reaches a new subscription, marked retained",
          n == 1u && is(&s_rx[0], s_topics.keep, "kept") && s_rx[0].retained);

    (void)t->subscribe(t, s_topics.keep_filter, 1u);
    n = collect(t, env, 1u);
    check(report, ctx, failures, "the same filter again: what is kept comes again",
          n == 1u && is(&s_rx[0], s_topics.keep, "kept") && s_rx[0].retained);

    (void)t->publish(t, s_topics.keep, NULL, 0u, 1u, true);
    n = collect(t, env, 1u);
    check(report, ctx, failures, "an empty retained message reaches a subscription live",
          n == 1u && is(&s_rx[0], s_topics.keep, "") && !s_rx[0].retained);
    (void)t->subscribe(t, s_topics.keep_filter, 1u);
    check(report, ctx, failures, "an empty retained message clears what was kept",
          collect(t, env, 0u) == 0u);
}

static void disconnecting(port_telemetry_t *t, const port_telemetry_contract_env_t *env,
                          contract_report_fn report, void *ctx, unsigned *failures)
{
    const uint64_t before = now(env);
    const hal_status_t st = t->disconnect(t);
    check(report, ctx, failures, "disconnect() returns without waiting",
          now(env) - before <= (uint64_t)PORT_TELEMETRY_CONTRACT_MAX_CALL_MS);
    check(report, ctx, failures, "disconnect(): not connected once it returns",
          st == HAL_OK && !t->is_connected(t));
    check(report, ctx, failures, "after disconnect(): publish() is HAL_ERR_NOT_READY",
          pub(t, s_topics.live, "x", false) == HAL_ERR_NOT_READY);
    check(report, ctx, failures, "disconnect() again: HAL_OK", t->disconnect(t) == HAL_OK);

    const bool again = connect_and_wait(t, env);
    check(report, ctx, failures, "a new session after disconnect()", again);
    (void)pub(t, s_topics.live, "old subscription?", false);
    check(report, ctx, failures, "a new session starts with no subscription",
          collect(t, env, 0u) == 0u);
    (void)t->subscribe(t, s_topics.will, 1u);
    check(report, ctx, failures, "a clean disconnect leaves no will", collect(t, env, 0u) == 0u);
}

static void dropping(port_telemetry_t *t, const port_telemetry_contract_env_t *env,
                     contract_report_fn report, void *ctx, unsigned *failures)
{
    static const char *const checks[] = {
        "a dropped session: not connected: not exercised by this runner",
        "a dropped session: publish() is HAL_ERR_NOT_READY: not exercised by this runner",
        "a dropped session: its will is published: not exercised by this runner",
    };
    if (env->cut == NULL || env->restore == NULL) {
        not_exercised(report, ctx, failures, checks, sizeof(checks) / sizeof(checks[0]));
        return;
    }
    env->cut(env->ctx);
    check(report, ctx, failures, "a dropped session: not connected",
          wait_connected(t, env, false, PORT_TELEMETRY_CONTRACT_NOTICE_MS));
    check(report, ctx, failures, "a dropped session: publish() is HAL_ERR_NOT_READY",
          pub(t, s_topics.live, "x", false) == HAL_ERR_NOT_READY);
    // Past the broker's one and a half keepalives, and a little.
    env->advance(env->ctx, (uint64_t)PORT_TELEMETRY_CONTRACT_KEEPALIVE_S * 1500u +
                               PORT_TELEMETRY_CONTRACT_DELIVERY_MS);
    env->restore(env->ctx);
    (void)t->disconnect(t);  // whatever the adapter still holds
    const bool again = connect_and_wait(t, env);
    (void)t->subscribe(t, s_topics.will, 1u);
    const size_t n = collect(t, env, 1u);
    check(report, ctx, failures, "a dropped session: its will is published",
          again && n == 1u && is(&s_rx[0], s_topics.will, "gone") && s_rx[0].retained);
}

static void full_queue(port_telemetry_t *t, const port_telemetry_contract_env_t *env,
                       contract_report_fn report, void *ctx, unsigned *failures)
{
    static const char *const checks[] = {
        "a full queue: publish() is HAL_ERR_NO_MEM: not exercised by this runner",
        "a full queue: what it took is delivered, in order: not exercised by this runner",
    };
    if (env->stall == NULL) {
        not_exercised(report, ctx, failures, checks, sizeof(checks) / sizeof(checks[0]));
        return;
    }
    (void)t->subscribe(t, s_topics.q_filter, 1u);
    (void)collect(t, env, 0u);
    env->stall(env->ctx, true);
    unsigned accepted = 0u;
    // One buffer for every message, reused, as a caller's would be: what the
    // adapter took must be its own copy.
    uint8_t seq[2];
    hal_status_t st = HAL_OK;
    while (accepted < PORT_TELEMETRY_CONTRACT_MAX_QUEUE) {
        seq[0] = (uint8_t)(accepted & 0xFFu);
        seq[1] = (uint8_t)(accepted >> 8u);
        st = t->publish(t, s_topics.q, seq, sizeof(seq), 1u, false);
        if (st != HAL_OK) {
            break;
        }
        ++accepted;
    }
    check(report, ctx, failures, "a full queue: publish() is HAL_ERR_NO_MEM",
          st == HAL_ERR_NO_MEM && accepted > 0u);
    env->stall(env->ctx, false);
    env->advance(env->ctx, PORT_TELEMETRY_CONTRACT_DELIVERY_MS);
    // Counted, and checked in order, without keeping them.
    unsigned got = 0u;
    bool in_order = true;
    telemetry_message_t m;
    for (unsigned guard = 0u; guard < 4u * PORT_TELEMETRY_CONTRACT_MAX_QUEUE && t->poll(t, &m);
         ++guard) {
        in_order = in_order && m.len == 2u && m.payload[0] == (uint8_t)(got & 0xFFu) &&
                   m.payload[1] == (uint8_t)(got >> 8u);
        ++got;
    }
    check(report, ctx, failures, "a full queue: what it took is delivered, in order",
          got == accepted && in_order);
}

unsigned port_telemetry_contract_run(port_telemetry_t *t, const port_telemetry_contract_env_t *env,
                                     contract_report_fn report, void *report_ctx)
{
    unsigned failures = 0u;
    if (!root_ok(env->topic_root)) {
        check(report, report_ctx, &failures, "the runner's topic root is usable", false);
        return failures;
    }
    build_topics(env->topic_root);

    without_a_session(t, env, report, report_ctx, &failures);

    telemetry_session_t nameless = session_of(env);
    nameless.client_id = NULL;
    check(report, report_ctx, &failures, "connect(): no client id is HAL_ERR_INVALID_ARG",
          t->connect(t, &nameless) == HAL_ERR_INVALID_ARG);

    const telemetry_session_t s = session_of(env);
    const uint64_t before = now(env);
    const hal_status_t st = t->connect(t, &s);
    check(report, report_ctx, &failures, "connect() returns without waiting",
          now(env) - before <= (uint64_t)PORT_TELEMETRY_CONTRACT_MAX_CALL_MS);
    check(report, report_ctx, &failures, "connect() is HAL_OK", st == HAL_OK);
    const bool connected = wait_connected(t, env, true, PORT_TELEMETRY_CONTRACT_CONNECT_MS);
    check(report, report_ctx, &failures, "connected within PORT_TELEMETRY_CONTRACT_CONNECT_MS",
          connected);
    if (!connected) {
        return failures;  // nothing else can be checked without a session
    }
    check(report, report_ctx, &failures, "connect() while connected is HAL_ERR_BUSY",
          t->connect(t, &s) == HAL_ERR_BUSY && t->is_connected(t));

    // What an earlier run against the same broker may have left kept.
    (void)t->publish(t, s_topics.keep, NULL, 0u, 1u, true);
    (void)t->publish(t, s_topics.will, NULL, 0u, 1u, true);

    arguments(t, report, report_ctx, &failures);
    delivery(t, env, report, report_ctx, &failures);
    retained(t, env, report, report_ctx, &failures);
    disconnecting(t, env, report, report_ctx, &failures);
    dropping(t, env, report, report_ctx, &failures);
    full_queue(t, env, report, report_ctx, &failures);

    // Leave the broker as it was found: the will kept, cleared.
    (void)t->publish(t, s_topics.will, NULL, 0u, 1u, true);
    (void)collect(t, env, 0u);
    check(report, report_ctx, &failures, "nothing left over", collect(t, env, 0u) == 0u);
    (void)t->disconnect(t);
    return failures;
}
