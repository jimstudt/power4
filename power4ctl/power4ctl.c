/*
 * power4ctl — serial management tool for the power4 ESP32 device
 *
 * Usage: power4ctl [-p port] [-b baud] [-t seconds] command
 *
 * Commands:
 *   report batteries
 *   report relays
 *   report banks
 *
 * Connects to the serial console, waits for the "power4> " prompt (sending
 * "\r\r" periodically to elicit it), issues the command, then reads lines
 * until one matching the framed-JSON format "P4J1 <size> <sha1hex> <json>"
 * is found.  The JSON is printed to stdout.  On any error the tool exits
 * with a non-zero status and writes a message to stderr.
 *
 * flock(LOCK_EX|LOCK_NB) + TIOCEXCL prevent concurrent access by separate
 * invocations.
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/time.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "sha1.h"

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

static int open_serial(const char *port, int baud)
{
    struct termios tty;
    speed_t speed;
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

    if (ioctl(fd, TIOCEXCL) < 0) {
        fprintf(stderr, "%s: TIOCEXCL: %s\n", port, strerror(errno));
        close(fd);
        return -1;
    }

    speed = baud_to_speed(baud);
    if (speed == B0) {
        fprintf(stderr, "unsupported baud rate: %d\n", baud);
        close(fd);
        return -1;
    }

    if (tcgetattr(fd, &tty) < 0) {
        fprintf(stderr, "%s: tcgetattr: %s\n", port, strerror(errno));
        close(fd);
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
        close(fd);
        return -1;
    }

    tcflush(fd, TCIOFLUSH);
    return fd;
}

/* ------------------------------------------------------------------ */
/* Protocol                                                             */
/* ------------------------------------------------------------------ */

/* Forward declaration — defined in main section below */
static const char *g_port;

#define PROMPT          "power4> "
#define PROMPT_LEN      8
#define PROMPT_ATTEMPTS 3
#define LINEBUF         16384
#define B64_LINE_WIDTH  76      /* 57 input bytes → 76 base64 chars per line */

/*
 * Send a single "\r" then wait for "power4> ", repeating up to PROMPT_ATTEMPTS
 * times with the remaining timeout divided evenly across attempts.
 * Returns 1 on success, 0 on timeout.
 */
static int wait_for_prompt(int fd, const struct timespec *deadline)
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

        verbose_bytes(">>>", "\r", 1);
        (void)write(fd, "\r", 1);

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
 * Read the device's response after a policy upload.  Scans complete lines for
 * success ("uploaded staged configuration:") or failure (" failed:") keywords.
 * Stops when the "power4> " prompt returns or the deadline passes.
 * Returns 1 on success, -1 on device-reported failure, 0 on timeout.
 */
static int read_upload_response(int fd, const struct timespec *deadline)
{
    char buf[4096];
    int buflen = 0;
    int status = 0;

    while (!deadline_passed(deadline)) {
        int avail = (int)sizeof(buf) - buflen - 1;
        int n, i;
        char *start, *nl;

        if (avail <= 0) {
            /* Keep enough to detect a partial prompt */
            memmove(buf, buf + buflen - (PROMPT_LEN - 1), PROMPT_LEN - 1);
            buflen = PROMPT_LEN - 1;
            avail  = (int)sizeof(buf) - buflen - 1;
        }
        if (wait_readable(fd, deadline) <= 0)
            break;
        n = (int)read(fd, buf + buflen, (size_t)avail);
        if (n <= 0)
            break;
        verbose_bytes("<<<", buf + buflen, (size_t)n);
        buflen += n;
        buf[buflen] = '\0';

        /* Process complete newline-terminated lines */
        start = buf;
        while ((nl = (char *)memchr(start, '\n',
                                    (size_t)(buf + buflen - start))) != NULL) {
            size_t llen;
            *nl = '\0';
            llen = (size_t)(nl - start);
            if (llen > 0 && start[llen - 1] == '\r')
                start[--llen] = '\0';

            if (strstr(start, "uploaded staged configuration:") != NULL) {
                puts(start);
                status = 1;
            } else if (status == 0 && strstr(start, " failed:") != NULL) {
                fprintf(stderr, "%s\n", start);
                status = -1;
            }
            start = nl + 1;
        }

        /* Compact */
        {
            int remain = (int)(buf + buflen - start);
            if (remain > 0) memmove(buf, start, (size_t)remain);
            buflen = remain;
        }

        /* Scan raw buffer for the prompt (not newline-terminated) */
        for (i = 0; i <= buflen - PROMPT_LEN; i++) {
            if (memcmp(buf + i, PROMPT, PROMPT_LEN) == 0)
                return status == 0 ? 1 : status;
        }
    }

    return status == 0 ? 0 : status;
}

/*
 * Read filename, compute SHA-1, issue "policy upload <sha1>\r", send base64
 * body followed by a blank line, then wait for the device's response.
 * Returns 1 on success, -1 on error, 0 on timeout.
 */
static int do_stage(int fd, const char *filename, const struct timespec *deadline)
{
    FILE *f;
    long size;
    uint8_t *data;
    char sha1[41];
    char cmd[72]; /* "policy upload " + 40 hex + "\r" + NUL */
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

    cmdlen = snprintf(cmd, sizeof(cmd), "policy upload %s\r", sha1);
    verbose_bytes(">>>", cmd, (size_t)cmdlen);
    if (write(fd, cmd, (size_t)cmdlen) < 0) {
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

    result = read_upload_response(fd, deadline);
    if (result == 0)
        fprintf(stderr, "%s: timed out waiting for upload response\n", g_port);
    return result;
}

/*
 * Send a command verbatim and echo every line back to stdout until the
 * "power4> " prompt returns.  Used for unrecognized/passthrough commands.
 * Returns 1 when the prompt is seen, 0 on timeout.
 */
static int read_passthrough(int fd, const struct timespec *deadline)
{
    char buf[4096];
    int buflen = 0;

    while (!deadline_passed(deadline)) {
        int avail = (int)sizeof(buf) - buflen - 1;
        int n, i;
        char *start, *nl;

        if (avail <= 0) {
            memmove(buf, buf + buflen - (PROMPT_LEN - 1), PROMPT_LEN - 1);
            buflen = PROMPT_LEN - 1;
            avail  = (int)sizeof(buf) - buflen - 1;
        }
        if (wait_readable(fd, deadline) <= 0)
            break;
        n = (int)read(fd, buf + buflen, (size_t)avail);
        if (n <= 0)
            break;
        verbose_bytes("<<<", buf + buflen, (size_t)n);
        buflen += n;
        buf[buflen] = '\0';

        /* Print and consume complete lines */
        start = buf;
        while ((nl = (char *)memchr(start, '\n',
                                    (size_t)(buf + buflen - start))) != NULL) {
            size_t llen;
            *nl = '\0';
            llen = (size_t)(nl - start);
            if (llen > 0 && start[llen - 1] == '\r')
                start[--llen] = '\0';
            puts(start);
            start = nl + 1;
        }

        /* Compact */
        {
            int remain = (int)(buf + buflen - start);
            if (remain > 0) memmove(buf, start, (size_t)remain);
            buflen = remain;
        }

        /* Check for prompt in the partial (non-newline-terminated) tail */
        for (i = 0; i <= buflen - PROMPT_LEN; i++) {
            if (memcmp(buf + i, PROMPT, PROMPT_LEN) == 0)
                return 1;
        }
    }
    return 0;
}

/*
 * Parse one line for the P4J1 framed-JSON format:
 *   P4J1 <decimal-size> <40-char-sha1-hex> <json>
 *
 * Validates size and SHA-1, then prints the JSON to stdout.
 * Returns 1 on success, 0 if not a P4J1 line, -1 on validation error.
 */
static int parse_p4j1(const char *line)
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

    puts(p);
    return 1;
}

/*
 * Read lines from the serial port until a valid P4J1 line or deadline.
 * Non-P4J1 lines (log output, echo, etc.) are silently ignored.
 * Returns 1 on success, 0 on timeout, -1 on validation error.
 */
static int read_response(int fd, const struct timespec *deadline)
{
    static char buf[LINEBUF];   /* static: too large for the stack */
    int buflen = 0;

    while (!deadline_passed(deadline)) {
        int avail, n;
        char *start, *nl;

        if (wait_readable(fd, deadline) <= 0)
            break;

        avail = (int)sizeof(buf) - buflen - 1;
        if (avail <= 0) {
            /* Buffer full with no newline — discard; can't be a valid line */
            buflen = 0;
            avail  = (int)sizeof(buf) - 1;
        }

        n = (int)read(fd, buf + buflen, (size_t)avail);
        if (n <= 0)
            break;
        verbose_bytes("<<<", buf + buflen, (size_t)n);
        buflen += n;
        buf[buflen] = '\0';

        start = buf;
        while ((nl = (char *)memchr(start, '\n',
                                    (size_t)(buf + buflen - start))) != NULL) {
            size_t llen;
            int r;

            *nl = '\0';
            llen = (size_t)(nl - start);
            if (llen > 0 && start[llen - 1] == '\r')
                start[--llen] = '\0';

            r = parse_p4j1(start);
            if (r != 0)
                return r;

            start = nl + 1;
        }

        /* Compact: move any partial line to the front */
        {
            int remain = (int)(buf + buflen - start);
            if (remain > 0)
                memmove(buf, start, (size_t)remain);
            buflen = remain;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Main                                                                 */
/* ------------------------------------------------------------------ */

static const char *g_port    = "/dev/ttyACM0";
static int         g_baud    = 115200;
static int         g_timeout = 2;

static void usage(void)
{
    fprintf(stderr,
            "usage: power4ctl [-p port] [-b baud] [-t seconds] [-v] command\n"
            "\n"
            "options:\n"
            "  -p port      serial port  (default: /dev/ttyACM0)\n"
            "  -b baud      baud rate    (default: 115200)\n"
            "  -t seconds   timeout      (default: 2)\n"
            "  -v           verbose: log bytes sent/received to stderr\n"
            "\n"
            "commands:\n"
            "  json batteries\n"
            "  json banks\n"
            "  json relays\n"
            "  stage <filename>\n"
            "  <anything else>   sent verbatim; output echoed to stdout\n");
}

int main(int argc, char **argv)
{
    char command[128];
    int fd, i, opt, result;
    struct timespec deadline;

    while ((opt = getopt(argc, argv, "p:b:t:v")) != -1) {
        switch (opt) {
        case 'p': g_port    = optarg;       break;
        case 'b': g_baud    = atoi(optarg); break;
        case 't': g_timeout = atoi(optarg); break;
        case 'v': g_verbose = 1;            break;
        default:
            usage();
            return 1;
        }
    }

    if (optind >= argc) {
        usage();
        return 1;
    }

    if (strcmp(argv[optind], "stage") == 0) {
        if (argc - optind != 2) {
            usage();
            return 1;
        }
        fd = open_serial(g_port, g_baud);
        if (fd < 0)
            return 1;
        deadline = deadline_from_now(g_timeout);
        if (!wait_for_prompt(fd, &deadline)) {
            fprintf(stderr, "%s: timed out waiting for prompt\n", g_port);
            close(fd);
            return 1;
        }
        result = do_stage(fd, argv[optind + 1], &deadline);
        close(fd);
        return result == 1 ? 0 : 1;
    }

    command[0] = '\0';
    for (i = optind; i < argc; i++) {
        if (i > optind)
            strncat(command, " ", sizeof(command) - strlen(command) - 1);
        strncat(command, argv[i], sizeof(command) - strlen(command) - 1);
    }

    fd = open_serial(g_port, g_baud);
    if (fd < 0)
        return 1;

    deadline = deadline_from_now(g_timeout);

    if (!wait_for_prompt(fd, &deadline)) {
        fprintf(stderr, "%s: timed out waiting for prompt\n", g_port);
        close(fd);
        return 1;
    }

    if (strcmp(command, "json batteries") == 0 ||
        strcmp(command, "json banks")     == 0 ||
        strcmp(command, "json relays")    == 0) {
        /* "json X" → send "report X\r" to device, expect P4J1 response */
        char dev_cmd[sizeof(command) + 2];
        int llen = snprintf(dev_cmd, sizeof(dev_cmd), "report%s\r", command + 4);
        verbose_bytes(">>>", dev_cmd, (size_t)llen);
        if (write(fd, dev_cmd, (size_t)llen) < 0) {
            fprintf(stderr, "%s: write: %s\n", g_port, strerror(errno));
            close(fd);
            return 1;
        }
        result = read_response(fd, &deadline);
        close(fd);
        if (result == 1)
            return 0;
        if (result == 0)
            fprintf(stderr, "%s: timed out waiting for response\n", g_port);
        return 1;
    }

    /* Passthrough: send command verbatim and echo all output to stdout */
    {
        char line[sizeof(command) + 2];
        int llen = snprintf(line, sizeof(line), "%s\r", command);
        verbose_bytes(">>>", line, (size_t)llen);
        if (write(fd, line, (size_t)llen) < 0) {
            fprintf(stderr, "%s: write: %s\n", g_port, strerror(errno));
            close(fd);
            return 1;
        }
    }

    result = read_passthrough(fd, &deadline);
    close(fd);
    if (result == 0)
        fprintf(stderr, "%s: timed out waiting for prompt\n", g_port);
    return result == 1 ? 0 : 1;
}
