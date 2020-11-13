#define _GNU_SOURCE
#include <assert.h> // Use this, it is your friend!
#include <libunwind.h>
#include <stdlib.h>

#include <unistd.h>
#include <sys/syscall.h>
#define gettid() syscall(SYS_gettid)

#include <semaphore.h>
#include <string.h>

#define UNW_LOCAL_ONLY
#include <libunwind.h>
#include <stdio.h>

#include <stdbool.h>

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

// Total number of threads that are active
int g_thread_count = 0;
int STACKTRACE_THREAD_ID = -1;

// Keeps track of thread counts - index is gettid()
// We were told that there will only be up to 64 threads ever run
long int g_thread_ids[64] = { 0 };

// Used for interpose_start_routine in order to pass multiple args
typedef struct arg_struct {
  void *(*struct_func) (void *);
  void *struct_arg;
} arg_struct;

// Needed for PCT
#define DEBUG true
#define MAX_THREAD 64

#define PCT_THREAD_CALL 0
#define PCT_THREAD_START 1
#define PCT_THREAD_TERMINATE 2
#define PCT_THREAD_DO_NOTHING 3

struct thread_struct {
  pthread_t thread_id;
  bool active;
};

// g_runnable index of the current thread that is running
int g_current_thread = -1;

// Make array of threads with boolean indicating if they are runnable or not
// (Simpler data structures and a linked list)
struct thread_struct *g_runnable = NULL;

// Array of semaphores for each thread mapped to the g_runnable array
sem_t *g_semaphores = NULL;

// Mutex lock used in the PCT algorithm
pthread_mutex_lock_type g_orig_mutex_lock;
pthread_mutex_unlock_type g_orig_mutex_unlock;

////////////////////////////////////////////////////
///////////////////// HELPERS //////////////////////
////////////////////////////////////////////////////

// Used to find the thread's thread number (assigned via g_thread_count)
int find_thread_number(long int tid) {
  int thread_ids_size = sizeof(g_thread_ids) / sizeof(g_thread_ids[0]);
  for (int i = 0; i < thread_ids_size; i++) {
    if (g_thread_ids[i] == tid) {
      return i;
    }
  }
  // if it gets here, it has failed (should never fail)
  return -1;
}

////////////////////////////////////////////////////
//////////////////// STACKTRACE ////////////////////
////////////////////////////////////////////////////

// String array of functions to omit from stack trace
char omit_functions[3][25] = {
  "interpose_start_routine",
  "omit",
  "stacktrace"
};

bool omit(char * func) {
  int arr_size = sizeof(omit_functions) / sizeof(omit_functions)[0];
  for (int i = 0; i < arr_size; i++) {
    if (strcmp((char *)func, (char *)omit_functions[i]) == 0) {
      return true;
    }
  }
  return false;
}

void stacktrace() {
  if (get_stacktraces()) {
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
        char sym[256];
        if (unw_get_proc_name(&cursor, sym, sizeof(sym), &offset) == 0) {
          if (!omit(sym)) {
            // Makes sure that ONLY stack trace for target program exists.
            INFO("  0x%lx: (%s+0x%lx)\n", pc, sym, offset);
            fflush(stdout);
          }
        } else {
            INFO("  0x%lx: -- ERROR: unable to obtain symbol name for this frame\n", pc); 
            fflush(stdout);
        }
    }
  }
  STACKTRACE_THREAD_ID = -1;
}

////////////////////////////////////////////////////
//////////////////// PCT ///////////////////////////
////////////////////////////////////////////////////

void run_highest_priority() {
  sem_t PCT_lock;
  sem_init(&PCT_lock, 0, 1);
  sem_wait(&PCT_lock);

  // Find the highest priority thread available to be active
  int highest_priorty = -1;
  int thread_index = -1;

  for (int i = 0; i < MAX_THREAD; i++) {
    if ((g_runnable[i].active == true) && (get_priorities()[i] > highest_priorty)) {
      thread_index = i;
      highest_priorty = get_priorities()[thread_index];
    } 
  }

  if (thread_index != g_current_thread) {
    // Need ot suspend g_current_thread
    sem_wait(&(g_semaphores[g_current_thread]));
    // Unblock the highest priority thread
    g_current_thread = thread_index;
    sem_post(&(g_semaphores[g_current_thread]));
  }
  sem_post(&PCT_lock);
  return;
}

int find_next_available_thread() {
  sem_t PCT_lock;
  sem_init(&PCT_lock, 0, 1);
  sem_wait(&PCT_lock);

  // Find the highest priority thread available to be active
  int highest_priorty = -1;
  int thread_index = -1;

  for (int i = 0; i < MAX_THREAD; i++) {
    if ((g_runnable[i].active == true) && (get_priorities()[i] > highest_priorty)) {
      thread_index = i;
      highest_priorty = get_priorities()[thread_index];
    } 
  }

  sem_post(&PCT_lock);
  return thread_index;
}
void PCT(int pct_thread_state) {
  sem_t PCT_lock;
  sem_init(&PCT_lock, 0, 1);
  sem_wait(&PCT_lock);
  int new_thread;

  if (pct_thread_state == PCT_THREAD_START) {
    // Add a thread to the runnable list
    // find a priority for this thread
    // Find highest priority where the thread is not
    // active yet
    new_thread = find_next_available_thread();

    // Store the thread id in g_runnable
    g_runnable[new_thread].thread_id = gettid();
    g_runnable[new_thread].active = true;

    if (g_current_thread == -1) {
      // first thread to run
      g_current_thread = new_thread;
    } else {
      run_highest_priority();
    }
  } else if (pct_thread_state == PCT_THREAD_TERMINATE) {
    // pthread_exit or termination of thread
    // set the current thread to be the next available thread to run
    // Mark the current thread as not active in g_runnable
    g_runnable[g_current_thread].active = false;
    g_thread_count--;
    if (g_thread_count > 0) {
      run_highest_priority();
    } else {
      g_current_thread = -1;
    }
  } else if (pct_thread_state == PCT_THREAD_CALL) {
      run_highest_priority();
  }
  // else if pct_thread_state == PCT_THREAD_DO_NOTHING

  sem_post(&PCT_lock);
  return;
}

////////////////////////////////////////////////////
//////////////////// ALGORITHMS ////////////////////
////////////////////////////////////////////////////

void run_algorithm(int pct_thread_state) {
  //return;
  int algorithm = get_algorithm_ID();
  if (algorithm == kAlgorithmRandom) {
    // run random scheduling algorithm
    rsleep();
  }
  else if (algorithm == kAlgorithmPCT) {
    // run pct scheduling algorithm
    PCT(pct_thread_state);
  }
  // else algorithm = none, do nothing special
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
  //int count = ++g_thread_count;
  g_thread_count++;
  // save the count for this thread id (for later use)
  //g_thread_ids[count] = gettid();
  g_thread_ids[g_thread_count] = gettid();
  sem_post(&g_count_lock);

  run_algorithm(PCT_THREAD_START);

  sem_wait(&g_print_lock);
  //INFO("THREAD CREATED (%d, %ld)\n", count, gettid());
  INFO("THREAD CREATED (%d, %ld)\n", g_thread_count, gettid());
  fflush(stdout);
  sem_post(&g_print_lock);
  
  // Execute the function for the thread as normal
  void *return_val = start_routine(arg);

  run_algorithm(PCT_THREAD_TERMINATE);

  sem_wait(&g_print_lock);
  //INFO("THREAD EXITED (%d, %ld)\n", count, gettid());
  INFO("THREAD EXITED (%d, %ld)\n", g_thread_count, gettid());
  fflush(stdout);
  sem_post(&g_print_lock);
  return return_val;
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

  // Struct for multiple args
  struct arg_struct *args = malloc(sizeof(arg_struct));
  args->struct_func = start_routine;
  args->struct_arg = arg;

  run_algorithm(PCT_THREAD_DO_NOTHING);

  sem_wait(&g_print_lock);
  INFO("CALL pthread_create(%p, %p, %p, %p)\n", thread, attr, start_routine, arg);
  fflush(stdout);
  STACKTRACE_THREAD_ID = gettid();
  stacktrace();
  sem_post(&g_print_lock);

  int return_val = orig_create(thread, attr, &interpose_start_routine, (void *)args);

  run_algorithm(PCT_THREAD_DO_NOTHING);

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

  run_algorithm(PCT_THREAD_DO_NOTHING);

  sem_wait(&g_print_lock);  
  INFO("CALL pthread_exit(%p)\n", retval);
  fflush(stdout);
  STACKTRACE_THREAD_ID = gettid();
  stacktrace();
  sem_post(&g_print_lock);

  run_algorithm(PCT_THREAD_TERMINATE);

  sem_wait(&g_print_lock);
  int thread_number = find_thread_number(gettid());
  INFO("THREAD EXITED (%d, %ld)\n", thread_number, gettid());
  fflush(stdout);
  sem_post(&g_print_lock);

  orig_exit(retval);
  return;
}

int pthread_yield(void) {
  pthread_yield_type orig_yield;
  orig_yield = (pthread_yield_type)dlsym(RTLD_NEXT, "pthread_yield");

  if (STACKTRACE_THREAD_ID == gettid()) {
    // If this thread is currently printing the stacktrace, allow it to use the original function.
    return orig_yield();
  }

  run_algorithm(PCT_THREAD_CALL);

  sem_wait(&g_print_lock);
  INFO("CALL pthread_yield()\n");
  fflush(stdout);
  STACKTRACE_THREAD_ID = gettid();
  stacktrace();
  sem_post(&g_print_lock);

  int return_val = orig_yield();

  run_algorithm(PCT_THREAD_DO_NOTHING);

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

  run_algorithm(PCT_THREAD_CALL);

  sem_wait(&g_print_lock);  
  INFO("CALL pthread_cond_wait(%p, %p)\n", cond, mutex);
  fflush(stdout);
  STACKTRACE_THREAD_ID = gettid();
  stacktrace();
  sem_post(&g_print_lock);

  int return_val = orig_cond_wait(cond, mutex);

  run_algorithm(PCT_THREAD_DO_NOTHING);

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

  run_algorithm(PCT_THREAD_CALL);

  sem_wait(&g_print_lock);
  INFO("CALL pthread_cond_signal(%p)\n", cond);
  fflush(stdout);
  STACKTRACE_THREAD_ID = gettid();
  stacktrace();
  sem_post(&g_print_lock);

  int return_val = orig_cond_signal(cond);

  run_algorithm(PCT_THREAD_DO_NOTHING);

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
  
  run_algorithm(PCT_THREAD_CALL);

  sem_wait(&g_print_lock);
  INFO("CALL pthread_cond_broadcast(%p)\n", cond);
  fflush(stdout);
  STACKTRACE_THREAD_ID = gettid();
  stacktrace();
  sem_post(&g_print_lock);

  int return_val = orig_cond_broadcast(cond);

  run_algorithm(PCT_THREAD_DO_NOTHING);

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
  
  run_algorithm(PCT_THREAD_CALL);

  sem_wait(&g_print_lock);
  INFO("CALL pthread_mutex_lock(%p)\n", mutex);
  fflush(stdout);
  STACKTRACE_THREAD_ID = gettid();
  stacktrace();
  sem_post(&g_print_lock);
  
  int return_val = orig_mutex_lock(mutex);

  run_algorithm(PCT_THREAD_DO_NOTHING);

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
  
  run_algorithm(PCT_THREAD_CALL);

  sem_wait(&g_print_lock);
  INFO("CALL pthread_mutex_unlock(%p)\n", mutex);
  fflush(stdout);
  STACKTRACE_THREAD_ID = gettid();
  stacktrace();
  sem_post(&g_print_lock);

  int return_val = orig_mutex_unlock(mutex);

  run_algorithm(PCT_THREAD_DO_NOTHING);

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
    return orig_mutex_trylock(mutex);
  }

  run_algorithm(PCT_THREAD_CALL);

  sem_wait(&g_print_lock);
  INFO("CALL pthread_mutex_trylock(%p)\n", mutex);
  fflush(stdout);
  STACKTRACE_THREAD_ID = gettid();
  stacktrace();
  sem_post(&g_print_lock);

  int return_val = orig_mutex_trylock(mutex);

  run_algorithm(PCT_THREAD_DO_NOTHING);

  sem_wait(&g_print_lock);
  INFO("RETURN pthread_mutex_trylock(%p) = %d\n", mutex, return_val);
  fflush(stdout);
  sem_post(&g_print_lock);

  return return_val;
}

// This will get called at the start of the target programs main function
static __attribute__((constructor (200))) void init_testlib(void) {
  // Used in PCT
  g_orig_mutex_lock = (pthread_mutex_lock_type)dlsym(RTLD_NEXT, "pthread_mutex_lock");
  g_orig_mutex_unlock = (pthread_mutex_unlock_type)dlsym(RTLD_NEXT, "pthread_mutex_unlock");
  
  pthread_mutex_t init_lock;
  g_orig_mutex_lock(&init_lock);

  // You can initialize stuff here
  INFO("Testlib loaded!\n");
  fflush(stdout);
  INFO("Stacktraces is %i\n", get_stacktraces());
  fflush(stdout);
  INFO("Algorithm ID is %i\n", get_algorithm_ID());
  fflush(stdout);
  INFO("Seed is %i\n",(int) get_seed());
  fflush(stdout);

  sem_init(&g_print_lock, 0, 1);
  sem_init(&g_count_lock, 0, 1);
  
  // Needed for PCT
  g_runnable = (struct thread_struct*) malloc(MAX_THREAD * sizeof(struct thread_struct));
  g_semaphores = (sem_t *) malloc(MAX_THREAD * sizeof(sem_t));
  for (int i = 0; i < MAX_THREAD; i++) {
    g_runnable[i].active = false;
    sem_init(&g_semaphores[i], 0, 1);
  }
  if (DEBUG) {
    for (int i = 0; i < MAX_THREAD; i++) {
      INFO("priorities[%d] = %d \n", i, get_priorities()[i]);
      fflush(stdout);
    }
  }
  g_orig_mutex_unlock(&init_lock);
}
