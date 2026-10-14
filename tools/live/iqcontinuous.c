/* SPDX-License-Identifier: MIT
 * Bounded or long-running native-pool acquisition. Requires --pool WM func6.
 * Wire: IQC1, LE sequence, LE complex-sample count, LE sample rate, then CI8 I,Q.
 * One 8192-byte raw packet holds 1024 time samples on each of two RX paths.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static char base[512], relay[512];
static volatile sig_atomic_t stopping;
static void stopped(int sig) { (void)sig; stopping=1; }
static void die(const char *s) { perror(s); exit(1); }
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec*1e-9; }
static void put(const char *file,uint64_t v) {
 char p[640],s[40]; snprintf(p,sizeof(p),"%s/%s",base,file);
 int fd=open(p,O_WRONLY); if(fd<0) die(p);
 int n=snprintf(s,sizeof(s),"0x%llx\n",(unsigned long long)v);
 if(write(fd,s,n)!=n) die("write");
 close(fd);
}
static uint32_t rd(uint32_t addr) {
 char p[640],s[64]; put("regidx",addr); snprintf(p,sizeof(p),"%s/regval",base);
 int fd=open(p,O_RDONLY); if(fd<0) die(p); int n=read(fd,s,63); close(fd);
 if(n<=0) die("reg read");
 s[n]=0; return strtoul(s,0,0);
}
static void wr(uint32_t addr,uint32_t v) { put("regidx",addr); put("regval",v); }
static void kb(uint64_t v) { put("icap_prime",v); }
static void sw(unsigned off,uint32_t v) { for(int j=0;j<4;j++) kb(0x0100000000000000ULL|((uint64_t)(off+j)<<16)|((v>>(8*j))&255)); }
static void fire(void) { kb(0x1400000000000000ULL); }
static void locate(void) {
 DIR *d=opendir("/sys/kernel/debug/ieee80211"); struct dirent *e;
 if(!d) die("debugfs");
 while((e=readdir(d))) {
  char p[512]; if(e->d_name[0]=='.') continue;
  snprintf(p,sizeof(p),"/sys/kernel/debug/ieee80211/%s/mt76/icap_prime",e->d_name);
  if(access(p,W_OK)==0) { *strrchr(p,'/')=0; snprintf(base,sizeof(base),"%s",p); }
 }
 closedir(d); if(!base[0]) { fprintf(stderr,"diagnostic driver missing\n"); exit(1); }
 put("fw_debug_bin",0);
 d=opendir("/sys/kernel/debug/ieee80211");
 while((e=readdir(d))) {
  char p[512]; snprintf(p,sizeof(p),"/sys/kernel/debug/ieee80211/%s/mt76/fwlog_data",e->d_name);
  if(access(p,R_OK)==0) snprintf(relay,sizeof(relay),"%s",p);
 }
 closedir(d); if(!relay[0]) { fprintf(stderr,"relay missing\n"); exit(1); }
}
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static void set32(uint8_t *p,uint32_t x) { for(int i=0;i<4;i++) p[i]=x>>(8*i); }
static int sendall(int fd,const uint8_t *b,size_t n) {
 while(n && !stopping) { ssize_t k=send(fd,b,n,MSG_NOSIGNAL); if(k<0 && errno==EINTR) continue; if(k<=0) return -1; b+=k;n-=k; } return n ? -1 : 0;
}
/* Two-thread firehose: the single-thread read+send can't be on the right core
 * for BOTH the WM event-drain (NAPI) and the eth TX at once, so it starves one
 * and the 3.3 ms pool overruns. Split them: reader drains the relay into a big
 * RAM ring on the WM-NAPI core; sender pushes the ring to the socket on the
 * eth-TX core. SPSC ring, byte counters, release/acquire ordering. */
#define RINGSZ (64u<<20)
static uint8_t *ring;
static volatile uint64_t wpos=0, rpos=0;
static int g_relayfd, g_client, g_reader_cpu, g_sender_cpu;
static volatile int g_run=1, g_rerr=0;
static uint64_t g_want;
static void pin_cpu(int c){ cpu_set_t s; CPU_ZERO(&s); CPU_SET(c,&s); sched_setaffinity(0,sizeof(s),&s); }
static double mono(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec*1e-9; }
static void* reader_fn(void*_){
 (void)_; pin_cpu(g_reader_cpu); uint64_t fwd=0; double last=mono();
 while(g_run && fwd<g_want){
  uint64_t w=wpos, r=__atomic_load_n(&rpos,__ATOMIC_ACQUIRE);
  size_t space=RINGSZ-(size_t)(w-r);
  if(space<(1u<<16)){ usleep(50); continue; }        /* ring full: let sender catch up */
  size_t off=w%RINGSZ, chunk=RINGSZ-off; if(chunk>space)chunk=space; if(chunk>(1u<<20))chunk=1u<<20;
  ssize_t n=read(g_relayfd, ring+off, chunk);
  if(n<0 && errno!=EAGAIN && errno!=EINTR){ g_rerr=1; break; }
  if(n<=0){ if(mono()-last>2){ g_rerr=2; break; } usleep(20); continue; }
  last=mono(); fwd+=(uint64_t)n;
  __atomic_store_n(&wpos, w+(uint64_t)n, __ATOMIC_RELEASE);
 }
 g_run=0; return 0;
}
static void* sender_fn(void*_){
 (void)_; pin_cpu(g_sender_cpu); uint64_t sent=0;
 for(;;){
  uint64_t w=__atomic_load_n(&wpos,__ATOMIC_ACQUIRE), r=rpos; uint64_t avail=w-r;
  if(!avail){ if(!g_run) break; usleep(50); continue; }
  size_t off=r%RINGSZ, chunk=RINGSZ-off; if(chunk>avail)chunk=(size_t)avail;
  ssize_t k=send(g_client, ring+off, chunk, MSG_NOSIGNAL);
  if(k<=0){ g_rerr=3; break; }
  sent+=(uint64_t)k; __atomic_store_n(&rpos, r+(uint64_t)k, __ATOMIC_RELEASE);
 }
 return 0;
}

int main(int argc,char **argv) {
 uint32_t packets=81920,rate=40000000; int port=0,bandwidth=0,cpu=1; const char *outpath=0; int outfd=-1;
 for(int i=1;i<argc;i++) {
  if(!strcmp(argv[i],"--packets") && i+1<argc) packets=strtoul(argv[++i],0,0);
  else if(!strcmp(argv[i],"--listen") && i+1<argc) port=atoi(argv[++i]);
  else if(!strcmp(argv[i],"--bw") && i+1<argc) bandwidth=atoi(argv[++i]);
  else if(!strcmp(argv[i],"--cpu") && i+1<argc) cpu=atoi(argv[++i]);
  else if(!strcmp(argv[i],"--out") && i+1<argc) outpath=argv[++i];
  else { fprintf(stderr,"usage: %s [--packets N] [--listen PORT] [--bw 0|1] [--cpu N] [--out FILE]\n",argv[0]); return 2; }
 }
 if(!packets || bandwidth<0 || bandwidth>1 || port<0 || port>65535) return 2;
 rate <<= bandwidth;
 /* The 512 KiB pool is only ~3.3 ms at 40 MSps: if this reader is preempted
  * longer than that the producer overruns. Don't use SCHED_FIFO (it starves the
  * softirq that drains the DMA ring -> worse). Instead: nice -20, pin memory,
  * and pin to CPU1 so we don't fight the WiFi softirqs on CPU0. */
 setpriority(PRIO_PROCESS,0,-20);
 if(mlockall(MCL_CURRENT|MCL_FUTURE)) perror("mlockall (continuing)");
 { cpu_set_t set; CPU_ZERO(&set); CPU_SET(cpu,&set);
   if(sched_setaffinity(0,sizeof(set),&set)) perror("affinity (continuing)"); }
 signal(SIGINT,stopped); signal(SIGTERM,stopped); signal(SIGPIPE,SIG_IGN);
 int client=-1;
 if(port) {
  int srv=socket(AF_INET,SOCK_STREAM,0),one=1; if(srv<0) die("socket");
  setsockopt(srv,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));
  struct sockaddr_in sa={.sin_family=AF_INET,.sin_port=htons(port),.sin_addr.s_addr=INADDR_ANY};
  if(bind(srv,(void*)&sa,sizeof(sa)) || listen(srv,1)) die("listen");
  fprintf(stderr,"waiting for CI8 client on port%d\n",port);
  client=accept(srv,0,0);close(srv);if(client<0)die("accept");
  int sz=4*1024*1024;setsockopt(client,SOL_SOCKET,SO_SNDBUF,&sz,sizeof(sz));
  setsockopt(client,IPPROTO_TCP,TCP_NODELAY,&one,sizeof(one));
  struct timeval tv={.tv_sec=2};setsockopt(client,SOL_SOCKET,SO_SNDTIMEO,&tv,sizeof(tv));
 }
 locate(); int fd=open(relay,O_RDONLY|O_NONBLOCK);if(fd<0)die(relay);
 kb(0);sw(4,1);sw(16,0x204000);sw(20,4096);sw(24,4096);sw(44,bandwidth);fire();
 double deadline=now()+2;
 while(rd(0x80023090)!=0x401 && now()<deadline) usleep(100);
 if(rd(0x80023090)!=0x401) { fprintf(stderr,"capture prime timeout\n"); return 1; }
 kb(0);fire();usleep(20000);wr(0x80023090,0x400);
 uint8_t *buf=malloc(1024*1024);if(!buf)die("malloc");
 /* Batched raw forward: no per-sample CI8 conversion in the critical read loop
  * (that convert overran the 3.3 ms pool). Send the raw 8192-byte 2-path IQ
  * payload with a 16-byte IQR1 header, batched into ~1 MiB sends. Host converts. */
 while(read(fd,buf,1024*1024)>0) {}
 kb(0);sw(0,6);sw(4,packets);fire();
 uint32_t got=0;size_t fill=0;int failed=0;double start=now(),last=start;
 if(client>=0) {
  /* Two-thread firehose (see above): reader drains relay -> ring on --cpu
   * (place the mt7915e IRQ there), sender -> socket on the other core (eth TX). */
  ring=malloc(RINGSZ); if(!ring)die("ring malloc");
  g_relayfd=fd; g_client=client; g_want=(uint64_t)packets*8256;
  g_reader_cpu=cpu; g_sender_cpu=cpu?0:1;
  pthread_t rt,st; pthread_create(&rt,0,reader_fn,0); pthread_create(&st,0,sender_fn,0);
  pthread_join(rt,0); g_run=0; pthread_join(st,0);
  if(g_rerr){ if(g_rerr==2)fprintf(stderr,"stream timeout\n"); failed=1; }
 } else {
  while(got<packets && !stopping) {
   ssize_t n=read(fd,buf+fill,1024*1024-fill);
   if(n<0 && errno!=EAGAIN && errno!=EINTR) { failed=1;break; }
   if(n<=0) { if(now()-last>2) {fprintf(stderr,"stream timeout\n");failed=1;break;} usleep(20);continue; }
   fill+=n;last=now();size_t pos=0;
   while(fill-pos>=4 && !stopping) {
    uint32_t size=le32(buf+pos);if(size!=8252){fprintf(stderr,"bad record %u\n",size);failed=1;break;}
    if(fill-pos<size+4)break;
    const uint8_t *r=buf+pos+4;uint32_t seq=le32(r+48),src=le32(r+52);
    if(r[40]!=0xed || r[44]!=0xf1 || seq!=got || src!=0xe0200000+(got%64)*8192) {
     fprintf(stderr,"sequence/source error expected%u got%u source%08x\n",got,seq,src);failed=1;break;
    }
    got++;pos+=size+4;
   }
   if(failed)break;
   memmove(buf,buf+pos,fill-pos);fill-=pos;
  }
 }
 wr(0xe009b840,1);usleep(20000);
 uint32_t status=rd(0xe009b800),sent=rd(0xe009b804),elapsed=rd(0xe009b808),produced=rd(0xe009b818),peak=rd(0xe009b81c);
 fprintf(stderr,"received=%u requested=%u status=%u sent=%u elapsed_us=%u produced_words=%u peak_unread_words=%u wall_s=%.6f sample_MSps=%.6f\n",
  got,packets,status,sent,elapsed,produced,peak,now()-start,elapsed ? sent*1024.0/elapsed:0);
 close(fd);if(client>=0)close(client);free(buf);
 return failed || got!=packets || status!=2;
}
