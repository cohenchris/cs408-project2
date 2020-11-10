#define _GNU_SOURCE
#include <assert.h> // Use this, it is your friend!
#include <libunwind.h>
#include <stdlib.h>

#include "testlib.h"
#include "utils.h"

#include <dlfcn.h>
typedef int (*pthread_create_type)();
typedef void (*pthread_exit_type)();
typedef int (*pthread_yield_type)();
typedef int (*pthread_cond_wait_type)();
typedef int (*pthread_cond_signal_type)();
typedef int (*pthread_cond_broadcast_type)();
typedef int (*pthread_mutex_lock_type)();
typedef int (*pthread_mutex_unlock_type)();
typedef int (*pthread_mutex_trylock_type)();

// Thread Management
int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                   void *(*start_routine) (void *), void *arg) {

  // TODO: Implement;

  INFO("Called pthread_create\n");

  //This is an example of calling the original function. You may need to change this in your implementation
  pthread_create_type orig_create;
  orig_create = (pthread_create_type)dlsym(RTLD_NEXT, "pthread_create");

  return orig_create(thread, attr, start_routine, arg);

}

void pthread_exit(void *retval) {
  INFO("Called pthread_exit");
  
  pthread_exit_type orig_exit;
  orig_exit = (pthread_exit_type)dlsym(RTLD_NEXT, "pthread_exit");
}

int pthread_yield(void) {
  INFO("Called pthread_yield");

  pthread_yield_type orig_yield;
  orig_yield = (pthread_yield_type)dlsym(RTLD_NEXT, "pthread_yield");
  return 0;
}

// Condition variables
int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex) {
  INFO("Called pthread_cond_wait");
  
  pthread_cond_wait_type orig_cond_wait;
  orig_cond_wait = (pthread_cond_wait_type)dlsym(RTLD_NEXT, "pthread_cond_wait");
  return 0;
}

int pthread_cond_signal(pthread_cond_t *cond) {
  INFO("Called pthread_cond_signal");
  
  pthread_cond_signal_type orig_cond_signal;
  orig_cond_signal = (pthread_cond_signal_type)dlsym(RTLD_NEXT, "pthread_cond_signal");
  return 0;
}

int pthread_cond_broadcast(pthread_cond_t *cond) {
  INFO("Called pthread_cond_broadcast");
  
  pthread_cond_broadcast_type orig_cond_broadcast;
  orig_cond_broadcast = (pthread_cond_broadcast_type)dlsym(RTLD_NEXT, "pthread_cond_broadcast");
  return 0;
}

// Mutexes
int pthread_mutex_lock(pthread_mutex_t *mutex) {
  INFO("Called pthread_mutex_lock");

  pthread_mutex_lock_type orig_mutex_lock;
  orig_mutex_lock = (pthread_mutex_lock_type)dlsym(RTLD_NEXT, "pthread_mutex_lock");
  return 0;
}

int pthread_mutex_unlock(pthread_mutex_t *mutex) {
  INFO("Called pthread_mutex_unlock");
  
  pthread_mutex_unlock_type orig_mutex_unlock;
  orig_mutex_unlock = (pthread_mutex_unlock_type)dlsym(RTLD_NEXT, "pthread_mutex_unlock");
  return 0;
}

int pthread_mutex_trylock(pthread_mutex_t *mutex) {
  INFO("Called pthread_mutex_trylock");
  
  pthread_mutex_trylock_type orig_mutex_trylock;
  orig_mutex_trylock = (pthread_mutex_trylock_type)dlsym(RTLD_NEXT, "pthread_mutex_trylock");
  return 0;
}

// This will get called at the start of the target programs main function
static __attribute__((constructor (200))) void init_testlib(void) {
  // You can initialize stuff here
  INFO("Testlib loaded!\n");
  INFO("Stacktraces is %i\n", get_stacktraces());
  INFO("Algorithm ID is %i\n", get_algorithm_ID());
  INFO("Seed is %i\n",(int) get_seed());
}
