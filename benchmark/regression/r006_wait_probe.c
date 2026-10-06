#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

/* Diagnostic only. No allocation or output in request-path observers. */
#define THREADS 16
#define EVENTS 2048
struct event { uint64_t tid, kind, start, elapsed, cpu, object, caller, result; };
struct slot { uint64_t tid, used, dropped, calls[7], maximum[7]; struct event events[EVENTS]; };
static struct slot slots[THREADS];
static _Atomic unsigned next_slot;
static _Thread_local int index_ = -1;
static _Thread_local int observing;
static char output[4096];
static int (*real_lock)(pthread_mutex_t *);
static ssize_t (*real_write)(int,const void*,size_t);
static ssize_t (*real_recv)(int,void*,size_t,int);
static ssize_t (*real_sendfile)(int,int,off_t*,size_t);
static size_t (*real_fwrite)(const void*,size_t,size_t,FILE*);
static int (*real_fflush)(FILE*);
static uint64_t now(clockid_t clock) { struct timespec t; clock_gettime(clock,&t); return (uint64_t)t.tv_sec*1000000000+t.tv_nsec; }
static void record(unsigned kind,uint64_t start,uint64_t cpu,uintptr_t object,uintptr_t caller,long result) {
  uint64_t elapsed=now(CLOCK_MONOTONIC)-start, consumed=now(CLOCK_THREAD_CPUTIME_ID)-cpu;
  if(index_<0) index_=(int)atomic_fetch_add(&next_slot,1);
  if(index_>=THREADS) return;
  struct slot *s=&slots[index_];
  if(!s->tid) s->tid=(uint64_t)syscall(SYS_gettid);
  s->calls[kind]++;
  if(elapsed>s->maximum[kind]) s->maximum[kind]=elapsed;
  if(elapsed<5000000) return;
  if(s->used==EVENTS) { s->dropped++; return; }
  s->events[s->used++]=(struct event){s->tid,kind,start,elapsed,consumed,object,caller,(uint64_t)result};
}
static void dump(void) {
  int fd=(int)syscall(SYS_openat,AT_FDCWD,output,O_CREAT|O_WRONLY|O_EXCL,0600);
  if(fd<0) return;
  uint64_t header[8]={0x5230303657414954ULL,1,sizeof(struct slot),THREADS,EVENTS,atomic_load(&next_slot),(uint64_t)getpid(),1};
  size_t header_left=sizeof(header); const unsigned char *header_data=(const unsigned char*)header;
  while(header_left) { long amount=syscall(SYS_write,fd,header_data,header_left); if(amount<=0) { syscall(SYS_close,fd); return; } header_data+=amount; header_left-=(size_t)amount; }
  const unsigned char *data=(const unsigned char*)slots;
  size_t left=sizeof(slots);
  while(left) { long n=syscall(SYS_write,fd,data,left); if(n<=0) break; data+=n; left-=(size_t)n; }
  syscall(SYS_close,fd);
}
__attribute__((constructor)) static void initialize(void) {
  observing=1;
  real_lock=dlsym(RTLD_NEXT,"pthread_mutex_lock");
  real_write=dlsym(RTLD_NEXT,"write"); real_recv=dlsym(RTLD_NEXT,"recv"); real_sendfile=dlsym(RTLD_NEXT,"sendfile");
  real_fwrite=dlsym(RTLD_NEXT,"fwrite"); real_fflush=dlsym(RTLD_NEXT,"fflush");
  const char *path=getenv("R006_WAIT_OUTPUT");
  if(!path || strlen(path)>=sizeof(output)-6 || !real_lock || !real_write || !real_recv || !real_sendfile || !real_fwrite || !real_fflush) _exit(125);
  strcpy(output,path);
  char map_path[4096]; strcpy(map_path,path); strcat(map_path,".maps");
  int in=syscall(SYS_openat,AT_FDCWD,"/proc/self/maps",O_RDONLY,0);
  int out=syscall(SYS_openat,AT_FDCWD,map_path,O_CREAT|O_WRONLY|O_EXCL,0600);
  char buffer[4096]; long n;
  if(in<0 || out<0) _exit(125);
  while((n=syscall(SYS_read,in,buffer,sizeof(buffer)))>0) {
    long written=0;
    while(written<n) { long amount=syscall(SYS_write,out,buffer+written,n-written); if(amount<=0) _exit(125); written+=amount; }
  }
  syscall(SYS_close,in); syscall(SYS_close,out); atexit(dump); observing=0;
}
#define BEGIN if(observing) return
int pthread_mutex_lock(pthread_mutex_t *mutex) {
  if(!real_lock) _exit(125);
  if(observing) return real_lock(mutex);
  observing=1; uint64_t start=now(CLOCK_MONOTONIC),cpu=now(CLOCK_THREAD_CPUTIME_ID);
  int result=real_lock(mutex); int saved_errno=errno; record(1,start,cpu,(uintptr_t)mutex,(uintptr_t)__builtin_return_address(0),result); observing=0; errno=saved_errno; return result;
}
ssize_t write(int fd,const void *buf,size_t size) {
  if(!real_write) _exit(125);
  if(observing) return real_write(fd,buf,size);
  observing=1; uint64_t start=now(CLOCK_MONOTONIC),cpu=now(CLOCK_THREAD_CPUTIME_ID);
  ssize_t result=real_write(fd,buf,size); int saved_errno=errno; record(2,start,cpu,fd,(uintptr_t)__builtin_return_address(0),result); observing=0; errno=saved_errno; return result;
}
ssize_t recv(int fd,void *buf,size_t size,int flags) {
  if(!real_recv) _exit(125);
  if(observing) return real_recv(fd,buf,size,flags);
  observing=1; uint64_t start=now(CLOCK_MONOTONIC),cpu=now(CLOCK_THREAD_CPUTIME_ID);
  ssize_t result=real_recv(fd,buf,size,flags); int saved_errno=errno; record(3,start,cpu,fd,(uintptr_t)__builtin_return_address(0),result); observing=0; errno=saved_errno; return result;
}
ssize_t sendfile(int socket,int file,off_t *offset,size_t size) {
  if(!real_sendfile) _exit(125);
  if(observing) return real_sendfile(socket,file,offset,size);
  observing=1; uint64_t start=now(CLOCK_MONOTONIC),cpu=now(CLOCK_THREAD_CPUTIME_ID);
  ssize_t result=real_sendfile(socket,file,offset,size); int saved_errno=errno; record(4,start,cpu,socket,(uintptr_t)__builtin_return_address(0),result); observing=0; errno=saved_errno; return result;
}

size_t fwrite(const void *buf,size_t size,size_t count,FILE *stream) {
  if(!real_fwrite) _exit(125);
  if(observing) return real_fwrite(buf,size,count,stream);
  observing=1; uint64_t start=now(CLOCK_MONOTONIC),cpu=now(CLOCK_THREAD_CPUTIME_ID);
  size_t result=real_fwrite(buf,size,count,stream); int saved_errno=errno;
  record(5,start,cpu,(uintptr_t)stream,(uintptr_t)__builtin_return_address(0),(long)result);
  observing=0; errno=saved_errno; return result;
}
int fflush(FILE *stream) {
  if(!real_fflush) _exit(125);
  if(observing) return real_fflush(stream);
  observing=1; uint64_t start=now(CLOCK_MONOTONIC),cpu=now(CLOCK_THREAD_CPUTIME_ID);
  int result=real_fflush(stream); int saved_errno=errno;
  record(6,start,cpu,(uintptr_t)stream,(uintptr_t)__builtin_return_address(0),result);
  observing=0; errno=saved_errno; return result;
}
