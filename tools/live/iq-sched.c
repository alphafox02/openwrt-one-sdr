/* SPDX-License-Identifier: MIT
 * Set a selected capture thread's CPU and policy for controlled latency tests.
 * Usage: iq-sched PID CPU FIFO_PRIORITY (zero restores SCHED_OTHER).
 */
#define _GNU_SOURCE
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>
int main(int argc,char **argv) {
    if(argc!=4) { fprintf(stderr,"usage: %s PID CPU FIFO_PRIORITY\n",argv[0]); return 2; }
    int pid=atoi(argv[1]),cpu=atoi(argv[2]),prio=atoi(argv[3]);
    if(pid<1 || cpu<0 || cpu>=CPU_SETSIZE || prio<0 || prio>50) return 2;
    cpu_set_t cpus; CPU_ZERO(&cpus); CPU_SET(cpu,&cpus);
    struct sched_param param={.sched_priority=prio};
    /* musl's POSIX wrapper can return ENOSYS; use the Linux syscall semantics. */
    if(sched_setaffinity(pid,sizeof(cpus),&cpus) || syscall(SYS_sched_setscheduler,pid,prio?SCHED_FIFO:SCHED_OTHER,&param)) {
        fprintf(stderr,"pid=%d: %s\n",pid,strerror(errno)); return 1;
    }
    printf("pid=%d cpu=%d policy=%s priority=%d\n",pid,cpu,prio?"FIFO":"OTHER",prio);
    return 0;
}
