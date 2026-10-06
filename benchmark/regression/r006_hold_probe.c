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
struct slot { uint64_t tid, used, dropped, calls[8], maximum[8]; struct event events[EVENTS]; };
static struct slot slots[THREADS];
static _Atomic unsigned next_slot;
static _Thread_local int index_ = -1;
static _Thread_local int observing;
static uint64_t now(clockid_t clock);
struct held { pthread_mutex_t *mutex; uint64_t wall,cpu,caller; };
static _Thread_local struct held held[32];
static char output[4096];
static int (*real_unlock)(pthread_mutex_t *);
static int (*real_wait)(pthread_cond_t*,pthread_mutex_t*);
static int (*real_timedwait)(pthread_cond_t*,pthread_mutex_t*,const struct timespec*);
static int held_index(pthread_mutex_t *mutex) { for(int i=0;i<32;i++) if(held[i].mutex==mutex) return i; return -1; }
static void acquired(pthread_mutex_t *mutex,uintptr_t caller) {
  int index=held_index(mutex);
  if(index<0) for(int i=0;i<32;i++) if(!held[i].mutex) { index=i; break; }
  if(index<0) _exit(126);
  held[index]=(struct held){mutex,now(CLOCK_MONOTONIC),now(CLOCK_THREAD_CPUTIME_ID),caller};
}

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
  real_unlock=dlsym(RTLD_NEXT,"pthread_mutex_unlock"); real_wait=dlsym(RTLD_NEXT,"pthread_cond_wait"); real_timedwait=dlsym(RTLD_NEXT,"pthread_cond_timedwait");
  real_write=dlsym(RTLD_NEXT,"write"); real_recv=dlsym(RTLD_NEXT,"recv"); real_sendfile=dlsym(RTLD_NEXT,"sendfile");
  real_fwrite=dlsym(RTLD_NEXT,"fwrite"); real_fflush=dlsym(RTLD_NEXT,"fflush");
  const char *path=getenv("R006_WAIT_OUTPUT");
  if(!path || strlen(path)>=sizeof(output)-6 || !real_lock || !real_write || !real_recv || !real_sendfile || !real_fwrite || !real_fflush || !real_unlock || !real_wait || !real_timedwait) _exit(125);
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
  int result=real_lock(mutex); int saved_errno=errno; if(result==0) acquired(mutex,(uintptr_t)__builtin_return_address(0)); record(1,start,cpu,(uintptr_t)mutex,(uintptr_t)__builtin_return_address(0),result); observing=0; errno=saved_errno; return result;
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

int pthread_mutex_unlock(pthread_mutex_t *mutex) {
  if(!real_unlock) _exit(125);
  if(observing) return real_unlock(mutex);
  observing=1; int index=held_index(mutex); struct held value={0};
  if(index>=0) { value=held[index]; held[index].mutex=NULL; }
  if(index<0 && index_>=0 && index_<THREADS) slots[index_].calls[0]++;
  /* End holder interval immediately before the actual release. */
  if(index>=0) record(7,value.wall,value.cpu,(uintptr_t)mutex,value.caller,0);
  int result=real_unlock(mutex); int saved_errno=errno;
  observing=0; errno=saved_errno; return result;
}
int pthread_cond_wait(pthread_cond_t *condition,pthread_mutex_t *mutex) {
  if(!real_wait) _exit(125);
  if(observing) return real_wait(condition,mutex);
  int index=held_index(mutex); uintptr_t caller=index>=0?held[index].caller:0;
  if(index>=0) { struct held value=held[index]; record(7,value.wall,value.cpu,(uintptr_t)mutex,value.caller,0); held[index].mutex=NULL; }
  observing=1; int result=real_wait(condition,mutex); int saved_errno=errno;
  if(result==0 && caller) acquired(mutex,caller);
  observing=0; errno=saved_errno; return result;
}
int pthread_cond_timedwait(pthread_cond_t *condition,pthread_mutex_t *mutex,const struct timespec *deadline) {
  if(!real_timedwait) _exit(125);
  if(observing) return real_timedwait(condition,mutex,deadline);
  int index=held_index(mutex); uintptr_t caller=index>=0?held[index].caller:0;
  if(index>=0) { struct held value=held[index]; record(7,value.wall,value.cpu,(uintptr_t)mutex,value.caller,0); held[index].mutex=NULL; }
  observing=1; int result=real_timedwait(condition,mutex,deadline); int saved_errno=errno;
  if((result==0 || result==ETIMEDOUT) && caller) acquired(mutex,caller);
  observing=0; errno=saved_errno; return result;
}
