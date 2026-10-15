/* SPDX-License-Identifier: MIT
 * Copyright 2026 CEMAXECUTER LLC.
 *
 * iq-vita49 — emit CI8 IQ as VITA 49 (VRT) UDP, a standard SDR interchange
 * format. Sends a periodic IF-Context packet (RF reference frequency, sample
 * rate, data-packet payload format) and Signal-Data packets carrying the IQ.
 * All fields big-endian per ANSI/VITA 49.0.
 *
 * Source: interleaved I,Q from a file (default) or stdin (`--in -`). Payload
 * format follows --format: ci8 (int8, default) or ci16 (int16 host order; the
 * full 14-bit capture resolution sign-extended into 16 bits). Destination:
 * UDP IP:PORT (default 127.0.0.1:4991).
 *
 *   iq-vita49 --freq 2437000000 --rate 40000000 --dst 127.0.0.1:4991 cap.ci8
 *   iq-vita49 --format ci16 --freq 2437000000 --rate 40000000 cap.ci16
 */
#include <arpa/inet.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define SAMPLES_PER_PKT 512          /* 1024-byte CI8 payload, under a 1500 MTU */
#define CTX_EVERY 64                 /* resend context every N signal packets */

/* VITA 49.0 packet types */
#define VRT_SIGNAL_DATA 0x1
#define VRT_IF_CONTEXT  0x4
/* CIF0 field bits */
#define CIF_RF_REF_FREQ (1u << 27)
#define CIF_SAMPLE_RATE (1u << 21)
#define CIF_DATA_FORMAT (1u << 15)
#define STREAM_ID 0x00000001u

static void be32(uint8_t *p, uint32_t v) { p[0]=v>>24; p[1]=v>>16; p[2]=v>>8; p[3]=v; }
static void be64(uint8_t *p, int64_t v) { be32(p,(uint32_t)((uint64_t)v>>32)); be32(p+4,(uint32_t)v); }

int main(int argc, char **argv) {
	uint64_t freq = 2437000000ULL, rate = 40000000ULL;
	const char *dst = "127.0.0.1:4991", *inpath = NULL;
	int bits = 8;                              /* 8 => Ci8, 16 => Ci16 */
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--freq") && i+1<argc) freq = strtoull(argv[++i],0,0);
		else if (!strcmp(argv[i], "--rate") && i+1<argc) rate = strtoull(argv[++i],0,0);
		else if (!strcmp(argv[i], "--dst") && i+1<argc) dst = argv[++i];
		else if (!strcmp(argv[i], "--in") && i+1<argc) inpath = argv[++i];
		else if (!strcmp(argv[i], "--format") && i+1<argc) bits = strcmp(argv[++i],"ci16")?8:16;
		else inpath = argv[i];
	}
	int bytes_per_samp = (bits==16) ? 4 : 2;   /* complex: 2*(bits/8) */
	char ip[64]; int port = 4991;
	{ const char *c = strrchr(dst, ':'); if (c) { int n=c-dst; memcpy(ip,dst,n); ip[n]=0; port=atoi(c+1);} else snprintf(ip,sizeof(ip),"%s",dst); }

	FILE *in = (!inpath || !strcmp(inpath,"-")) ? stdin : fopen(inpath, "rb");
	if (!in) { perror(inpath); return 1; }
	int s = socket(AF_INET, SOCK_DGRAM, 0);
	struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(port) };
	inet_pton(AF_INET, ip, &sa.sin_addr);

	/* IF-Context packet: word0, stream_id, CIF0, RF freq(2), sample rate(2), data format(2) = 9 words */
	uint8_t ctx[36]; uint8_t ccount = 0;
	/* Signal-Data packet: word0, stream_id, then payload (max SAMPLES_PER_PKT samples) */
	uint8_t sig[8 + SAMPLES_PER_PKT*4]; uint8_t scount = 0;
	uint8_t iq[SAMPLES_PER_PKT*4];

	/* keep each datagram's payload <= 1024 bytes (under a 1500 MTU) */
	size_t spp = (bits==16) ? SAMPLES_PER_PKT/2 : SAMPLES_PER_PKT;
	uint64_t pkts = 0;
	for (;;) {
		size_t want = spp * (size_t)bytes_per_samp;
		size_t n = fread(iq, 1, want, in);
		if (n == 0) break;
		size_t payload = n - (n % bytes_per_samp); /* whole complex samples */
		if (payload == 0) break;

		if (pkts % CTX_EVERY == 0) {               /* (re)send context */
			be32(ctx+0, (VRT_IF_CONTEXT<<28) | (ccount<<16) | 9);
			be32(ctx+4, STREAM_ID);
			be32(ctx+8, CIF_RF_REF_FREQ | CIF_SAMPLE_RATE | CIF_DATA_FORMAT);
			be64(ctx+12, (int64_t)(freq << 20));   /* RF ref freq, Hz, 20-bit fraction */
			be64(ctx+20, (int64_t)(rate << 20));   /* sample rate, Hz, 20-bit fraction */
			be32(ctx+28, (1u<<29) | (((uint32_t)bits-1u)<<6) | ((uint32_t)bits-1u));
			be32(ctx+32, 0);
			sendto(s, ctx, sizeof(ctx), 0, (void*)&sa, sizeof(sa));
			ccount = (ccount+1) & 0xF;
		}
		if (bits == 16) {                          /* host int16 -> big-endian VRT */
			uint16_t *src = (uint16_t*)iq, *dst16 = (uint16_t*)(sig+8);
			for (size_t i = 0; i < payload/2; i++) dst16[i] = htons(src[i]);
		} else {
			memcpy(sig+8, iq, payload);            /* int8 needs no swap */
		}
		uint32_t size_w = 2 + (uint32_t)(payload/4);
		be32(sig+0, (VRT_SIGNAL_DATA<<28) | (scount<<16) | size_w);
		be32(sig+4, STREAM_ID);
		sendto(s, sig, 8+payload, 0, (void*)&sa, sizeof(sa));
		scount = (scount+1) & 0xF;
		pkts++;
		/* pace at the sample rate when streaming from a pipe/live source */
		if (in != stdin) { struct timespec t={0, (long)((payload/bytes_per_samp)*1e9/rate)}; nanosleep(&t,0); }
	}
	fprintf(stderr, "iq-vita49: sent %llu signal packets to %s:%d (freq=%llu rate=%llu)\n",
		(unsigned long long)pkts, ip, port, (unsigned long long)freq, (unsigned long long)rate);
	return 0;
}
