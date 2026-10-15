/* SPDX-License-Identifier: MIT
 * Copyright 2026 CEMAXECUTER LLC.
 *
 * vita49-shm — receive a VITA 49 (VRT) IQ stream over UDP and publish it into
 * a w1700k-stream shared-memory ring (host/stream/include/w1700k_stream.h), so
 * the w1700k-spectrum viewer renders it unmodified. Ci16 payload (14-bit left-
 * justified) is shifted back to the ring's native 14-bit scale; Ci8 is scaled up.
 *
 *   vita49-shm [--port 4991] [--stream /w1700k-stream] [--ring 2048]
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "w1700k_stream.h"
#include "vrt-parse.h"

#define LOAD(p)   __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define STORE(p,v) __atomic_store_n((p), (v), __ATOMIC_RELEASE)

static uint64_t now_ns(void) { return w1700k_stream_now_ns(); }

int main(int argc, char **argv)
{
	int port = 4991;
	const char *name = W1700K_STREAM_NAME;
	uint64_t ring = 2048;                 /* power of two */
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--port") && i+1 < argc) port = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--stream") && i+1 < argc) name = argv[++i];
		else if (!strcmp(argv[i], "--ring") && i+1 < argc) ring = strtoull(argv[++i], 0, 0);
	}
	if (!ring || (ring & (ring - 1)) || ring > (1u<<20) || port < 1 || port > 65535) { fprintf(stderr, "--ring must be a power of two\n"); return 1; }

	const uint64_t tags_off = W1700K_STREAM_HEADER_BYTES;
	const uint64_t samples_off = tags_off + ring * 8;
	const uint64_t total = samples_off + ring * W1700K_STREAM_SLICE * 2 * sizeof(int16_t);

	shm_unlink(name);
	int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0644);
	if (fd < 0) { perror("shm_open"); return 1; }
	if (ftruncate(fd, total)) { perror("ftruncate"); return 1; }
	struct w1700k_stream_header *h = mmap(0, total, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
	close(fd);
	if (h == MAP_FAILED) { perror("mmap"); return 1; }
	memset(h, 0, total);
	h->magic = W1700K_STREAM_MAGIC; h->version = W1700K_STREAM_VERSION;
	h->header_bytes = W1700K_STREAM_HEADER_BYTES; h->slice_samples = W1700K_STREAM_SLICE;
	h->ring_slices = ring; h->tags_offset = tags_off; h->samples_offset = samples_off;
	h->total_bytes = total; h->service_pid = getpid();
	STORE(&h->heartbeat_ns, now_ns());
	STORE(&h->run, (uint64_t)1);
	STORE(&h->run_started_ns, now_ns());

	int s = socket(AF_INET, SOCK_DGRAM, 0);
	struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(port),
				  .sin_addr.s_addr = htonl(INADDR_ANY) };
	int rb = 16 << 20; setsockopt(s, SOL_SOCKET, SO_RCVBUF, &rb, sizeof(rb));
	struct timeval tv = { .tv_sec = 0, .tv_usec = 200000 };   /* wake to refresh heartbeat */
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	if (bind(s, (void *)&sa, sizeof(sa))) { perror("bind"); return 1; }
	fprintf(stderr, "vita49-shm: UDP :%d -> shm %s (%llu slices, %.1f MiB)\n",
		port, name, (unsigned long long)ring, total / 1048576.0);

	/* batched receive: ~78k datagrams/s at 40 MSps vrt8 would swamp per-packet
	 * recvfrom; recvmmsg grabs a burst per syscall. */
	enum { VLEN = 128 };
	static uint8_t bufs[VLEN][2048];
	struct mmsghdr msgs[VLEN];
	struct iovec iov[VLEN];
	for (int i = 0; i < VLEN; i++) {
		iov[i].iov_base = bufs[i]; iov[i].iov_len = sizeof(bufs[i]);
		memset(&msgs[i].msg_hdr, 0, sizeof(msgs[i].msg_hdr));
		msgs[i].msg_hdr.msg_iov = &iov[i]; msgs[i].msg_hdr.msg_iovlen = 1;
	}
	int16_t *slicebuf = malloc(W1700K_STREAM_SLICE * 2 * sizeof(int16_t));
	uint64_t index = 0;      /* slice index being filled */
	unsigned fill = 0;       /* complex samples in slicebuf */
	struct vrt_state parser = {0};
	int pending_boundary = 0;
	uint64_t restarts = 0, last_data = 0;
	uint64_t last_hb = 0, datagrams = 0;

	for (;;) {
		int got = recvmmsg(s, msgs, VLEN, MSG_WAITFORONE, 0);
		uint64_t t_ns = now_ns();
		if (t_ns - last_hb > 100000000ull) {          /* heartbeat ~10 Hz, data or not */
			STORE(&h->heartbeat_ns, t_ns);
			STORE(&h->datagrams, datagrams);
			last_hb = t_ns;
		}
		if (got <= 0) continue;
		for (int mi = 0; mi < got; mi++) {
			const uint8_t *d = bufs[mi];
			unsigned n = msgs[mi].msg_len;
            struct vrt_packet pkt;
            int kind = (msgs[mi].msg_hdr.msg_flags & MSG_TRUNC) ? 0 : vrt_parse(d, n, &parser, &pkt);
            if (!kind) { STORE(&h->bad, LOAD(&h->bad)+1); pending_boundary = 1; continue; }
            if (kind == 1) {
                STORE(&h->radio_sample_rate, parser.rate_fixed >> 20);
                pending_boundary |= pkt.boundary;
                continue;
            }
            if (pending_boundary || pkt.boundary || restarts != LOAD(&h->restart_requests) ||
                (last_data && t_ns-last_data > 1000000000)) {
                STORE(&h->position_seq, LOAD(&h->position_seq)|1);
                STORE(&h->run, LOAD(&h->run)+1);
                STORE(&h->newest, 0); STORE(&h->received, 0);
                STORE(&h->run_started_ns, t_ns);
                STORE(&h->position_seq, LOAD(&h->position_seq)+1);
                index = 0; fill = 0; pending_boundary = 0;
                restarts = LOAD(&h->restart_requests);
            }
            last_data = t_ns;
            const uint8_t *p = pkt.samples;
            unsigned bits = parser.bits;
            unsigned packed = parser.packed;   /* 14-bit link-efficient */
			datagrams++;
			STORE(&h->last_any_ns, t_ns);
			unsigned nsamp = pkt.count;  /* complex samples */
			for (unsigned k = 0; k < nsamp; k++) {
				int16_t I, Q;
				if (packed) {
					I = (int16_t)vrt_get14(p, (size_t)k*28);
					Q = (int16_t)vrt_get14(p, (size_t)k*28 + 14);
				} else if (bits == 16) {
					I = (int16_t)((p[k*4]   << 8) | p[k*4+1]) >> 2;   /* 14-bit scale */
					Q = (int16_t)((p[k*4+2] << 8) | p[k*4+3]) >> 2;
				} else {
					I = (int8_t)p[k*2] * 64;             /* CI8 -> 14-bit */
					Q = (int8_t)p[k*2+1] * 64;
				}
				slicebuf[fill*2] = I; slicebuf[fill*2+1] = Q;
				if (++fill == W1700K_STREAM_SLICE) {
					STORE(&w1700k_stream_tags(h)[index & (ring - 1)], 0);
					int16_t *dst = w1700k_stream_slice(h, index);
					memcpy(dst, slicebuf, W1700K_STREAM_SLICE * 2 * sizeof(int16_t));
					STORE(&w1700k_stream_tags(h)[index & (ring - 1)],
					      w1700k_stream_tag(LOAD(&h->run), index));
					STORE(&h->position_seq, LOAD(&h->position_seq) | 1);   /* odd */
					STORE(&h->newest, index + 1);
					STORE(&h->received, index + 1);
					STORE(&h->position_seq, LOAD(&h->position_seq) + 1);   /* even */
					STORE(&h->slices, LOAD(&h->slices) + 1);
					index++; fill = 0;
				}
			}
		}
	}
}
