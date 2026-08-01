#ifndef POWER4D_SYSTEM_H
#define POWER4D_SYSTEM_H

#include <stddef.h>
#include <stdint.h>

enum Power4IOStatus {
    POWER4_IO_OK = 0,
    POWER4_IO_TIMEOUT = 1,
    POWER4_IO_CLOSED = 2,
    POWER4_IO_BUSY = 3,
    POWER4_IO_ERROR = 4,
};

int64_t power4_monotonic_milliseconds(void);
const char *power4_system_error_string(int error);
const char *power4_resolve_error_string(int error);

enum Power4SerialValidationStatus {
    POWER4_SERIAL_VALID = 0,
    POWER4_SERIAL_NOT_CHARACTER_DEVICE = 1,
    POWER4_SERIAL_INACCESSIBLE = 2,
};

int power4_validate_serial_path(const char *path, int *system_error);

int power4_resolve_ipv4(const char *hostname,
                        uint32_t *addresses,
                        size_t capacity,
                        size_t *count,
                        int *system_error);

int power4_connect_ipv4(uint32_t address,
                        uint16_t port,
                        int64_t deadline_milliseconds,
                        int *socket_fd,
                        int *system_error);

int power4_open_serial(const char *path,
                       int baud,
                       int *serial_fd,
                       int *system_error);

int power4_read_some(int fd,
                     void *buffer,
                     size_t capacity,
                     int64_t deadline_milliseconds,
                     size_t *count,
                     int *system_error);

int power4_write_all(int fd,
                     const void *buffer,
                     size_t length,
                     int64_t deadline_milliseconds,
                     int *system_error);

void power4_close(int fd);

#endif
