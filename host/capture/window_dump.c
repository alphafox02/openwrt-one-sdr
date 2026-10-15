/* SPDX-License-Identifier: MIT
 * Read a retired MT7981 capture window through mt76 debugfs. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
static void die(const char *s) { perror(s); exit(1); }
int main(int argc, char **argv) {
    if(argc != 3 && argc != 4) { fprintf(stderr,"usage: %s debugfs-directory word-count [ROM-page-base]\n",argv[0]); return 2; }
    unsigned long n=strtoul(argv[2],NULL,0);
    if(!n || n>16384) return 2;
    unsigned long base=argc==4 ? strtoul(argv[3],NULL,0) : 0x00400000;
    if (argc==4 && ((base & 3) || base<0x00800000 || base+n*4>0x00840000)) return 2;
    char idxpath[512],valpath[512],buf[64];
    snprintf(idxpath,sizeof(idxpath),"%s/regidx",argv[1]);
    snprintf(valpath,sizeof(valpath),"%s/regval",argv[1]);
    uint32_t *data=calloc(n,4); if(!data) die("calloc");
    for(unsigned long i=0;i<n;i++) {
        int len=snprintf(buf,sizeof(buf),"0x%08lx\n",base+i*4);
        int idx=open(idxpath,O_WRONLY); if(idx<0) die(idxpath);
        if(write(idx,buf,len)!=len) die("regidx");
        close(idx);
        int val=open(valpath,O_RDONLY); if(val<0) die(valpath);
        ssize_t got=read(val,buf,sizeof(buf)-1); if(got<=0) die("regval");
        close(val);
        buf[got]=0; char *end;
        errno=0; unsigned long word=strtoul(buf,&end,0);
        if(errno || end==buf || word>UINT32_MAX) { fprintf(stderr,"invalid register value\n"); return 1; }
        data[i]=(uint32_t)word;
    }
    if(fwrite(data,4,n,stdout)!=n || fflush(stdout)) die("stdout");
    free(data); return 0;
}
