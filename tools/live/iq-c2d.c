/* SPDX-License-Identifier: MIT
 * Copyright 2026 CEMAXECUTER LLC.
 *
 * iq-c2d — router-side control daemon for the IQ exporter. A small UDP service:
 * it answers control requests, streams VITA 49 back to whoever asked (reading
 * the command's own source address, so the host never has to announce its IP),
 * and broadcasts a discovery beacon so a host finds the unit regardless of DHCP.
 *
 * Line protocol on UDP (default :4992), one request per datagram:
 *   DISCOVER                                  -> SDR <capabilities>
 *   STATUS                                    -> OK <iq_udp status line>
 *   START [port=P] [rate=40] [freq=HZ] [fmt=vrt16|vrt8|iqd1] [packets=N]
 *                                             -> OK streaming to IP:PORT ...
 *   STOP                                      -> OK stopped
 *   TUNE freq=HZ                              -> OK tuned freq=HZ capture=stopped
 * The beacon (every ~1 s, to the broadcast address) is the DISCOVER reply.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <ctype.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CTRL_PORT 4992
#define START_SH  "/root/iq-udp-start.sh"
#define BEACON    "SDR model=openwrt-one bands=2g,5g rates=40 formats=vrt16,vrt8,iqd1 ctrl=4992 stream=4991"

static const char *debugfs_iq_udp(char *buf, size_t n)
{
	/* find the phy whose mt76 dir has iq_udp */
	FILE *p = popen("ls -d /sys/kernel/debug/ieee80211/phy*/mt76 2>/dev/null", "r");
	char line[256];
	buf[0] = 0;
	if (!p) return buf;
	while (fgets(line, sizeof(line), p)) {
		line[strcspn(line, "\n")] = 0;
		char path[300];
		snprintf(path, sizeof(path), "%s/iq_udp", line);
		if (access(path, F_OK) == 0) { snprintf(buf, n, "%s", path); break; }
	}
	pclose(p);
	return buf;
}

/* MAC + iface for an IPv4 peer, from /proc/net/arp (populated by its datagram). */
static int arp_lookup(const char *ip, char *mac, char *iface)
{
	FILE *f = fopen("/proc/net/arp", "r");
	char l[256], a[64], hw[64], fl[64], m[64], mask[64], dev[64];
	int got = 0;
	if (!f) return 0;
	if (fgets(l, sizeof(l), f)) {}            /* header */
	while (fgets(l, sizeof(l), f)) {
		if (sscanf(l, "%63s %63s %63s %63s %63s %63s", a, hw, fl, m, mask, dev) == 6 &&
		    strcmp(a, ip) == 0 && strcmp(m, "00:00:00:00:00:00") != 0) {
			strcpy(mac, m); strcpy(iface, dev); got = 1; break;
		}
	}
	fclose(f);
	return got;
}

/* Our source IP toward the peer: connect a UDP socket and read its local name. */
static int src_for(const char *peer, char *src)
{
	int s = socket(AF_INET, SOCK_DGRAM, 0);
	struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(9) };
	struct sockaddr_in me; socklen_t ml = sizeof(me);
	inet_pton(AF_INET, peer, &sa.sin_addr);
	if (connect(s, (void *)&sa, sizeof(sa)) || getsockname(s, (void *)&me, &ml)) { close(s); return 0; }
	inet_ntop(AF_INET, &me.sin_addr, src, INET_ADDRSTRLEN);
	close(s);
	return 1;
}

static const char *kv(const char *req, const char *key, const char *def, char *out, size_t n)
{
	const char *p = strstr(req, key);
	size_t kl = strlen(key);
	if (p && (p == req || p[-1] == ' ') && p[kl] == '=') {
		p += kl + 1; size_t i = 0;
		while (*p && *p != ' ' && *p != '\n' && i < n - 1) out[i++] = *p++;
		out[i] = 0; return out;
	}
	snprintf(out, n, "%s", def);
	return out;
}

/* The central helper verifies UCI against the operating channel. */
static int profile(unsigned long long *freq, unsigned *rate, unsigned *band, unsigned *bw, unsigned *width)
{
    FILE *p = popen("/root/iq-radio.sh --profile", "r");
    if (!p) return 0;
    int n = fscanf(p, "%llu %u %u %u %u", freq, rate, width, band, bw);
    int rc = pclose(p);
    return n == 5 && rc == 0 && *band <= 1;
}
static int execute(char *const argv[])
{
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) { execv(argv[0], argv); _exit(127); }
    int status;
    while (waitpid(pid, &status, 0) < 0) if (errno != EINTR) return -1;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}
static int digits(const char *s)
{
    if (!*s) return 0;
    while (*s) if (!isdigit((unsigned char)*s++)) return 0;
    return 1;
}

int main(void)
{
	int s = socket(AF_INET, SOCK_DGRAM, 0);
	int one = 1;
	setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	setsockopt(s, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
	struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(CTRL_PORT),
				  .sin_addr.s_addr = htonl(INADDR_ANY) };
	if (bind(s, (void *)&sa, sizeof(sa))) { perror("bind"); return 1; }
	struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	struct sockaddr_in bc = { .sin_family = AF_INET, .sin_port = htons(CTRL_PORT) };
	bc.sin_addr.s_addr = htonl(INADDR_BROADCAST);
	time_t last_beacon = 0;
	char req[512], reply[512], knob[300];

	for (;;) {
		struct sockaddr_in from; socklen_t fl = sizeof(from);
		ssize_t n = recvfrom(s, req, sizeof(req) - 1, 0, (void *)&from, &fl);
		time_t now = time(0);
		if (now - last_beacon >= 1) {                     /* discovery beacon */
			sendto(s, BEACON, strlen(BEACON), 0, (void *)&bc, sizeof(bc));
			last_beacon = now;
		}
		if (n <= 0) continue;
		req[n] = 0;
		char peer[INET_ADDRSTRLEN];
		inet_ntop(AF_INET, &from.sin_addr, peer, sizeof(peer));
		debugfs_iq_udp(knob, sizeof(knob));
		reply[0] = 0;

		if (!strncmp(req, "DISCOVER", 8)) {
			snprintf(reply, sizeof(reply), "%s", BEACON);
		} else if (!strncmp(req, "STATUS", 6)) {
			char st[256] = "unknown"; FILE *f = knob[0] ? fopen(knob, "r") : 0;
			if (f) { if (fgets(st, sizeof(st), f)) st[strcspn(st, "\n")] = 0; fclose(f); }
			snprintf(reply, sizeof(reply), "OK %s", st);
		} else if (!strncmp(req, "PROFILE", 7)) {
			/* Current radio centre/rate/band over the network, so a controller
			 * need not SSH in to read it. */
			unsigned long long f; unsigned r, b, w, width;
			if (profile(&f, &r, &b, &w, &width))
				snprintf(reply, sizeof(reply), "OK freq=%llu rate=%u band=%u bw=%u bandwidth=%u", f, r, b, w, width);
			else
				snprintf(reply, sizeof(reply), "ERR radio profile unavailable");
		} else if (!strncmp(req, "STOP", 4)) {
			/* Stop acquisition and drain DMA before detaching the exporter. */
			int result = system("/root/iq-udp-stop.sh");
			snprintf(reply, sizeof(reply), result == 0 ? "OK stopped" : "ERR producer stop failed");
		} else if (!strncmp(req, "TUNE", 4)) {
			char fr[32]; kv(req, "freq", "", fr, sizeof(fr));
            char *args[] = {"/root/vita49-apply.sh", "rf_freq_hz", fr, NULL};
            int result = digits(fr) ? execute(args) : -1;
            snprintf(reply, sizeof(reply), result == 0 ? "OK tuned freq=%s capture=stopped" : "ERR tune failed freq=%s", fr);
        } else if (!strncmp(req, "START", 5)) {
            char port[16], rate[16], freq[32], fmt[16], pkts[16], mac[64], iface[64], src[INET_ADDRSTRLEN];
            unsigned long long actual_freq;
            unsigned actual_rate, actual_band, actual_bw, actual_width;
            if (!profile(&actual_freq, &actual_rate, &actual_band, &actual_bw, &actual_width)) {
                snprintf(reply, sizeof(reply), "ERR radio profile unavailable");
            } else {
                char default_freq[32], default_rate[16], band[8], bw[8];
                snprintf(default_freq,sizeof(default_freq),"%llu",actual_freq);
                snprintf(default_rate,sizeof(default_rate),"%u",actual_rate/1000000);
                snprintf(band,sizeof(band),"%u",actual_band);
                snprintf(bw,sizeof(bw),"%u",actual_bw);
                kv(req,"port","4991",port,sizeof(port));
                kv(req,"rate",default_rate,rate,sizeof(rate));
                kv(req,"freq",default_freq,freq,sizeof(freq));
                kv(req,"fmt","vrt16",fmt,sizeof(fmt));
                kv(req,"packets","40000000",pkts,sizeof(pkts));
                if (!digits(port) || strtoull(port,NULL,10)<1 || strtoull(port,NULL,10)>65535 ||
                    !digits(pkts) || strtoull(pkts,NULL,10)<1 || strtoull(pkts,NULL,10)>40000000 ||
                    strcmp(rate,default_rate) || strcmp(freq,default_freq) ||
                    (strcmp(fmt,"vrt16") && strcmp(fmt,"vrt8") && strcmp(fmt,"iqd1"))) {
                    snprintf(reply,sizeof(reply),"ERR invalid request or rate/frequency differs from active profile");
                } else if (!arp_lookup(peer,mac,iface) || !src_for(peer,src)) {
                    snprintf(reply,sizeof(reply),"ERR no direct neighbor/route for %s",peer);
                } else {
                    char *args[] = {START_SH,iface,src,peer,mac,port,pkts,bw,freq,fmt,band,NULL};
                    int result = execute(args);
                    snprintf(reply,sizeof(reply),result == 0 ? "OK streaming to %s:%s fmt=%s freq=%s rate=%s" : "ERR start failed to %s:%s fmt=%s freq=%s rate=%s",peer,port,fmt,freq,rate);
                }
            }
		} else {
			snprintf(reply, sizeof(reply), "ERR unknown: %.32s", req);
		}
		if (reply[0]) sendto(s, reply, strlen(reply), 0, (void *)&from, fl);
		while (waitpid(-1, 0, WNOHANG) > 0) {}             /* reap finished start scripts */
	}
}
