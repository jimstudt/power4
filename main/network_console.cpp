#include "network_console.hpp"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "console.hpp"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "mbedtls/base64.h"
#include "mbedtls/md.h"
#include "nvs.h"

namespace {

constexpr const char *kTag = "network_console";
constexpr const char *kNamespace = "network_console";
constexpr const char *kPasswordKey = "password";
constexpr size_t kChallengeRandomBytes = 32;
constexpr size_t kChallengeTextBytes = 43;
constexpr size_t kChallengeEncodedBufferBytes = 45;
constexpr size_t kHmacHexBytes = 64;
constexpr size_t kCommandLineBytes = 256;
constexpr uint32_t kTaskStackBytes = 8192;
constexpr UBaseType_t kTaskPriority = 4;
constexpr int kAuthenticationTimeoutSeconds = 5;
constexpr int kCommandIoTimeoutSeconds = 5;
constexpr int kCommandIoDeadlineSeconds = 15;
constexpr int kSessionIdleTimeoutSeconds = 60;
constexpr int kSendTimeoutSeconds = 2;
constexpr char kToolCommandPrefix[] = "p4exec ";

StaticSemaphore_t g_password_mutex_storage = {};
SemaphoreHandle_t g_password_mutex = nullptr;
char g_password[kNetworkConsolePasswordMaxBytes + 1] = {};
size_t g_password_length = 0;
bool g_started = false;

struct SocketStream {
    int socket;
    bool skip_lf;
    bool command_active;
    int64_t command_deadline_us;
};

bool password_valid(const char *password)
{
    if (password == nullptr) {
        return false;
    }
    const size_t length = strlen(password);
    if (length < kNetworkConsolePasswordMinBytes ||
        length > kNetworkConsolePasswordMaxBytes) {
        return false;
    }
    for (size_t i = 0; i < length; ++i) {
        const unsigned char ch = static_cast<unsigned char>(password[i]);
        if (ch < ' ' || ch > '~') {
            return false;
        }
    }
    return true;
}

esp_err_t load_password(void)
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(kNamespace, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    size_t length = sizeof(g_password);
    err = nvs_get_str(handle, kPasswordKey, g_password, &length);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        g_password[0] = '\0';
        g_password_length = 0;
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    if (!password_valid(g_password)) {
        memset(g_password, 0, sizeof(g_password));
        g_password_length = 0;
        return ESP_ERR_INVALID_STATE;
    }
    g_password_length = strlen(g_password);
    return ESP_OK;
}

esp_err_t save_password(const char *password)
{
    nvs_handle_t handle = 0;
    ESP_RETURN_ON_ERROR(nvs_open(kNamespace, NVS_READWRITE, &handle),
                        kTag,
                        "failed to open password storage");
    esp_err_t err = nvs_set_str(handle, kPasswordKey, password);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

void set_socket_timeout(int socket, int option, int seconds)
{
    timeval timeout = {};
    timeout.tv_sec = seconds;
    setsockopt(socket, SOL_SOCKET, option, &timeout, sizeof(timeout));
}

bool send_all(int socket, const char *data, size_t length)
{
    size_t sent = 0;
    while (sent < length) {
        const ssize_t result = send(socket, data + sent, length - sent, 0);
        if (result <= 0) {
            return false;
        }
        sent += static_cast<size_t>(result);
    }
    return true;
}

bool read_line(int socket, char *line, size_t capacity)
{
    size_t length = 0;
    while (length + 1 < capacity) {
        char ch = '\0';
        const ssize_t result = recv(socket, &ch, 1, 0);
        if (result <= 0) {
            return false;
        }
        if (ch == '\n') {
            if (length > 0 && line[length - 1] == '\r') {
                --length;
            }
            line[length] = '\0';
            return true;
        }
        line[length++] = ch;
    }
    line[capacity - 1] = '\0';
    return false;
}

void base64url_encode(const uint8_t *input,
                      size_t input_length,
                      char *output,
                      size_t output_capacity)
{
    size_t encoded_length = 0;
    if (mbedtls_base64_encode(reinterpret_cast<unsigned char *>(output),
                              output_capacity,
                              &encoded_length,
                              input,
                              input_length) != 0) {
        output[0] = '\0';
        return;
    }
    while (encoded_length > 0 && output[encoded_length - 1] == '=') {
        --encoded_length;
    }
    for (size_t i = 0; i < encoded_length; ++i) {
        if (output[i] == '+') {
            output[i] = '-';
        } else if (output[i] == '/') {
            output[i] = '_';
        }
    }
    output[encoded_length] = '\0';
}

bool password_snapshot(char *password, size_t capacity, size_t *length)
{
    if (xSemaphoreTake(g_password_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        return false;
    }
    const bool configured = g_password_length > 0 && capacity > g_password_length;
    if (configured) {
        memcpy(password, g_password, g_password_length + 1);
        *length = g_password_length;
    }
    xSemaphoreGive(g_password_mutex);
    return configured;
}

bool hmac_sha256(const char *password,
                 size_t password_length,
                 const char *challenge,
                 uint8_t digest[32])
{
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    return info != nullptr &&
           mbedtls_md_hmac(info,
                           reinterpret_cast<const unsigned char *>(password),
                           password_length,
                           reinterpret_cast<const unsigned char *>(challenge),
                           strlen(challenge),
                           digest) == 0;
}

bool parse_hmac_hex(const char *text, uint8_t digest[32])
{
    if (text == nullptr || strlen(text) != kHmacHexBytes) {
        return false;
    }
    for (size_t i = 0; i < 32; ++i) {
        const int high = isdigit(static_cast<unsigned char>(text[i * 2]))
                             ? text[i * 2] - '0'
                             : tolower(static_cast<unsigned char>(text[i * 2])) - 'a' + 10;
        const int low = isdigit(static_cast<unsigned char>(text[i * 2 + 1]))
                            ? text[i * 2 + 1] - '0'
                            : tolower(static_cast<unsigned char>(text[i * 2 + 1])) - 'a' + 10;
        if (high < 0 || high > 15 || low < 0 || low > 15) {
            return false;
        }
        digest[i] = static_cast<uint8_t>((high << 4) | low);
    }
    return true;
}

bool constant_time_equal(const uint8_t a[32], const uint8_t b[32])
{
    uint8_t difference = 0;
    for (size_t i = 0; i < 32; ++i) {
        difference |= static_cast<uint8_t>(a[i] ^ b[i]);
    }
    return difference == 0;
}

bool authenticate_client(int socket)
{
    char password[kNetworkConsolePasswordMaxBytes + 1] = {};
    size_t password_length = 0;
    if (!password_snapshot(password, sizeof(password), &password_length)) {
        ESP_LOGW(kTag, "authentication rejected: password is not configured");
        send_all(socket, "authentication unavailable\r\n", 28);
        return false;
    }

    uint8_t random[kChallengeRandomBytes] = {};
    char challenge[kChallengeEncodedBufferBytes] = {};
    char request[96] = {};
    esp_fill_random(random, sizeof(random));
    base64url_encode(random, sizeof(random), challenge, sizeof(challenge));
    memset(random, 0, sizeof(random));
    if (strlen(challenge) != kChallengeTextBytes) {
        ESP_LOGE(kTag, "authentication failed: challenge encoding failed");
        memset(password, 0, sizeof(password));
        return false;
    }

    const int request_length =
        snprintf(request, sizeof(request), "authenticate %s\r\n", challenge);
    if (request_length <= 0 ||
        !send_all(socket, request, static_cast<size_t>(request_length))) {
        ESP_LOGW(kTag, "authentication failed: challenge send failed errno=%d", errno);
        memset(password, 0, sizeof(password));
        return false;
    }

    char response[96] = {};
    if (!read_line(socket, response, sizeof(response))) {
        ESP_LOGW(kTag, "authentication failed: response timeout or disconnect");
        memset(password, 0, sizeof(password));
        return false;
    }
    static constexpr char kPrefix[] = "authenticate ";
    uint8_t supplied[32] = {};
    uint8_t expected[32] = {};
    const bool valid =
        strncmp(response, kPrefix, sizeof(kPrefix) - 1) == 0 &&
        parse_hmac_hex(response + sizeof(kPrefix) - 1, supplied) &&
        hmac_sha256(password, password_length, challenge, expected) &&
        constant_time_equal(supplied, expected);
    memset(password, 0, sizeof(password));
    memset(supplied, 0, sizeof(supplied));
    memset(expected, 0, sizeof(expected));
    if (!valid) {
        ESP_LOGW(kTag, "authentication rejected: invalid response");
        send_all(socket, "authentication failed\r\n", 23);
        return false;
    }
    ESP_LOGI(kTag, "TCP console client authenticated");
    return send_all(socket, "authenticated\r\npower4> ", 23);
}

int socket_stream_read(void *cookie, char *buffer, int length)
{
    auto *stream = static_cast<SocketStream *>(cookie);
    int produced = 0;
    while (produced < length) {
        if (stream->command_active &&
            esp_timer_get_time() >= stream->command_deadline_us) {
            errno = ETIMEDOUT;
            return produced > 0 ? produced : -1;
        }
        char ch = '\0';
        const ssize_t result = recv(stream->socket, &ch, 1, 0);
        if (result <= 0) {
            return produced > 0 ? produced : -1;
        }
        if (stream->skip_lf && ch == '\n') {
            stream->skip_lf = false;
            continue;
        }
        stream->skip_lf = false;
        if (ch == '\r') {
            buffer[produced++] = '\n';
            stream->skip_lf = true;
        } else {
            buffer[produced++] = ch;
        }
        if (ch == '\r' || ch == '\n') {
            break;
        }
    }
    return produced;
}

int socket_stream_write(void *cookie, const char *buffer, int length)
{
    auto *stream = static_cast<SocketStream *>(cookie);
    int sent = 0;
    const int64_t send_deadline_us =
        esp_timer_get_time() + (kSendTimeoutSeconds * 1000000LL);
    while (sent < length) {
        const int64_t now_us = esp_timer_get_time();
        const int64_t deadline_us =
            stream->command_active &&
                    stream->command_deadline_us < send_deadline_us
                ? stream->command_deadline_us
                : send_deadline_us;
        if (now_us >= deadline_us) {
            errno = ETIMEDOUT;
            return -1;
        }

        fd_set write_fds;
        FD_ZERO(&write_fds);
        FD_SET(stream->socket, &write_fds);
        const int64_t remaining_us = deadline_us - now_us;
        timeval timeout = {
            .tv_sec = static_cast<time_t>(remaining_us / 1000000LL),
            .tv_usec = static_cast<suseconds_t>(remaining_us % 1000000LL),
        };
        const int ready =
            select(stream->socket + 1, nullptr, &write_fds, nullptr, &timeout);
        if (ready <= 0) {
            if (ready == 0) {
                errno = ETIMEDOUT;
            }
            return -1;
        }

        const ssize_t result =
            send(stream->socket,
                 buffer + sent,
                 static_cast<size_t>(length - sent),
                 MSG_DONTWAIT);
        if (result <= 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            return -1;
        }
        sent += static_cast<int>(result);
    }
    return length;
}

void serve_client(int socket)
{
    set_socket_timeout(socket, SO_RCVTIMEO, kAuthenticationTimeoutSeconds);
    set_socket_timeout(socket, SO_SNDTIMEO, kSendTimeoutSeconds);
    if (!authenticate_client(socket)) {
        return;
    }

    SocketStream context = {
        .socket = socket,
        .skip_lf = false,
        .command_active = false,
        .command_deadline_us = 0,
    };
    // Keep socket input and output in separate FILE objects. A single update
    // stream cannot be switched from fgets() to printf() without an intervening
    // positioning operation, which sockets do not support.
    FILE *input_stream =
        funopen(&context, socket_stream_read, nullptr, nullptr, nullptr);
    if (input_stream == nullptr) {
        return;
    }
    FILE *output_stream =
        funopen(&context, nullptr, socket_stream_write, nullptr, nullptr);
    if (output_stream == nullptr) {
        fclose(input_stream);
        return;
    }
    setvbuf(input_stream, nullptr, _IONBF, 0);
    setvbuf(output_stream, nullptr, _IONBF, 0);

    set_socket_timeout(socket, SO_RCVTIMEO, kSessionIdleTimeoutSeconds);
    char line[kCommandLineBytes] = {};
    while (fgets(line, sizeof(line), input_stream) != nullptr) {
        size_t length = strlen(line);
        if (length == 0 || line[length - 1] != '\n') {
            if (length == sizeof(line) - 1) {
                fprintf(output_stream, "command too long\r\n");
            }
            break;
        }
        while (length > 0 && (line[length - 1] == '\r' || line[length - 1] == '\n')) {
            line[--length] = '\0';
        }
        if (length == 0) {
            fprintf(output_stream, "power4> ");
            continue;
        }

        set_socket_timeout(socket, SO_RCVTIMEO, kCommandIoTimeoutSeconds);
        context.command_active = true;
        context.command_deadline_us =
            esp_timer_get_time() + (kCommandIoDeadlineSeconds * 1000000LL);
        const char *logged_command =
            strncmp(line, kToolCommandPrefix, sizeof(kToolCommandPrefix) - 1) == 0
                ? line + sizeof(kToolCommandPrefix) - 1
                : line;
        const int64_t command_started_us = esp_timer_get_time();
        ESP_LOGI(kTag, "TCP command started: %s", logged_command);
        int command_result = 0;
        const esp_err_t command_err =
            power4_console_execute(line,
                                   Power4CommandSource::Tcp,
                                   input_stream,
                                   output_stream,
                                   &command_result);
        const int64_t elapsed_ms =
            (esp_timer_get_time() - command_started_us) / 1000;
        ESP_LOGI(kTag,
                 "TCP command finished: status=%s result=%d elapsed=%lldms",
                 esp_err_to_name(command_err),
                 command_result,
                 static_cast<long long>(elapsed_ms));
        context.command_active = false;
        if (command_err == ESP_ERR_TIMEOUT &&
            strncmp(line, kToolCommandPrefix, sizeof(kToolCommandPrefix) - 1) != 0) {
            fprintf(output_stream, "console busy; try again\r\n");
        }
        if (ferror(input_stream) || ferror(output_stream)) {
            break;
        }
        fprintf(output_stream, "power4> ");
        if (ferror(output_stream)) {
            break;
        }
        set_socket_timeout(socket, SO_RCVTIMEO, kSessionIdleTimeoutSeconds);
    }
    fclose(output_stream);
    fclose(input_stream);
}

void network_console_task(void *)
{
    while (true) {
        const int server = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
        if (server < 0) {
            ESP_LOGE(kTag, "socket failed: errno=%d", errno);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        int reuse = 1;
        setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        sockaddr_in address = {};
        address.sin_family = AF_INET;
        address.sin_port = htons(kNetworkConsolePort);
        address.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(server, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 ||
            listen(server, 2) != 0) {
            ESP_LOGE(kTag, "bind/listen failed: errno=%d", errno);
            close(server);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        ESP_LOGI(kTag, "TCP console listening on port %u", kNetworkConsolePort);
        while (true) {
            sockaddr_in peer = {};
            socklen_t peer_length = sizeof(peer);
            const int client =
                accept(server, reinterpret_cast<sockaddr *>(&peer), &peer_length);
            if (client < 0) {
                ESP_LOGW(kTag, "accept failed: errno=%d", errno);
                break;
            }
            char peer_address[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET,
                      &peer.sin_addr,
                      peer_address,
                      sizeof(peer_address));
            ESP_LOGI(kTag,
                     "TCP console connection from %s:%u",
                     peer_address[0] != '\0' ? peer_address : "unknown",
                     ntohs(peer.sin_port));
            serve_client(client);
            shutdown(client, SHUT_RDWR);
            close(client);
        }
        close(server);
    }
}

}  // namespace

esp_err_t network_console_init(void)
{
    if (g_password_mutex != nullptr) {
        return ESP_OK;
    }
    g_password_mutex = xSemaphoreCreateMutexStatic(&g_password_mutex_storage);
    ESP_RETURN_ON_FALSE(g_password_mutex != nullptr,
                        ESP_ERR_NO_MEM,
                        kTag,
                        "failed to create password mutex");
    const esp_err_t err = load_password();
    if (err == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(kTag, "stored password is invalid; authentication disabled");
        return ESP_OK;
    }
    return err;
}

esp_err_t network_console_password_set(const char *password)
{
    ESP_RETURN_ON_FALSE(password_valid(password),
                        ESP_ERR_INVALID_ARG,
                        kTag,
                        "password must be 16-128 printable bytes");
    ESP_RETURN_ON_ERROR(save_password(password), kTag, "failed to save password");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(g_password_mutex, pdMS_TO_TICKS(500)) == pdTRUE,
                        ESP_ERR_TIMEOUT,
                        kTag,
                        "password state is busy");
    memset(g_password, 0, sizeof(g_password));
    strlcpy(g_password, password, sizeof(g_password));
    g_password_length = strlen(g_password);
    xSemaphoreGive(g_password_mutex);
    return ESP_OK;
}

esp_err_t network_console_password_generate(char *password, size_t capacity)
{
    ESP_RETURN_ON_FALSE(password != nullptr && capacity > kChallengeTextBytes,
                        ESP_ERR_INVALID_ARG,
                        kTag,
                        "generated password buffer is too small");
    uint8_t random[kChallengeRandomBytes] = {};
    esp_fill_random(random, sizeof(random));
    base64url_encode(random, sizeof(random), password, capacity);
    memset(random, 0, sizeof(random));
    ESP_RETURN_ON_FALSE(strlen(password) == kChallengeTextBytes,
                        ESP_FAIL,
                        kTag,
                        "password encoding failed");
    const esp_err_t err = network_console_password_set(password);
    if (err != ESP_OK) {
        memset(password, 0, capacity);
    }
    return err;
}

esp_err_t network_console_password_get(char *password, size_t capacity, bool *configured)
{
    ESP_RETURN_ON_FALSE(password != nullptr && capacity > 0 && configured != nullptr,
                        ESP_ERR_INVALID_ARG,
                        kTag,
                        "invalid password output");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(g_password_mutex, pdMS_TO_TICKS(500)) == pdTRUE,
                        ESP_ERR_TIMEOUT,
                        kTag,
                        "password state is busy");
    *configured = g_password_length > 0;
    if (*configured && capacity <= g_password_length) {
        xSemaphoreGive(g_password_mutex);
        return ESP_ERR_INVALID_SIZE;
    }
    if (*configured) {
        memcpy(password, g_password, g_password_length + 1);
    } else {
        password[0] = '\0';
    }
    xSemaphoreGive(g_password_mutex);
    return ESP_OK;
}

esp_err_t network_console_start(void)
{
    ESP_RETURN_ON_FALSE(g_password_mutex != nullptr,
                        ESP_ERR_INVALID_STATE,
                        kTag,
                        "network console is not initialized");
    if (g_started) {
        return ESP_OK;
    }
    const BaseType_t created = xTaskCreate(network_console_task,
                                           "tcp_console",
                                           kTaskStackBytes,
                                           nullptr,
                                           kTaskPriority,
                                           nullptr);
    ESP_RETURN_ON_FALSE(created == pdPASS,
                        ESP_ERR_NO_MEM,
                        kTag,
                        "failed to create TCP console task");
    g_started = true;
    return ESP_OK;
}
