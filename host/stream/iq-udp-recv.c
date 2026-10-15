/* SPDX-License-Identifier: MIT
 * Validate IQD1 UDP sequence/sample positions; optionally save a bounded prefix.
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
static volatile sig_atomic_t stop;
static void interrupted(int sig){(void)sig;stop=1;}
static uint32_t le32(const unsigned char*p){return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static uint16_t le16(const unsigned char*p){return p[0]|p[1]<<8;}
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;}
int main(int argc,char**argv){
 int port=argc>1?atoi(argv[1]):7701;
 uint64_t want=argc>2?strtoull(argv[2],0,0):0;
 FILE *save=argc>3?fopen(argv[3],"wb"):NULL;
 if(argc>3&&!save){perror("save");return 2;}
 int fd=socket(AF_INET,SOCK_DGRAM,0);if(fd<0){perror("socket");return 2;}
 int size=16<<20;setsockopt(fd,SOL_SOCKET,SO_RCVBUF,&size,sizeof(size));
 struct timeval timeout={.tv_sec=1};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
 struct sockaddr_in a={.sin_family=AF_INET,.sin_port=htons(port),.sin_addr.s_addr=INADDR_ANY};
 if(bind(fd,(void*)&a,sizeof(a))){perror("bind");return 2;}
 unsigned char buffers[BATCH][1200];struct mmsghdr msgs[BATCH]={0};struct iovec iov[BATCH];
 for(int j=0;j<BATCH;j++){iov[j]=(struct iovec){buffers[j],sizeof(buffers[j])};msgs[j].msg_hdr.msg_iov=&iov[j];msgs[j].msg_hdr.msg_iovlen=1;}
 signal(SIGINT,interrupted);signal(SIGTERM,interrupted);
 uint64_t packets=0,expected=0,gaps=0,late=0,bad=0,saved=0;uint32_t run=0,rate=0;
 double start=now(),first=0,last=start,report=start;
 fprintf(stderr,"listening UDP port=%d expected_datagrams=%llu\n",port,(unsigned long long)want);fflush(stderr);
 while(!stop && (!want||packets<want)){
  int n=recvmmsg(fd,msgs,BATCH,MSG_WAITFORONE,NULL);
  if(n<0){if(errno!=EINTR&&errno!=EAGAIN&&errno!=EWOULDBLOCK){perror("recv");bad++;break;}if(now()-last>5 && packets)break;if(now()-start>60&&!packets)break;continue;}
  last=now();if(!first)first=last;
  for(int j=0;j<n;j++){
   unsigned char*p=buffers[j];
   if(msgs[j].msg_len!=1056 || memcmp(p,"IQD1",4) || le16(p+14)!=512 || le16(p+20)!=1 || (le16(p+12)!=0 && le16(p+12)!=512)){bad++;continue;}
   if(!packets){run=le32(p+4);rate=le32(p+16);}
   if(le32(p+4)!=run || le32(p+16)!=rate || le32(p+24)!=0xe0200000+(le32(p+8)%64)*8192){bad++;continue;}
   uint64_t seq=(uint64_t)le32(p+8)*2+le16(p+12)/512;
   if(seq>expected)gaps+=seq-expected;
   if(seq<expected)late++;else expected=seq+1;
   packets++;
   if(save&&saved<(1u<<20)){if(fwrite(p+32,1,1024,save)!=1024){perror("save");stop=1;}saved+=1024;}
  }
  if(last-report>1){fprintf(stderr,"packets=%llu missing=%llu late=%llu bad=%llu\n",(unsigned long long)packets,(unsigned long long)gaps,(unsigned long long)late,(unsigned long long)bad);report=last;}
 }
 double elapsed=last-first;
 printf("run=%u rate_label=%u packets=%llu samples=%llu missing=%llu late=%llu bad=%llu reception_s=%.6f payload_Mbps=%.3f\n",run,rate,(unsigned long long)packets,(unsigned long long)(packets*512),(unsigned long long)gaps,(unsigned long long)late,(unsigned long long)bad,elapsed,elapsed>0?packets*1024.0*8/elapsed/1e6:0);
 if(save)fclose(save);
 close(fd);
 return !packets||gaps||late||bad||(want&&packets!=want);
}
