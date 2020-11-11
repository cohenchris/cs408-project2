#define _GNU_SOURCE
#include <assert.h> // Use this, it is your friend!
#include <libunwind.h>
#include <stdlib.h>

#include <unistd.h>
#include <sys/syscall.h>
#define gettid() syscall(SYS_gettid)

#include <semaphore.h>

#define UNW_LOCAL_ONLY
#include <libunwind.h>
#include <stdio.h>

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

sem_t g_count_lock;
sem_t g_print_lock;

int g_thread_count = 0;
int STACKTRACE_THREAD_ID = 0;

// Used for interpose_start_routine in order to pass multiple args
typedef struct arg_struct {
  void *(*struct_func) (void *);
  void *struct_arg;
} arg_struct;


////////////////////////////////////////////////////
//////////////////// STACKTRACE ////////////////////
////////////////////////////////////////////////////

void stacktrace() {
    unw_cursor_t cursor;
    unw_context_t context;
    
    // Initialize cursor to current frame for local unwinding.
    unw_getcontext(&context); // Takes a snapshot of the current CPU registers
    unw_init_local(&cursor, &context);  // Initializes the cursor to beginning of 'context' (HANGING RN)

    INFO("Stacktrace: \n");
    fflush(stdout);
    // Unwind frames one by one, going up the frame stack. 
    while (unw_step(&cursor) > 0) {
        unw_word_t offset, pc; 
        unw_get_reg(&cursor, UNW_REG_IP, &pc);
        if (pc == 0) {
            break; 
        }
        INFO("  0x%lx:", pc);
        fflush(stdout);
        char sym[256];
        if (unw_get_proc_name(&cursor, sym, sizeof(sym), &offset) == 0) {
            INFO(" (%s+0x%lx)\n", sym, offset);
            fflush(stdout);
        } else {
            INFO(" -- ERROR: unable to obtain symbol name for this frame\n"); 
            fflush(stdout);
        }
    }
}

////////////////////////////////////////////////////
////////////////////////////////////////////////////

// Thread Management
void *interpose_start_routine(void *argument) {
  // Deconstruct the struct into [ function to execute, arg ]
  struct arg_struct *arguments = argument;
  void *(*start_routine) (void *) = arguments->struct_func;
  void *arg = arguments->struct_arg;

  sem_wait(&g_count_lock);
  int count = g_thread_count++;
  sem_post(&g_count_lock);

  sem_wait(&g_print_lock);
  INFO("THREAD CREATED (%d, %ld)\n", count, gettid());
  fflush(stdout);
  sem_post(&g_print_lock);

  // Execute the function for the thread as normal
  start_routine(arg);

  sem_wait(&g_print_lock);
  INFO("THREAD EXITED (%d, %ld)\n", count, gettid());
  fflush(stdout);
  sem_post(&g_print_lock);
  return NULL;
}

////////////////////////////////////////////////////
////////// BEGINNING OF PTHREAD FUNCTIONS //////////
////////////////////////////////////////////////////

int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                   void *(*start_routine) (void *), void *arg) {
  pthread_create_type orig_create;
  orig_create = (pthread_create_type)dlsym(RTLD_NEXT, "pthread_create");

  if (STACKTRACE_THREAD_ID == gettid()) {
    // If this thread is currently printing the stacktrace, allow it to use the original function.
    return orig_create(thread, attr, start_routine, arg);
  }

  sem_wait(&g_print_lock);
  INFO("CALL pthread_create(%p, %p, %p, %p)\n", thread, attr, start_routine, arg);
  fflush(stdout);
  STACKTRACE_THREAD_ID = gettid();
  stacktrace();
  sem_post(&g_print_lock);

  // Struct for multiple args
  struct arg_struct *args = malloc(sizeof(arg_struct));
  args->struct_func = start_routine;
  args->struct_arg = arg;

  int return_val = orig_create(thread, attr, &interpose_start_routine, (void *)args);

  sem_wait(&g_print_lock);  
  INFO("RETURN pthread_create(%p, %p, %p, %p) = %d\n", thread, attr, start_routine, arg, return_val);
  fflush(stdout);
  sem_post(&g_print_lock);

  return return_val;
}

void pthread_exit(void *retval) {
  pthread_exit_type orig_exit;
  orig_exit = (pthread_exit_type)dlsym(RTLD_NEXT, "pthread_exit");

  if (STACKTRACE_THREAD_ID == gettid()) {
    // If this thread is currently printing the stacktrace, allow it to use the original function.
    orig_exit();
    return;
  }

  sem_wait(&g_print_lock);  
  INFO("CALL pthread_exit(%p)\n", retval);
  fflush(stdout);
  STACKTRACE_THREAD_ID = gettid();
  stacktrace();
  sem_post(&g_print_lock);

  orig_exit(retval);

  sem_wait(&g_print_lock);  
  INFO("THREAD EXITED (%d, %ld)", g_thread_count, gettid());
  fflush(stdout);
  sem_post(&g_print_lock);

  return;
}

int pthread_yield(void) {
  pthread_yield_type orig_yield;
  orig_yield = (pthread_yield_type)dlsym(RTLD_NEXT, "pthread_yield");

  if (STACKTRACE_THREAD_ID == gettid()) {
    // If this thread is currently printing the stacktrace, allow it to use the original function.
    return orig_yield();
  }

  sem_wait(&g_print_lock);
  INFO("CALL pthread_yield()\n");
  fflush(stdout);
  STACKTRACE_THREAD_ID = gettid();
  stacktrace();
  sem_post(&g_print_lock);

  int return_val = orig_yield();

  sem_wait(&g_print_lock);  
  INFO("RETURN pthread_yield() = %d\n", return_val);
  fflush(stdout);
  sem_post(&g_print_lock);

  return return_val;
}

// Condition variables
int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex) {
  pthread_cond_wait_type orig_cond_wait;
  orig_cond_wait = (pthread_cond_wait_type)dlsym(RTLD_NEXT, "pthread_cond_wait");

  if (STACKTRACE_THREAD_ID == gettid()) {
    // If this thread is currently printing the stacktrace, allow it to use the original function.
    return orig_cond_wait(cond, mutex);
  }

  sem_wait(&g_print_lock);  
  INFO("CALL pthread_cond_wait(%p, %p)\n", cond, mutex);
  fflush(stdout);
  STACKTRACE_THREAD_ID = gettid();
  stacktrace();
  sem_post(&g_print_lock);


  int return_val = orig_cond_wait(cond, mutex);

  sem_wait(&g_print_lock);  
  INFO("RETURN pthread_cond_wait(%p, %p) = %d\n", cond, mutex, return_val);
  fflush(stdout);
  sem_post(&g_print_lock);

  return return_val;
}

int pthread_cond_signal(pthread_cond_t *cond) {
  pthread_cond_signal_type orig_cond_signal;
  orig_cond_signal = (pthread_cond_signal_type)dlsym(RTLD_NEXT, "pthread_cond_signal");

  if (STACKTRACE_THREAD_ID == gettid()) {
    // If this thread is currently printing the stacktrace, allow it to use the original function.
    return orig_cond_signal(cond);
  }

  sem_wait(&g_print_lock);
  INFO("CALL pthread_cond_signal(%p)\n", cond);
  fflush(stdout);
  STACKTRACE_THREAD_ID = gettid();
  stacktrace();
  sem_post(&g_print_lock);

  int return_val = orig_cond_signal(cond);

  sem_wait(&g_print_lock);
  INFO("RETURN pthread_cond_signal(%p) = %d\n", cond, return_val);
  fflush(stdout);
  sem_post(&g_print_lock);

  return return_val;
}

int pthread_cond_broadcast(pthread_cond_t *cond) {
  pthread_cond_broadcast_type orig_cond_broadcast;
  orig_cond_broadcast = (pthread_cond_broadcast_type)dlsym(RTLD_NEXT, "pthread_cond_broadcast");

  if (STACKTRACE_THREAD_ID == gettid()) {
    // If this thread is currently printing the stacktrace, allow it to use the original function.
    return orig_cond_broadcast(cond);
  }

  sem_wait(&g_print_lock);
  INFO("CALL pthread_cond_broadcast(%p)\n", cond);
  fflush(stdout);
  STACKTRACE_THREAD_ID = gettid();
  stacktrace();
  sem_post(&g_print_lock);

  int return_val = orig_cond_broadcast(cond);

  sem_wait(&g_print_lock);
  INFO("RETURN pthread_cond_broadcast(%p) = %d\n", cond, return_val);
  fflush(stdout);
  sem_post(&g_print_lock);

  return return_val;
}

// Mutexes
int pthread_mutex_lock(pthread_mutex_t *mutex) {
  pthread_mutex_lock_type orig_mutex_lock;
  orig_mutex_lock = (pthread_mutex_lock_type)dlsym(RTLD_NEXT, "pthread_mutex_lock");

  if (STACKTRACE_THREAD_ID == gettid()) {
    // If this thread is currently printing the stacktrace, allow it to use the original function.
    return orig_mutex_lock(mutex);
  }

  sem_wait(&g_print_lock);
  INFO("CALL pthread_mutex_lock(%p)\n", mutex);
  fflush(stdout);
  STACKTRACE_THREAD_ID = gettid();
  stacktrace();
  sem_post(&g_print_lock);
  
  int return_val = orig_mutex_lock(&mutex);

  sem_wait(&g_print_lock);
  INFO("RETURN pthread_mutex_lock(%p) = %d\n", mutex, return_val);
  fflush(stdout);
  sem_post(&g_print_lock);

  return return_val;
}

int pthread_mutex_unlock(pthread_mutex_t *mutex) {
  pthread_mutex_unlock_type orig_mutex_unlock = NULL;
  orig_mutex_unlock = (pthread_mutex_unlock_type)dlsym(RTLD_NEXT, "pthread_mutex_unlock");

  if (STACKTRACE_THREAD_ID == gettid()) {
    // If this thread is currently printing the stacktrace, allow it to use the original function.
    return orig_mutex_unlock(mutex);
  }

  sem_wait(&g_print_lock);
  INFO("CALL pthread_mutex_unlock(%p)\n", mutex);
  fflush(stdout);
  STACKTRACE_THREAD_ID = gettid();
  stacktrace();
  sem_post(&g_print_lock);

  int return_val = orig_mutex_unlock(&mutex);

  sem_wait(&g_print_lock);
  INFO("RETURN pthread_mutex_unlock(%p) = %d\n", mutex, return_val);
  fflush(stdout);
  sem_post(&g_print_lock);

  return return_val;
}

int pthread_mutex_trylock(pthread_mutex_t *mutex) {
  pthread_mutex_trylock_type orig_mutex_trylock;
  orig_mutex_trylock = (pthread_mutex_trylock_type)dlsym(RTLD_NEXT, "pthread_mutex_trylock");

  if (STACKTRACE_THREAD_ID == gettid()) {
    // If this thread is currently printing the stacktrace, allow it to use the original function.
    return pthread_mutex_trylock(mutex);
  }

  sem_wait(&g_print_lock);
  INFO("CALL pthread_mutex_trylock(%p)\n", mutex);
  fflush(stdout);
  STACKTRACE_THREAD_ID = gettid();
  stacktrace();
  sem_post(&g_print_lock);

  int return_val = orig_mutex_trylock(&mutex);

  sem_wait(&g_print_lock);
  INFO("RETURN pthread_mutex_trylock(%p) = %d\n", mutex, return_val);
  fflush(stdout);
  sem_post(&g_print_lock);

  return return_val;
}

// This will get called at the start of the target programs main function
static __attribute__((constructor (200))) void init_testlib(void) {
  // You can initialize stuff here
  INFO("Testlib loaded!\n");
  INFO("Stacktraces is %i\n", get_stacktraces());
  INFO("Algorithm ID is %i\n", get_algorithm_ID());
  INFO("Seed is %i\n",(int) get_seed());

  sem_init(&g_print_lock, 0, 1);
  sem_init(&g_count_lock, 0, 1);
}