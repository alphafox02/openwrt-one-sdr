/* SPDX-License-Identifier: MIT
 * Copyright 2026 CEMAXECUTER LLC.
 *
 * iqd — OpenWrt One SDR capture daemon, runs ON the router.
 *
 * Each frame: native EXT56 capture -> poll CR0 -> retire -> fire the DMA
 * producer (EXT56 func 3, firmware/streaming/window_dma.S) -> read the
 * eight 0xf1 events from the fwlog relay -> concatenate their 8192-byte
 * payloads into one 65536-byte native window. Served as 'IQ01' + 65536 bytes
 * over TCP, one frame after another, to a single connected client.
 *
 * Driving the debugfs with syscalls instead of a shell replaces ~56 ms/frame
 * of fork/exec with microseconds; the DMA export replaces the ~209 ms
 * register-bus window read with ~0.08 ms.
 *
 * This is still a gapped snapshot stream (each frame is ~8192 samples/path),
 * not gap-free acquisition. The decode (bank/path layout) is on the host.
 */
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define WINDOW 65536
#define CHUNK 8192
#define NCHUNK 8
#define RECSZ 8252              /* relay record: 4B size already consumed + body */
#define CR0 0x80023090

static char cmd_dir[384], relay_path[384], fw_debug_bin[400];
static char icap_path[400], regidx_path[400], regval_path[400];
static int relay_fd = -1;
static uint32_t node = 2113536, len = 4096, stopcycle = 4096;

static void die(const char *s) { perror(s); exit(1); }

/* debugfs attr files want a fresh open (position 0) per write, like `echo >`. */
static void put(const char *path, const char *s) {
	int fd = open(path, O_WRONLY);
	if (fd < 0) die(path);
	if (write(fd, s, strlen(s)) < 0) die("debugfs write");
	close(fd);
}
static uint32_t get(const char *path) {
	char b[64];
	int fd = open(path, O_RDONLY);
	if (fd < 0) die(path);
	ssize_t n = read(fd, b, sizeof(b) - 1);
	close(fd);
	if (n <= 0) die("debugfs read");
	b[n] = 0;
	return (uint32_t)strtoul(b, NULL, 0);
}

static void kb(uint64_t v) {                 /* one icap_prime u64 command */
	char b[32];
	snprintf(b, sizeof(b), "0x%llx\n", (unsigned long long)v);
	put(icap_path, b);
}
static void sb(unsigned off, unsigned val) {
	kb(0x0100000000000000ULL | ((uint64_t)(off & 0xffff) << 16) | (val & 0xff));
}
static void sw(unsigned off, uint32_t val) {
	sb(off, val); sb(off + 1, val >> 8); sb(off + 2, val >> 16); sb(off + 3, val >> 24);
}

static void reg_wr(uint32_t addr, uint32_t val) {
	char b[32];
	snprintf(b, sizeof(b), "0x%x\n", addr); put(regidx_path, b);
	snprintf(b, sizeof(b), "0x%x\n", val);  put(regval_path, b);
}
static uint32_t reg_rd(uint32_t addr) {
	char b[32];
	snprintf(b, sizeof(b), "0x%x\n", addr); put(regidx_path, b);
	return get(regval_path);
}

static void body_common(void) {              /* enable, node, length, stopcycle */
	sw(4, 1); sw(16, node); sw(20, len); sw(24, stopcycle);
}
static void native_capture(void) {
	kb(0); kb(0x0200000000000058ULL); body_common(); kb(0x1400000000000000ULL);
}
static void native_retire(void) {
	kb(0); kb(0x0200000000000058ULL); kb(0x1400000000000000ULL);
	reg_wr(CR0, 0x400);
}
static void native_stream(void) {            /* func 3 = DMA producer (retired mem) */
	kb(0); kb(0x0200000000000058ULL); sw(0, 3); body_common(); kb(0x1400000000000000ULL);
}
static void native_stream4(void) {           /* func 4 = ring + cache writeback + DMA (live) */
	kb(0); kb(0x0200000000000058ULL); sw(0, 4); body_common(); kb(0x1400000000000000ULL);
}
static int use_func4 = 0;

/* Read `want` bytes from the (non-blocking) relay, spinning up to ~0.2 s. */
static int relay_read(uint8_t *buf, int want) {
	int got = 0, idle = 0;
	while (got < want) {
		ssize_t n = read(relay_fd, buf + got, want - got);
		if (n > 0) { got += n; idle = 0; continue; }
		if (n < 0 && errno != EAGAIN) return -1;
		if (++idle > 2000) return got;        /* ~0.2 s with no data */
		usleep(100);
	}
	return got;
}

static void relay_drain(void) {
	uint8_t tmp[4096];
	while (read(relay_fd, tmp, sizeof(tmp)) > 0) ;
}

/* One frame into out[WINDOW]; returns 0 on success, <0 on desync. */
static int frame(uint8_t *out) {
	if (use_func4) {
		native_stream4();                  /* ring + writeback + DMA, no retire */
	} else {
		native_capture();
		for (int t = 0; t < 2000 && reg_rd(CR0) != 0x401; t++) ;
		native_retire();
		native_stream();                   /* produces exactly 8 relay records */
	}

	uint8_t rec[RECSZ];
	for (int i = 0; i < NCHUNK; i++) {
		uint8_t pfx[4];
		if (relay_read(pfx, 4) != 4) return -1;
		uint32_t sz = pfx[0] | pfx[1] << 8 | pfx[2] << 16 | pfx[3] << 24;
		if (sz != RECSZ) { relay_drain(); return -2; }
		if (relay_read(rec, RECSZ) != RECSZ) return -1;
		uint32_t seq = rec[12 + 36] | rec[12 + 37] << 8 | rec[12 + 38] << 16 | rec[12 + 39] << 24;
		if (rec[12 + 28] != 0xed || rec[12 + 32] != 0xf1 || seq != (uint32_t)i) {
			relay_drain();
			return -3;
		}
		memcpy(out + i * CHUNK, rec + 56, CHUNK);
	}
	return 0;
}

int main(int argc, char **argv) {
	int port = 7700;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--node") && i + 1 < argc) node = strtoul(argv[++i], 0, 0);
		else if (!strcmp(argv[i], "--len") && i + 1 < argc) len = stopcycle = strtoul(argv[++i], 0, 0);
		else if (!strcmp(argv[i], "--func4")) use_func4 = 1;
	}
	signal(SIGPIPE, SIG_IGN);

	/* locate the command knob (icap_prime) and the relay (may be a different phy) */
	DIR *d = opendir("/sys/kernel/debug/ieee80211");
	struct dirent *e;
	char base[400];
	while (d && (e = readdir(d))) {
		if (e->d_name[0] == '.') continue;
		snprintf(base, sizeof(base), "/sys/kernel/debug/ieee80211/%s/mt76", e->d_name);
		char p[320];
		snprintf(p, sizeof(p), "%s/icap_prime", base);
		if (access(p, W_OK) == 0) snprintf(cmd_dir, sizeof(cmd_dir), "%s", base);
		snprintf(p, sizeof(p), "%s/fwlog_data", base);
		if (access(p, R_OK) == 0) snprintf(relay_path, sizeof(relay_path), "%s", p);
	}
	if (d) closedir(d);
	if (!cmd_dir[0]) { fprintf(stderr, "icap_prime not found (diagnostic driver loaded?)\n"); return 1; }

	snprintf(icap_path, sizeof(icap_path), "%s/icap_prime", cmd_dir);
	snprintf(regidx_path, sizeof(regidx_path), "%s/regidx", cmd_dir);
	snprintf(regval_path, sizeof(regval_path), "%s/regval", cmd_dir);
	snprintf(fw_debug_bin, sizeof(fw_debug_bin), "%s/fw_debug_bin", cmd_dir);
	int fb = open(fw_debug_bin, O_WRONLY);
	if (fb >= 0) { if (write(fb, "0\n", 2) < 0) {} close(fb); }   /* (re)create + reset relay */
	if (!relay_path[0]) {   /* relay file appears only after fw_debug_bin is set */
		DIR *d2 = opendir("/sys/kernel/debug/ieee80211");
		while (d2 && (e = readdir(d2))) {
			if (e->d_name[0] == '.') continue;
			char p[320];
			snprintf(p, sizeof(p), "/sys/kernel/debug/ieee80211/%s/mt76/fwlog_data", e->d_name);
			if (access(p, R_OK) == 0) { snprintf(relay_path, sizeof(relay_path), "%s", p); break; }
		}
		if (d2) closedir(d2);
	}
	if (!relay_path[0]) { fprintf(stderr, "fwlog_data relay not found\n"); return 1; }
	relay_fd = open(relay_path, O_RDONLY | O_NONBLOCK);
	if (relay_fd < 0) die("open relay");
	fprintf(stderr, "iqd: cmd=%s relay=%s port=%d node=%#x len=%u\n",
		cmd_dir, relay_path, port, node, len);

	int srv = socket(AF_INET, SOCK_STREAM, 0);
	int one = 1;
	setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = INADDR_ANY };
	if (bind(srv, (void *)&sa, sizeof(sa)) < 0) die("bind");
	if (listen(srv, 1) < 0) die("listen");

	uint8_t *out = malloc(4 + WINDOW);
	memcpy(out, "IQ01", 4);
	for (;;) {
		int c = accept(srv, NULL, NULL);
		if (c < 0) continue;
		setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
		fprintf(stderr, "iqd: client connected%s\n", use_func4 ? " (func4 ring+flush)" : "");
		if (use_func4) {                   /* set up the datapath once; func4 arms the ring */
			native_capture();
			for (int t = 0; t < 2000 && reg_rd(CR0) != 0x401; t++) ;
		}
		relay_drain();
		int fail = 0;
		while (fail < 5) {
			if (frame(out + 4) < 0) { fail++; continue; }
			fail = 0;
			ssize_t off = 0, tot = 4 + WINDOW;
			while (off < tot) {
				ssize_t n = write(c, out + off, tot - off);
				if (n <= 0) { off = -1; break; }
				off += n;
			}
			if (off < 0) break;
		}
		close(c);
		reg_wr(CR0, 0x400);
		fprintf(stderr, "iqd: client gone\n");
	}
}
