/* SPDX-License-Identifier: MIT
 * Copyright 2026 CEMAXECUTER LLC.
 *
 * vrt-save — minimal lossless VITA 49 (VRT) receiver for validation. Binds a
 * UDP port, decodes Signal-Data packets, and writes interleaved int16 I,Q
 * (host order) to a file for offline spectrum analysis. Context packets print
 * the detected RF frequency, sample rate and data format.
 *
 *   vrt-save PORT OUTFILE SAMPLES
 */
#include <arpa/inet.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

int main(int argc, char **argv)
{
	int port = argc > 1 ? atoi(argv[1]) : 4991;
	const char *outp = argc > 2 ? argv[2] : "vrt.ci16";
	long want = argc > 3 ? atol(argv[3]) : 2000000;   /* complex samples */
	int s = socket(AF_INET, SOCK_DGRAM, 0);
	struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(port),
				  .sin_addr.s_addr = htonl(INADDR_ANY) };
	int rb = 16 << 20;
	setsockopt(s, SOL_SOCKET, SO_RCVBUF, &rb, sizeof(rb));
	if (bind(s, (void *)&sa, sizeof(sa))) { perror("bind"); return 1; }
	FILE *f = fopen(outp, "wb");
	if (!f) { perror(outp); return 1; }

	uint8_t d[65536];
	int16_t iq[32768];
	long got = 0; unsigned long pkts = 0, gaps = 0; int last = -1; int bits = 16;
	while (got < want) {
		ssize_t n = recvfrom(s, d, sizeof(d), 0, 0, 0);
		if (n < 8) continue;
		uint32_t w0 = ntohl(*(uint32_t *)d);
		unsigned t = (w0 >> 28) & 0xF, sz = (w0 & 0xFFFF) * 4, cnt = (w0 >> 16) & 0xF;
		if (t == 0x4) {
			uint32_t cif0 = ntohl(*(uint32_t *)(d + 8)); unsigned off = 12;
			double rf = 0, sr = 0; unsigned fmtw = 0;
			if (cif0 & (1u << 27)) { int64_t v = ((int64_t)ntohl(*(uint32_t*)(d+off))<<32)|ntohl(*(uint32_t*)(d+off+4)); rf = v/1048576.0; off += 8; }
			if (cif0 & (1u << 21)) { int64_t v = ((int64_t)ntohl(*(uint32_t*)(d+off))<<32)|ntohl(*(uint32_t*)(d+off+4)); sr = v/1048576.0; off += 8; }
			if (cif0 & (1u << 15)) { fmtw = ntohl(*(uint32_t*)(d+off)); bits = (fmtw & 0x1F) + 1; }
			fprintf(stderr, "context: rf=%.0f Hz rate=%.0f Hz bits=%d\n", rf, sr, bits);
			continue;
		}
		if (t != 0x0 && t != 0x1) continue;
		unsigned hdr = 1 + (t == 0x1 ? 1 : 0);
		if (sz > (unsigned)n) sz = n;
		unsigned pbytes = sz - hdr * 4;
		const uint8_t *p = d + hdr * 4;
		if (last >= 0 && ((last + 1) & 0xF) != (int)cnt) gaps++;
		last = cnt; pkts++;
		unsigned nv = pbytes / 2;                     /* int16 values */
		if (nv > 32768) nv = 32768;
		for (unsigned i = 0; i < nv; i++)
			iq[i] = (int16_t)((p[i*2] << 8) | p[i*2+1]);   /* BE -> host */
		fwrite(iq, 2, nv, f);
		got += nv / 2;
	}
	fclose(f);
	fprintf(stderr, "saved %ld complex samples, %lu pkts, %lu seq gaps, bits=%d\n",
		got, pkts, gaps, bits);
	return 0;
}
