/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright (c) 2013-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define _XOPEN_SOURCE 600
#define _BSD_SOURCE 1
#define _DEFAULT_SOURCE 1
#define _GNU_SOURCE 1

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <pthread.h>
#include <termios.h>
#include <stdbool.h>
#include <ctype.h>
#include <poll.h>
#include <assert.h>
#include <errno.h>
#include <string.h>
#include <dirent.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include "uart-proto.h"

struct reply_ctx {
    unsigned char data[2048];
    unsigned int length;
};

#ifdef UNIT_TEST
#include "test/test_tcu_com_events.h"

#define ut_static
#define exit(code)  ut_exit(code, __FILE__, __LINE__, __func__)
#define abort() ut_abort(__FILE__, __LINE__, __func__)
#undef assert
#define assert(cond) ut_assert(cond, #cond, __FILE__, __LINE__, __func__)

int main(int argc, char *argv[]);
int open_tty_device(void);
int reopen_tty_device(int old_fd);
int write_data_to_uart(unsigned char pty_idx, const unsigned char *data, size_t len);
void uucp_unlock_tty_device(void);
int uucp_set_filelock_name(void);
bool uucp_check_is_locked(void);
int uucp_lock_tty_device(void);

#else
#define ut_static static
#endif

#define BUF_SIZE            64
#define MAX_PATH            256
#define DEFAULT_BAUDRATE 115200
#define DEFAULT_POLL_OUTPUT_TIMEOUT 300
#define DEFAULT_TTY_DEVICE "/dev/ttyUSB3"
#define UUCP_DIR "/var/lock"
#define RAW_PTY "RAW"
#define AUTOPILOT_CONTROL_PTY "autopilot_control"
#define MAX_WRITE_CHUNK_SIZE 4096  // Maximum bytes to process at once (16KB encoded max)
#define THREAD_STACK_SIZE (128 * 1024)  // 128KB stack per thread (sufficient for this application)
#define VIRTIOSO_OUTER_RAW 0
#define VIRTIOSO_OUTER_NVIDIA_TCU 1
#define VIRTIOSO_MAX_PTY_COUNT 257
#define VIRTIOSO_CONTROL_IDLE 0
#define VIRTIOSO_CONTROL_CMD 1
#define VIRTIOSO_CONTROL_STREAM_ID 2
#define VIRTIOSO_CONTROL_NAME_LEN 3
#define VIRTIOSO_CONTROL_NAME 4

ut_static bool tcu_muxer_started = true; // Needed to exit endless while loops during testing
ut_static const char* tty_device = DEFAULT_TTY_DEVICE;
ut_static int int_baudrate = DEFAULT_BAUDRATE;
ut_static int pty_max_count;
ut_static int raw_pty_idx;
ut_static bool uucp_locked = false;
ut_static char filelock[MAX_PATH];
static int poll_output_timeout = DEFAULT_POLL_OUTPUT_TIMEOUT;
static char path[MAX_PATH];
static bool enable_write_raw_pty = false;
static bool disable_uucp_lock = false;
static bool virtioso_mode_enabled = false;
static int virtioso_outer_mode = VIRTIOSO_OUTER_RAW;
static char *virtioso_registry_path = NULL;
static char *virtioso_outer_tag_name = NULL;
static int virtioso_outer_tag_idx = -1;
static bool virtioso_wait_for_announce = false;
static int autopilot_control_pty_idx = -1;

struct tag {
    char *name;
    unsigned char value;
};

const struct tag chip_tags[] = {
    {
        .name = "PSC",
        .value = 0xe1
    },
    {
        .name = "BPMP",
        .value = 0xe2
    },
    {
        .name = "OOBHUB",
        .value = 0xe3
    },
    {
        .name = "SatMC",
        .value = 0xe4
    },
    {
        .name = "RAS",
        .value = 0xe5
    },
    {
        .name = "CCPLEX-ROOT",
        .value = 0xe6
    },
    {
        .name = "TZ",
        .value = 0xe7
    },
    {
        .name = "CCPLEX-REALM",
        .value = 0xe8
    },
    {
        .name = "MSEQ",
        .value = 0xea
    },
    {
        .name = "PCORE",
        .value = 0xeb
    },
    {
        .name = "C2C",
        .value = 0xec
    },
    {
        .name = "DBG2",
        .value = 0xed
    },
    {
        .name = "RSVD15",
        .value = 0xef
    },
    {
        .name = "RSVD16",
        .value = 0xf0
    },
    {
        .name = "CCPLEX",
        .value = 0xe9
    }
};

struct tag *tags = NULL;
int num_proc = 0;
int default_tag_idx = 0;

int disable_patch = 1; // patch line ending
bool log_timestamp_enabled = false;
ut_static char* save_output_path = NULL;

ut_static const size_t temporary_buffer_size = 1024;

// prototype
bool should_retry_io(ssize_t ret);
int putch_or_exit(int fd, const unsigned char ch);
int putbuf_or_exit(int fd, const unsigned char *buf, size_t len);
size_t get_timestamp(char *buf, const size_t len);
int flush_stream(int fd, int tag, unsigned char ch);
int patch2flush_stream(int fd, int tag, unsigned char ch, int *p_seen_n, int *p_seen_r);
void* tty_input_handler(void *arg);
void* pty_input_handler(void* arg);
void print_usage(char *argv[]);
int load_virtioso_registry(const char *registry_path);
int create_pty_at_index(int pty_idx, const char *name, bool start_thread);
int invalid_baudrate(int baudrate);
speed_t get_baudrate(int baudrate);
int create_thread_with_stack(pthread_t *thread, void *(*start_routine)(void *), void *arg);

struct thread_data
{
    int fd;
    int log_fd;
    unsigned int id;
    pthread_mutex_t write_lock;
    char device_name[128];
    char last_ch;
};

struct thread_data tty_data;
struct thread_data *pty_data;
pthread_t *pty_thread;
int pty_capacity;

#define for_each_tags(tag_idx, tag) \
    for (tag_idx = 0, tag = &tags[tag_idx]; tag_idx < num_proc; tag_idx++, tag++)

#define for_each_pty(i, pty) \
    for (i = 0, pty = &pty_data[i]; i < pty_max_count; i++, pty++)


static char *xstrndup(const char *src, size_t len)
{
    char *dst = malloc(len + 1);
    if (!dst) {
        return NULL;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
    return dst;
}

static char *read_text_file(const char *path_arg, size_t *out_len)
{
    struct stat st;
    char *buf;
    FILE *f;
    size_t len;

    if (stat(path_arg, &st) != 0) {
        fprintf(stderr, "ERROR: failed to stat %s: %s\n", path_arg, strerror(errno));
        return NULL;
    }
    if (st.st_size < 0) {
        fprintf(stderr, "ERROR: invalid file size for %s\n", path_arg);
        return NULL;
    }
    len = (size_t)st.st_size;
    buf = malloc(len + 1);
    if (!buf) {
        fprintf(stderr, "ERROR: failed to allocate %zu bytes for %s\n", len + 1, path_arg);
        return NULL;
    }
    f = fopen(path_arg, "rb");
    if (!f) {
        fprintf(stderr, "ERROR: failed to open %s: %s\n", path_arg, strerror(errno));
        free(buf);
        return NULL;
    }
    if (fread(buf, 1, len, f) != len) {
        fprintf(stderr, "ERROR: failed to read %s\n", path_arg);
        fclose(f);
        free(buf);
        return NULL;
    }
    fclose(f);
    buf[len] = '\0';
    if (out_len) {
        *out_len = len;
    }
    return buf;
}

static char *json_string_value_after(const char *start, const char *key)
{
    const char *pos = strstr(start, key);
    const char *value_start;
    const char *value_end;

    if (!pos) {
        return NULL;
    }
    pos += strlen(key);
    while (*pos && isspace((unsigned char)*pos)) {
        pos++;
    }
    if (*pos != ':') {
        return NULL;
    }
    pos++;
    while (*pos && isspace((unsigned char)*pos)) {
        pos++;
    }
    if (*pos != '"') {
        return NULL;
    }
    value_start = ++pos;
    while (*pos && *pos != '"') {
        if (*pos == '\\') {
            return NULL;
        }
        pos++;
    }
    if (*pos != '"') {
        return NULL;
    }
    value_end = pos;
    return xstrndup(value_start, (size_t)(value_end - value_start));
}

static int json_int_value_after(const char *start, const char *key, int *out)
{
    const char *pos = strstr(start, key);
    char *endptr;
    long value;

    if (!pos) {
        return -1;
    }
    pos += strlen(key);
    while (*pos && isspace((unsigned char)*pos)) {
        pos++;
    }
    if (*pos != ':') {
        return -1;
    }
    pos++;
    errno = 0;
    value = strtol(pos, &endptr, 10);
    if (errno || endptr == pos || value < -1 || value > 0xff) {
        return -1;
    }
    *out = (int)value;
    return 0;
}

static int append_virtioso_stream(struct tag **stream_tags, int *count, int *capacity, int stream_id, char *name)
{
    struct tag *new_tags;

    if (stream_id < 0) {
        free(name);
        return 0;
    }
    if (stream_id <= 0 || stream_id > 0xff ||
        stream_id == VIRTIOSO_UART_PROTO_ESC_START ||
        stream_id == VIRTIOSO_UART_PROTO_ESC_CONTROL) {
        fprintf(stderr, "ERROR: invalid Virtioso stream id %d for %s\n", stream_id, name);
        free(name);
        return -1;
    }
    for (int i = 0; i < *count; i++) {
        if ((*stream_tags)[i].value == (unsigned char)stream_id) {
            fprintf(stderr, "ERROR: duplicate Virtioso stream id %d\n", stream_id);
            free(name);
            return -1;
        }
        if (strcmp((*stream_tags)[i].name, name) == 0) {
            fprintf(stderr, "ERROR: duplicate Virtioso stream name %s\n", name);
            free(name);
            return -1;
        }
    }
    if (*count == *capacity) {
        if (*capacity >= VIRTIOSO_MAX_PTY_COUNT) {
            fprintf(stderr, "ERROR: too many Virtioso streams\n");
            free(name);
            return -1;
        }
        int next_capacity = *capacity ? *capacity * 2 : 8;
        if (next_capacity > VIRTIOSO_MAX_PTY_COUNT) {
            next_capacity = VIRTIOSO_MAX_PTY_COUNT;
        }
        new_tags = realloc(*stream_tags, sizeof(**stream_tags) * (size_t)next_capacity);
        if (!new_tags) {
            free(name);
            return -1;
        }
        *stream_tags = new_tags;
        *capacity = next_capacity;
    }
    (*stream_tags)[*count].name = name;
    (*stream_tags)[*count].value = (unsigned char)stream_id;
    (*count)++;
    return 0;
}

static int init_virtioso_tags(void)
{
    tags = calloc(VIRTIOSO_MAX_PTY_COUNT, sizeof(*tags));
    if (!tags) {
        return -1;
    }
    tags[0].name = AUTOPILOT_CONTROL_PTY;
    tags[0].value = 0;
    num_proc = 1;
    default_tag_idx = 0;
    raw_pty_idx = 0;
    autopilot_control_pty_idx = 0;
    return 0;
}

int load_virtioso_registry(const char *registry_path)
{
    char *json;
    const char *pos;
    struct tag *stream_tags = NULL;
    int stream_count = 0;
    int stream_capacity = 0;

    if (init_virtioso_tags() != 0) {
        return -1;
    }
    stream_tags = tags;
    stream_count = num_proc;
    stream_capacity = VIRTIOSO_MAX_PTY_COUNT;

    json = read_text_file(registry_path, NULL);
    if (!json) {
        return -1;
    }
    pos = json;
    while ((pos = strstr(pos, "\"stream_id\"")) != NULL) {
        const char *object_end = strchr(pos, '}');
        const char *component_pos;
        int stream_id;
        char *name;

        if (!object_end) {
            fprintf(stderr, "ERROR: malformed Virtioso registry near stream_id\n");
            goto fail;
        }
        if (json_int_value_after(pos, "\"stream_id\"", &stream_id) != 0) {
            fprintf(stderr, "ERROR: malformed Virtioso stream_id\n");
            goto fail;
        }
        component_pos = strstr(pos, "\"component\"");
        if (!component_pos || component_pos > object_end) {
            fprintf(stderr, "ERROR: malformed Virtioso component name\n");
            goto fail;
        }
        name = json_string_value_after(component_pos, "\"component\"");
        if (!name) {
            free(name);
            fprintf(stderr, "ERROR: malformed Virtioso component name\n");
            goto fail;
        }
        if (append_virtioso_stream(&stream_tags, &stream_count, &stream_capacity, stream_id, name) != 0) {
            goto fail;
        }
        pos = object_end + 1;
    }
    free(json);
    if (stream_count == 1) {
        fprintf(stderr, "ERROR: Virtioso registry has no valid streams\n");
        goto fail_no_json;
    }
    num_proc = stream_count;
    default_tag_idx = 0;
    return 0;

fail:
    free(json);
fail_no_json:
    for (int i = 1; i < stream_count; i++) {
        free(stream_tags[i].name);
    }
    free(stream_tags);
    return -1;
}

static int find_chip_tag_idx_by_name(const char *name)
{
    int tag_idx;
    const struct tag *tag;

    for (tag_idx = 0, tag = &chip_tags[tag_idx];
         tag_idx < (int)(sizeof(chip_tags) / sizeof(chip_tags[0]));
         tag_idx++, tag++) {
        if (strcmp(tag->name, name) == 0) {
            return tag_idx;
        }
    }
    return -1;
}

static int find_virtioso_pty_idx_by_stream_id(unsigned char stream_id)
{
    int tag_idx;
    const struct tag *tag;

    for_each_tags(tag_idx, tag) {
        if (tag->value == stream_id) {
            return tag_idx;
        }
    }
    return -1;
}

static void write_autopilot_control_stream_announce(unsigned char stream_id, const char *name)
{
    char event[(MAX_PATH * 2) + 128];
    size_t pos = 0;
    int prefix_len;

    if (autopilot_control_pty_idx < 0 ||
        autopilot_control_pty_idx >= pty_max_count ||
        pty_data[autopilot_control_pty_idx].fd < 0) {
        return;
    }

    prefix_len = snprintf(
        event,
        sizeof(event),
        "{\"event\":\"stream_announce\",\"stream_id\":%u,\"name\":\"",
        stream_id
    );
    if (prefix_len <= 0 || prefix_len >= (int)sizeof(event)) {
        return;
    }
    pos = (size_t)prefix_len;
    for (const char *p = name; *p != '\0' && pos + 3 < sizeof(event); p++) {
        if (*p == '"' || *p == '\\') {
            event[pos++] = '\\';
        }
        event[pos++] = *p;
    }
    if (pos + 3 >= sizeof(event)) {
        return;
    }
    event[pos++] = '"';
    event[pos++] = '}';
    event[pos++] = '\n';

    (void)write(pty_data[autopilot_control_pty_idx].fd, event, pos);
    if (pty_data[autopilot_control_pty_idx].log_fd >= 0) {
        (void)write(pty_data[autopilot_control_pty_idx].log_fd, event, pos);
    }
}

static int append_announced_virtioso_stream(unsigned char stream_id, const char *name)
{
    char *owned_name;

    if (!virtioso_mode_enabled) {
        return -1;
    }
    if (stream_id == 0 ||
        stream_id == VIRTIOSO_UART_PROTO_ESC_START ||
        stream_id == VIRTIOSO_UART_PROTO_ESC_CONTROL) {
        fprintf(stderr, "ERROR: invalid announced Virtioso stream id %u\n", stream_id);
        return -1;
    }
    if (find_virtioso_pty_idx_by_stream_id(stream_id) >= 0) {
        return 0;
    }
    if (num_proc >= pty_capacity || num_proc >= VIRTIOSO_MAX_PTY_COUNT) {
        fprintf(stderr, "ERROR: no space for announced Virtioso stream %u\n", stream_id);
        return -1;
    }
    owned_name = strdup(name);
    if (!owned_name) {
        return -1;
    }
    tags[num_proc].name = owned_name;
    tags[num_proc].value = stream_id;
    if (create_pty_at_index(num_proc, tags[num_proc].name, true) != 0) {
        free(owned_name);
        tags[num_proc].name = NULL;
        tags[num_proc].value = 0;
        return -1;
    }
    write_autopilot_control_stream_announce(stream_id, name);
    num_proc++;
    pty_max_count = num_proc;
    return 0;
}

bool should_retry_io(ssize_t ret)
{
    return (ret == -1) &&
        (
        errno == EINTR ||
        errno == EAGAIN ||
        errno == EWOULDBLOCK
        );
}

int putbuf_or_exit(int fd, const unsigned char *buf, size_t len)
{
    ssize_t ret;
    struct pollfd pfd;

    pfd.fd = fd;
    pfd.events = POLLOUT;
    pfd.revents = 0;

    do {
        ret = poll(&pfd, 1, poll_output_timeout /* ms */);
    } while (should_retry_io(ret));

    if (ret == 0) {
        fprintf(stderr, "ERROR: poll() on TTY device timed out.\n");
        return -1;
    }
    assert(ret == 1);
    assert(!(pfd.revents & POLLNVAL));

    do {
        ret = write(fd, buf, len);
    } while (should_retry_io(ret));

    if (ret < 0) {
        fprintf(stderr, "ERROR: failed to write - %s\n", strerror(errno));
        return ret;
    }

    if ((size_t)ret != len) {
        fprintf(stderr, "ERROR: failed to write bytes. Only %zd written but %zu requested\n",
            ret, len);
        return ret;
    }

    return 0;
}

int putch_or_exit(int fd, const unsigned char ch)
{
    ssize_t ret;
    struct pollfd pfd;

    pfd.fd = fd;
    pfd.events = POLLOUT;
    pfd.revents = 0;

    do {
        ret = poll(&pfd, 1, poll_output_timeout /* ms */);
    } while (should_retry_io(ret));

    if (ret == 0) {
        fprintf(stderr, "ERROR: poll() on TTY device timed out.\n");
        return -1;
    }
    assert(ret == 1);
    assert(!(pfd.revents & POLLNVAL));

    do {
        ret = write(fd, &ch, 1);
    } while (should_retry_io(ret));

    if (ret <= 0) {
        fprintf(stderr, "ERROR: failed to write - %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

size_t get_timestamp(char *buf, const size_t maxsize)
{
    /* 'tsfmt' stores 28 chars: "[YYYY-mm-dd HH:MM:SS.%06u] \0" */
    /* resize 'fmt' accordingly when changing the timestamp format 'tsfmt' */
    const char tsfmt[] = "[%Y-%m-%d %H:%M:%S.%%06u] ";
    char fmt[28];

    struct timeval tv;
    struct tm *tm = NULL;
    int len = 0;

    if (buf && maxsize && !gettimeofday(&tv, NULL) && (tm = localtime(&tv.tv_sec))) {
        if (strftime(fmt, sizeof fmt, tsfmt, tm)) {
            /* fill in microseconds */
            len = snprintf(buf, maxsize, fmt, tv.tv_usec);
            if (0 < len && len < (int)maxsize ) {
                /* len should less than maxsize since at least one byte */
                /* must be reserved for the terminating null character */
                return (size_t)len;
            }
        }
    }
    return 0;
}

int flush_stream(int fd, int pty_idx, unsigned char ch)
{
    char timestamp[30];
    size_t len_ts = 0;
    /* 'timestamp' stores 30 chars: "[YYYY-mm-dd HH:MM:SS.ssssss] \0" */
    /* resize it accordingly when changing the timestamp format */

    if ((pty_idx < pty_max_count) && pty_data[pty_idx].log_fd >= 0) {
        if (log_timestamp_enabled) {
            if ('\n' == pty_data[pty_idx].last_ch || '\0' == pty_data[pty_idx].last_ch) {
                len_ts = get_timestamp(timestamp, sizeof timestamp);
                if (0 < len_ts && len_ts < sizeof timestamp)
                    if( write(pty_data[pty_idx].log_fd, timestamp, len_ts) < 0){
                        return -2;
                    }
            }
            pty_data[pty_idx].last_ch = ch;
        }

        if (write(pty_data[pty_idx].log_fd, &ch, 1) < 0){
            return -3;
        }
    }

    // write to pty path
    if (write(fd, &ch, 1) < 0)
        return -1;

    return 0;
}

int patch2flush_stream(int fd, int pty_idx, unsigned char ch, int *p_seen_n, int *p_seen_r)
{
    unsigned char patch_ch = 0;
    int ret_val = 0;

    if (disable_patch) {
        return flush_stream(fd, pty_idx, ch);
    }

    if (ch == '\n') {
        *p_seen_n = 1;
    } else if (ch == '\r') {
        *p_seen_r = 1;
    } else {
        if (*p_seen_r) {
            // write a \n
            patch_ch = '\n';
            flush_stream(fd, pty_idx, patch_ch);
            *p_seen_r = 0;
            *p_seen_n = 0;
        }
        if (*p_seen_n) {
            // write a \r
            patch_ch = '\r';
            flush_stream(fd, pty_idx, patch_ch);
            *p_seen_r = 0;
            *p_seen_n = 0;
        }
    }
    if ((ret_val = flush_stream(fd, pty_idx, ch)) < 0)
        return ret_val;
    if (*p_seen_r && *p_seen_n) {
        *p_seen_r = 0;
        *p_seen_n = 0;
    }

    return 0;
}

static int feed_virtioso_byte(
    unsigned char ch,
    bool *in_escape,
    int *cur_rx_stream,
    int *control_state,
    unsigned char *control_stream_id,
    unsigned char *control_name_len,
    unsigned char *control_name_pos,
    char *control_name,
    int *seen_n,
    int *seen_r
)
{
    int stream_idx;

    if (*control_state != VIRTIOSO_CONTROL_IDLE) {
        switch (*control_state) {
            case VIRTIOSO_CONTROL_CMD:
                if (ch == VIRTIOSO_UART_PROTO_CONTROL_STREAM_ANNOUNCE) {
                    *control_state = VIRTIOSO_CONTROL_STREAM_ID;
                } else {
                    *control_state = VIRTIOSO_CONTROL_IDLE;
                }
                break;
            case VIRTIOSO_CONTROL_STREAM_ID:
                *control_stream_id = ch;
                *control_state = VIRTIOSO_CONTROL_NAME_LEN;
                break;
            case VIRTIOSO_CONTROL_NAME_LEN:
                *control_name_len = ch;
                *control_name_pos = 0;
                if (*control_name_len == 0) {
                    *control_state = VIRTIOSO_CONTROL_IDLE;
                } else {
                    *control_state = VIRTIOSO_CONTROL_NAME;
                }
                break;
            case VIRTIOSO_CONTROL_NAME:
                if (*control_name_pos < MAX_PATH - 1) {
                    control_name[*control_name_pos] = (char)ch;
                }
                (*control_name_pos)++;
                if (*control_name_pos >= *control_name_len) {
                    control_name[*control_name_len] = '\0';
                    (void)append_announced_virtioso_stream(*control_stream_id, control_name);
                    *control_state = VIRTIOSO_CONTROL_IDLE;
                }
                break;
            default:
                *control_state = VIRTIOSO_CONTROL_IDLE;
                break;
        }
        return 0;
    }

    if (*in_escape) {
        *in_escape = false;
        if (ch == VIRTIOSO_UART_PROTO_ESC_ESC) {
            if (*cur_rx_stream >= 0) {
                return patch2flush_stream(
                    pty_data[*cur_rx_stream].fd,
                    *cur_rx_stream,
                    ch,
                    &seen_n[*cur_rx_stream],
                    &seen_r[*cur_rx_stream]
                );
            }
            return 0;
        }
        if (ch == VIRTIOSO_UART_PROTO_ESC_CONTROL) {
            *control_state = VIRTIOSO_CONTROL_CMD;
            return 0;
        }
        stream_idx = find_virtioso_pty_idx_by_stream_id(ch);
        if (stream_idx >= 0) {
            *cur_rx_stream = stream_idx;
        } else {
            *cur_rx_stream = -1;
        }
        return 0;
    }

    if (ch == VIRTIOSO_UART_PROTO_ESC_START) {
        *in_escape = true;
        return 0;
    }

    if (*cur_rx_stream < 0) {
        return 0;
    }
    return patch2flush_stream(
        pty_data[*cur_rx_stream].fd,
        *cur_rx_stream,
        ch,
        &seen_n[*cur_rx_stream],
        &seen_r[*cur_rx_stream]
    );
}

static int feed_nvidia_outer_or_virtioso_raw(
    unsigned char ch,
    bool *outer_in_escape,
    int *cur_outer_tag,
    bool *virtioso_in_escape,
    int *cur_rx_stream,
    int *control_state,
    unsigned char *control_stream_id,
    unsigned char *control_name_len,
    unsigned char *control_name_pos,
    char *control_name,
    int *seen_n,
    int *seen_r
)
{
    int tag_idx;
    const struct tag *tag;

    if (virtioso_outer_mode == VIRTIOSO_OUTER_RAW) {
        return feed_virtioso_byte(
            ch,
            virtioso_in_escape,
            cur_rx_stream,
            control_state,
            control_stream_id,
            control_name_len,
            control_name_pos,
            control_name,
            seen_n,
            seen_r
        );
    }

    if (*outer_in_escape) {
        *outer_in_escape = false;
        if (ch == UART_PROTO_ESC_ESC) {
            if (*cur_outer_tag == virtioso_outer_tag_idx) {
                return feed_virtioso_byte(
                    UART_PROTO_ESC_START,
                    virtioso_in_escape,
                    cur_rx_stream,
                    control_state,
                    control_stream_id,
                    control_name_len,
                    control_name_pos,
                    control_name,
                    seen_n,
                    seen_r
                );
            }
            return 0;
        }
        if (ch == UART_PROTO_ESC_RESET) {
            *cur_outer_tag = -1;
            *cur_rx_stream = -1;
            *virtioso_in_escape = false;
            return 0;
        }
        for (tag_idx = 0, tag = &chip_tags[tag_idx];
             tag_idx < (int)(sizeof(chip_tags) / sizeof(chip_tags[0]));
             tag_idx++, tag++) {
            if (ch == tag->value) {
                *cur_outer_tag = tag_idx;
                return 0;
            }
        }
        return 0;
    }

    if (ch == UART_PROTO_ESC_START) {
        *outer_in_escape = true;
        return 0;
    }
    if (*cur_outer_tag == virtioso_outer_tag_idx) {
        return feed_virtioso_byte(
            ch,
            virtioso_in_escape,
            cur_rx_stream,
            control_state,
            control_stream_id,
            control_name_len,
            control_name_pos,
            control_name,
            seen_n,
            seen_r
        );
    }
    return 0;
}

ut_static void uucp_unlock_tty_device(void)
{
    if (filelock[0] && uucp_locked) {
        unlink(filelock);
        uucp_locked = false;
    }
}

static void handle_sigint(int sig) {
    (void)sig;
    uucp_unlock_tty_device();
    exit(0);
}

ut_static int uucp_set_filelock_name(void)
{
    char *ptr;
    char buf[MAX_PATH];
    int ret, len, i;

    ptr = strchr(tty_device + 1, '/');
    ptr = ptr ? ptr + 1 : (char*)tty_device;
    strncpy(buf, ptr, sizeof(buf) - 1);
    ptr = buf;
    len = strlen(ptr);
    for (i = 0; i < len; i++) {
        if (ptr[i] == '/')
            ptr[i] = '_';
    }

    ret = snprintf(filelock, sizeof(filelock), "%s/LCK..%s", UUCP_DIR, ptr);
    if (ret < 0 || (size_t)ret >= sizeof(filelock))
        return -1;

    return 0;
}

ut_static bool uucp_check_is_locked(void)
{
    char buf[MAX_PATH];
    int fd, n, pid;

    fd = open(filelock, O_RDONLY);
    if (fd >= 0) {
        memset(buf, 0, sizeof(buf));
        n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        pid = (n == 4) ? *(int *)buf : strtol(buf, NULL, 10);
        if (pid > 0 && kill((pid_t)pid, 0) < 0 && errno == ESRCH) {
            /* stale lock file */
            fprintf(stdout, "Removing stale lock: %s\n", filelock);
            sleep(1);
            unlink(filelock);
        } else {
            return true;
        }
    }

    return false;
}

ut_static int uucp_lock_tty_device(void)
{
    char buf[MAX_PATH];
    int ret, fd;

    fd = open(filelock, O_WRONLY|O_CREAT|O_EXCL, 0666);
    if ( fd < 0 )
        return -1;
    ret = snprintf(buf, sizeof(buf), "%10d\n", getpid());
    if (ret < 0 || (size_t)ret >= sizeof(buf)) {
        close(fd);
        return -1;
    }
    if (write(fd, buf, strlen(buf)) < 0){
        close(fd);
        return -2;
    }
    close(fd);

    signal(SIGINT, handle_sigint);
    uucp_locked = true;
    return 0;
}

ut_static int open_tty_device(void)
{
    int fd = -1;
    speed_t baudrate;
    struct flock tty_flock;
    struct termios options;
    struct stat sbuf;
    unsigned char chr;
    int retries = 40;

    // use uucp locking if lock file directory is present
    // since minicom still uses it
    if (!disable_uucp_lock && stat(UUCP_DIR, &sbuf) == 0 && !uucp_locked) {
        if (uucp_set_filelock_name())  {
            fprintf(stderr, "ERROR: unable to set the filelock name\n");
            goto err;
        }
        if (uucp_check_is_locked() || uucp_lock_tty_device()) {
            fprintf(stderr, "ERROR: unable to obtain file lock\n");
            goto err;
        }
    }

    // bounds for -d arg
    while ((fd = open(tty_device, O_RDWR|O_NOCTTY)) < 0) {
        if (--retries < 0)
            break;
        usleep(50000);
    }
    if (fd < 0) {
        fprintf(stderr, "ERROR: failed to open %s\n", tty_device);
        goto err;
    }

    // bounds for -r arg
    if (invalid_baudrate(int_baudrate)) {
        fprintf(stderr, "ERROR: invalid baudrate\n");
        goto err;
    }
    baudrate = get_baudrate(int_baudrate);

    // try to lock tty device
    tty_flock.l_type = F_WRLCK;
    tty_flock.l_whence = SEEK_SET;
    tty_flock.l_start = 0;
    tty_flock.l_len = 0;
    if (fcntl(fd, F_SETLK, &tty_flock)) {
        fprintf(stderr, "ERROR: TTY %s possibly locked by other uart_muxer instance!\n", tty_device);
        goto err;
    }

    tcgetattr(fd, &options);

    // set baudrate for in/out
    if (cfsetispeed(&options, baudrate)) {
        fprintf(stderr, "ERROR: failed to set in_baudrate %d\n", int_baudrate);
        goto err;
    }
    if (cfsetospeed(&options, baudrate)) {
        fprintf(stderr, "ERROR: failed to set out_baudrate %d\n", int_baudrate);
        goto err;
    }

    cfmakeraw (&options);

    /* MIN == 0; TIME == 0: If data is available, read(2) returns
     * immediately, with the lesser of the number of bytes available,
     * or the number of bytes requested. If no data is available, read(2)
     * returns 0 */
    options.c_cc[VMIN] = 0;
    options.c_cc[VTIME] = 0;

    options.c_iflag &= ~(INLCR | ICRNL | IGNPAR);
    options.c_iflag |= IGNBRK;

    options.c_oflag &= ~(ONLCR | OCRNL | ONOCR);
    options.c_lflag &= ~(IEXTEN | ECHO | ECHOK | ECHOE | ECHOKE | ECHOCTL | ECHONL);
    options.c_cflag |= CS8;
    options.c_cflag &= ~CRTSCTS;

    if (tcsetattr(fd, TCSANOW, &options) < 0) {
        fprintf(stderr, "ERROR: on tcsetattr - %s\n", strerror(errno));
        goto err;
    }

    //TODO: Confirm that all attributes were set correctly

    chr = '\0';
    putch_or_exit(fd, chr);
    chr = UART_PROTO_ESC_START;
    putch_or_exit(fd, chr);
    chr = UART_PROTO_ESC_RESET;
    putch_or_exit(fd, chr);

    return fd;

err:
    fprintf(stderr, "%s: failed\n", __func__);
    if (fd >= 0)
        close(fd);
    return -1;
}

ut_static int reopen_tty_device(int old_fd)
{
    if (old_fd >= 0)
        close(old_fd);
    return open_tty_device();
}

void* tty_input_handler(void *arg)
{
    (void)arg;
    unsigned char *buf = NULL;
    struct pollfd pfd;

    unsigned char ch;
    bool tag_match = false;
    bool in_escape = false;
    bool virtioso_in_escape = false;
    bool virtioso_outer_in_escape = false;
    int cur_rx_guest = 0;
    int cur_rx_stream = -1;
    int cur_outer_tag = -1;
    int virtioso_control_state = VIRTIOSO_CONTROL_IDLE;
    unsigned char virtioso_control_stream_id = 0;
    unsigned char virtioso_control_name_len = 0;
    unsigned char virtioso_control_name_pos = 0;
    char virtioso_control_name[MAX_PATH];
    int tag_idx;
    const struct tag *tag;

    int index;
    ssize_t len;
    int ret_val;
    int seen_n[pty_max_count]; // flag set for seeing '\n'
    int seen_r[pty_max_count]; // flag set for seeing '\r'

    memset(seen_n, 0, sizeof(seen_n));
    memset(seen_r, 0, sizeof(seen_r));

    buf = malloc(temporary_buffer_size);
    if (!buf) {
        fprintf(stderr,
            "ERROR: Failed to allocate temporary buffer with size %zu bytes\n",
            temporary_buffer_size);
        goto out;
    }

    while (tcu_muxer_started) {
        pfd.fd = tty_data.fd;
        pfd.events = POLLIN;
        pfd.revents = 0;

        ret_val = poll(&pfd, 1, -1);

        if (should_retry_io(ret_val))
            continue;
        assert(ret_val == 1);
        assert(!(pfd.revents & POLLNVAL));

        if (pfd.revents & (POLLHUP|POLLERR)) {
#ifdef UNIT_TEST
            // This additional check is needed to allow the thread to exit gracefully during unit testing.
            // Poll() returns POLLUP when the mock device closes at the end of a test, so we check here if the test
            // is over to avoid failing the test.
            if(!tcu_muxer_started)
                goto out;
#endif
            tty_data.fd = reopen_tty_device(tty_data.fd);
            if (tty_data.fd >= 0) {
                continue;
            } else {
                fprintf(stderr, "ERROR: tty_input_handler: hangup\n");
                exit(-1);
            }
        }

        len = read(tty_data.fd, buf, temporary_buffer_size);
        if (len < 0) {
            fprintf(stderr, "ERROR: failed to read\n");
            goto out;
        }
        if (tty_data.log_fd >= 0){
            if (write(tty_data.log_fd, buf, len) < 0){
                fprintf(stderr, "ERROR: failed to write\n");
                goto out;
            }
        }

        for (index = 0; index < len; index++) {
            ch = buf[index];

            if (!virtioso_mode_enabled) {
                patch2flush_stream(pty_data[raw_pty_idx].fd, raw_pty_idx,
                    ch, &seen_n[raw_pty_idx], &seen_r[raw_pty_idx]);
            }

            if (virtioso_mode_enabled) {
                ret_val = feed_nvidia_outer_or_virtioso_raw(
                    ch,
                    &virtioso_outer_in_escape,
                    &cur_outer_tag,
                    &virtioso_in_escape,
                    &cur_rx_stream,
                    &virtioso_control_state,
                    &virtioso_control_stream_id,
                    &virtioso_control_name_len,
                    &virtioso_control_name_pos,
                    virtioso_control_name,
                    seen_n,
                    seen_r
                );
                if (ret_val < 0) {
                    goto out;
                }
                continue;
            }

            if (in_escape) {
                in_escape = false;
                // Handle UTC control characters
                switch (ch) {
                    case UART_PROTO_ESC_ESC:
                        ch = UART_PROTO_ESC_START;
                        goto not_escape_ch;

                    case UART_PROTO_ESC_RESET:
                        cur_rx_guest = default_tag_idx;
                        break;

                    default:
                        // Check for tag matches
                        tag_match = false;
                        for_each_tags(tag_idx, tag) {
                            if (ch == tag->value) {
                                cur_rx_guest = tag_idx;
                                tag_match = true;
                                break;
                            }
                        }
                        if (!tag_match) {
                            // this shouldn't happen.
                            // Commenting out the log for now...
                            // fprintf(stderr, "Invalid control character 0x%x, ignoring...\n", ch);
                        }
                        break;
                }
            } else {
                if (ch == UART_PROTO_ESC_START) {
                    in_escape = true;
                } else {
not_escape_ch:
                        assert(cur_rx_guest < (int) (pty_max_count));
                        ret_val = patch2flush_stream(pty_data[cur_rx_guest].fd, cur_rx_guest,
                                ch, &seen_n[cur_rx_guest], &seen_r[cur_rx_guest]);
                }
            }
        }
    }
out:
    if (buf)
        free(buf);
    pthread_exit(NULL);
}

static int write_virtioso_data_to_uart(unsigned char pty_idx, const unsigned char *data, size_t len)
{
    size_t processed = 0;
    static bool write_raw_pty_warning_shown = false;
    ssize_t r = 0;

    if (pty_idx >= pty_max_count) {
        fprintf(stderr, "ERROR: Invalid pty\n");
        return -EINVAL;
    }

    if (pty_idx == raw_pty_idx) {
        if (!enable_write_raw_pty) {
            if (!write_raw_pty_warning_shown) {
                fprintf(stderr, "WARNING: Writing to autopilot_control is disabled. Use -w to enable raw UART writes.\n");
                write_raw_pty_warning_shown = true;
            }
            return 0;
        }
        pthread_mutex_lock(&tty_data.write_lock);
        r = putbuf_or_exit(tty_data.fd, data, len);
        pthread_mutex_unlock(&tty_data.write_lock);
        return (int)r;
    }

    pthread_mutex_lock(&tty_data.write_lock);
    while (processed < len) {
        size_t chunk_size = (len - processed > MAX_WRITE_CHUNK_SIZE) ?
                            MAX_WRITE_CHUNK_SIZE : (len - processed);
        size_t inner_max = (chunk_size * 2) + 2;
        size_t outer_max = (inner_max * 2) + 2;
        unsigned char *inner_buf = malloc(inner_max);
        unsigned char *outer_buf = malloc(outer_max);
        size_t inner_idx = 0;
        size_t outer_idx = 0;

        if (!inner_buf || !outer_buf) {
            free(inner_buf);
            free(outer_buf);
            pthread_mutex_unlock(&tty_data.write_lock);
            return -ENOMEM;
        }

        inner_buf[inner_idx++] = VIRTIOSO_UART_PROTO_ESC_START;
        inner_buf[inner_idx++] = tags[pty_idx].value;
        for (size_t data_index = 0; data_index < chunk_size; data_index++) {
            unsigned char byte = data[processed + data_index];
            if (byte == VIRTIOSO_UART_PROTO_ESC_START) {
                inner_buf[inner_idx++] = VIRTIOSO_UART_PROTO_ESC_ESC;
            }
            inner_buf[inner_idx++] = byte;
        }

        if (virtioso_outer_mode == VIRTIOSO_OUTER_NVIDIA_TCU) {
            outer_buf[outer_idx++] = UART_PROTO_ESC_START;
            outer_buf[outer_idx++] = chip_tags[virtioso_outer_tag_idx].value;
            for (size_t inner_pos = 0; inner_pos < inner_idx; inner_pos++) {
                unsigned char byte = inner_buf[inner_pos];
                outer_buf[outer_idx++] = byte;
                if (byte == UART_PROTO_ESC_START) {
                    outer_buf[outer_idx++] = UART_PROTO_ESC_ESC;
                }
            }
            r = putbuf_or_exit(tty_data.fd, outer_buf, outer_idx);
        } else {
            r = putbuf_or_exit(tty_data.fd, inner_buf, inner_idx);
        }

        free(inner_buf);
        free(outer_buf);
        if (r < 0) {
            pthread_mutex_unlock(&tty_data.write_lock);
            return (int)r;
        }
        processed += chunk_size;
    }
    pthread_mutex_unlock(&tty_data.write_lock);
    return 0;
}

ut_static int write_data_to_uart(unsigned char pty_idx, const unsigned char *data, size_t len)
{
    const unsigned char esc = UART_PROTO_ESC_START;
    const unsigned char esc_esc = UART_PROTO_ESC_ESC;
    size_t data_index;
    ssize_t r;
    size_t encoded_buf_index = 0;
    size_t processed = 0;
    size_t chunk_size;
    static bool write_raw_pty_warning_shown = false;

    if (virtioso_mode_enabled) {
        return write_virtioso_data_to_uart(pty_idx, data, len);
    }

    if (pty_idx >= pty_max_count) {
        fprintf(stderr, "ERROR: Invalid pty\n");
        return -EINVAL;
    }

    if (pty_idx == raw_pty_idx && !enable_write_raw_pty) {
        if (!write_raw_pty_warning_shown) {
            fprintf(stderr, "WARNING: Writing to RAW client is disabled. Use -w to enable.\n");
            write_raw_pty_warning_shown = true;
        }
        return 0;
    }

    /*
     * Process data in chunks to limit memory allocation.
     * Maximum encoded buffer size: MAX_WRITE_CHUNK_SIZE * 4 + 16
     * This prevents excessive memory usage for large writes.
     */
    const size_t max_encoded_buf_len = MAX_WRITE_CHUNK_SIZE * 4 + 16;
    unsigned char *encoded_buf = malloc(max_encoded_buf_len);
    if (!encoded_buf) {
        fprintf(stderr, "ERROR: Failed to allocate %zu bytes.\n", max_encoded_buf_len);
        return -ENOMEM;
    }

#define WRITE_BYTE_TO_ENCODED_BUF(x)                                     \
    do {                                                                 \
        if (encoded_buf_index >= max_encoded_buf_len)                    \
        {                                                                \
            fprintf(stderr,                                              \
                "ERROR: Buffer not large enough. Buf size: %zu. Index: %zu\n",  \
                max_encoded_buf_len, encoded_buf_index);                 \
            free(encoded_buf);                                           \
            return -ENOMEM;                                              \
        }                                                                \
        encoded_buf[encoded_buf_index++] = (x);                          \
    } while (0)

    pthread_mutex_lock(&tty_data.write_lock);

    while (processed < len) {
        encoded_buf_index = 0;
        chunk_size = (len - processed > MAX_WRITE_CHUNK_SIZE) ? 
                     MAX_WRITE_CHUNK_SIZE : (len - processed);

        /* Add tag header only for the first chunk of non-raw PTY */
        if (processed == 0 && pty_idx != raw_pty_idx) {
            WRITE_BYTE_TO_ENCODED_BUF(esc);
            WRITE_BYTE_TO_ENCODED_BUF(tags[pty_idx].value);
        }

        for (data_index = 0; data_index < chunk_size; data_index++) {
            // send data
            WRITE_BYTE_TO_ENCODED_BUF(data[processed + data_index]);
            if (data[processed + data_index] == esc && pty_idx != raw_pty_idx) {
                WRITE_BYTE_TO_ENCODED_BUF(esc_esc);
            }
        }

        r = putbuf_or_exit(tty_data.fd, encoded_buf, encoded_buf_index);
        if (r < 0) {
            fprintf(stderr, "ERROR: Failed to write buffer. Error: %zd\n", r);
            goto fail;
        }

        processed += chunk_size;
    }

    pthread_mutex_unlock(&tty_data.write_lock);
    free(encoded_buf);

#undef WRITE_BYTE_TO_ENCODED_BUF

    return 0;

fail:
    pthread_mutex_unlock(&tty_data.write_lock);
    if (encoded_buf)
        free(encoded_buf);

    return r;
}

void* pty_input_handler(void* arg)
{
    struct thread_data *t = arg;
    unsigned char pty_idx = t->id;

    unsigned char *buf = NULL;
    struct pollfd pfd;

    ssize_t len;
    int ret;

    buf = malloc(temporary_buffer_size);
    if (!buf) {
        fprintf(stderr,
            "ERROR: Failed to allocate temporary buffer with size %zu bytes\n",
            temporary_buffer_size);
        pthread_exit(NULL);
    }

    while (tcu_muxer_started) {
        pfd.fd = t->fd;
        pfd.events = POLLIN;
        pfd.revents = 0;

        ret = poll(&pfd, 1, -1);
        if (should_retry_io(ret))
            continue;
        assert(ret == 1);
        assert(!(pfd.revents & POLLNVAL));

        /* Guest pseudo-terminals continuously report
         * POLLIN when disconnected, so we spin
         * slowly to wait for valid data. */
        if (!(pfd.revents & POLLIN)) {
            sleep(1);
            continue;
        }

        /* Read the data from pts */
        len = read(t->fd, buf, temporary_buffer_size);
        if (len > 0) {
            /* Send data */
            ret = write_data_to_uart(pty_idx, buf, (size_t)len);
            if (ret < 0)
                fprintf(stderr, "Failed to write to uart\n");
        }
    }

    if (buf)
        free(buf);

    return 0;
}

speed_t get_baudrate(int baudrate)
{
    speed_t speed = -1;
    switch(baudrate) {
        case 0:
            speed = B0;
            break;
        case 50:
            speed = B50;
            break;
        case 75:
            speed = B75;
            break;
        case 110:
            speed = B110;
            break;
        case 134:
            speed = B134;
            break;
        case 150:
            speed = B150;
            break;
        case 200:
            speed = B200;
            break;
        case 300:
            speed = B300;
            break;
        case 600:
            speed = B600;
            break;
        case 1200:
            speed = B1200;
            break;
        case 1800:
            speed = B1800;
            break;
        case 2400:
            speed = B2400;
            break;
        case 4800:
            speed = B4800;
            break;
        case 9600:
            speed = B9600;
            break;
        case 19200:
            speed = B19200;
            break;
        case 38400:
            speed = B38400;
            break;
        case 57600:
            speed = B57600;
            break;
        case 115200:
            speed = B115200;
            break;
        case 230400:
            speed = B230400;
            break;
        default:
            break;
    }
    return speed;
}

int invalid_baudrate(int baudrate)
{
    if((int)get_baudrate(baudrate) == -1)
        return 1;
    return 0;
}

int create_thread_with_stack(pthread_t *thread, void *(*start_routine)(void *), void *arg)
{
    pthread_attr_t attr;
    int ret;

    ret = pthread_attr_init(&attr);
    if (ret != 0) {
        fprintf(stderr, "ERROR: pthread_attr_init failed: %s\n", strerror(ret));
        return ret;
    }

    ret = pthread_attr_setstacksize(&attr, THREAD_STACK_SIZE);
    if (ret != 0) {
        fprintf(stderr, "ERROR: pthread_attr_setstacksize failed: %s\n", strerror(ret));
        pthread_attr_destroy(&attr);
        return ret;
    }

    ret = pthread_create(thread, &attr, start_routine, arg);
    if (ret != 0) {
        fprintf(stderr, "ERROR: pthread_create failed: %s\n", strerror(ret));
        pthread_attr_destroy(&attr);
        return ret;
    }

    pthread_attr_destroy(&attr);
    return 0;
}

int create_pty_at_index(int pty_idx, const char *name, bool start_thread)
{
    struct thread_data *pty;
    struct termios options;
    int fd;

    if (pty_idx < 0 || pty_idx >= pty_capacity) {
        fprintf(stderr, "ERROR: PTY index %d out of range\n", pty_idx);
        return -1;
    }

    pty = &pty_data[pty_idx];
    fd = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "ERROR: posix_openpt failed for guest %d\n", pty_idx);
        return -1;
    }
    if (grantpt(fd)) {
        fprintf(stderr, "ERROR: grantpt failed for guest %d\n", pty_idx);
        close(fd);
        return -1;
    }
    if (unlockpt(fd)) {
        fprintf(stderr, "ERROR: unlockpt failed for guest %d\n", pty_idx);
        close(fd);
        return -1;
    }

    if (save_output_path) {
        char savefile[1024];
        snprintf(savefile, sizeof(savefile), "%s/%s", save_output_path, name);
        strncat(savefile, ".txt", sizeof(savefile) - strlen(savefile) - 1);
        pty->log_fd = open(savefile, O_APPEND|O_CREAT|O_WRONLY,
                           S_IRUSR|S_IRGRP|S_IROTH|S_IWUSR);
        if (pty->log_fd < 0)
            fprintf(stderr, "ERROR: logfile: %s open failed!\n", savefile);
    } else {
        pty->log_fd = -1;
    }
    ptsname_r(fd, pty->device_name, sizeof(pty->device_name));
    pty->fd = fd;
    pty->id = (unsigned int)pty_idx;
    pty->last_ch = '\0';
    pthread_mutex_init(&pty->write_lock, NULL);
    {
        int slave = open(pty->device_name, O_RDWR);
        if (slave < 0) {
            fprintf(stderr, "ERROR: failed to open %s\n", pty->device_name);
            return -1;
        }
        tcgetattr(slave, &options);
        cfmakeraw(&options);
        tcsetattr(slave, TCSANOW, &options);
        close(slave);
    }
    fprintf(stdout, "%s\t%s\n", pty->device_name, name);
    fflush(stdout);

    if (start_thread && create_thread_with_stack(&pty_thread[pty_idx], pty_input_handler, pty)) {
        fprintf(stderr, "ERROR: failed to spawn pty thread %d\n", pty_idx);
        return -1;
    }
    return 0;
}

void print_usage(char *argv[])
{
    fprintf(stderr, "Usage: %s [OPTION]...\n", argv[0]);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "\t -h       : "
            "Print this help screen\n");
    fprintf(stderr, "\t -i       : "
            "Enable the patch for line ending\n");
    fprintf(stderr, "\t -u       : "
            "Use separate uart_muxer tool for Guest console\n");
    fprintf(stderr, "\t -d <dev> : "
            "Select UART device <dev> to communicate with. Default: %s\n", DEFAULT_TTY_DEVICE);
    fprintf(stderr, "\t -r <rate>: "
            "Select UART i/o speed <rate>. Default: %d\n", DEFAULT_BAUDRATE);
    fprintf(stderr, "\t -s <path>: "
            "Save the output to directory <path>\n");
    fprintf(stderr, "\t -t       : "
            "Prefix the output with a timestamp\n");
    fprintf(stderr, "\t -p <int> : "
            "Set the timeout for output polling ready status. Default: %d\n", DEFAULT_POLL_OUTPUT_TIMEOUT);
    fprintf(stderr, "\t -l <path>: "
            "Save the raw output with tags to log file <path>\n");
    fprintf(stderr, "\t -w       : "
            "Enable writing to RAW client\n");
    fprintf(stderr, "\t -L       : "
            "Disable UUCP lock file handling for router-managed PTYs\n");
    fprintf(stderr, "\t -V <path>: "
            "Enable Virtioso inner 0xfe demux using generated stream registry JSON\n");
    fprintf(stderr, "\t -A       : "
            "Enable Virtioso inner 0xfe demux with autopilot_control and live stream announcements\n");
    fprintf(stderr, "\t -O <mode>: "
            "Virtioso outer input mode: raw or nvidia-tcu. Default: raw\n");
    fprintf(stderr, "\t -C <tag> : "
            "NVIDIA TCU client tag carrying Virtioso 0xfe traffic when -O nvidia-tcu is used. Default: CCPLEX\n");
}

int main(int argc, char *argv[])
{
    char *raw_log_file_path = NULL;

    pthread_t tty_thread;
    struct termios options;
    int opt;
    int i;
    size_t len;
    struct thread_data *pty;

    while ((opt = getopt(argc, argv, ":d:r:s:l:p:V:O:C:AhitwL")) != -1) {
        switch (opt)
        {
            case 'd':
                tty_device = optarg;
                break;
            case 'r':
                int_baudrate = atoi(optarg);
                break;
            case 's':
                len = strlen(optarg) + 1;
                if (len > sizeof(path)) {
                    fprintf(stderr, "-s argument length exceeds buffer size\n");
                    return -1;
                }
                strncpy(path, optarg, sizeof(path) - 1);
                save_output_path = path;
                if (save_output_path[strlen(save_output_path) - 1] == '/')
                    save_output_path[strlen(save_output_path) - 1] = '\0';
                break;
            case 'l':
                raw_log_file_path = optarg;
                break;
            case 'h':
                print_usage(argv);
                return 0;
            case 'i':
                disable_patch = 0;
                break;
            case 't':
                log_timestamp_enabled = true;
                break;
            case 'w':
                enable_write_raw_pty = true;
                break;
            case 'L':
                disable_uucp_lock = true;
                break;
            case 'p':
                poll_output_timeout = atoi(optarg);
                break;
            case 'V':
                virtioso_registry_path = optarg;
                virtioso_mode_enabled = true;
                break;
            case 'A':
                virtioso_wait_for_announce = true;
                virtioso_mode_enabled = true;
                break;
            case 'O':
                if (strcmp(optarg, "raw") == 0) {
                    virtioso_outer_mode = VIRTIOSO_OUTER_RAW;
                } else if (strcmp(optarg, "nvidia-tcu") == 0) {
                    virtioso_outer_mode = VIRTIOSO_OUTER_NVIDIA_TCU;
                } else {
                    fprintf(stderr, "ERROR: invalid -O mode %s\n", optarg);
                    return -1;
                }
                break;
            case 'C':
                virtioso_outer_tag_name = optarg;
                break;
            case ':':
                fprintf(stderr, "Option `-%c` requires an argument.\n", optopt);
                return -1;
            case '?':
                if (isprint(optopt))
                    fprintf(stderr, "Unknown option `-%c`.\n", optopt);
                else
                    fprintf(stderr, "Unknown option ``\\x%x`.\n", optopt);
                return -1;
            default:
                abort();
        }
    }

    if (virtioso_mode_enabled) {
        if (virtioso_registry_path) {
            if (load_virtioso_registry(virtioso_registry_path) != 0) {
                goto err;
            }
        } else if (virtioso_wait_for_announce) {
            if (init_virtioso_tags() != 0) {
                goto err;
            }
        } else {
            fprintf(stderr, "ERROR: Virtioso mode requires -V <path> or -A\n");
            goto err;
        }
        if (virtioso_outer_mode == VIRTIOSO_OUTER_NVIDIA_TCU) {
            if (!virtioso_outer_tag_name) {
                virtioso_outer_tag_name = "CCPLEX";
            }
            virtioso_outer_tag_idx = find_chip_tag_idx_by_name(virtioso_outer_tag_name);
            if (virtioso_outer_tag_idx < 0) {
                fprintf(stderr, "ERROR: unknown NVIDIA TCU client tag %s\n", virtioso_outer_tag_name);
                goto err;
            }
        }
    } else {
        tags = (struct tag *)chip_tags;
        num_proc = sizeof(chip_tags) / sizeof(chip_tags[0]);
        default_tag_idx = num_proc - 1; // last tag is the default tag
    }

    if (virtioso_mode_enabled) {
        pty_max_count = num_proc;
        pty_capacity = VIRTIOSO_MAX_PTY_COUNT;
    } else {
        pty_max_count = num_proc + 1;
        raw_pty_idx = pty_max_count - 1; // last pty is the raw pty
        pty_capacity = pty_max_count;
    }

    pty_thread = calloc((size_t)pty_capacity, sizeof(pthread_t));
    pty_data = calloc((size_t)pty_capacity, sizeof(struct thread_data));
    if (!pty_thread || !pty_data) {
        fprintf(stderr, "ERROR: failed to allocate pty data\n");
        goto err;
    }

    tty_data.fd = open_tty_device();
    if (tty_data.fd < 0) {
        fprintf(stderr, "ERROR: failed to open in read mode %s\n", tty_device);
        goto err;
    }

    // bounds for -s arg
    if (save_output_path) {
        // check if save directory path exists
        DIR* dir = opendir(save_output_path);
        if (!dir) {
            fprintf(stderr, "ERROR: failed to open directory %s\n", save_output_path);
            goto err;
        }
        closedir(dir);
    }

    if (raw_log_file_path) {
        tty_data.log_fd = open(raw_log_file_path, O_CREAT|O_APPEND|O_WRONLY,
                               S_IRUSR|S_IRGRP|S_IROTH|S_IWUSR);
        if (tty_data.log_fd < 0)
            fprintf(stderr, "ERROR: raw log file open failed!\n");
    } else {
        tty_data.log_fd = -1;
    }

#ifdef UNIT_TEST
    // Exit main early if only testing argument parsing.
    if(ut_main_args_done()) {
        uucp_unlock_tty_device();
        return 0;
    }
#endif

    tty_data.id = -1;
    pthread_mutex_init(&tty_data.write_lock, NULL);

    // create pseudo-terminals and thread locks
    for_each_pty(i, pty) {
        char *name;

        if (virtioso_mode_enabled) {
            name = tags[i].name;
        } else if (i != raw_pty_idx) {
            name = tags[i].name;
        } else {
            name = RAW_PTY;
        }
        assert(name != NULL);
        (void)pty;
        (void)options;
        if (create_pty_at_index(i, name, false) != 0) {
            goto err;
        }
    }

    fflush(stdout);

    if (create_thread_with_stack(&tty_thread, tty_input_handler, &tty_data)) {
        fprintf(stderr, "ERROR: failed to spawn tty thread\n");
        goto err;
    }

    for_each_pty(i, pty) {
        if (create_thread_with_stack(&pty_thread[i], pty_input_handler, pty)) {
            fprintf(stderr, "ERROR: failed to spawn pty thread %d\n", i);
            goto err;
        }
    }

#ifdef UNIT_TEST
    //inform test program that tcu_muxer is now ready to do i/o
    ut_main_threads_started();
#endif

    pthread_join(tty_thread, NULL);
    for_each_pty(i, pty) {
        pthread_join(pty_thread[i], NULL);
    }

    // flock will get released automatically when we close tty fd
    close(tty_data.fd);
    if (raw_log_file_path && tty_data.log_fd >= 0)
        close(tty_data.log_fd);
    for_each_pty(i, pty) {
        close(pty->fd);
        if (save_output_path && pty->log_fd >= 0)
            close(pty->log_fd);
    }
    uucp_unlock_tty_device();
    return 0;
err:
    uucp_unlock_tty_device();
    return -1;
}
