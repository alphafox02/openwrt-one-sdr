/* SPDX-License-Identifier: MIT
 * Copyright 2026 CEMAXECUTER LLC.
 *
 * iqr_recv — host consumer for the OpenWrt One continuous IQ firehose.
 *
 * iqcontinuous --listen forwards the raw fwlog-relay stream: size-prefixed
 * records, [u32 len=8252][8252-byte record]. Each record carries, from its
 * start: eid @40 (0xed), ext @44 (0xf1), seq @48, src @52, then 8192 bytes of
 * 2-path 14-bit IQ at @56 (1024 time samples per RX path, 4 bytes each).
 *
 * This reads the stream at full rate, validates that seq is strictly
 * contiguous (the gap-free proof), measures throughput, and optionally writes
 * the raw 8192-byte payloads to a file.
 *
 *   iqr_recv HOST PORT [seconds] [--out FILE]
 */
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define UNIT 8256          /* 4-byte length + 8252-byte record */
#define RECLEN 8252

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec*1e-9; }
static uint32_t le32(const uint8_t *p){return (uint32_t)p[0]|p[1]<<8|p[2]<<16|(uint32_t)p[3]<<24;}

int main(int argc, char **argv) {
	if (argc < 3) { fprintf(stderr, "usage: %s HOST PORT [seconds] [--out FILE]\n", argv[0]); return 2; }
	const char *host = argv[1]; int port = atoi(argv[2]);
	double seconds = argc > 3 && argv[3][0] != '-' ? atof(argv[3]) : 8.0;
	const char *outpath = NULL;
	for (int i = 3; i < argc; i++) if (!strcmp(argv[i], "--out") && i+1 < argc) outpath = argv[++i];
	int outfd = outpath ? open(outpath, O_WRONLY|O_CREAT|O_TRUNC, 0644) : -1;
	if (outpath && outfd < 0) { perror(outpath); return 1; }

	int s = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(port) };
	inet_pton(AF_INET, host, &sa.sin_addr);
	if (connect(s, (void *)&sa, sizeof(sa))) { perror("connect"); return 1; }
	int one = 1, rb = 32<<20;
	setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	setsockopt(s, SOL_SOCKET, SO_RCVBUF, &rb, sizeof(rb));

	uint8_t *buf = malloc(4<<20); size_t fill = 0;
	uint64_t total = 0, recs = 0, gaps = 0, badmagic = 0;
	int64_t first = -1, last = -1;
	double t0 = now(), tlog = t0;
	uint64_t total_at_log = 0, recs_at_log = 0;

	while (now() - t0 < seconds) {
		ssize_t n = recv(s, buf + fill, (4<<20) - fill, 0);
		if (n <= 0) { fprintf(stderr, "closed by device after %llu bytes\n", (unsigned long long)total); break; }
		total += n; fill += n;
		size_t pos = 0;
		while (fill - pos >= UNIT) {
			const uint8_t *u = buf + pos;
			if (le32(u) != RECLEN) { badmagic++; pos++; continue; }  /* resync */
			const uint8_t *r = u + 4;
			int64_t seq = le32(r + 48);
			if (first < 0) first = seq;
			else if (seq != last + 1) gaps++;
			last = seq;
			if (outfd >= 0) { if (write(outfd, r + 56, 8192) != 8192) { perror("write"); } }
			recs++; pos += UNIT;
		}
		memmove(buf, buf + pos, fill - pos); fill -= pos;
		double t = now();
		if (t - tlog >= 1.0) {
			double dt = t - tlog;
			fprintf(stderr, "  +%.0fs  %.1f MB/s  %.2f MSps/path  seq=%lld  gaps=%llu\n",
				t - t0, (total - total_at_log)/dt/1e6, (recs - recs_at_log)*1024.0/dt/1e6,
				(long long)last, (unsigned long long)gaps);
			tlog = t; total_at_log = total; recs_at_log = recs;
		}
	}
	double dt = now() - t0;
	printf("records=%llu seq[%lld..%lld] span=%lld gaps=%llu badmagic=%llu\n",
		(unsigned long long)recs, (long long)first, (long long)last,
		first < 0 ? 0 : (long long)(last - first + 1), (unsigned long long)gaps, (unsigned long long)badmagic);
	printf("rate=%.0f Mbit/s (%.1f MB/s)  samples=%.2f MSps/path  %s\n",
		total*8/dt/1e6, total/dt/1e6, recs*1024.0/dt/1e6,
		gaps == 0 && first >= 0 ? "GAP-FREE" : "has gaps");
	if (outfd >= 0) close(outfd);
	close(s);
	return gaps != 0 || first < 0;
}
