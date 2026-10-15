// SPDX-License-Identifier: MIT
// Copyright (c) 2026 CEMAXECUTER LLC.
//
// VITA 49 -> shared-memory bridge with the decimation roll-off correction
// ("flatten"), applied when the viewer sets the shared-memory flatten word.
// The correction (flatten.{h,cpp}, vendored from W1700K-SDR) is a fixed FIR,
// so it does not suppress real signals.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "include/w1700k_stream.h"
#include "flatten.h"
#include "vrt-parse.h"

namespace {

constexpr unsigned kSlice = W1700K_STREAM_SLICE;
constexpr uint64_t kHeartbeatNs = 100'000'000;
// DC blocker smoothing: a per-component running mean subtracted from each
// sample removes the direct-conversion LO offset at 0 Hz. The notch is
// alpha*fs/2pi wide (~64 Hz at 40 Msps), sub-bin for any usable FFT.
constexpr double kDcAlpha = 1e-5;

uint64_t load(const uint64_t *p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
void store(uint64_t *p, uint64_t v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }

int16_t clamp16(float v) {
    long r = lroundf(v);
    return (int16_t)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
}

// Float FIFO: append at the back, consume from the front by index, compact
// amortized. Avoids O(n^2) front-erase at the sample rate.
class FloatFifo {
public:
    void push(float v) { buf_.push_back(v); }
    void push(const float *p, size_t n) { if (n) buf_.insert(buf_.end(), p, p + n); }
    size_t size() const { return buf_.size() - head_; }
    const float *data() const { return buf_.empty() ? nullptr : buf_.data() + head_; }
    void clear() { buf_.clear(); head_ = 0; }
    void drop(size_t n) {
        head_ += n;
        if (head_ >= (1u << 16) && head_ * 2 >= buf_.size()) {
            buf_.erase(buf_.begin(), buf_.begin() + head_);
            head_ = 0;
        }
    }

private:
    std::vector<float> buf_;
    size_t head_ = 0;
};

// The shared-memory stream ring (the service side of w1700k_stream.h).
class StreamRing {
public:
    StreamRing(const char *name, uint64_t slices) : ring_(slices) {
        const uint64_t tags = W1700K_STREAM_HEADER_BYTES;
        const uint64_t samp = tags + slices * 8;
        bytes_ = samp + slices * kSlice * 2 * sizeof(int16_t);
        shm_unlink(name);
        int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0644);
        if (fd < 0 || ftruncate(fd, bytes_)) { perror("shm"); exit(1); }
        h_ = (w1700k_stream_header *)mmap(nullptr, bytes_, PROT_READ | PROT_WRITE,
                                          MAP_SHARED, fd, 0);
        close(fd);
        if (h_ == MAP_FAILED) { perror("mmap"); exit(1); }
        memset(h_, 0, bytes_);
        h_->magic = W1700K_STREAM_MAGIC;
        h_->version = W1700K_STREAM_VERSION;
        h_->header_bytes = W1700K_STREAM_HEADER_BYTES;
        h_->slice_samples = kSlice;
        h_->ring_slices = slices;
        h_->tags_offset = tags;
        h_->samples_offset = samp;
        h_->total_bytes = bytes_;
        h_->service_pid = getpid();
        store(&h_->run, 1);
        store(&h_->run_started_ns, now());
        store(&h_->heartbeat_ns, now());
    }
    ~StreamRing() { if (h_ != MAP_FAILED) munmap(h_, bytes_); }
    StreamRing(const StreamRing &) = delete;
    StreamRing &operator=(const StreamRing &) = delete;

    static uint64_t now() { return w1700k_stream_now_ns(); }
    w1700k_stream_header *header() const { return h_; }

    void boundary() {
        store(&h_->position_seq, load(&h_->position_seq) | 1);
        store(&h_->run, load(&h_->run) + 1);
        store(&h_->newest, 0);
        store(&h_->received, 0);
        store(&h_->run_started_ns, now());
        index_ = 0;
        store(&h_->position_seq, load(&h_->position_seq) + 1);
    }

    void publish(const float *iq, bool flat) {
        store(&w1700k_stream_tags(h_)[index_ & (ring_ - 1)], 0);
        int16_t *dst = w1700k_stream_slice(h_, index_);
        for (unsigned k = 0; k < kSlice * 2; k++) dst[k] = clamp16(iq[k]);
        uint64_t tag = w1700k_stream_tag(load(&h_->run), index_);
        if (flat) tag |= W1700K_STREAM_TAG_FLAT;
        store(&w1700k_stream_tags(h_)[index_ & (ring_ - 1)], tag);
        store(&h_->position_seq, load(&h_->position_seq) | 1);
        store(&h_->newest, index_ + 1);
        store(&h_->received, index_ + 1);
        store(&h_->position_seq, load(&h_->position_seq) + 1);
        store(&h_->slices, load(&h_->slices) + 1);
        index_++;
    }

private:
    w1700k_stream_header *h_ = (w1700k_stream_header *)MAP_FAILED;
    uint64_t ring_, bytes_, index_ = 0;
};

// Batched UDP receiver.
class UdpIn {
public:
    explicit UdpIn(int port) {
        fd_ = socket(AF_INET, SOCK_DGRAM, 0);
        sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_port = htons(port);
        sa.sin_addr.s_addr = htonl(INADDR_ANY);
        int rb = 16 << 20;
        setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &rb, sizeof(rb));
        timeval tv{0, 200000};
        setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        if (bind(fd_, (sockaddr *)&sa, sizeof(sa))) { perror("bind"); exit(1); }
        for (int i = 0; i < kVlen; i++) {
            iov_[i].iov_base = bufs_[i];
            iov_[i].iov_len = sizeof(bufs_[i]);
            memset(&msgs_[i].msg_hdr, 0, sizeof(msgs_[i].msg_hdr));
            msgs_[i].msg_hdr.msg_iov = &iov_[i];
            msgs_[i].msg_hdr.msg_iovlen = 1;
        }
    }
    ~UdpIn() { if (fd_ >= 0) close(fd_); }
    UdpIn(const UdpIn &) = delete;
    UdpIn &operator=(const UdpIn &) = delete;

    int recv() { return recvmmsg(fd_, msgs_, kVlen, MSG_WAITFORONE, nullptr); }
    const uint8_t *datagram(int i) const { return bufs_[i]; }
    unsigned length(int i) const { return msgs_[i].msg_len; }
    bool truncated(int i) const { return msgs_[i].msg_hdr.msg_flags & MSG_TRUNC; }

    static constexpr int kVlen = 128;

private:
    int fd_ = -1;
    uint8_t bufs_[kVlen][2048];
    mmsghdr msgs_[kVlen];
    iovec iov_[kVlen];
};

}  // namespace

int main(int argc, char **argv) {
    int port = 4991;
    const char *name = W1700K_STREAM_NAME;
    uint64_t ring = 16384;
    bool dc_block = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--stream") && i + 1 < argc) name = argv[++i];
        else if (!strcmp(argv[i], "--ring") && i + 1 < argc) ring = strtoull(argv[++i], nullptr, 0);
        else if (!strcmp(argv[i], "--dc-block")) dc_block = true;
    }

    if (!ring || (ring & (ring-1)) || ring > (1u<<20) || port < 1 || port > 65535) {
        fprintf(stderr, "invalid port or ring (power of two, 1..1048576)\n"); return 2;
    }
    StreamRing stream(name, ring);
    w1700k_stream_header *h = stream.header();
    UdpIn in(port);
    const Flatten *flat = nullptr;
    FlattenRun run;
    fprintf(stderr, "vita49-shm-flat: UDP :%d -> shm %s (%llu slices)\n",
            port, name, (unsigned long long)ring);

    FloatFifo raw, out;
    std::vector<float> block(Flatten::kBlock * 2);
    vrt_state parser{};
    bool pending_boundary = false;
    uint64_t restarts = 0, last_data = 0;
    uint64_t last_hb = 0, datagrams = 0;
    bool was_flat = false;
    double dc_i = 0, dc_q = 0;  // running means for the optional DC blocker

    for (;;) {
        int got = in.recv();
        uint64_t t = StreamRing::now();
        if (t - last_hb > kHeartbeatNs) {
            store(&h->heartbeat_ns, t);
            store(&h->datagrams, datagrams);
            last_hb = t;
        }
        bool want_flat = flat && load(&h->flatten);
        store(&h->flatten_active, want_flat ? 1 : 0);
        store(&h->flatten_rate, want_flat ? (uint64_t)flat->rate() : 0);
        if (load(&h->restart_requests) != restarts) {
            restarts = load(&h->restart_requests);
            pending_boundary = true;
        }

        for (int mi = 0; mi < got; mi++) {
            const uint8_t *d = in.datagram(mi);
            unsigned n = in.length(mi);
            vrt_packet pkt{};
            int kind = in.truncated(mi) ? 0 : vrt_parse(d, n, &parser, &pkt);
            if (!kind) {
                store(&h->bad, load(&h->bad) + 1);
                pending_boundary = true;
                continue;
            }
            if (kind == 1) {
                flat = Flatten::for_rate((double)parser.rate_fixed / 1048576.0);
                store(&h->radio_sample_rate, parser.rate_fixed >> 20);
                if (pkt.boundary) {
                    raw.clear(); out.clear(); dc_i = dc_q = 0;
                    stream.boundary();
                }
                want_flat = flat && load(&h->flatten);
                continue;
            }
            // A pause is a conservative boundary even if the 4-bit count matches.
            if (pending_boundary || pkt.boundary || (last_data && t-last_data > 1000000000)) {
                raw.clear(); out.clear(); dc_i = dc_q = 0;
                stream.boundary();
                pending_boundary = false;
            }
            last_data = t;
            const uint8_t *p = pkt.samples;
            size_t nsamp = pkt.count;
            unsigned bits = parser.bits;
            bool packed = parser.packed;  // 14-bit link-efficient, 28 bits/complex
            datagrams++;
            store(&h->last_any_ns, t);
            for (unsigned k = 0; k < nsamp; k++) {
                float fi, fq;
                if (packed) {
                    fi = (float)vrt_get14(p, (size_t)k*28);
                    fq = (float)vrt_get14(p, (size_t)k*28 + 14);
                } else if (bits == 16) {
                    fi = (float)((int16_t)((p[k*4] << 8) | p[k*4+1]) >> 2);
                    fq = (float)((int16_t)((p[k*4+2] << 8) | p[k*4+3]) >> 2);
                } else {
                    fi = (float)((int8_t)p[k*2] * 64);
                    fq = (float)((int8_t)p[k*2+1] * 64);
                }
                if (dc_block) {
                    dc_i += kDcAlpha * (fi - dc_i);
                    dc_q += kDcAlpha * (fq - dc_q);
                    fi -= (float)dc_i;
                    fq -= (float)dc_q;
                }
                raw.push(fi);
                raw.push(fq);
            }
        }

        // A mode change alters the pipeline delay; drop buffered samples so the
        // two paths never mix into one slice.
        if (want_flat != was_flat) {
            raw.clear(); out.clear(); dc_i = dc_q = 0;
            stream.boundary(); was_flat = want_flat;
        }
        store(&h->flatten_active, want_flat ? 1 : 0);
        store(&h->flatten_rate, want_flat ? (uint64_t)flat->rate() : 0);

        if (want_flat) {
            const size_t need = (size_t)Flatten::kFft * 2;    // kBlock + kTaps-1 complex
            const size_t step = (size_t)Flatten::kBlock * 2;
            while (raw.size() >= need) {
                run.apply(*flat, raw.data(), Flatten::kBlock, block.data());
                out.push(block.data(), step);
                raw.drop(step);
            }
        } else {
            out.push(raw.data(), raw.size());
            raw.clear();
        }

        while (out.size() >= kSlice * 2) {
            stream.publish(out.data(), want_flat);
            out.drop(kSlice * 2);
        }
    }
}
