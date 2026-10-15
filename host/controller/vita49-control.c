// SPDX-License-Identifier: MIT
// Copyright (c) 2026 CEMAXECUTER LLC.
//
// Experimental, timestamp-free VITA 49.2 command subset. Loopback compatibility
// tests do not establish standards conformance or live hardware interoperability.
// Writes are serialized, validated with "CMD --check FIELD VALUE", then applied
// with "CMD FIELD VALUE". State changes only after successful completion.
// Failed/unsupported commands receive no success acknowledgement (controller
// timeout); structured V49.2 error responses remain to be implemented.

#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <math.h>
#include <time.h>
#include <fcntl.h>

// Packet type (header bits 31:28) and header flags.
enum { PKT_COMMAND = 0x6, PKT_EXT_COMMAND = 0x7 };
enum { HDR_ACKNOWLEDGE = 1u << 26, HDR_CLASS_ID = 1u << 27 };

// Control & Acknowledgement Mode word (section 8.3.1).
enum {
    CAM_CONTROLLEE_ENABLE = 1u << 31,
    CAM_CONTROLLEE_UUID   = 1u << 30,
    CAM_CONTROLLER_ENABLE = 1u << 29,
    CAM_CONTROLLER_UUID   = 1u << 28,
    CAM_ACTION_SHIFT      = 23,          // 2 bits at 24:23
    CAM_ACTION_MASK       = 0x3u << 23,
    CAM_REQ_VALIDATION    = 1u << 20,
    CAM_REQ_EXECUTION     = 1u << 19,
    CAM_REQ_QUERY_STATE   = 1u << 18,
    CAM_REQ_WARNINGS      = 1u << 17,
    CAM_REQ_ERRORS        = 1u << 16,
    CAM_ACK_WARNINGS      = 1u << 17,    // reused in an acknowledgement
    CAM_ACK_ERRORS        = 1u << 16,
    CAM_SCHEDULED_EXECUTED = 1u << 10,
};
enum { ACTION_NONE = 0, ACTION_DRY_RUN = 1, ACTION_EXECUTE = 2 };

// CIF0 indicator bits and their field sizes in 32-bit words (section 7.1.5.1).
enum {
    CIF0_CHANGE_IND  = 1u << 31,
    CIF0_BANDWIDTH   = 1u << 29,
    CIF0_IF_REF_FREQ = 1u << 28,
    CIF0_RF_REF_FREQ = 1u << 27,
    CIF0_REF_LEVEL   = 1u << 24,
    CIF0_GAIN        = 1u << 23,
    CIF0_OVER_RANGE  = 1u << 22,
    CIF0_SAMPLE_RATE = 1u << 21,
    CIF0_DATA_FORMAT = 1u << 15,
    CIF0_CIF7_ENABLE = 1u << 7,
    CIF0_CIF3_ENABLE = 1u << 3,
    CIF0_CIF2_ENABLE = 1u << 2,
    CIF0_CIF1_ENABLE = 1u << 1,
};
enum { RADIX_FREQ = 20, RADIX_RATE = 20, RADIX_BW = 20, RADIX_GAIN = 7 };

static const struct { uint32_t bit; int words; } kCIF0[] = {
    { 1u << 30, 1 }, { CIF0_BANDWIDTH, 2 }, { CIF0_IF_REF_FREQ, 2 },
    { CIF0_RF_REF_FREQ, 2 }, { 1u << 26, 2 }, { 1u << 25, 2 },
    { CIF0_REF_LEVEL, 1 }, { CIF0_GAIN, 1 }, { CIF0_OVER_RANGE, 1 },
    { CIF0_SAMPLE_RATE, 2 }, { 1u << 20, 2 }, { 1u << 19, 2 },
    { 1u << 18, 1 }, { 1u << 17, 1 }, { 1u << 16, 1 }, { CIF0_DATA_FORMAT, 2 },
    { 1u << 14, 11 }, { 1u << 13, 11 }, { 1u << 12, 13 }, { 1u << 11, 9 },
    { 1u << 10, 1 }, { 1u << 9, -1 }, { 1u << 8, -1 },
};

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static uint64_t rd64(const uint8_t *p) {
    return (uint64_t)rd32(p) << 32 | rd32(p + 4);
}
static double fixed64(uint64_t raw, int radix) {
    return (double)(int64_t)raw / (double)(1LL << radix);
}

// A field buffer that grows as 32-bit words are appended, big-endian.
struct wbuf { uint8_t *p; size_t n, cap; };

static void wb_init(struct wbuf *b, uint8_t *storage, size_t cap) {
    b->p = storage; b->n = 0; b->cap = cap;
}
static void wb_u32(struct wbuf *b, uint32_t v) {
    if (b->n + 4 > b->cap) return;
    b->p[b->n++] = v >> 24; b->p[b->n++] = v >> 16;
    b->p[b->n++] = v >> 8;  b->p[b->n++] = v;
}
static void wb_u64(struct wbuf *b, uint64_t v) {
    wb_u32(b, (uint32_t)(v >> 32)); wb_u32(b, (uint32_t)v);
}
static void wb_fixed64(struct wbuf *b, double value, int radix) {
    wb_u64(b, (uint64_t)(int64_t)(value * (double)(1LL << radix)));
}

// The receiver's current settings, reported in query and acknowledgement CIF0.
struct state { double rf_freq, sample_rate, bandwidth, gain; };

// Fields found in a command's CIF0, with a flag for each.
struct request {
    int have_rf_freq, have_sample_rate, have_bandwidth, have_gain;
    double rf_freq, sample_rate, bandwidth, gain;
};

// Walk a CIF0 indicator and its field body, collecting the fields we act on and
// stepping correctly over any others. Returns the word index past the body, or
// (size_t)-1 on a malformed walk. Mirrors SoapyVITA49's parseCIF0.
static size_t parse_cif0(const uint8_t *vrt, size_t size_words, size_t w,
                         struct request *req) {
    if (w >= size_words) return (size_t)-1;
    uint32_t cif0 = rd32(vrt + w * 4);
    w += 1;
    if (cif0 & ~(CIF0_CHANGE_IND | CIF0_BANDWIDTH | CIF0_RF_REF_FREQ |
                 CIF0_GAIN | CIF0_SAMPLE_RATE)) return (size_t)-1;

    for (size_t i = 0; i < sizeof(kCIF0) / sizeof(kCIF0[0]); i++) {
        uint32_t bit = kCIF0[i].bit;
        if (!(cif0 & bit)) continue;
        int words = kCIF0[i].words;
        if (words < 0) {           // variable-length: not sent in commands
            return (size_t)-1;     // refuse rather than desynchronise
        }
        if (words == 0) continue;
        if (w + (size_t)words > size_words) return (size_t)-1;
        const uint8_t *f = vrt + w * 4;
        if (bit == CIF0_RF_REF_FREQ) {
            req->rf_freq = fixed64(rd64(f), RADIX_FREQ); req->have_rf_freq = 1;
        } else if (bit == CIF0_SAMPLE_RATE) {
            req->sample_rate = fixed64(rd64(f), RADIX_RATE); req->have_sample_rate = 1;
        } else if (bit == CIF0_BANDWIDTH) {
            req->bandwidth = fixed64(rd64(f), RADIX_BW); req->have_bandwidth = 1;
        } else if (bit == CIF0_GAIN) {
            uint32_t g = rd32(f);  // stage 1 in the low half (section 7.1.5.9)
            if (g >> 16) return (size_t)-1; // stage 2 is unsupported
            req->gain = (double)(int16_t)(g & 0xFFFF) / (double)(1 << RADIX_GAIN);
            req->have_gain = 1;
        }
        w += (size_t)words;
    }
    return w;
}

// Wait for completion; a bounded hook cannot block the control port forever.
// Failure after execution may leave hardware partially changed: state queries
// are withheld until fresh readback succeeds (or verified startup without readback).
static int run_hook(const char *cmd, const char *field, double value, int check, char *reply, size_t capacity) {
    if (!cmd) return -1;
    char val[64];
    snprintf(val, sizeof(val), "%.17g", value);
    int pipefd[2] = {-1,-1};
    if (reply && pipe(pipefd)) return -1;
    pid_t pid = fork();
    if (pid < 0) { if (reply) { close(pipefd[0]); close(pipefd[1]); } return -1; }
    if (pid == 0) {
        setpgid(0, 0);
        if (reply) {
            close(pipefd[0]);
            if (dup2(pipefd[1], STDOUT_FILENO) < 0) _exit(127);
            close(pipefd[1]);
            execlp(cmd, cmd, "--state", (char *)NULL);
            _exit(127);
        }
        if (check) execlp(cmd, cmd, "--check", field, val, (char *)NULL);
        else execlp(cmd, cmd, field, val, (char *)NULL);
        _exit(127);
    }
    setpgid(pid, pid);
    if (reply) { close(pipefd[1]); fcntl(pipefd[0], F_SETFL, O_NONBLOCK); }
    int result = -1;
    for (int i = 0; i < 450; i++) {
        int status;
        pid_t done = waitpid(pid, &status, WNOHANG);
        if (done == pid) { result = WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1; goto finished; }
        if (done < 0 && errno != EINTR) goto finished;
        struct timespec delay = {0, 100000000};
        nanosleep(&delay, NULL);
    }
    kill(-pid, SIGKILL);
    while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
finished:
    if (reply) {
        ssize_t n = read(pipefd[0], reply, capacity-1);
        if (n <= 0 || (size_t)n == capacity-1) result = -1;
        else reply[n] = 0;
        close(pipefd[0]);
    }
    return result;
}
static int apply_field(const char *cmd, const char *field, double value, int check) {
    return run_hook(cmd, field, value, check, NULL, 0);
}
static int read_state(const char *cmd, struct state *st) {
    char reply[256], extra;
    struct state next = *st;
    if (run_hook(cmd, NULL, 0, 0, reply, sizeof(reply)) ||
        sscanf(reply, "%lf %lf %lf %c", &next.rf_freq, &next.sample_rate, &next.bandwidth, &extra) != 3 ||
        !isfinite(next.rf_freq) || next.rf_freq <= 0 || next.rf_freq >= 8796093022208.0 ||
        !isfinite(next.sample_rate) || next.sample_rate <= 0 || next.sample_rate >= 8796093022208.0 ||
        !isfinite(next.bandwidth) || next.bandwidth <= 0 || next.bandwidth >= 8796093022208.0) return -1;
    *st = next;
    return 0;
}

int main(int argc, char **argv) {
    int port = 4991;                 // SoapyVITA49 defaults the control port to the data port
    const char *addr = "0.0.0.0";
    const char *apply = NULL;
    int readback = 0;
    struct state st = { 2437000000.0, 40000000.0, 40000000.0, 0.0 };

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--addr") && i + 1 < argc) addr = argv[++i];
        else if (!strcmp(argv[i], "--apply") && i + 1 < argc) apply = argv[++i];
        else if (!strcmp(argv[i], "--readback")) readback = 1;
        else if (!strcmp(argv[i], "--rf-freq") && i + 1 < argc) st.rf_freq = atof(argv[++i]);
        else if (!strcmp(argv[i], "--sample-rate") && i + 1 < argc) st.sample_rate = atof(argv[++i]);
        else {
            fprintf(stderr, "usage: %s [--addr IP] [--port N] [--apply CMD] [--readback]"
                    " [--rf-freq HZ] [--sample-rate HZ]\n", argv[0]);
            return 2;
        }
    }

    if (port < 1 || port > 65535 || !isfinite(st.rf_freq) ||
        st.rf_freq < 0 || st.rf_freq >= 8796093022208.0 ||
        !isfinite(st.sample_rate) || st.sample_rate <= 0 || st.sample_rate >= 8796093022208.0) {
        fprintf(stderr, "invalid startup settings\n"); return 2;
    }
    int state_valid = 1;
    // Startup supplies RF/rate only. Do not invent gain/bandwidth readbacks.
    uint32_t known_fields = CIF0_RF_REF_FREQ | CIF0_SAMPLE_RATE;
    if (readback) {
        if (read_state(apply, &st)) { fprintf(stderr, "radio readback failed\n"); return 1; }
        known_fields |= CIF0_BANDWIDTH;
    }
    signal(SIGPIPE, SIG_IGN);

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return 1; }
    struct sockaddr_in sa = { 0 };
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    if (inet_pton(AF_INET, addr, &sa.sin_addr) != 1) {
        fprintf(stderr, "bad address %s\n", addr); return 1;
    }
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa))) { perror("bind"); return 1; }
    fprintf(stderr, "vita49-control: listening on %s:%d%s%s\n", addr, port,
            apply ? ", apply=" : ", apply=none", apply ? apply : "");

    uint8_t pkt[65536];
    for (;;) {
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        ssize_t n = recvfrom(fd, pkt, sizeof(pkt), 0,
                             (struct sockaddr *)&from, &flen);
        if (n < 8) continue;

        uint32_t w0 = rd32(pkt);
        unsigned type = (w0 >> 28) & 0xF;
        if (type != PKT_COMMAND) continue;
        if (w0 & 0x0ff00000u) continue;          // an echoed ack is not for us
        size_t size_words = w0 & 0xFFFF;
        if (size_words < 4 || size_words * 4 != (size_t)n) continue;

        // Prologue: header, stream id, optional class id (no timestamp in a
        // command from SoapyVITA49, but honour the bits for robustness).
        size_t w = 1;
        uint32_t stream_id = rd32(pkt + w * 4); w += 1;   // command always has a stream id
        if (w0 & HDR_CLASS_ID) w += 2;
        if ((w0 >> 22) & 0x3) w += 1;                     // TSI
        if ((w0 >> 20) & 0x3) w += 2;                     // TSF
        if (w + 2 > size_words) continue;

        uint32_t cam = rd32(pkt + w * 4); w += 1;
        uint32_t message_id = rd32(pkt + w * 4); w += 1;
        size_t ids_start = w;
        if (cam & CAM_CONTROLLEE_ENABLE) w += (cam & CAM_CONTROLLEE_UUID) ? 4 : 1;
        if (cam & CAM_CONTROLLER_ENABLE) w += (cam & CAM_CONTROLLER_UUID) ? 4 : 1;
        if (w > size_words) continue;
        size_t ids_end = w;
        // Scheduled execution, partial actions and extra CAM modes are unsupported.
        uint32_t allowed_cam = CAM_CONTROLLEE_ENABLE | CAM_CONTROLLEE_UUID |
            CAM_CONTROLLER_ENABLE | CAM_CONTROLLER_UUID | CAM_ACTION_MASK |
            CAM_REQ_VALIDATION | CAM_REQ_EXECUTION | CAM_REQ_QUERY_STATE |
            CAM_REQ_WARNINGS | CAM_REQ_ERRORS | (1u<<26) | (1u<<25);
        if (cam & ~allowed_cam) continue;
        if ((cam & CAM_CONTROLLEE_UUID) && !(cam & CAM_CONTROLLEE_ENABLE)) continue;
        if ((cam & CAM_CONTROLLER_UUID) && !(cam & CAM_CONTROLLER_ENABLE)) continue;

        unsigned action = (cam & CAM_ACTION_MASK) >> CAM_ACTION_SHIFT;

        struct request req = { 0 };
        if (w < size_words) {
            if (parse_cif0(pkt, size_words, w, &req) != size_words) continue;
        }

        if (action > ACTION_EXECUTE) continue;
        if (readback) {
            state_valid = !read_state(apply, &st);
            if (state_valid) known_fields |= CIF0_BANDWIDTH;
        }
        if (!state_valid) continue;
        if (action != ACTION_NONE) {
            // Single-field transactions only: no partially successful batch.
            int fields = req.have_rf_freq + req.have_sample_rate + req.have_bandwidth + req.have_gain;
            if (fields != 1) continue;
            const char *field;
            double value, *actual;
            if (req.have_rf_freq) { field = "rf_freq_hz"; value = req.rf_freq; actual = &st.rf_freq; }
            else if (req.have_sample_rate) { field = "sample_rate_hz"; value = req.sample_rate; actual = &st.sample_rate; }
            else if (req.have_bandwidth) { field = "bandwidth_hz"; value = req.bandwidth; actual = &st.bandwidth; }
            else { field = "gain_db"; value = req.gain; actual = &st.gain; }
            if ((!req.have_gain && value <= 0) || apply_field(apply, field, value, 1)) {
                fprintf(stderr, "rejected message %u: unsupported or invalid %s\n", message_id, field);
                continue;
            }
            if (action == ACTION_EXECUTE) {
                if (apply_field(apply, field, value, 0)) {
                    state_valid = 0;
                    fprintf(stderr, "message %u failed: hardware state unknown; verify and restart responder\n", message_id);
                    continue;
                }
                *actual = value;
                if (readback && (read_state(apply, &st) || *actual != value)) {
                    state_valid = 0;
                    fprintf(stderr, "message %u failed radio readback\n", message_id);
                    continue;
                }
                known_fields |= req.have_rf_freq ? CIF0_RF_REF_FREQ :
                    req.have_sample_rate ? CIF0_SAMPLE_RATE :
                    req.have_bandwidth ? CIF0_BANDWIDTH : CIF0_GAIN;
            }
        }
        if (!(cam & (CAM_REQ_QUERY_STATE | CAM_REQ_EXECUTION | CAM_REQ_VALIDATION))) continue;

        // Build the acknowledgement: echo message id and ids, report state.
        uint8_t out[512];
        struct wbuf b;
        wb_init(&b, out, sizeof(out));
        wb_u32(&b, 0);                                   // header, size filled below
        wb_u32(&b, stream_id);
        uint32_t ack_cam = cam & (CAM_CONTROLLEE_ENABLE | CAM_CONTROLLEE_UUID |
                                  CAM_CONTROLLER_ENABLE | CAM_CONTROLLER_UUID);
        if (action == ACTION_EXECUTE) ack_cam |= CAM_SCHEDULED_EXECUTED | CAM_REQ_EXECUTION;
        else if (action == ACTION_DRY_RUN) ack_cam |= CAM_REQ_VALIDATION;
        else ack_cam |= CAM_REQ_QUERY_STATE;
        wb_u32(&b, ack_cam);
        wb_u32(&b, message_id);
        for (size_t idw = ids_start; idw < ids_end; idw++) wb_u32(&b, rd32(pkt + idw*4));

        // Report only supplied startup values and successfully applied fields.
        wb_u32(&b, known_fields);
        if (known_fields & CIF0_BANDWIDTH) wb_fixed64(&b, st.bandwidth, RADIX_BW);
        if (known_fields & CIF0_RF_REF_FREQ) wb_fixed64(&b, st.rf_freq, RADIX_FREQ);
        if (known_fields & CIF0_GAIN)
            wb_u32(&b, (uint16_t)(int16_t)(st.gain * (double)(1 << RADIX_GAIN)));
        if (known_fields & CIF0_SAMPLE_RATE) wb_fixed64(&b, st.sample_rate, RADIX_RATE);

        uint32_t ack_hdr = ((uint32_t)PKT_COMMAND << 28) | HDR_ACKNOWLEDGE |
                           (uint32_t)(b.n / 4);
        out[0] = ack_hdr >> 24; out[1] = ack_hdr >> 16;
        out[2] = ack_hdr >> 8;  out[3] = ack_hdr;

        sendto(fd, out, b.n, 0, (struct sockaddr *)&from, flen);
    }
}
