// SPDX-License-Identifier: Apache-2.0
//
// fork/exec, kill, waitpid and sockets are POSIX, not C11: the feature macro
// comes before the first system header (platform/host/host_time.c says why).
#define _POSIX_C_SOURCE 200809L

#include "mqtt_test_broker.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define POLL_MS 20u
#define STOP_MS 2000u
#define NS_PER_MS 1000000L

static void nap(void)
{
    const struct timespec t = {.tv_sec = 0, .tv_nsec = (long)POLL_MS * NS_PER_MS};
    (void)nanosleep(&t, NULL);
}

static struct sockaddr_in loopback(uint16_t port)
{
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return a;
}

// A port nothing listens on now: the kernel's choice, given back at once.
static uint16_t free_port(void)
{
    const int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        return 0u;
    }
    struct sockaddr_in a = loopback(0u);
    socklen_t len = (socklen_t)sizeof(a);
    uint16_t port = 0u;
    if (bind(s, (struct sockaddr *)&a, (socklen_t)sizeof(a)) == 0 &&
        getsockname(s, (struct sockaddr *)&a, &len) == 0) {
        port = ntohs(a.sin_port);
    }
    (void)close(s);
    return port;
}

static bool accepts(uint16_t port)
{
    const int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        return false;
    }
    const struct sockaddr_in a = loopback(port);
    const bool ok = connect(s, (const struct sockaddr *)&a, (socklen_t)sizeof(a)) == 0;
    (void)close(s);
    return ok;
}

bool mqtt_test_broker_start(mqtt_test_broker_t *b, const char *binary, const char *work_dir,
                            char *why, size_t why_cap)
{
    memset(b, 0, sizeof(*b));
    b->port = free_port();
    if (b->port == 0u) {
        (void)snprintf(why, why_cap, "no free port on 127.0.0.1");
        return false;
    }
    char conf_path[MQTT_TEST_BROKER_PATH_MAX];
    (void)snprintf(conf_path, sizeof(conf_path), "%s/mosquitto-%u.conf", work_dir,
                   (unsigned)b->port);
    (void)snprintf(b->log_path, sizeof(b->log_path), "%s/mosquitto-%u.log", work_dir,
                   (unsigned)b->port);
    FILE *conf = fopen(conf_path, "w");
    if (conf == NULL) {
        (void)snprintf(why, why_cap, "cannot write %s", conf_path);
        return false;
    }
    (void)fprintf(conf,
                  "listener %u 127.0.0.1\n"
                  "allow_anonymous true\n"
                  "persistence false\n"
                  "log_dest file %s\n"
                  "log_type all\n",
                  (unsigned)b->port, b->log_path);
    (void)fclose(conf);

    const pid_t pid = fork();
    if (pid < 0) {
        (void)snprintf(why, why_cap, "fork: %s", strerror(errno));
        return false;
    }
    if (pid == 0) {
        execl(binary, "mosquitto", "-c", conf_path, (char *)NULL);
        _exit(127);
    }
    b->pid = (long)pid;

    for (unsigned waited = 0u; waited < MQTT_TEST_BROKER_START_MS; waited += POLL_MS) {
        int status = 0;
        if (waitpid(pid, &status, WNOHANG) == pid) {
            b->pid = 0;
            (void)snprintf(why, why_cap, "%s exited at once (status %d); its log is %s", binary,
                           status, b->log_path);
            return false;
        }
        if (accepts(b->port)) {
            return true;
        }
        nap();
    }
    mqtt_test_broker_stop(b);
    (void)snprintf(why, why_cap, "%s did not accept a connection on port %u within %u ms", binary,
                   (unsigned)b->port, MQTT_TEST_BROKER_START_MS);
    return false;
}

void mqtt_test_broker_stop(mqtt_test_broker_t *b)
{
    if (b->pid == 0) {
        return;
    }
    const pid_t pid = (pid_t)b->pid;
    (void)kill(pid, SIGTERM);
    for (unsigned waited = 0u; waited < STOP_MS; waited += POLL_MS) {
        int status = 0;
        if (waitpid(pid, &status, WNOHANG) == pid) {
            b->pid = 0;
            return;
        }
        nap();
    }
    (void)kill(pid, SIGKILL);
    int status = 0;
    (void)waitpid(pid, &status, 0);
    b->pid = 0;
}
