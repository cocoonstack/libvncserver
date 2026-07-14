/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
#include <rfb/keysym.h>
#include <rfb/rfb.h>

#define SCRCPY_CODEC_H264 UINT32_C(0x68323634)
#define SCRCPY_PACKET_FLAG_SESSION (UINT64_C(1) << 63)
#define SCRCPY_PACKET_FLAG_CONFIG (UINT64_C(1) << 62)
#define SCRCPY_PACKET_FLAG_KEY_FRAME (UINT64_C(1) << 61)
#define SCRCPY_POINTER_ID_MOUSE UINT64_MAX
#define SCRCPY_CONTROL_RESET_VIDEO 17

#define FRAME_QUEUE_CAPACITY 90
#define MAX_PACKET_SIZE (8U * 1024U * 1024U)

struct frame {
    uint8_t *data;
    size_t size;
    int key_frame;
    uint64_t sequence;
};

struct client_state {
    uint64_t next_sequence;
    int waiting_for_key_frame;
    int framebuffer_lock_held;
    int previous_left_button;
    int last_pointer_x;
    int last_pointer_y;
};

static struct frame frame_queue[FRAME_QUEUE_CAPACITY];
static size_t frame_queue_head;
static size_t frame_queue_count;
static uint64_t frame_next_sequence = 1;
static uint64_t frame_generation;
static pthread_mutex_t frame_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t frame_cond = PTHREAD_COND_INITIALIZER;

static uint8_t *codec_config;
static size_t codec_config_size;

static AVCodecContext *decoder_context;
static AVFrame *decoder_frame;
static struct SwsContext *sws_context;
static uint8_t *fallback_buffer;
static size_t framebuffer_size;
static int fallback_frame_ready;
static int fallback_decoder_ready_logged;
static pthread_mutex_t fallback_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t screen_buffer_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t screen_buffer_cond = PTHREAD_COND_INITIALIZER;
static int screen_frame_ready;
static volatile sig_atomic_t fallback_mode;
static volatile sig_atomic_t fallback_decoder_reset;
static pthread_mutex_t pointer_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t control_mutex = PTHREAD_MUTEX_INITIALIZER;

static int video_fd = -1;
static int control_fd = -1;
static int video_width;
static int video_height;
static volatile sig_atomic_t running = 1;
static rfbScreenInfoPtr rfb_screen;
static rfbClientPtr pointer_owner;

static void client_gone(rfbClientPtr client);
static int send_touch(uint8_t action, int x, int y, int pressed);

static uint32_t read_u32be(const uint8_t *p) {
    return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16)
         | ((uint32_t) p[2] << 8) | p[3];
}

static uint64_t read_u64be(const uint8_t *p) {
    return ((uint64_t) read_u32be(p) << 32) | read_u32be(p + 4);
}

static void write_u16be(uint8_t *p, uint16_t value) {
    p[0] = value >> 8;
    p[1] = value;
}

static void write_u32be(uint8_t *p, uint32_t value) {
    p[0] = value >> 24;
    p[1] = value >> 16;
    p[2] = value >> 8;
    p[3] = value;
}

static void write_u64be(uint8_t *p, uint64_t value) {
    write_u32be(p, value >> 32);
    write_u32be(p + 4, value);
}

static int recv_all(int fd, void *buffer, size_t size) {
    uint8_t *p = buffer;
    while (size) {
        ssize_t n = recv(fd, p, size, 0);
        if (n == 0) {
            return 0;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        p += n;
        size -= (size_t) n;
    }
    return 1;
}

static int send_all(int fd, const void *buffer, size_t size) {
    const uint8_t *p = buffer;
    while (size) {
        ssize_t n = send(fd, p, size, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        p += n;
        size -= (size_t) n;
    }
    return 0;
}

static int connect_tcp(const char *host, uint16_t port) {
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        fprintf(stderr, "invalid IPv4 address: %s\n", host);
        return -1;
    }

    for (int attempt = 0; attempt < 100 && running; ++attempt) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            return -1;
        }
        if (connect(fd, (struct sockaddr *) &addr, sizeof(addr)) == 0) {
            return fd;
        }
        close(fd);
        struct timespec delay = {.tv_sec = 0, .tv_nsec = 50 * 1000 * 1000};
        nanosleep(&delay, NULL);
    }
    return -1;
}

static void free_frame(struct frame *frame) {
    free(frame->data);
    memset(frame, 0, sizeof(*frame));
}

static void clear_frame_queue_locked(void) {
    while (frame_queue_count) {
        free_frame(&frame_queue[frame_queue_head]);
        frame_queue_head = (frame_queue_head + 1) % FRAME_QUEUE_CAPACITY;
        --frame_queue_count;
    }
}

static uint64_t latest_key_sequence_locked(void) {
    uint64_t sequence = frame_next_sequence;
    for (size_t i = 0; i < frame_queue_count; ++i) {
        size_t index = (frame_queue_head + i) % FRAME_QUEUE_CAPACITY;
        if (frame_queue[index].key_frame) {
            sequence = frame_queue[index].sequence;
        }
    }
    return sequence;
}

static void enqueue_frame(uint8_t *data, size_t size, int key_frame) {
    pthread_mutex_lock(&frame_mutex);

    if (frame_queue_count == FRAME_QUEUE_CAPACITY) {
        free_frame(&frame_queue[frame_queue_head]);
        frame_queue_head = (frame_queue_head + 1) % FRAME_QUEUE_CAPACITY;
        --frame_queue_count;
    }

    size_t tail = (frame_queue_head + frame_queue_count) % FRAME_QUEUE_CAPACITY;
    frame_queue[tail].data = data;
    frame_queue[tail].size = size;
    frame_queue[tail].key_frame = key_frame;
    frame_queue[tail].sequence = frame_next_sequence++;
    ++frame_queue_count;
    ++frame_generation;
    pthread_cond_broadcast(&frame_cond);
    pthread_mutex_unlock(&frame_mutex);
}

static int copy_next_frame(rfbClientPtr client, struct client_state *state,
                           struct frame *frame) {
    pthread_mutex_lock(&frame_mutex);
    while (running && state && client->state == RFB_NORMAL &&
           client->sock != RFB_INVALID_SOCKET) {
        struct frame *source = NULL;

        if (frame_queue_count) {
            uint64_t oldest_sequence = frame_queue[frame_queue_head].sequence;
            if (state->next_sequence < oldest_sequence) {
                state->next_sequence = oldest_sequence;
                state->waiting_for_key_frame = 1;
            }

            uint64_t latest_key = latest_key_sequence_locked();
            if (latest_key > state->next_sequence &&
                frame_next_sequence - state->next_sequence > 6) {
                state->next_sequence = latest_key;
                state->waiting_for_key_frame = 0;
            }

            for (size_t i = 0; i < frame_queue_count; ++i) {
                size_t index = (frame_queue_head + i) % FRAME_QUEUE_CAPACITY;
                struct frame *candidate = &frame_queue[index];
                if (candidate->sequence < state->next_sequence) {
                    continue;
                }
                if (state->waiting_for_key_frame && !candidate->key_frame) {
                    continue;
                }
                source = candidate;
                break;
            }
        }

        if (source) {
            frame->data = malloc(source->size);
            if (!frame->data) {
                pthread_mutex_unlock(&frame_mutex);
                return 0;
            }
            memcpy(frame->data, source->data, source->size);
            frame->size = source->size;
            frame->key_frame = source->key_frame;
            frame->sequence = source->sequence;
            state->next_sequence = source->sequence + 1;
            state->waiting_for_key_frame = 0;
            pthread_mutex_unlock(&frame_mutex);
            return 1;
        }

        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_nsec += 100 * 1000 * 1000;
        if (deadline.tv_nsec >= 1000 * 1000 * 1000) {
            ++deadline.tv_sec;
            deadline.tv_nsec -= 1000 * 1000 * 1000;
        }
        pthread_cond_timedwait(&frame_cond, &frame_mutex, &deadline);
    }

    pthread_mutex_unlock(&frame_mutex);
    return 0;
}

static int init_fallback_decoder(void) {
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) {
        fprintf(stderr, "H.264 fallback decoder not found\n");
        return -1;
    }

    decoder_context = avcodec_alloc_context3(codec);
    decoder_frame = av_frame_alloc();
    if (!decoder_context || !decoder_frame
            || avcodec_open2(decoder_context, codec, NULL) < 0) {
        fprintf(stderr, "failed to initialize H.264 fallback decoder\n");
        return -1;
    }
    return 0;
}

static void decode_fallback_frame(const uint8_t *data, size_t size,
                                  int key_frame, int *decoder_active) {
    if (!fallback_mode) {
        *decoder_active = 0;
        return;
    }

    if (fallback_decoder_reset) {
        *decoder_active = 0;
        fallback_decoder_reset = 0;
    }

    if (!*decoder_active) {
        if (!key_frame) {
            return;
        }
        avcodec_flush_buffers(decoder_context);
        *decoder_active = 1;
    }

    AVPacket *packet = av_packet_alloc();
    if (!packet || size > INT_MAX || av_new_packet(packet, (int) size) < 0) {
        av_packet_free(&packet);
        return;
    }
    memcpy(packet->data, data, size);

    int send_result = avcodec_send_packet(decoder_context, packet);
    if (send_result < 0) {
        char error[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(send_result, error, sizeof(error));
        fprintf(stderr, "fallback decoder rejected packet: %s\n", error);
        av_packet_free(&packet);
        return;
    }
    av_packet_free(&packet);

    while (avcodec_receive_frame(decoder_context, decoder_frame) >= 0) {
        if (decoder_frame->width != video_width
                || decoder_frame->height != video_height) {
            fprintf(stderr, "fallback decoder size mismatch: %dx%d\n",
                    decoder_frame->width, decoder_frame->height);
            av_frame_unref(decoder_frame);
            continue;
        }

        sws_context = sws_getCachedContext(
                sws_context,
                decoder_frame->width, decoder_frame->height,
                (enum AVPixelFormat) decoder_frame->format,
                video_width, video_height, AV_PIX_FMT_RGBA,
                SWS_FAST_BILINEAR, NULL, NULL, NULL);
        if (!sws_context) {
            av_frame_unref(decoder_frame);
            return;
        }

        uint8_t *destinations[] = {fallback_buffer, NULL, NULL, NULL};
        int strides[] = {video_width * 4, 0, 0, 0};
        pthread_mutex_lock(&fallback_mutex);
        sws_scale(sws_context,
                  (const uint8_t *const *) decoder_frame->data,
                  decoder_frame->linesize, 0, decoder_frame->height,
                  destinations, strides);
        fallback_frame_ready = 1;
        if (!fallback_decoder_ready_logged) {
            fprintf(stderr,
                    "ordinary VNC decoder ready: %dx%d format=%d\n",
                    decoder_frame->width, decoder_frame->height,
                    decoder_frame->format);
            fallback_decoder_ready_logged = 1;
        }
        pthread_mutex_unlock(&fallback_mutex);
        av_frame_unref(decoder_frame);
    }
}

static void *video_reader_main(void *unused) {
    (void) unused;
    uint8_t header[12];
    int decoder_active = 0;

    while (running) {
        int status = recv_all(video_fd, header, sizeof(header));
        if (status <= 0) {
            break;
        }

        uint64_t pts_flags = read_u64be(header);
        if (pts_flags & SCRCPY_PACKET_FLAG_SESSION) {
            int width = (int) read_u32be(header + 4);
            int height = (int) read_u32be(header + 8);
            fprintf(stderr, "scrcpy session: %dx%d%s\n", width, height,
                    (width == video_width && height == video_height) ? "" : " (resize requires restart in POC)");
            continue;
        }

        uint32_t packet_size = read_u32be(header + 8);
        if (!packet_size || packet_size > MAX_PACKET_SIZE) {
            fprintf(stderr, "invalid scrcpy packet size: %u\n", packet_size);
            break;
        }

        uint8_t *packet = malloc(packet_size);
        if (!packet || recv_all(video_fd, packet, packet_size) <= 0) {
            free(packet);
            break;
        }

        if (pts_flags & SCRCPY_PACKET_FLAG_CONFIG) {
            free(codec_config);
            codec_config = packet;
            codec_config_size = packet_size;
            continue;
        }

        int key_frame = !!(pts_flags & SCRCPY_PACKET_FLAG_KEY_FRAME);
        if (key_frame && codec_config_size) {
            uint8_t *merged = malloc(codec_config_size + packet_size);
            if (!merged) {
                free(packet);
                break;
            }
            memcpy(merged, codec_config, codec_config_size);
            memcpy(merged + codec_config_size, packet, packet_size);
            free(packet);
            packet = merged;
            packet_size += (uint32_t) codec_config_size;
        }

        decode_fallback_frame(packet, packet_size, key_frame, &decoder_active);
        enqueue_frame(packet, packet_size, key_frame);
    }

    fprintf(stderr, "scrcpy video stream ended\n");
    running = 0;
    pthread_cond_broadcast(&frame_cond);
    return NULL;
}

static void *control_drain_main(void *unused) {
    (void) unused;
    uint8_t buffer[4096];
    while (running) {
        ssize_t n = recv(control_fd, buffer, sizeof(buffer), 0);
        if (n <= 0) {
            break;
        }
    }
    return NULL;
}

static rfbBool h264_encoder_callback(rfbClientPtr client, char **buffer, size_t *size) {
    struct frame frame = {0};
    struct client_state *state = client->clientData;
    if (!copy_next_frame(client, state, &frame)) {
        return FALSE;
    }

    *buffer = (char *) frame.data;
    *size = frame.size;
    return TRUE;
}

static enum rfbNewClientAction new_client(rfbClientPtr client) {
    struct client_state *state = calloc(1, sizeof(*state));
    if (!state) {
        return RFB_CLIENT_REFUSE;
    }

    pthread_mutex_lock(&frame_mutex);
    state->next_sequence = latest_key_sequence_locked();
    state->waiting_for_key_frame = state->next_sequence == frame_next_sequence;
    pthread_mutex_unlock(&frame_mutex);

    client->clientData = state;
    client->clientGoneHook = client_gone;
    fprintf(stderr, "VNC client connected: %s\n", client->host);
    return RFB_CLIENT_ACCEPT;
}

static void client_gone(rfbClientPtr client) {
    struct client_state *state = client->clientData;
    pthread_mutex_lock(&pointer_mutex);
    if (pointer_owner == client) {
        if (state && state->previous_left_button) {
            send_touch(1, state->last_pointer_x, state->last_pointer_y, 0);
        }
        pointer_owner = NULL;
    }
    pthread_mutex_unlock(&pointer_mutex);
    free(client->clientData);
    client->clientData = NULL;
    fprintf(stderr, "VNC client disconnected\n");
}

static int send_touch(uint8_t action, int x, int y, int pressed) {
    uint8_t message[32] = {0};
    message[0] = 2;
    message[1] = action;
    write_u64be(message + 2, SCRCPY_POINTER_ID_MOUSE);
    write_u32be(message + 10, (uint32_t) x);
    write_u32be(message + 14, (uint32_t) y);
    write_u16be(message + 18, (uint16_t) video_width);
    write_u16be(message + 20, (uint16_t) video_height);
    write_u16be(message + 22, pressed ? UINT16_MAX : 0);
    write_u32be(message + 24, pressed ? 1U : 0U);
    write_u32be(message + 28, pressed ? 1U : 0U);
    pthread_mutex_lock(&control_mutex);
    int result = send_all(control_fd, message, sizeof(message));
    pthread_mutex_unlock(&control_mutex);
    return result;
}

static int reset_scrcpy_video(void) {
    const uint8_t message = SCRCPY_CONTROL_RESET_VIDEO;
    pthread_mutex_lock(&control_mutex);
    int result = send_all(control_fd, &message, sizeof(message));
    pthread_mutex_unlock(&control_mutex);
    return result;
}

static void pointer_event(int button_mask, int x, int y, rfbClientPtr client) {
    struct client_state *state = client->clientData;
    if (!state) {
        return;
    }
    pthread_mutex_lock(&pointer_mutex);
    int left_button = !!(button_mask & 1);
    uint8_t action;
    if (left_button && !state->previous_left_button) {
        if (pointer_owner && pointer_owner != client) {
            goto done;
        }
        pointer_owner = client;
        action = 0;
    } else if (!left_button && state->previous_left_button) {
        if (pointer_owner != client) {
            goto done;
        }
        action = 1;
    } else if (left_button) {
        if (pointer_owner != client) {
            goto done;
        }
        action = 2;
    } else {
        goto done;
    }

    if (send_touch(action, x, y, left_button) < 0) {
        fprintf(stderr, "failed to send touch event\n");
    }
    state->last_pointer_x = x;
    state->last_pointer_y = y;
    state->previous_left_button = left_button;
    if (!left_button) {
        pointer_owner = NULL;
    }

done:
    pthread_mutex_unlock(&pointer_mutex);
}

static int keysym_to_android_keycode(rfbKeySym key) {
    switch (key) {
        case XK_Home: return 3;
        case XK_Escape: return 4;
        case XK_Up: return 19;
        case XK_Down: return 20;
        case XK_Left: return 21;
        case XK_Right: return 22;
        case XK_Tab: return 61;
        case XK_space: return 62;
        case XK_Return: return 66;
        case XK_KP_Enter: return 66;
        case XK_BackSpace: return 67;
        case XK_Delete: return 112;
        default: return -1;
    }
}

static void keyboard_event(rfbBool down, rfbKeySym key, rfbClientPtr client) {
    (void) client;
    int keycode = keysym_to_android_keycode(key);
    if (keycode >= 0) {
        uint8_t message[14] = {0};
        message[0] = 0;
        message[1] = down ? 0 : 1;
        write_u32be(message + 2, (uint32_t) keycode);
        pthread_mutex_lock(&control_mutex);
        send_all(control_fd, message, sizeof(message));
        pthread_mutex_unlock(&control_mutex);
        return;
    }

    if (down && key >= 0x20 && key <= 0x7e) {
        uint8_t message[6];
        message[0] = 1;
        write_u32be(message + 1, 1);
        message[5] = (uint8_t) key;
        pthread_mutex_lock(&control_mutex);
        send_all(control_fd, message, sizeof(message));
        pthread_mutex_unlock(&control_mutex);
    }
}

static int is_h264_encoding(int encoding) {
    return encoding == rfbEncodingOpenH264 || encoding == rfbEncodingH264;
}

/* Ordinary encoders read screen->frameBuffer while their independent output
 * threads are sending. Hold a shared publication lock for the whole update so
 * a newly decoded frame cannot be copied over a client mid-rectangle. The
 * first ordinary update also waits for the reset-triggered keyframe instead
 * of exposing the calloc()ed black framebuffer. H.264 clients bypass this
 * lock because their callback reads the packet queue, not the framebuffer. */
static void display_hook(rfbClientPtr client) {
    struct client_state *state = client->clientData;
    if (!state || is_h264_encoding(client->preferredEncoding)) {
        return;
    }

    pthread_mutex_lock(&screen_buffer_mutex);
    while (running && !screen_frame_ready
            && client->sock != RFB_INVALID_SOCKET) {
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_nsec += 100 * 1000 * 1000;
        if (deadline.tv_nsec >= 1000 * 1000 * 1000) {
            ++deadline.tv_sec;
            deadline.tv_nsec -= 1000 * 1000 * 1000;
        }
        pthread_cond_timedwait(&screen_buffer_cond, &screen_buffer_mutex,
                               &deadline);
    }
    if (!running || client->sock == RFB_INVALID_SOCKET) {
        pthread_mutex_unlock(&screen_buffer_mutex);
        return;
    }
    state->framebuffer_lock_held = 1;
}

static void display_finished_hook(rfbClientPtr client, int result) {
    (void) result;
    struct client_state *state = client->clientData;
    if (state && state->framebuffer_lock_held) {
        state->framebuffer_lock_held = 0;
        pthread_mutex_unlock(&screen_buffer_mutex);
    }
}

static void count_client_modes(int *h264_clients, int *standard_clients) {
    *h264_clients = 0;
    *standard_clients = 0;

    rfbClientIteratorPtr iterator = rfbGetClientIterator(rfb_screen);
    rfbClientPtr client;
    while ((client = rfbClientIteratorNext(iterator)) != NULL) {
        if (client->state != RFB_NORMAL || client->preferredEncoding == -1) {
            continue;
        }
        if (is_h264_encoding(client->preferredEncoding)) {
            ++*h264_clients;
        } else {
            ++*standard_clients;
        }
    }
    rfbReleaseClientIterator(iterator);
}

static void handle_signal(int signal_number) {
    (void) signal_number;
    running = 0;
    pthread_cond_broadcast(&frame_cond);
}

int main(int argc, char **argv) {
    const char *scrcpy_host = "127.0.0.1";
    uint16_t scrcpy_port = 27183;

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGPIPE, SIG_IGN);

    video_fd = connect_tcp(scrcpy_host, scrcpy_port);
    if (video_fd < 0) {
        fprintf(stderr, "failed to connect scrcpy video socket\n");
        return 1;
    }
    control_fd = connect_tcp(scrcpy_host, scrcpy_port);
    if (control_fd < 0) {
        fprintf(stderr, "failed to connect scrcpy control socket\n");
        return 1;
    }

    uint8_t codec_header[4];
    uint8_t session_header[12];
    if (recv_all(video_fd, codec_header, sizeof(codec_header)) <= 0
            || read_u32be(codec_header) != SCRCPY_CODEC_H264) {
        fprintf(stderr, "scrcpy did not negotiate H.264\n");
        return 1;
    }
    if (recv_all(video_fd, session_header, sizeof(session_header)) <= 0
            || !(read_u64be(session_header) & SCRCPY_PACKET_FLAG_SESSION)) {
        fprintf(stderr, "missing initial scrcpy session metadata\n");
        return 1;
    }
    video_width = (int) read_u32be(session_header + 4);
    video_height = (int) read_u32be(session_header + 8);
    fprintf(stderr, "scrcpy H.264 stream: %dx%d\n", video_width, video_height);

    framebuffer_size = (size_t) video_width * (size_t) video_height * 4;
    fallback_buffer = calloc(1, framebuffer_size);
    if (!fallback_buffer || init_fallback_decoder() < 0) {
        free(fallback_buffer);
        return 1;
    }

    pthread_t video_thread;
    pthread_t control_thread;
    pthread_create(&video_thread, NULL, video_reader_main, NULL);
    pthread_create(&control_thread, NULL, control_drain_main, NULL);

    rfb_screen = rfbGetScreen(&argc, argv, video_width, video_height, 8, 3, 4);
    if (!rfb_screen) {
        return 1;
    }
    rfb_screen->desktopName = "ReDroid scrcpy H.264 passthrough";
    rfb_screen->frameBuffer = calloc(1, framebuffer_size);
    if (!rfb_screen->frameBuffer) {
        return 1;
    }
    rfb_screen->alwaysShared = TRUE;
    rfb_screen->neverShared = FALSE;
    rfb_screen->newClientHook = new_client;
    rfb_screen->ptrAddEvent = pointer_event;
    rfb_screen->kbdAddEvent = keyboard_event;
    rfb_screen->h264EncoderCallback = h264_encoder_callback;
    rfb_screen->displayHook = display_hook;
    rfb_screen->displayFinishedHook = display_finished_hook;
    rfb_screen->ipv6port = -1;
    rfbInitServer(rfb_screen);
    rfbRunEventLoop(rfb_screen, -1, TRUE);

    fprintf(stderr, "RFB listening on port %d (H.264 passthrough + Tight/JPEG fallback)\n",
            rfb_screen->port);
    uint64_t last_marked_generation = 0;
    int previous_h264_clients = -1;
    int previous_standard_clients = -1;
    while (running && rfbIsActive(rfb_screen)) {
        int h264_clients;
        int standard_clients;
        count_client_modes(&h264_clients, &standard_clients);
        if (h264_clients != previous_h264_clients
                || standard_clients != previous_standard_clients) {
            fprintf(stderr, "active VNC clients: H.264=%d ordinary=%d\n",
                    h264_clients, standard_clients);
            previous_h264_clients = h264_clients;
            previous_standard_clients = standard_clients;
        }

        int requested_fallback = standard_clients > 0;
        if (requested_fallback != fallback_mode) {
            fallback_mode = requested_fallback;
            fprintf(stderr, "ordinary VNC fallback %s\n",
                    requested_fallback ? "enabled" : "disabled");
            if (requested_fallback) {
                pthread_mutex_lock(&screen_buffer_mutex);
                screen_frame_ready = 0;
                pthread_mutex_unlock(&screen_buffer_mutex);
                fallback_decoder_reset = 1;
                if (reset_scrcpy_video() < 0) {
                    fprintf(stderr, "failed to request a fallback keyframe\n");
                }
            }
        }

        pthread_mutex_lock(&frame_mutex);
        uint64_t generation = frame_generation;
        pthread_mutex_unlock(&frame_mutex);

        int mark_modified = 0;
        if (h264_clients > 0 && generation != last_marked_generation) {
            mark_modified = 1;
        }
        last_marked_generation = generation;

        pthread_mutex_lock(&fallback_mutex);
        if (fallback_frame_ready) {
            pthread_mutex_lock(&screen_buffer_mutex);
            memcpy(rfb_screen->frameBuffer, fallback_buffer, framebuffer_size);
            screen_frame_ready = 1;
            pthread_cond_broadcast(&screen_buffer_cond);
            pthread_mutex_unlock(&screen_buffer_mutex);
            fallback_frame_ready = 0;
            mark_modified = 1;
        }
        pthread_mutex_unlock(&fallback_mutex);

        if (mark_modified && h264_clients + standard_clients > 0) {
            rfbMarkRectAsModified(rfb_screen, 0, 0, video_width, video_height);
        }

        struct timespec delay = {.tv_sec = 0, .tv_nsec = 2 * 1000 * 1000};
        nanosleep(&delay, NULL);
    }

    running = 0;
    rfbShutdownServer(rfb_screen, TRUE);
    shutdown(video_fd, SHUT_RDWR);
    shutdown(control_fd, SHUT_RDWR);
    pthread_cond_broadcast(&frame_cond);
    pthread_cond_broadcast(&screen_buffer_cond);
    pthread_join(video_thread, NULL);
    pthread_join(control_thread, NULL);

    close(video_fd);
    close(control_fd);
    free(codec_config);
    sws_freeContext(sws_context);
    av_frame_free(&decoder_frame);
    avcodec_free_context(&decoder_context);
    free(fallback_buffer);
    pthread_mutex_lock(&frame_mutex);
    clear_frame_queue_locked();
    pthread_mutex_unlock(&frame_mutex);
    free(rfb_screen->frameBuffer);
    rfb_screen->frameBuffer = NULL;
    rfbScreenCleanup(rfb_screen);
    return 0;
}
