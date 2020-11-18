#define _GNU_SOURCE
#include <assert.h>
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
#include <dlfcn.h>
#include <stdbool.h>

#include "testlib.h"
#include "utils.h"

#define DEBUG true
#define MAX_THREADS 64
#define MAX_MUTEXES 64




// Original Functions
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

pthread_create_type orig_create;
pthread_exit_type orig_exit;
pthread_yield_type orig_yield;
pthread_cond_wait_type orig_cond_wait;
pthread_cond_signal_type orig_cond_signal;
pthread_cond_broadcast_type orig_cond_broadcast;
pthread_mutex_lock_type orig_mutex_lock;
pthread_mutex_unlock_type orig_mutex_unlock;
pthread_mutex_trylock_type orig_mutex_trylock;


// Semaphores
sem_t g_count_lock;
sem_t g_print_lock;
sem_t g_cond_lock;
sem_t g_queue_lock;

struct thread_struct {
  pthread_t thread_id;  // Proccess ID for current thread
  int state;            // thread state (running, blocked, etc)
  int index;            // index in g_threads
};

// Total number of threads that are active
int g_thread_count = 0;
int STACKTRACE_THREAD_ID = -1;

// Keeps track of thread counts
// We were told that there will only be up to 64 threads ever run
long int g_thread_ids[MAX_THREADS] = { 0 };


// Used for interpose_start_routine in order to pass multiple args
typedef struct arg_struct {
  void *(*struct_func) (void *);
  void *struct_arg;
} arg_struct;



// ------------ PTHREAD_CONDITION VARIABLES ------------

// A list of all condition variables active, based on g_thread_count
pthread_cond_t *cond_vars[MAX_THREADS] = { 0 };
int num_cond_vars = 0;
int queue_rear = 0;

// A queue for each condition variable active. The first index corresponds to the cond_vars array (cond_vars[5]'s queue will be at cond_vars_queue[5]).
struct thread_struct **cond_vars_queue;
// empty thread struct for comparison
struct thread_struct EMPTY_THREAD_STRUCT;






//////////////////////////////////////////////////////////////////////
////////////////////////////// HELPERS ///////////////////////////////
//////////////////////////////////////////////////////////////////////
/*
 * Used to find the thread's thread number (assigned via g_thread_count)
 */
int find_thread_number(long int tid) {
  int thread_ids_size = sizeof(g_thread_ids) / sizeof(g_thread_ids[0]);
  for (int i = 0; i < thread_ids_size; i++) {
    if (g_thread_ids[i] == tid) {
      return i;
    }
  }
  return -1;
}

/*
 * Runs either Random, PCT or no algorithm at all.
 * Depends on the arguments passed to the framework.
 */
void run_scheduling_algorithm() {
  int algorithm = get_algorithm_ID();
  if (algorithm == kAlgorithmRandom) {
    // run random scheduling algorithm
    rsleep();
  }
  else if (algorithm == kAlgorithmPCT) {
    // run pct scheduling algorithm
  }
  // else algorithm = none, do nothing special
}

/*
 * Compares thread_struct for PCT against a 'NULL' struct
 */
bool compare_thread_structs(struct thread_struct *t1, struct thread_struct *t2) {
  if ((t1->thread_id == t2->thread_id) &&
      (t1->state == t2->state) &&
      (t1->index == t2->index)) {
    return true;
  }
  return false;
}

//////////////////////////////////////////////////////////////////////
//////////////////////////// STACKTRACE //////////////////////////////
//////////////////////////////////////////////////////////////////////
// String array of functions to omit from stack trace
char omit_functions[4][25] = {
  "interpose_start_routine",
  "omit",
  "stacktrace",
  "find_thread_number"
};

/*
 * Returns true if 'func' should be omitted from stacktrace.
 */
bool omit(char * func) {
  int arr_size = sizeof(omit_functions) / sizeof(omit_functions)[0];
  for (int i = 0; i < arr_size; i++) {
    if (strcmp((char *)func, (char *)omit_functions[i]) == 0) {
      return true;
    }
  }
  return false;
}

/*
 * Prints the stacktrace. Code from the PDF handout for this project.
 */
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


//////////////////////////////////////////////////////////////////////
/////////////////////// THREADING FUNCTIONS //////////////////////////
//////////////////////////////////////////////////////////////////////
/*
 * Wrapper for starting the actual function meant to execute in the thread.
 * Allows for logging and other scheduling to be done before the thread starts.
 */
void *interpose_start_routine(void *argument) {
  // Deconstruct the struct into [ function to execute, arg ]
  sem_wait(&g_count_lock);
  struct arg_struct *arguments = argument;
  void *(*start_routine) (void *) = arguments->struct_func;
  void *arg = arguments->struct_arg;
  // Tell which semaphore this thread should wait on
  sem_post(&g_count_lock);

  run_scheduling_algorithm();

  sem_wait(&g_count_lock);
  g_thread_count++;
  g_thread_ids[g_thread_count] = gettid();
  sem_post(&g_count_lock);

  sem_wait(&g_print_lock);
  int thread_number = find_thread_number(gettid());
  INFO("THREAD CREATED (%d, %ld)\n", thread_number, gettid());
  fflush(stdout);
  sem_post(&g_print_lock);
  
  // Execute the function for the thread as normal
  void *return_val = start_routine(arg);

  run_scheduling_algorithm();

  sem_wait(&g_print_lock);
  INFO("THREAD EXITED (%d, %ld)\n", thread_number, gettid());
  fflush(stdout);
  sem_post(&g_print_lock);
  return return_val;
}

/*
 * Creates a thread and starts execution.
 */
int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                   void *(*start_routine) (void *), void *arg) {
  run_scheduling_algorithm();

  // Struct for multiple args
  struct arg_struct *args = malloc(sizeof(arg_struct));
  args->struct_func = start_routine;
  args->struct_arg = arg;

  sem_wait(&g_print_lock);
  INFO("CALL pthread_create(%p, %p, %p, %p)\n", thread, attr, start_routine, arg);
  fflush(stdout);
  STACKTRACE_THREAD_ID = gettid();
  stacktrace();
  sem_post(&g_print_lock);

  int return_val = orig_create(thread, attr, &interpose_start_routine, (void *)args);

  run_scheduling_algorithm();

  sem_wait(&g_print_lock);  
  INFO("RETURN pthread_create(%p, %p, %p, %p) = %d\n", thread, attr, start_routine, arg, return_val);
  fflush(stdout);
  sem_post(&g_print_lock);

  return return_val;
}

/*
 * Terminates the calling thread.
 */
void pthread_exit(void *retval) {
  run_scheduling_algorithm();

  sem_wait(&g_print_lock);  
  INFO("CALL pthread_exit(%p)\n", retval);
  fflush(stdout);
  STACKTRACE_THREAD_ID = gettid();
  stacktrace();
  sem_post(&g_print_lock);

  run_scheduling_algorithm();

  sem_wait(&g_print_lock);
  int thread_number = find_thread_number(gettid());
  INFO("THREAD EXITED (%d, %ld)\n", thread_number, gettid());
  fflush(stdout);
  sem_post(&g_print_lock);

  orig_exit(retval);
  return;
}

/*
 * Causes the calling thread to relinquish the CPU.
 */
int pthread_yield(void) {
  run_scheduling_algorithm();

  sem_wait(&g_print_lock);
  INFO("CALL pthread_yield()\n");
  fflush(stdout);
  STACKTRACE_THREAD_ID = gettid();
  stacktrace();
  sem_post(&g_print_lock);

  int return_val = orig_yield();

  run_scheduling_algorithm();

  sem_wait(&g_print_lock);  
  INFO("RETURN pthread_yield() = %d\n", return_val);
  fflush(stdout);
  sem_post(&g_print_lock);

  return return_val;
}



//////////////////////////////////////////////////////////////////////
/////////////////////// CONDITION FUNCTIONS //////////////////////////
//////////////////////////////////////////////////////////////////////



//////////////////////////////////////////////////////////////////////
////////////////////////////// MUTEXES ///////////////////////////////
//////////////////////////////////////////////////////////////////////
/*
 * Locks a mutex. If the mutex is already locked, it blocks until the mutex becomes available.
 */
int pthread_mutex_lock(pthread_mutex_t *mutex) {
  if (STACKTRACE_THREAD_ID == gettid()) {
    // If this thread is currently printing the stacktrace, allow it to use the original function.
    return orig_mutex_lock(mutex);
  } 
  
  run_scheduling_algorithm();

  sem_wait(&g_print_lock);
  INFO("CALL pthread_mutex_lock(%p)\n", mutex);
  fflush(stdout);
  STACKTRACE_THREAD_ID = gettid();
  stacktrace();
  sem_post(&g_print_lock);
  
  int return_val = orig_mutex_lock(mutex);

  run_scheduling_algorithm();

  sem_wait(&g_print_lock);
  INFO("RETURN pthread_mutex_lock(%p) = %d\n", mutex, return_val);
  fflush(stdout);
  sem_post(&g_print_lock);

  return return_val;
}

/*
 * Releases (unlocks) the object referenced by 'mutex'.
 * Once unlocked, the scheduler determines which new thread should acquire the mutex, if any.
 */
int pthread_mutex_unlock(pthread_mutex_t *mutex) {
  if (STACKTRACE_THREAD_ID == gettid()) {
    // If this thread is currently printing the stacktrace, allow it to use the original function.
    return orig_mutex_unlock(mutex);
  }
  
  run_scheduling_algorithm();

  sem_wait(&g_print_lock);
  INFO("CALL pthread_mutex_unlock(%p)\n", mutex);
  fflush(stdout);
  STACKTRACE_THREAD_ID = gettid();
  stacktrace();
  sem_post(&g_print_lock);

  int return_val = orig_mutex_unlock(mutex);

  run_scheduling_algorithm();

  sem_wait(&g_print_lock);
  INFO("RETURN pthread_mutex_unlock(%p) = %d\n", mutex, return_val);
  fflush(stdout);
  sem_post(&g_print_lock);

  return return_val;
}

/*
 * Equivalent to pthread_mutex_lock(), except that if it’s already locked, the call returns immediately.
 */
int pthread_mutex_trylock(pthread_mutex_t *mutex) {
  run_scheduling_algorithm();

  sem_wait(&g_print_lock);
  INFO("CALL pthread_mutex_trylock(%p)\n", mutex);
  fflush(stdout);
  STACKTRACE_THREAD_ID = gettid();
  stacktrace();
  sem_post(&g_print_lock);

  int return_val = orig_mutex_trylock(mutex);

  run_scheduling_algorithm();

  sem_wait(&g_print_lock);
  INFO("RETURN pthread_mutex_trylock(%p) = %d\n", mutex, return_val);
  fflush(stdout);
  sem_post(&g_print_lock);

  return return_val;
}



//////////////////////////////////////////////////////////////////////
//////////////////////////// CONSTRUCTOR /////////////////////////////
//////////////////////////////////////////////////////////////////////
/*
 * Executes before any other function
 */
static __attribute__((constructor (200))) void init_testlib(void) {
  //////////////////////////////////////////////////
  ////////////// ORIGINAL FUNCTIONS ////////////////
  orig_mutex_lock = (pthread_mutex_lock_type)dlsym(RTLD_NEXT, "pthread_mutex_lock");
  orig_mutex_unlock = (pthread_mutex_unlock_type)dlsym(RTLD_NEXT, "pthread_mutex_unlock");
  orig_create = (pthread_create_type)dlsym(RTLD_NEXT, "pthread_create");
  orig_exit = (pthread_exit_type)dlsym(RTLD_NEXT, "pthread_exit");
  orig_yield = (pthread_yield_type)dlsym(RTLD_NEXT, "pthread_yield");
  orig_mutex_trylock = (pthread_mutex_trylock_type)dlsym(RTLD_NEXT, "pthread_mutex_trylock");
  orig_cond_wait = (pthread_cond_wait_type)dlsym(RTLD_NEXT, "pthread_cond_wait");
  orig_cond_signal = (pthread_cond_signal_type)dlsym(RTLD_NEXT, "pthread_cond_signal");
  orig_cond_broadcast = (pthread_cond_broadcast_type)dlsym(RTLD_NEXT, "pthread_cond_broadcast");

  //////////////////////////////////////////////////
  /////////////// INITIALIZATION ///////////////////
  pthread_mutex_t init_lock;
  orig_mutex_lock(&init_lock);

  INFO("Testlib loaded!\n");
  fflush(stdout);
  INFO("Stacktraces is %i\n", get_stacktraces());
  fflush(stdout);
  INFO("Algorithm ID is %i\n", get_algorithm_ID());
  fflush(stdout);
  INFO("Seed is %i\n",(int) get_seed());
  fflush(stdout);


  //////////////////////////////////////////////////
  ////////////////// SEMAPHORES ////////////////////
  sem_init(&g_print_lock, 0, 1);
  sem_init(&g_count_lock, 0, 1);
  sem_init(&g_cond_lock, 0, 1);
  sem_init(&g_queue_lock, 0, 1);


  //////////////////////////////////////////////////
  ////////////// CONDITION VARIABLES ///////////////
  EMPTY_THREAD_STRUCT.thread_id = -1;
  EMPTY_THREAD_STRUCT.state = -1;
  EMPTY_THREAD_STRUCT.index = -1;

  // Initialize empty cond_vars_queue
  cond_vars_queue = malloc(sizeof(struct thread_struct*) * MAX_THREADS);
  for(int i = 0; i < MAX_THREADS; i++) {
    cond_vars_queue[i] = malloc(sizeof(struct thread_struct) * MAX_THREADS);
  }
  for (int i = 0; i < MAX_THREADS; i++) {
    for (int j = 0; j < MAX_THREADS; j++) {
      cond_vars_queue[i][j] = EMPTY_THREAD_STRUCT;
    }
  }

  //////////////////////////////////////////////////
  ///////////////////// BEGIN //////////////////////
  orig_mutex_unlock(&init_lock);
}