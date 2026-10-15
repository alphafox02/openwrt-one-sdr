/* SPDX-License-Identifier: MIT
 * Validate this exporter's bounded VRT runs without storing the entire stream.
 * The four-bit VRT counter alone cannot detect losses of multiples of 16;
 * supply the expected signal packet total and check firmware completion too.
 * Usage: vrt-check PORT EXPECTED_SIGNAL_PACKETS [8|16] [PREFIX_FILE]
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#define BATCH 64
static volatile sig_atomic_t stopped;
static void stop(int sig) { (void)sig; stopped = 1; }
static uint32_t be32(const unsigned char *p) {
    return (uint32_t)p[0]<<24 | (uint32_t)p[1]<<16 | (uint32_t)p[2]<<8 | p[3];
}
static uint64_t be64(const unsigned char *p) {
    return (uint64_t)be32(p)<<32 | be32(p+4);
}
static double now(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec*1e-9;
}
int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr,"usage: %s PORT SIGNAL_PACKETS [8|16] [PREFIX]\n",argv[0]); return 2; }
    int port=atoi(argv[1]), bits=argc>3?atoi(argv[3]):16;
    uint64_t want=strtoull(argv[2],0,0);
    if (port<1 || port>65535 || !want || (bits!=8 && bits!=16)) return 2;
    FILE *save=argc>4?fopen(argv[4],"wb"):NULL;
    if (argc>4 && !save) { perror("prefix"); return 2; }
    int fd=socket(AF_INET,SOCK_DGRAM,0), size=16<<20, one=1;
    if(fd<0) { perror("socket"); return 2; }
    setsockopt(fd,SOL_SOCKET,SO_RCVBUF,&size,sizeof(size));
    setsockopt(fd,SOL_SOCKET,SO_RXQ_OVFL,&one,sizeof(one));
    struct timeval timeout={.tv_sec=1};
    setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
    struct sockaddr_in addr={.sin_family=AF_INET,.sin_port=htons(port),.sin_addr.s_addr=INADDR_ANY};
    if(bind(fd,(void*)&addr,sizeof(addr))) { perror("bind"); return 2; }
    unsigned char buf[BATCH][2048], control[BATCH][CMSG_SPACE(sizeof(uint32_t))];
    struct mmsghdr msgs[BATCH]={0}; struct iovec iov[BATCH];
    for(int j=0;j<BATCH;j++) {
        iov[j]=(struct iovec){buf[j],sizeof(buf[j])};
        msgs[j].msg_hdr.msg_iov=&iov[j]; msgs[j].msg_hdr.msg_iovlen=1;
        msgs[j].msg_hdr.msg_control=control[j];
    }
    signal(SIGINT,stop); signal(SIGTERM,stop);
    uint64_t packets=0,contexts=0,bad=0,gaps=0,saved=0,rate=0,freq=0,samples=0,bytes=0;
    uint32_t socket_drops=0; unsigned next=0,context_next=0;
    double start=now(),first=0,last=start,report=start;
    fprintf(stderr,"listening VRT UDP %d bits=%d expected=%llu\n",port,bits,(unsigned long long)want);
    while(!stopped) {
        for(int j=0;j<BATCH;j++) { msgs[j].msg_hdr.msg_controllen=sizeof(control[j]); msgs[j].msg_hdr.msg_flags=0; }
        int n=recvmmsg(fd,msgs,BATCH,MSG_WAITFORONE,NULL);
        if(n<0) {
            if(errno!=EINTR && errno!=EAGAIN && errno!=EWOULDBLOCK) { perror("recv"); bad++; break; }
            if((packets && now()-last>3) || (!packets && now()-start>60)) break;
            continue;
        }
        last=now();
        for(int j=0;j<n;j++) {
            struct msghdr *m=&msgs[j].msg_hdr;
            for(struct cmsghdr *c=CMSG_FIRSTHDR(m);c;c=CMSG_NXTHDR(m,c))
                if(c->cmsg_level==SOL_SOCKET && c->cmsg_type==SO_RXQ_OVFL) memcpy(&socket_drops,CMSG_DATA(c),4);
            unsigned char *p=buf[j]; unsigned len=msgs[j].msg_len;
            if((m->msg_flags&MSG_TRUNC) || len<8 || (be32(p)&0xffff)*4!=len || be32(p+4)!=1) { bad++; continue; }
            uint32_t h=be32(p);
            if((h>>28)==4) {
                if(len!=36 || (h&0x0ff00000) || be32(p+8)!=((1u<<27)|(1u<<21)|(1u<<15)) ||
                   be32(p+28)!=((1u<<29)|((bits-1)<<6)|(bits-1)) || be32(p+32)) { bad++; continue; }
                if(((h>>16)&15)!=context_next) gaps++;
                context_next=(((h>>16)&15)+1)&15;
                uint64_t f=be64(p+12),r=be64(p+20);
                if(!contexts) { freq=f; rate=r; }
                if(f!=freq || r!=rate || !r) bad++;
                contexts++; continue;
            }
            if((h>>28)!=1 || (h&0x0ff00000) || len<12 || len>1472 || (len-8)%(bits/4)) { bad++; continue; }
            if(!first) first=last;
            unsigned seq=(h>>16)&15;
            if(seq!=next) gaps++;
            next=(seq+1)&15;
            if(bits==16) for(unsigned k=8;k<len;k+=2) if(p[k+1]&3) { bad++; break; }
            packets++;
            samples+=(len-8)/(bits/4); bytes+=len-8;
            if(save && saved<(1u<<20)) {
                unsigned amount=len-8;
                if(amount>(1u<<20)-saved) amount=(1u<<20)-saved;
                if(fwrite(p+8,1,amount,save)!=amount) { perror("write"); bad++; stopped=1; }
                saved+=amount;
            }
        }
        if(last-report>10) { fprintf(stderr,"signal=%llu context=%llu sequence_errors=%llu bad=%llu socket_drops=%u\n",
            (unsigned long long)packets,(unsigned long long)contexts,(unsigned long long)gaps,(unsigned long long)bad,socket_drops); report=last; }
    }
    double elapsed=first?last-first:0;
    printf("signal=%llu expected=%llu contexts=%llu sequence_errors=%llu bad=%llu socket_drops=%u samples=%llu rate_hz=%.3f freq_hz=%.3f reception_s=%.6f payload_Mbps=%.3f\n",
        (unsigned long long)packets,(unsigned long long)want,(unsigned long long)contexts,(unsigned long long)gaps,(unsigned long long)bad,socket_drops,
        (unsigned long long)samples,rate/1048576.0,freq/1048576.0,elapsed,elapsed?bytes*8.0/elapsed/1e6:0);
    if(save && fclose(save)) bad++;
    close(fd);
    return packets!=want || contexts!=(want+127)/128 || gaps || bad || socket_drops || !rate;
}
