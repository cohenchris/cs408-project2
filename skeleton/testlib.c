#define _GNU_SOURCE
#include <assert.h> // Use this, it is your friend!
#include <libunwind.h>
#include <stdlib.h>

#include <unistd.h>
#include <sys/syscall.h>
#define gettid() syscall(SYS_gettid)

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

pthread_mutex_t count_lock;
pthread_mutex_t print_lock;

int g_thread_count = 0;

typedef struct arg_struct {
  void *(*struct_func) (void *);
  void *struct_arg;
} arg_struct;

/*
 * locking function to lock the thread count and any print statements
 */
int lock(pthread_mutex_t *mutex) {
  // TODO - implement
  return 0;
}

/*
 *  unlocking function to unlock the thread count and any print statements
 */
int unlock(pthread_mutex_t *mutex) {
  // TODO - implement
  return 0;
}


// Thread Management
void *interpose_start_routine(void *argument) {
  // Deconstruct the struct
  struct arg_struct *arguments = argument;
  void *(*start_routine) (void *) = arguments->struct_func;
  void *arg = arguments->struct_arg;

  lock(&count_lock);
  int count = g_thread_count++;
  unlock(&count_lock);

  lock(&print_lock);
  INFO("THREAD CREATED (%d, %ld)\n", count, gettid());
  fflush(stdout);
  unlock(&print_lock);

  // Execute the function for the thread as normal
  start_routine(arg);

  lock(&print_lock);
  INFO("THREAD EXITED (%d, %ld)\n", count, gettid());
  fflush(stdout);
  unlock(&print_lock);
  return NULL;
}

int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                   void *(*start_routine) (void *), void *arg) {
  lock(&print_lock);
  INFO("CALL pthread_create(%p, %p, %p, %p)\n", thread, attr, start_routine, arg);
  fflush(stdout);
  unlock(&print_lock);

  pthread_create_type orig_create;
  orig_create = (pthread_create_type)dlsym(RTLD_NEXT, "pthread_create");

  // Struct for multiple args
  struct arg_struct *args = malloc(sizeof(arg_struct));
  args->struct_func = start_routine;
  args->struct_arg = arg;

  int return_val = orig_create(thread, attr, &interpose_start_routine, (void *)args);

  lock(&print_lock);  
  INFO("RETURN pthread_create(%p, %p, %p, %p) = %d\n", thread, attr, start_routine, arg, return_val);
  fflush(stdout);
  unlock(&print_lock);

  return return_val;
}

void pthread_exit(void *retval) {
  lock(&print_lock);  
  INFO("CALL pthread_exit(%p)\n", retval);
  fflush(stdout);
  unlock(&print_lock);
  
  pthread_exit_type orig_exit;
  orig_exit = (pthread_exit_type)dlsym(RTLD_NEXT, "pthread_exit");

  orig_exit(retval);

  lock(&print_lock);  
  INFO("THREAD EXITED (%d, %ld)", g_thread_count, gettid());
  fflush(stdout);
  unlock(&print_lock);

  return;
}

int pthread_yield(void) {
  lock(&print_lock);
  INFO("CALL pthread_yield()\n");
  fflush(stdout);
  unlock(&print_lock);

  pthread_yield_type orig_yield;
  orig_yield = (pthread_yield_type)dlsym(RTLD_NEXT, "pthread_yield");

  int return_val = orig_yield();

  lock(&print_lock);  
  INFO("RETURN pthread_yield() = %d\n", return_val);
  fflush(stdout);
  unlock(&print_lock);

  return return_val;
}

// Condition variables
int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex) {
  lock(&print_lock);  
  INFO("CALL pthread_cond_wait(%p, %p)\n", cond, mutex);
  fflush(stdout);
  unlock(&print_lock);
  
  pthread_cond_wait_type orig_cond_wait;
  orig_cond_wait = (pthread_cond_wait_type)dlsym(RTLD_NEXT, "pthread_cond_wait");

  int return_val = orig_cond_wait(cond, mutex);

  lock(&print_lock);  
  INFO("RETURN pthread_cond_wait(%p, %p) = %d\n", cond, mutex, return_val);
  fflush(stdout);
  unlock(&print_lock);

  return return_val;
}

int pthread_cond_signal(pthread_cond_t *cond) {
  lock(&print_lock);
  INFO("CALL pthread_cond_signal(%p)\n", cond);
  fflush(stdout);
  unlock(&print_lock);
  
  pthread_cond_signal_type orig_cond_signal;
  orig_cond_signal = (pthread_cond_signal_type)dlsym(RTLD_NEXT, "pthread_cond_signal");

  int return_val = orig_cond_signal(cond);

  lock(&print_lock);
  INFO("RETURN pthread_cond_signal(%p) = %d\n", cond, return_val);
  fflush(stdout);
  unlock(&print_lock);

  return return_val;
}

int pthread_cond_broadcast(pthread_cond_t *cond) {
  lock(&print_lock);
  INFO("CALL pthread_cond_broadcast(%p)\n", cond);
  fflush(stdout);
  unlock(&print_lock);
  
  pthread_cond_broadcast_type orig_cond_broadcast;
  orig_cond_broadcast = (pthread_cond_broadcast_type)dlsym(RTLD_NEXT, "pthread_cond_broadcast");

  int return_val = orig_cond_broadcast(cond);

  lock(&print_lock);
  INFO("RETURN pthread_cond_broadcast(%p) = %d\n", cond, return_val);
  fflush(stdout);
  unlock(&print_lock);

  return return_val;
}

// Mutexes
int pthread_mutex_lock(pthread_mutex_t *mutex) {
  lock(&print_lock);
  INFO("CALL pthread_mutex_lock(%p)\n", mutex);
  fflush(stdout);
  unlock(&print_lock);

  pthread_mutex_lock_type orig_mutex_lock;
  orig_mutex_lock = (pthread_mutex_lock_type)dlsym(RTLD_NEXT, "pthread_mutex_lock");
  
  int return_val = orig_mutex_lock(&mutex);

  lock(&print_lock);
  INFO("RETURN pthread_mutex_lock(%p) = %d\n", mutex, return_val);
  fflush(stdout);
  unlock(&print_lock);

  return return_val;
}

int pthread_mutex_unlock(pthread_mutex_t *mutex) {
  lock(&print_lock);
  INFO("CALL pthread_mutex_unlock(%p)\n", mutex);
  fflush(stdout);
  unlock(&print_lock);

  pthread_mutex_unlock_type orig_mutex_unlock = NULL;
  orig_mutex_unlock = (pthread_mutex_unlock_type)dlsym(RTLD_NEXT, "pthread_mutex_unlock");

  int return_val = orig_mutex_unlock(&mutex);

  lock(&print_lock);
  INFO("RETURN pthread_mutex_unlock(%p) = %d\n", mutex, return_val);
  fflush(stdout);
  unlock(&print_lock);

  return return_val;
}

int pthread_mutex_trylock(pthread_mutex_t *mutex) {
  lock(&print_lock);
  INFO("CALL pthread_mutex_trylock(%p)\n", mutex);
  fflush(stdout);
  unlock(&print_lock);

  pthread_mutex_trylock_type orig_mutex_trylock;
  orig_mutex_trylock = (pthread_mutex_trylock_type)dlsym(RTLD_NEXT, "pthread_mutex_trylock");

  int return_val = orig_mutex_trylock(&mutex);

  lock(&print_lock);
  INFO("RETURN pthread_mutex_trylock(%p) = %d\n", mutex, return_val);
  fflush(stdout);
  unlock(&print_lock);

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
