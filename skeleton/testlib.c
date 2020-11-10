#define _GNU_SOURCE
#include <assert.h> // Use this, it is your friend!
#include <libunwind.h>
#include <stdlib.h>

#include "testlib.h"
#include "utils.h"

#include <dlfcn.h>
typedef void (*start_routine_type)();
typedef int (*pthread_create_type)();
typedef void (*pthread_exit_type)();
typedef int (*pthread_yield_type)();
typedef int (*pthread_cond_wait_type)();
typedef int (*pthread_cond_signal_type)();
typedef int (*pthread_cond_broadcast_type)();
typedef int (*pthread_mutex_lock_type)();
typedef int (*pthread_mutex_unlock_type)();
typedef int (*pthread_mutex_trylock_type)();

int g_thread_count = 0;

// Thread Management
void interpose_start_routine(void *(*start_routine) (void *), void *arg) {
  // TODO: lock output
  g_thread_count++;
  
  INFO("THREAD CREATED (%d, %d)", g_thread_count, gettid());
  start_routine(arg);
  INFO("THREAD EXITED (%d, %d)", g_thread_count, gettid());
  return;
}

int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                   void *(*start_routine) (void *), void *arg) {
  INFO("CALL pthread_create(%x, %x, %x, %x)\n", thread, attr, start_routine, arg);

  pthread_create_type orig_create;
  orig_create = (pthread_create_type)dlsym(RTLD_NEXT, "pthread_create");

  int return_val = orig_create(thread, attr, interpose_start_routine, arg);
  INFO("RETURN pthread_create(%x, %x, %x, %x) = %d\n", thread, attr, start_routine, arg, return_val);
  return return_val;
}

void pthread_exit(void *retval) {
  INFO("CALL pthread_exit(%x)\n", retval);
  
  pthread_exit_type orig_exit;
  orig_exit = (pthread_exit_type)dlsym(RTLD_NEXT, "pthread_exit");

  // TODO: Lock output
  INFO("THREAD EXITED (%d, %d)", g_thread_count, gettid());
  return;
}

int pthread_yield(void) {
  INFO("CALL pthread_yield()\n");

  pthread_yield_type orig_yield;
  orig_yield = (pthread_yield_type)dlsym(RTLD_NEXT, "pthread_yield");

  int return_val = orig_yield();
  INFO("RETURN pthread_yield() = %d\n", return_val);
  return return_val;
}

// Condition variables
int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex) {
  INFO("CALL pthread_cond_wait(%x, %x)\n", cond, mutex);
  
  pthread_cond_wait_type orig_cond_wait;
  orig_cond_wait = (pthread_cond_wait_type)dlsym(RTLD_NEXT, "pthread_cond_wait");

  int return_val = orig_cond_wait(cond, mutex);
  INFO("RETURN pthread_cond_wait(%x, %x) = %d\n", cond, mutex, return_val);
  return return_val;
}

int pthread_cond_signal(pthread_cond_t *cond) {
  INFO("CALL pthread_cond_signal(%x)\n", cond);
  
  pthread_cond_signal_type orig_cond_signal;
  orig_cond_signal = (pthread_cond_signal_type)dlsym(RTLD_NEXT, "pthread_cond_signal");

  int return_val = orig_cond_signal(cond);
  INFO("RETURN pthread_cond_signal(%x) = %d\n", cond, return_val);
  return return_val;
}

int pthread_cond_broadcast(pthread_cond_t *cond) {
  INFO("CALL pthread_cond_broadcast(%x)\n", cond);
  
  pthread_cond_broadcast_type orig_cond_broadcast;
  orig_cond_broadcast = (pthread_cond_broadcast_type)dlsym(RTLD_NEXT, "pthread_cond_broadcast");

  int return_val = orig_cond_broadcast(cond);
  INFO("RETURN pthread_cond_broadcast(%x) = %d\n", cond, return_val);
  return return_val;
}

// Mutexes
int pthread_mutex_lock(pthread_mutex_t *mutex) {
  INFO("CALL pthread_mutex_lock(%x)\n", mutex);

  pthread_mutex_lock_type orig_mutex_lock;
  orig_mutex_lock = (pthread_mutex_lock_type)dlsym(RTLD_NEXT, "pthread_mutex_lock");

  int return_val = orig_mutex_lock(mutex);
  INFO("RETURN pthread_mutex_lock(%x) = %d\n", mutex), return_val;
  return return_val;
}

int pthread_mutex_unlock(pthread_mutex_t *mutex) {
  INFO("CALL pthread_mutex_unlock(%x)\n", mutex);
  
  pthread_mutex_unlock_type orig_mutex_unlock;
  orig_mutex_unlock = (pthread_mutex_unlock_type)dlsym(RTLD_NEXT, "pthread_mutex_unlock");

  int return_val = orig_mutex_unlock(mutex);
  INFO("RETURN pthread_mutex_unlock(%x) = %d\n", mutex, return_val);
  return return_val;
}

int pthread_mutex_trylock(pthread_mutex_t *mutex) {
  INFO("CALL pthread_mutex_trylock(%x)\n", mutex);
  
  pthread_mutex_trylock_type orig_mutex_trylock;
  orig_mutex_trylock = (pthread_mutex_trylock_type)dlsym(RTLD_NEXT, "pthread_mutex_trylock");

  int return_val = orig_mutex_trylock(mutex);
  INFO("RETURN pthread_mutex_trylock(%x) = %d\n", mutex, return_val);
  return return_val;
}

// This will get called at the start of the target programs main function
static __attribute__((constructor (200))) void init_testlib(void) {
  // You can initialize stuff here
  INFO("Testlib loaded!\n");
  INFO("Stacktraces is %i\n", get_stacktraces());
  INFO("Algorithm ID is %i\n", get_algorithm_ID());
  INFO("Seed is %i\n",(int) get_seed());
}
