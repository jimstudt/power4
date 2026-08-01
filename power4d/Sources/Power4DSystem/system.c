#include "system.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <poll.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

int64_t power4_monotonic_milliseconds(void)
{
    struct timespec now = {0};
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return -1;
    }
    return ((int64_t)now.tv_sec * 1000) + (now.tv_nsec / 1000000);
}

const char *power4_system_error_string(int error)
{
    return strerror(error);
}

const char *power4_resolve_error_string(int error)
{
    return gai_strerror(error);
}

int power4_validate_serial_path(const char *path, int *system_error)
{
    if (path == NULL || system_error == NULL) {
        if (system_error != NULL) {
            *system_error = EINVAL;
        }
        return POWER4_SERIAL_INACCESSIBLE;
    }

    struct stat information = {0};
    if (stat(path, &information) != 0) {
        *system_error = errno;
        return POWER4_SERIAL_INACCESSIBLE;
    }
    if (!S_ISCHR(information.st_mode)) {
        return POWER4_SERIAL_NOT_CHARACTER_DEVICE;
    }
    if (access(path, R_OK | W_OK) != 0) {
        *system_error = errno;
        return POWER4_SERIAL_INACCESSIBLE;
    }
    return POWER4_SERIAL_VALID;
}

static int wait_for_fd(int fd,
                       short events,
                       int64_t deadline_milliseconds,
                       int *system_error)
{
    for (;;) {
        const int64_t now = power4_monotonic_milliseconds();
        if (now < 0) {
            *system_error = errno;
            return POWER4_IO_ERROR;
        }
        const int64_t remaining = deadline_milliseconds - now;
        if (remaining <= 0) {
            return POWER4_IO_TIMEOUT;
        }

        struct pollfd descriptor = {
            .fd = fd,
            .events = events,
            .revents = 0,
        };
        const int timeout = remaining > INT_MAX ? INT_MAX : (int)remaining;
        const int result = poll(&descriptor, 1, timeout);
        if (result > 0) {
            if ((descriptor.revents & POLLNVAL) != 0) {
                *system_error = EBADF;
                return POWER4_IO_ERROR;
            }
            return POWER4_IO_OK;
        }
        if (result == 0) {
            return POWER4_IO_TIMEOUT;
        }
        if (errno != EINTR) {
            *system_error = errno;
            return POWER4_IO_ERROR;
        }
    }
}

int power4_resolve_ipv4(const char *hostname,
                        uint32_t *addresses,
                        size_t capacity,
                        size_t *count,
                        int *system_error)
{
    if (hostname == NULL || addresses == NULL || capacity == 0 || count == NULL ||
        system_error == NULL) {
        if (system_error != NULL) {
            *system_error = EINVAL;
        }
        return POWER4_IO_ERROR;
    }

    struct addrinfo hints = {0};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *results = NULL;
    const int error = getaddrinfo(hostname, NULL, &hints, &results);
    if (error != 0) {
        *system_error = error;
        return POWER4_IO_ERROR;
    }

    *count = 0;
    for (const struct addrinfo *item = results;
         item != NULL && *count < capacity;
         item = item->ai_next) {
        if (item->ai_family != AF_INET || item->ai_addrlen < sizeof(struct sockaddr_in)) {
            continue;
        }
        const uint32_t address =
            ((const struct sockaddr_in *)item->ai_addr)->sin_addr.s_addr;
        size_t index = 0;
        while (index < *count && addresses[index] != address) {
            ++index;
        }
        if (index == *count) {
            addresses[(*count)++] = address;
        }
    }
    freeaddrinfo(results);
    if (*count == 0) {
        *system_error = EAI_NONAME;
        return POWER4_IO_ERROR;
    }
    return POWER4_IO_OK;
}

int power4_connect_ipv4(uint32_t address,
                        uint16_t port,
                        int64_t deadline_milliseconds,
                        int *socket_fd,
                        int *system_error)
{
    if (socket_fd == NULL || system_error == NULL || port == 0) {
        if (system_error != NULL) {
            *system_error = EINVAL;
        }
        return POWER4_IO_ERROR;
    }
    *socket_fd = -1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        *system_error = errno;
        return POWER4_IO_ERROR;
    }

#ifdef SO_NOSIGPIPE
    int no_sigpipe = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe));
#endif
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 ||
        fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
        *system_error = errno;
        close(fd);
        return POWER4_IO_ERROR;
    }

    struct sockaddr_in destination = {0};
    destination.sin_family = AF_INET;
    destination.sin_port = htons(port);
    destination.sin_addr.s_addr = address;
    if (connect(fd,
                (const struct sockaddr *)&destination,
                sizeof(destination)) == 0) {
        *socket_fd = fd;
        return POWER4_IO_OK;
    }
    if (errno != EINPROGRESS) {
        *system_error = errno;
        close(fd);
        return POWER4_IO_ERROR;
    }

    int status = wait_for_fd(fd, POLLOUT, deadline_milliseconds, system_error);
    if (status != POWER4_IO_OK) {
        close(fd);
        return status;
    }
    int socket_error = 0;
    socklen_t error_length = sizeof(socket_error);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &error_length) != 0) {
        *system_error = errno;
        close(fd);
        return POWER4_IO_ERROR;
    }
    if (socket_error != 0) {
        *system_error = socket_error;
        close(fd);
        return POWER4_IO_ERROR;
    }
    *socket_fd = fd;
    return POWER4_IO_OK;
}

static speed_t baud_speed(int baud)
{
    switch (baud) {
    case 1200: return B1200;
    case 2400: return B2400;
    case 4800: return B4800;
    case 9600: return B9600;
    case 19200: return B19200;
    case 38400: return B38400;
    case 57600: return B57600;
    case 115200: return B115200;
    case 230400: return B230400;
    default: return 0;
    }
}

int power4_open_serial(const char *path,
                       int baud,
                       int *serial_fd,
                       int *system_error)
{
    if (path == NULL || serial_fd == NULL || system_error == NULL) {
        if (system_error != NULL) {
            *system_error = EINVAL;
        }
        return POWER4_IO_ERROR;
    }
    *serial_fd = -1;
    const speed_t speed = baud_speed(baud);
    if (speed == 0) {
        *system_error = EINVAL;
        return POWER4_IO_ERROR;
    }

    int fd = open(path, O_RDWR | O_NOCTTY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        *system_error = errno;
        return errno == EBUSY ? POWER4_IO_BUSY : POWER4_IO_ERROR;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        *system_error = errno;
        close(fd);
        return errno == EWOULDBLOCK || errno == EAGAIN ? POWER4_IO_BUSY
                                                       : POWER4_IO_ERROR;
    }
    if (ioctl(fd, TIOCEXCL) != 0) {
        *system_error = errno;
        close(fd);
        return errno == EBUSY ? POWER4_IO_BUSY : POWER4_IO_ERROR;
    }

    struct termios settings = {0};
    if (tcgetattr(fd, &settings) != 0) {
        *system_error = errno;
        close(fd);
        return POWER4_IO_ERROR;
    }
    cfmakeraw(&settings);
    cfsetispeed(&settings, speed);
    cfsetospeed(&settings, speed);
    settings.c_cflag |= CLOCAL | CREAD;
    settings.c_cflag &= ~CRTSCTS;
    settings.c_cflag = (settings.c_cflag & ~CSIZE) | CS8;
    settings.c_cflag &= ~(PARENB | PARODD | CSTOPB);
    if (tcsetattr(fd, TCSANOW, &settings) != 0) {
        *system_error = errno;
        close(fd);
        return POWER4_IO_ERROR;
    }
    (void)tcflush(fd, TCIOFLUSH);
    *serial_fd = fd;
    return POWER4_IO_OK;
}

int power4_read_some(int fd,
                     void *buffer,
                     size_t capacity,
                     int64_t deadline_milliseconds,
                     size_t *count,
                     int *system_error)
{
    if (fd < 0 || buffer == NULL || capacity == 0 || count == NULL ||
        system_error == NULL) {
        if (system_error != NULL) {
            *system_error = EINVAL;
        }
        return POWER4_IO_ERROR;
    }
    *count = 0;
    for (;;) {
        const int status =
            wait_for_fd(fd, POLLIN, deadline_milliseconds, system_error);
        if (status != POWER4_IO_OK) {
            return status;
        }
        const ssize_t result = read(fd, buffer, capacity);
        if (result > 0) {
            *count = (size_t)result;
            return POWER4_IO_OK;
        }
        if (result == 0) {
            return POWER4_IO_CLOSED;
        }
        if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
            *system_error = errno;
            return POWER4_IO_ERROR;
        }
    }
}

int power4_write_all(int fd,
                     const void *buffer,
                     size_t length,
                     int64_t deadline_milliseconds,
                     int *system_error)
{
    if (fd < 0 || (buffer == NULL && length != 0) || system_error == NULL) {
        if (system_error != NULL) {
            *system_error = EINVAL;
        }
        return POWER4_IO_ERROR;
    }
    const uint8_t *bytes = (const uint8_t *)buffer;
    size_t written = 0;
    while (written < length) {
        const int status =
            wait_for_fd(fd, POLLOUT, deadline_milliseconds, system_error);
        if (status != POWER4_IO_OK) {
            return status;
        }
#ifdef MSG_NOSIGNAL
        ssize_t result = send(fd, bytes + written, length - written, MSG_NOSIGNAL);
        if (result < 0 && errno == ENOTSOCK) {
            result = write(fd, bytes + written, length - written);
        }
#else
        const ssize_t result = write(fd, bytes + written, length - written);
#endif
        if (result > 0) {
            written += (size_t)result;
            continue;
        }
        if (result == 0) {
            return POWER4_IO_CLOSED;
        }
        if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
            *system_error = errno;
            return POWER4_IO_ERROR;
        }
    }
    return POWER4_IO_OK;
}

void power4_close(int fd)
{
    if (fd >= 0) {
        close(fd);
    }
}
