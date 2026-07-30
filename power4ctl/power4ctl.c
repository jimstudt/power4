/*
 * power4ctl — serial and authenticated TCP management tool for power4
 *
 * Usage: power4ctl [-p port] [-b baud] [-t seconds] [-v] command
 *        power4ctl [-p port] [-b baud] [-t seconds] [-v]
 *        power4ctl [-p port] [-b baud] [-t seconds] [-v] \
 *                  -D [-i interval] [-l lock-seconds] [-o outdir]
 *
 * Commands:
 *   json batteries / banks / inputs / logs / parameters / relays
 *   stage <filename>
 *   <anything else>   sent verbatim; output echoed to stdout
 *
 * Interactive (REPL) mode — invoked with no command argument:
 *   Reads commands from stdin with libedit line editing and history.
 *   The selected transport is opened per command and closed between prompts.
 *   "exit" or "quit" (or Ctrl-D) end the session.
 *
 * Daemon mode (-D):
 *   Loops forever, opening the selected transport each cycle, collecting JSON
 *   reports (batteries, banks, relays, inputs, parameters, logs), writing them atomically to
 *   the output directory, then closing the port and sleeping until the
 *   next interval.
 *
 * flock(LOCK_EX|LOCK_NB) + TIOCEXCL prevent concurrent access by separate
 * invocations.
 */

#include <ctype.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <histedit.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "sha1.h"
#include "sha256.h"

#define NETWORK_CONSOLE_PORT 4244
#define PASSWORD_MIN_BYTES 16
#define PASSWORD_MAX_BYTES 128

/* ------------------------------------------------------------------ */
/* Utilities                                                            */
/* ------------------------------------------------------------------ */

static int g_verbose = 0;

/* Print bytes to stderr with a direction prefix (e.g. ">>>" or "<<<").
   Non-printable characters are shown as \n, \r, \t, or \xNN. */
static void verbose_bytes(const char *dir, const char *data, size_t len)
{
    size_t i;
    if (!g_verbose)
        return;
    fprintf(stderr, "%s ", dir);
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)data[i];
        if      (c == '\n') fputs("\\n", stderr);
        else if (c == '\r') fputs("\\r", stderr);
        else if (c == '\t') fputs("\\t", stderr);
        else if (c >= ' ' && c <= '~') fputc(c, stderr);
        else fprintf(stderr, "\\x%02x", c);
    }
    fputc('\n', stderr);
}

static struct timespec deadline_from_now(int seconds)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    ts.tv_sec += seconds;
    return ts;
}

static int deadline_passed(const struct timespec *dl)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec > dl->tv_sec ||
           (now.tv_sec == dl->tv_sec && now.tv_nsec >= dl->tv_nsec);
}

static const struct timespec *timespec_earlier(const struct timespec *a,
                                               const struct timespec *b)
{
    if (a->tv_sec < b->tv_sec ||
        (a->tv_sec == b->tv_sec && a->tv_nsec <= b->tv_nsec))
        return a;
    return b;
}

/* Wait up to dl for fd to be readable. Returns >0 ready, 0 timeout, -1 error. */
static int wait_readable(int fd, const struct timespec *dl)
{
    struct timespec now;
    fd_set rfds;
    struct timeval tv;
    long ms;

    clock_gettime(CLOCK_MONOTONIC, &now);
    ms = (dl->tv_sec  - now.tv_sec)  * 1000L +
         (dl->tv_nsec - now.tv_nsec) / 1000000L;
    if (ms <= 0)
        return 0;

    tv.tv_sec  = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);
    return select(fd + 1, &rfds, NULL, NULL, &tv);
}

static int read_line_fd(int fd,
                        const struct timespec *deadline,
                        char *line,
                        size_t capacity)
{
    size_t length = 0;

    while (length + 1 < capacity && !deadline_passed(deadline)) {
        char ch;
        ssize_t result;
        if (wait_readable(fd, deadline) <= 0)
            return 0;
        result = read(fd, &ch, 1);
        if (result <= 0)
            return 0;
        if (ch == '\n') {
            if (length > 0 && line[length - 1] == '\r')
                length--;
            line[length] = '\0';
            return 1;
        }
        line[length++] = ch;
    }
    line[capacity - 1] = '\0';
    return 0;
}

static void digest_to_hex(const uint8_t *digest, size_t length, char *hex)
{
    static const char digits[] = "0123456789abcdef";
    size_t i;
    for (i = 0; i < length; i++) {
        hex[i * 2] = digits[digest[i] >> 4];
        hex[i * 2 + 1] = digits[digest[i] & 0x0f];
    }
    hex[length * 2] = '\0';
}

/* ------------------------------------------------------------------ */
/* Serial port                                                          */
/* ------------------------------------------------------------------ */

static speed_t baud_to_speed(int baud)
{
    switch (baud) {
    case 1200:   return B1200;
    case 2400:   return B2400;
    case 4800:   return B4800;
    case 9600:   return B9600;
    case 19200:  return B19200;
    case 38400:  return B38400;
    case 57600:  return B57600;
    case 115200: return B115200;
    case 230400: return B230400;
    default:     return B0;
    }
}

/* Apply TIOCEXCL and raw 8N1 termios settings to an already-open fd.
   Returns 0 on success, -1 on error (caller must close fd). */
static int setup_serial(int fd, const char *port, int baud)
{
    struct termios tty;
    speed_t speed;

    if (ioctl(fd, TIOCEXCL) < 0) {
        fprintf(stderr, "%s: TIOCEXCL: %s\n", port, strerror(errno));
        return -1;
    }

    speed = baud_to_speed(baud);
    if (speed == B0) {
        fprintf(stderr, "unsupported baud rate: %d\n", baud);
        return -1;
    }

    if (tcgetattr(fd, &tty) < 0) {
        fprintf(stderr, "%s: tcgetattr: %s\n", port, strerror(errno));
        return -1;
    }

    cfmakeraw(&tty);
    cfsetispeed(&tty, speed);
    cfsetospeed(&tty, speed);
    tty.c_cflag |= CLOCAL | CREAD;
    tty.c_cflag &= ~CRTSCTS;
    tty.c_cflag  = (tty.c_cflag & ~CSIZE) | CS8;
    tty.c_cflag &= ~(PARENB | PARODD | CSTOPB);

    if (tcsetattr(fd, TCSANOW, &tty) < 0) {
        fprintf(stderr, "%s: tcsetattr: %s\n", port, strerror(errno));
        return -1;
    }

    tcflush(fd, TCIOFLUSH);
    return 0;
}

/* Open and configure the serial port, acquiring an exclusive lock.
   Returns fd on success, -1 on error. */
static int open_serial(const char *port, int baud)
{
    int fd;

    fd = open(port, O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "open %s: %s\n", port, strerror(errno));
        return -1;
    }

    if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
        fprintf(stderr, "%s: device busy (flock): %s\n", port, strerror(errno));
        close(fd);
        return -1;
    }

    if (setup_serial(fd, port, baud) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

/* Like open_serial but retries for up to wait_secs if the port is busy.
   Returns fd on success, -1 on timeout or hard error. */
static int open_serial_wait(const char *port, int baud, int wait_secs)
{
    struct timespec deadline = deadline_from_now(wait_secs);

    for (;;) {
        int fd = open(port, O_RDWR | O_NOCTTY | O_CLOEXEC);
        if (fd < 0) {
            fprintf(stderr, "open %s: %s\n", port, strerror(errno));
            return -1;
        }

        if (flock(fd, LOCK_EX | LOCK_NB) == 0) {
            if (setup_serial(fd, port, baud) < 0) {
                close(fd);
                return -1;
            }
            return fd;
        }

        if (errno != EWOULDBLOCK && errno != EAGAIN) {
            fprintf(stderr, "%s: flock: %s\n", port, strerror(errno));
            close(fd);
            return -1;
        }
        close(fd);

        if (deadline_passed(&deadline)) {
            fprintf(stderr, "%s: port busy after %d seconds\n", port, wait_secs);
            return -1;
        }

        /* Poll every 500 ms */
        {
            struct timespec ts = {0, 500000000L};
            nanosleep(&ts, NULL);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Protocol                                                             */
/* ------------------------------------------------------------------ */

/* Forward declaration — defined in main section below */
static const char *g_port;
static const char *g_address;
static int write_all_fd(int fd, const void *data, size_t length);

#define PROMPT          "power4> "
#define PROMPT_LEN      8
#define PROMPT_ATTEMPTS 3
/* Must hold the largest single P4J1 frame. The logs report carries a 16 KB
   log buffer that can grow to ~6x under JSON escaping, plus the frame
   header, so allow for the worst case. */
#define LINEBUF         131072
#define B64_LINE_WIDTH  76      /* 57 input bytes → 76 base64 chars per line */
#define TOOL_PREFIX     "p4exec "

enum dot_response_kind {
    DOT_RESPONSE_PASSTHROUGH,
    DOT_RESPONSE_JSON,
    DOT_RESPONSE_UPLOAD
};

static int read_dot_response(int fd,
                             const struct timespec *deadline,
                             const char *echoed_command,
                             enum dot_response_kind kind,
                             char *json_out,
                             size_t json_size);

/*
 * Wait for "power4> ". Serial mode sends a single "\r" before each attempt
 * because the tool may attach in the middle of a prompt. TCP mode already has
 * a synchronized prompt from authentication or the previous command, so it
 * only reads; injecting an extra blank command can leave a second prompt queued
 * ahead of the real command response.
 * Returns 1 on success, 0 on timeout.
 */
static int wait_for_prompt(int fd,
                           const struct timespec *deadline,
                           int request_prompt)
{
    char buf[512];
    int buflen = 0;
    int attempt;

    for (attempt = 0; attempt < PROMPT_ATTEMPTS; attempt++) {
        struct timespec now, attempt_dl;
        const struct timespec *rdl;
        long remaining_ms, slice_ms;

        if (deadline_passed(deadline))
            return 0;

        if (request_prompt) {
            verbose_bytes(">>>", "\r", 1);
            (void)write(fd, "\r", 1);
        }

        /* Give this attempt an equal share of the remaining time */
        clock_gettime(CLOCK_MONOTONIC, &now);
        remaining_ms = (deadline->tv_sec  - now.tv_sec)  * 1000L
                     + (deadline->tv_nsec - now.tv_nsec) / 1000000L;
        slice_ms = remaining_ms / (PROMPT_ATTEMPTS - attempt);
        attempt_dl.tv_sec  = now.tv_sec  + slice_ms / 1000;
        attempt_dl.tv_nsec = now.tv_nsec + (slice_ms % 1000) * 1000000L;
        if (attempt_dl.tv_nsec >= 1000000000L) {
            attempt_dl.tv_sec++;
            attempt_dl.tv_nsec -= 1000000000L;
        }
        rdl = timespec_earlier(&attempt_dl, deadline);

        while (!deadline_passed(rdl)) {
            int avail = (int)sizeof(buf) - buflen - 1;
            int n, i;
            if (avail <= 0) {
                /* Keep last PROMPT_LEN-1 bytes in case prompt straddles reads */
                memmove(buf, buf + buflen - (PROMPT_LEN - 1), PROMPT_LEN - 1);
                buflen = PROMPT_LEN - 1;
                avail  = (int)sizeof(buf) - buflen - 1;
            }
            if (wait_readable(fd, rdl) <= 0)
                break;
            n = (int)read(fd, buf + buflen, (size_t)avail);
            if (n <= 0)
                break;
            verbose_bytes("<<<", buf + buflen, (size_t)n);
            buflen += n;
            for (i = 0; i <= buflen - PROMPT_LEN; i++) {
                if (memcmp(buf + i, PROMPT, PROMPT_LEN) == 0)
                    return 1;
            }
        }
    }
    return 0;
}

/*
 * Base64-encode data and write it to fd in B64_LINE_WIDTH-char lines, each
 * terminated with "\r".  Sends a final blank line ("\r") to end the upload.
 */
static int send_base64(int fd, const uint8_t *data, size_t len)
{
    static const char b64[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char line[B64_LINE_WIDTH + 2]; /* chars + \r + \0 */
    int lpos = 0;
    size_t i = 0;

    while (i < len) {
        uint8_t a, b = 0, c = 0;
        int consumed = 1;
        uint32_t v;

        a = data[i++];
        if (i < len) { b = data[i++]; consumed++; }
        if (i < len) { c = data[i++]; consumed++; }
        int pad = 3 - consumed;

        v = ((uint32_t)a << 16) | ((uint32_t)b << 8) | c;
        line[lpos++] = b64[(v >> 18) & 0x3f];
        line[lpos++] = b64[(v >> 12) & 0x3f];
        line[lpos++] = pad >= 2 ? '=' : b64[(v >> 6) & 0x3f];
        line[lpos++] = pad >= 1 ? '=' : b64[v        & 0x3f];

        if (lpos >= B64_LINE_WIDTH || i >= len) {
            line[lpos++] = '\r';
            verbose_bytes(">>>", line, (size_t)lpos);
            if (write(fd, line, (size_t)lpos) < 0)
                return -1;
            lpos = 0;
        }
    }

    /* Blank line signals end of upload */
    verbose_bytes(">>>", "\r", 1);
    if (write(fd, "\r", 1) < 0)
        return -1;

    return 0;
}

/*
 * Read filename, compute SHA-1, issue the tool-wrapped policy upload command,
 * send the base64 body followed by a blank line, then read its dot frame.
 * Returns 1 on success, -1 on error, 0 on timeout.
 */
static int do_stage(int fd, const char *filename, const struct timespec *deadline)
{
    FILE *f;
    long size;
    uint8_t *data;
    char sha1[41];
    char command[80];
    char wire[82];
    int cmdlen, result;

    f = fopen(filename, "rb");
    if (!f) {
        fprintf(stderr, "%s: %s\n", filename, strerror(errno));
        return -1;
    }
    if (fseek(f, 0, SEEK_END) < 0 || (size = ftell(f)) < 0) {
        fprintf(stderr, "%s: cannot determine size: %s\n", filename, strerror(errno));
        fclose(f);
        return -1;
    }
    rewind(f);

    data = (uint8_t *)malloc((size_t)size);
    if (!data) {
        fprintf(stderr, "out of memory\n");
        fclose(f);
        return -1;
    }
    if (fread(data, 1, (size_t)size, f) != (size_t)size) {
        fprintf(stderr, "%s: read error\n", filename);
        free(data);
        fclose(f);
        return -1;
    }
    fclose(f);

    sha1_hex_of(data, (size_t)size, sha1);

    snprintf(command, sizeof(command), TOOL_PREFIX "policy upload %s", sha1);
    cmdlen = snprintf(wire, sizeof(wire), "%s\r", command);
    verbose_bytes(">>>", wire, (size_t)cmdlen);
    if (write_all_fd(fd, wire, (size_t)cmdlen) != 0) {
        fprintf(stderr, "%s: write: %s\n", g_port, strerror(errno));
        free(data);
        return -1;
    }

    if (send_base64(fd, data, (size_t)size) < 0) {
        fprintf(stderr, "%s: write error during upload\n", g_port);
        free(data);
        return -1;
    }
    free(data);

    result = read_dot_response(fd,
                               deadline,
                               g_address == NULL ? command : NULL,
                               DOT_RESPONSE_UPLOAD,
                               NULL,
                               0);
    if (result == 0)
        fprintf(stderr, "%s: timed out waiting for upload response\n", g_port);
    return result;
}

/*
 * Parse one line for the P4J1 framed-JSON format:
 *   P4J1 <decimal-size> <40-char-sha1-hex> <json>
 *
 * Validates size and SHA-1.  If json_out is non-NULL the JSON is copied there;
 * otherwise it is printed to stdout.
 * Returns 1 on success, 0 if not a P4J1 line, -1 on validation error.
 */
static int parse_p4j1(const char *line, char *json_out, size_t json_size)
{
    char sha1_expected[41], sha1_actual[41];
    unsigned long size;
    const char *p;
    char *end;
    size_t json_len;
    int i;

    if (strncmp(line, "P4J1 ", 5) != 0)
        return 0;
    p = line + 5;

    size = strtoul(p, &end, 10);
    if (end == p || *end != ' ')
        return 0;
    p = end + 1;

    if (strlen(p) < 41 || p[40] != ' ')
        return 0;
    for (i = 0; i < 40; i++) {
        if (!isxdigit((unsigned char)p[i]))
            return 0;
    }
    memcpy(sha1_expected, p, 40);
    sha1_expected[40] = '\0';
    p += 41;

    json_len = strlen(p);
    if (json_len != (size_t)size) {
        fprintf(stderr, "P4J1: size mismatch (expected %lu, got %zu)\n", size, json_len);
        return -1;
    }

    sha1_hex_of(p, json_len, sha1_actual);
    if (strcmp(sha1_expected, sha1_actual) != 0) {
        fprintf(stderr, "P4J1: SHA-1 mismatch (expected %s, got %s)\n",
                sha1_expected, sha1_actual);
        return -1;
    }

    if (json_out != NULL)
        snprintf(json_out, json_size, "%s", p);
    else
        puts(p);
    return 1;
}

/*
 * Read one SMTP-style dot-stuffed command response. The firmware always
 * terminates the response with "." on a line by itself. Reading exactly one
 * line at a time deliberately leaves the following console prompt queued for
 * the next command in a persistent session.
 */
static int read_dot_response(int fd,
                             const struct timespec *deadline,
                             const char *echoed_command,
                             enum dot_response_kind kind,
                             char *json_out,
                             size_t json_size)
{
    static char line[LINEBUF];
    int status = 0;
    int echo_pending = echoed_command != NULL;

    while (!deadline_passed(deadline)) {
        int parsed;
        if (!read_line_fd(fd, deadline, line, sizeof(line)))
            return 0;
        verbose_bytes("<<<", line, strlen(line));

        if (echo_pending && strcmp(line, echoed_command) == 0) {
            echo_pending = 0;
            continue;
        }
        echo_pending = 0;

        if (strcmp(line, ".") == 0) {
            if (kind == DOT_RESPONSE_PASSTHROUGH)
                return 1;
            if (kind == DOT_RESPONSE_UPLOAD)
                return status == 0 ? 1 : status;
            return status;
        }
        if (line[0] == '.') {
            if (line[1] != '.')
                return -1;
            memmove(line, line + 1, strlen(line));
        }

        if (kind == DOT_RESPONSE_PASSTHROUGH) {
            puts(line);
            continue;
        }
        if (kind == DOT_RESPONSE_JSON) {
            parsed = parse_p4j1(line, json_out, json_size);
            if (parsed != 0)
                status = parsed;
            continue;
        }
        if (strstr(line, "uploaded staged configuration:") != NULL) {
            puts(line);
            status = 1;
        } else if (status == 0 && strstr(line, " failed:") != NULL) {
            fprintf(stderr, "%s\n", line);
            status = -1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Command dispatch                                                     */
/* ------------------------------------------------------------------ */

static const char *g_port         = "/dev/ttyACM0";
static const char *g_address      = NULL;
static const char *g_password_env = NULL;
static const char *g_password_file = NULL;
static char        g_password[PASSWORD_MAX_BYTES + 1];
static int         g_port_explicit = 0;
static int         g_baud         = 115200;
static int         g_timeout      = 2;
static int         g_daemon       = 0;
static int         g_interval     = 60;
static int         g_lock_timeout = 5;
static const char *g_outdir       = "/run/power4";

static int write_all_fd(int fd, const void *data, size_t length)
{
    const uint8_t *cursor = (const uint8_t *)data;
    size_t written = 0;
    while (written < length) {
        ssize_t result = write(fd, cursor + written, length - written);
        if (result <= 0)
            return -1;
        written += (size_t)result;
    }
    return 0;
}

static int authenticate_network(int fd)
{
    struct timespec deadline = deadline_from_now(g_timeout);
    char challenge_line[96];
    char response[96];
    char result_line[96];
    uint8_t digest[32];
    char digest_hex[65];
    static const char prefix[] = "authenticate ";
    int response_length;

    if (!read_line_fd(fd, &deadline, challenge_line, sizeof(challenge_line))) {
        fprintf(stderr, "%s: timed out waiting for authentication challenge\n", g_address);
        return -1;
    }
    if (strncmp(challenge_line, prefix, sizeof(prefix) - 1) != 0 ||
        strlen(challenge_line + sizeof(prefix) - 1) != 43) {
        fprintf(stderr, "%s: invalid authentication challenge\n", g_address);
        return -1;
    }

    hmac_sha256(g_password,
                strlen(g_password),
                challenge_line + sizeof(prefix) - 1,
                43,
                digest);
    digest_to_hex(digest, sizeof(digest), digest_hex);
    memset(digest, 0, sizeof(digest));
    response_length =
        snprintf(response, sizeof(response), "authenticate %s\r\n", digest_hex);
    memset(digest_hex, 0, sizeof(digest_hex));
    if (response_length <= 0 ||
        write_all_fd(fd, response, (size_t)response_length) != 0) {
        fprintf(stderr, "%s: authentication write failed: %s\n",
                g_address,
                strerror(errno));
        return -1;
    }
    memset(response, 0, sizeof(response));

    deadline = deadline_from_now(g_timeout);
    if (!read_line_fd(fd, &deadline, result_line, sizeof(result_line))) {
        fprintf(stderr, "%s: timed out waiting for authentication result\n", g_address);
        return -1;
    }
    if (strcmp(result_line, "authenticated") != 0) {
        fprintf(stderr, "%s: authentication failed\n", g_address);
        return -1;
    }
    // Leave the initial prompt in the socket. The normal command path consumes
    // it, just as it consumes each prompt following a command.
    return 0;
}

static int open_network(void)
{
    int fd;
    struct sockaddr_in address;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        fprintf(stderr, "socket: %s\n", strerror(errno));
        return -1;
    }
    (void)fcntl(fd, F_SETFD, FD_CLOEXEC);

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(NETWORK_CONSOLE_PORT);
    if (inet_pton(AF_INET, g_address, &address.sin_addr) != 1) {
        fprintf(stderr, "invalid IPv4 address: %s\n", g_address);
        close(fd);
        return -1;
    }
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        fprintf(stderr, "connect %s:%d: %s\n",
                g_address,
                NETWORK_CONSOLE_PORT,
                strerror(errno));
        close(fd);
        return -1;
    }
    if (authenticate_network(fd) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int open_transport(int wait_for_lock)
{
    if (g_address != NULL)
        return open_network();
    return wait_for_lock
               ? open_serial_wait(g_port, g_baud, g_lock_timeout)
               : open_serial(g_port, g_baud);
}

/*
 * Open the selected transport, execute one command, and close it.
 * cmd is the full command string, e.g. "json batteries", "stage foo.lua",
 * or any passthrough command like "show relays".
 * Returns 0 on success, 1 on error.
 */
static int run_device_command(const char *cmd)
{
    int fd, result;
    struct timespec deadline;

    if (strncmp(cmd, "stage", 5) == 0 && (cmd[5] == ' ' || cmd[5] == '\0')) {
        const char *filename = cmd + 5;
        while (*filename == ' ') filename++;
        if (*filename == '\0') {
            fprintf(stderr, "usage: stage <filename>\n");
            return 1;
        }
        fd = open_transport(0);
        if (fd < 0) return 1;
        deadline = deadline_from_now(g_timeout);
        if (!wait_for_prompt(fd, &deadline, g_address == NULL)) {
            fprintf(stderr, "%s: timed out waiting for prompt\n", g_port);
            close(fd);
            return 1;
        }
        deadline = deadline_from_now(g_timeout);
        result = do_stage(fd, filename, &deadline);
        close(fd);
        return result == 1 ? 0 : 1;
    }

    fd = open_transport(0);
    if (fd < 0) return 1;
    deadline = deadline_from_now(g_timeout);

    if (!wait_for_prompt(fd, &deadline, g_address == NULL)) {
        fprintf(stderr, "%s: timed out waiting for prompt\n", g_port);
        close(fd);
        return 1;
    }

    if (strncmp(cmd, "json ", 5) == 0) {
        char command[160];
        char wire[162];
        int llen;
        snprintf(command, sizeof(command), TOOL_PREFIX "report%s", cmd + 4);
        llen = snprintf(wire, sizeof(wire), "%s\r", command);
        verbose_bytes(">>>", wire, (size_t)llen);
        if (write_all_fd(fd, wire, (size_t)llen) != 0) {
            fprintf(stderr, "%s: write: %s\n", g_port, strerror(errno));
            close(fd);
            return 1;
        }
        deadline = deadline_from_now(g_timeout);
        result = read_dot_response(fd,
                                   &deadline,
                                   g_address == NULL ? command : NULL,
                                   DOT_RESPONSE_JSON,
                                   NULL,
                                   0);
        close(fd);
        if (result == 1) return 0;
        if (result == 0)
            fprintf(stderr, "%s: timed out waiting for response\n", g_port);
        return 1;
    }

    /* Tool command: read dot-stuffed output through its explicit terminator. */
    {
        size_t cmdlen = strlen(cmd);
        char command[1034];
        char wire[1036];
        int llen;
        if (cmdlen > sizeof(command) - sizeof(TOOL_PREFIX))
            cmdlen = sizeof(command) - sizeof(TOOL_PREFIX);
        snprintf(command, sizeof(command), TOOL_PREFIX "%.*s", (int)cmdlen, cmd);
        llen = snprintf(wire, sizeof(wire), "%s\r", command);
        verbose_bytes(">>>", wire, (size_t)llen);
        if (write_all_fd(fd, wire, (size_t)llen) != 0) {
            fprintf(stderr, "%s: write: %s\n", g_port, strerror(errno));
            close(fd);
            return 1;
        }
        deadline = deadline_from_now(g_timeout);
        result = read_dot_response(fd,
                                   &deadline,
                                   g_address == NULL ? command : NULL,
                                   DOT_RESPONSE_PASSTHROUGH,
                                   NULL,
                                   0);
    }
    close(fd);
    if (result == 0)
        fprintf(stderr, "%s: timed out waiting for response terminator\n", g_port);
    return result == 1 ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/* Daemon support                                                       */
/* ------------------------------------------------------------------ */

static volatile sig_atomic_t g_stop = 0;

static void handle_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

/* Write json (a NUL-terminated string) to dir/name.json atomically via
   dir/.tmp.name.json.  Returns 0 on success, -1 on error. */
static int write_json_atomic(const char *dir, const char *name, const char *json)
{
    char tmp_path[512], final_path[512];
    FILE *f;

    snprintf(tmp_path,   sizeof(tmp_path),   "%s/.tmp.%s.json", dir, name);
    snprintf(final_path, sizeof(final_path), "%s/%s.json",      dir, name);

    f = fopen(tmp_path, "w");
    if (!f) {
        fprintf(stderr, "%s: %s\n", tmp_path, strerror(errno));
        return -1;
    }
    if (fputs(json, f) < 0 || fputc('\n', f) < 0) {
        fprintf(stderr, "%s: write error\n", tmp_path);
        fclose(f);
        return -1;
    }
    if (fclose(f) != 0) {
        fprintf(stderr, "%s: close error: %s\n", tmp_path, strerror(errno));
        return -1;
    }
    if (rename(tmp_path, final_path) < 0) {
        fprintf(stderr, "rename %s: %s\n", tmp_path, strerror(errno));
        return -1;
    }
    return 0;
}

static const char * const REPORTS[] = {
    "batteries", "banks", "relays", "inputs", "parameters", "logs"
};
#define NREPORTS ((int)(sizeof(REPORTS) / sizeof(REPORTS[0])))

/*
 * Open the port, collect all JSON reports, write them to g_outdir.
 * Returns 0 on success (even if some reports fail), -1 if port unavailable.
 */
static int do_one_cycle(void)
{
    static char json_buf[LINEBUF];
    int fd, i;

    fd = open_transport(1);
    if (fd < 0)
        return -1;

    for (i = 0; i < NREPORTS && !g_stop; i++) {
        struct timespec deadline = deadline_from_now(g_timeout);
        char dev_cmd[32];
        int llen;

        if (!wait_for_prompt(fd, &deadline, g_address == NULL)) {
            fprintf(stderr, "daemon: timed out waiting for prompt\n");
            break;
        }

        char command[48];
        snprintf(command, sizeof(command), TOOL_PREFIX "report %s", REPORTS[i]);
        llen = snprintf(dev_cmd, sizeof(dev_cmd), "%s\r", command);
        verbose_bytes(">>>", dev_cmd, (size_t)llen);
        if (write_all_fd(fd, dev_cmd, (size_t)llen) != 0) {
            fprintf(stderr, "%s: write: %s\n", g_port, strerror(errno));
            break;
        }

        deadline = deadline_from_now(g_timeout);
        json_buf[0] = '\0';
        if (read_dot_response(fd,
                              &deadline,
                              g_address == NULL ? command : NULL,
                              DOT_RESPONSE_JSON,
                              json_buf,
                              sizeof(json_buf)) == 1)
            write_json_atomic(g_outdir, REPORTS[i], json_buf);
        else
            fprintf(stderr, "daemon: timed out waiting for %s report\n", REPORTS[i]);
    }

    close(fd);
    return 0;
}

static void do_daemon(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);

    if (access(g_outdir, W_OK) < 0) {
        fprintf(stderr, "%s: %s\n", g_outdir, strerror(errno));
        return;
    }

    while (!g_stop) {
        struct timespec cycle_start, now, sleep_ts;
        long elapsed_ms, sleep_ms;

        clock_gettime(CLOCK_MONOTONIC, &cycle_start);
        do_one_cycle();

        if (g_stop)
            break;

        clock_gettime(CLOCK_MONOTONIC, &now);
        elapsed_ms = (now.tv_sec  - cycle_start.tv_sec)  * 1000L
                   + (now.tv_nsec - cycle_start.tv_nsec) / 1000000L;
        sleep_ms = (long)g_interval * 1000L - elapsed_ms;

        if (sleep_ms > 0) {
            sleep_ts.tv_sec  = sleep_ms / 1000;
            sleep_ts.tv_nsec = (sleep_ms % 1000) * 1000000L;
            /*
             * nanosleep() is available on both Linux and macOS. The elapsed
             * time above is still measured with CLOCK_MONOTONIC, and an
             * arriving SIGTERM/SIGINT interrupts this sleep so g_stop can be
             * checked immediately.
             */
            nanosleep(&sleep_ts, NULL);
        }
    }
}

/* ------------------------------------------------------------------ */
/* REPL                                                                 */
/* ------------------------------------------------------------------ */

static char *repl_prompt(EditLine *el)
{
    (void)el;
    static char p[] = "power4ctl> ";
    return p;
}

static void do_repl(void)
{
    EditLine *el;
    History  *hist;
    HistEvent ev;
    char      histpath[512];
    const char *histfile = NULL;
    const char *home;
    const char *raw;
    int count;

    home = getenv("HOME");
    if (home != NULL &&
        snprintf(histpath, sizeof(histpath), "%s/.power4ctl_history", home)
            < (int)sizeof(histpath))
        histfile = histpath;

    hist = history_init();
    history(hist, &ev, H_SETSIZE, 200);

    el = el_init("power4ctl", stdin, stdout, stderr);
    el_set(el, EL_PROMPT, repl_prompt);
    el_set(el, EL_HIST, history, hist);
    el_set(el, EL_EDITOR, "emacs");

    if (histfile != NULL)
        history(hist, &ev, H_LOAD, histfile);

    while ((raw = el_gets(el, &count)) != NULL) {
        const char *p, *end;
        char cmd[1024];
        size_t len;

        /* Trim leading/trailing whitespace including the trailing newline */
        p = raw;
        while (*p == ' ' || *p == '\t') p++;
        end = p + strlen(p);
        while (end > p && (end[-1] == '\n' || end[-1] == '\r' ||
                           end[-1] == ' '  || end[-1] == '\t'))
            end--;

        len = (size_t)(end - p);
        if (len == 0)
            continue;
        if (len >= sizeof(cmd))
            len = sizeof(cmd) - 1;
        memcpy(cmd, p, len);
        cmd[len] = '\0';

        if (strcmp(cmd, "exit") == 0 || strcmp(cmd, "quit") == 0)
            break;

        history(hist, &ev, H_ENTER, raw);
        run_device_command(cmd);
    }
    fputc('\n', stdout);

    if (histfile != NULL)
        history(hist, &ev, H_SAVE, histfile);

    el_end(el);
    history_end(hist);
}

/* ------------------------------------------------------------------ */
/* Main                                                                 */
/* ------------------------------------------------------------------ */

static void usage(void)
{
    fprintf(stderr,
            "usage: power4ctl [-p port | -a address (-e name | -f file)]\n"
            "                 [-b baud] [-t seconds] [-v] command [args...]\n"
            "       power4ctl [-p port | -a address (-e name | -f file)]\n"
            "                 [-b baud] [-t seconds] [-v]\n"
            "       power4ctl [-p port | -a address (-e name | -f file)]\n"
            "                 [-b baud] [-t seconds] [-v]\n"
            "                 -D [-i interval] [-l lock-seconds] [-o outdir]\n"
            "\n"
            "options:\n"
            "  -p port          serial port  (default: /dev/ttyACM0)\n"
            "  -a address       use authenticated TCP console at IPv4 address\n"
            "  -e name          read TCP password from environment variable name\n"
            "  -f file          read TCP password from file, trimming whitespace\n"
            "  -b baud          baud rate    (default: 115200)\n"
            "  -t seconds       timeout per operation  (default: 2)\n"
            "  -v               verbose: log bytes sent/received to stderr\n"
            "  -D               daemon mode: collect JSON reports on a loop\n"
            "  -i seconds       daemon poll interval  (default: 60)\n"
            "  -l seconds       port lock wait timeout  (default: 5)\n"
            "  -o dir           daemon output directory  (default: /run/power4)\n"
            "\n"
            "commands:\n"
            "  json batteries\n"
            "  json banks\n"
            "  json inputs\n"
            "  json logs\n"
            "  json parameters\n"
            "  json relays\n"
            "  stage <filename>\n"
            "  <anything else>   sent verbatim; output echoed to stdout\n"
            "\n"
            "  Invoked with no command: enter interactive REPL mode.\n"
            "  Type 'exit' or 'quit', or press Ctrl-D to leave the REPL.\n");
}

static int password_copy_checked(const char *source)
{
    size_t length;
    size_t i;

    if (source == NULL) {
        return -1;
    }
    length = strlen(source);
    if (length < PASSWORD_MIN_BYTES || length > PASSWORD_MAX_BYTES) {
        fprintf(stderr, "password must be %d-%d printable bytes\n",
                PASSWORD_MIN_BYTES,
                PASSWORD_MAX_BYTES);
        return -1;
    }
    for (i = 0; i < length; i++) {
        unsigned char ch = (unsigned char)source[i];
        if (ch < ' ' || ch > '~') {
            fprintf(stderr, "password contains a non-printable byte\n");
            return -1;
        }
    }
    memcpy(g_password, source, length + 1);
    return 0;
}

static int load_password_file(const char *path)
{
    FILE *file;
    char contents[1024];
    size_t length;
    char *start;
    char *end;
    int result;

    file = fopen(path, "rb");
    if (file == NULL) {
        fprintf(stderr, "%s: %s\n", path, strerror(errno));
        return -1;
    }
    length = fread(contents, 1, sizeof(contents) - 1, file);
    if (ferror(file) || (!feof(file) && length == sizeof(contents) - 1)) {
        fprintf(stderr, "%s: password file is too large or unreadable\n", path);
        fclose(file);
        memset(contents, 0, sizeof(contents));
        return -1;
    }
    fclose(file);
    contents[length] = '\0';

    start = contents;
    while (*start != '\0' && isspace((unsigned char)*start))
        start++;
    end = contents + length;
    while (end > start && isspace((unsigned char)end[-1]))
        end--;
    *end = '\0';
    result = password_copy_checked(start);
    memset(contents, 0, sizeof(contents));
    return result;
}

static void clear_password(void)
{
    volatile unsigned char *cursor = (volatile unsigned char *)g_password;
    size_t i;
    for (i = 0; i < sizeof(g_password); i++)
        cursor[i] = 0;
}

static int configure_transport_credentials(void)
{
    if (g_address == NULL) {
        if (g_password_env != NULL || g_password_file != NULL) {
            fprintf(stderr, "-e and -f require -a\n");
            return -1;
        }
        return 0;
    }
    if (g_port_explicit) {
        fprintf(stderr, "-p and -a are mutually exclusive\n");
        return -1;
    }
    if ((g_password_env == NULL) == (g_password_file == NULL)) {
        fprintf(stderr, "-a requires exactly one of -e or -f\n");
        return -1;
    }
    if (g_password_env != NULL) {
        const char *value = getenv(g_password_env);
        if (value == NULL) {
            fprintf(stderr, "environment variable %s is not set\n", g_password_env);
            return -1;
        }
        return password_copy_checked(value);
    }
    return load_password_file(g_password_file);
}

int main(int argc, char **argv)
{
    int opt;

    while ((opt = getopt(argc, argv, "p:a:e:f:b:t:vDi:l:o:")) != -1) {
        switch (opt) {
        case 'p': g_port         = optarg; g_port_explicit = 1; break;
        case 'a': g_address      = optarg;       break;
        case 'e': g_password_env = optarg;       break;
        case 'f': g_password_file = optarg;      break;
        case 'b': g_baud         = atoi(optarg); break;
        case 't': g_timeout      = atoi(optarg); break;
        case 'v': g_verbose      = 1;            break;
        case 'D': g_daemon       = 1;            break;
        case 'i': g_interval     = atoi(optarg); break;
        case 'l': g_lock_timeout = atoi(optarg); break;
        case 'o': g_outdir       = optarg;       break;
        default:
            usage();
            return 1;
        }
    }

    if (configure_transport_credentials() != 0)
        return 1;
    atexit(clear_password);

    if (g_daemon) {
        if (optind < argc) {
            fprintf(stderr, "power4ctl: -D takes no command arguments\n");
            usage();
            return 1;
        }
        do_daemon();
        return 0;
    }

    /* No command: interactive REPL */
    if (optind >= argc) {
        do_repl();
        return 0;
    }

    /* Single-shot command: build command string from remaining argv */
    {
        char command[1024];
        int i;

        command[0] = '\0';
        for (i = optind; i < argc; i++) {
            if (i > optind)
                strncat(command, " ", sizeof(command) - strlen(command) - 1);
            strncat(command, argv[i], sizeof(command) - strlen(command) - 1);
        }

        return run_device_command(command);
    }
}
