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

// Thread States
#define THREAD_INVALID 0
#define THREAD_RUNNABLE 1
#define THREAD_BLOCKED 2
#define THREAD_TERMINATED 3


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
sem_t g_global_vars_lock;
sem_t g_print_lock;
sem_t g_cond_lock;
sem_t g_queue_lock;
sem_t g_sched_lock;

// Global Array Semaphores
sem_t g_threads_lock;
sem_t g_semaphores_lock;
sem_t g_mutexes_lock;

// Function locks

struct thread_struct {
  long int tid;      // thread id from getttid()
                     // can get index from thread id using get_thread_index(long int tid)
  int state;         // thread state (running, blocked, etc)
};

// Total number of threads that are active
int g_thread_count = 0;
int STACKTRACE_THREAD_ID = -1;

// Used for interpose_start_routine in order to pass multiple args
typedef struct arg_struct {
  void *(*struct_func) (void *);
  void *struct_arg;
} arg_struct;

// ------------ GLOBAL ARRAYS ------------

// Keeps track of thread counts
// We were told that there will only be up to 64 threads ever run
// Helps map tid to index of thread in other arrays
long int g_thread_ids[MAX_THREADS] = { 0 };

// Store semaphores to control thread execution
sem_t g_semaphores[MAX_THREADS];

// Thread struct array to keep track of thread states and tid
struct thread_struct g_threads[MAX_THREADS];

// Store mutexes that a particular thread is blocked on
pthread_mutex_t **g_mutexes;

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
 * MUST ALWAYS BE CALLED WITHIN sem_wait(&g_global_vars_lock); and sem_post(&g_global_vars_lock);
 */
int get_thread_index(long int tid) {
  //sem_wait(&g_global_vars_lock);
  for (int i = 0; i < MAX_THREADS; i++) {
    if (g_thread_ids[i] == tid) {
      sem_post(&g_global_vars_lock);
      return i;
    }
  }
  //sem_post(&g_global_vars_lock);
  return -1;
}

/*
 * Compares thread_struct for PCT against a 'NULL' struct
 */
bool compare_thread_structs(struct thread_struct *t1, struct thread_struct *t2) {
  if ((t1->tid == t2->tid) &&
      (t1->state == t2->state)) {
    return true;
  }
  return false;
}

//////////////////////////////////////////////////////////////////////
//////////////////////////// STACKTRACE //////////////////////////////
//////////////////////////////////////////////////////////////////////
// String array of functions to omit from stack trace
char omit_functions[7][30] = {
  "interpose_start_routine",
  "omit",
  "stacktrace",
  "get_thread_index",
  "compare_thread_structs",
  "get_highest_priority_index",
  "run_scheduling_algorithm"
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

/*
 * Find thread with the highest priority out of all threads that are 
 * in a runnable state.
 * MUST ALWAYS BE CALLED WITHIN sem_wait(&g_global_vars_lock); and sem_post(&g_global_vars_lock);
 */
int get_highest_priority_index() {
  // sem_wait(&g_print_lock);
  // INFO("\nPRIORITIES\n");
  // fflush(stdout);
  // for (int i = 0; i < MAX_THREADS; i++) {
  //   INFO("thread num: %d, tid: %lu, priority: %d, state: %d\n", get_thread_index(g_threads[i].tid), g_threads[i].tid, get_priorities()[i], g_threads[i].state);
  //   fflush(stdout);
  // }
  // sem_post(&g_print_lock);

  // Find the first runnable thread to compare against the rest
  // If no runnable threads, return -1 and let current thread finish executing
  int index = -1;
  for (int i = 0; i < MAX_THREADS; i++) {
    if (g_threads[i].state == THREAD_RUNNABLE) {
      index = i;
    }
  }

  if (index != -1) {
    for (int i = 0; i < MAX_THREADS; i++) {
      if (g_threads[i].state == THREAD_RUNNABLE && get_priorities()[i] > get_priorities()[index]) {
        index = i;
      }
    }
  }
  return index;
}

int get_next_highest_priority_index(int curr_priority) {
  // Find the first runnable thread to compare against the rest
  // If no runnable threads, return -1 and let current thread finish executing
  int index = -1;
  for (int i = 0; i < MAX_THREADS; i++) {
    if ((g_threads[i].state == THREAD_RUNNABLE || g_threads[i].state == THREAD_BLOCKED)
        && get_priorities()[i] < curr_priority) {
      index = i;
      break;
    }
  }

  // Find the highest priority thread that is greater than priority[index] but less than curr_priority
  for (int i = 0; i < MAX_THREADS; i++) {
    sem_wait(&g_global_vars_lock);
    if ((g_threads[i].state == THREAD_RUNNABLE || g_threads[i].state == THREAD_BLOCKED)
     && get_priorities()[i] < get_priorities()[curr_priority] && get_priorities()[i] > get_priorities()[index]) {
      index = i;
    }
  }
  return index;
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

    sem_wait(&g_global_vars_lock);
    // Get current highest priority thread as baseline
    int curr_priority = get_highest_priority_index();
    // Get current thread index
    int curr_thread = get_thread_index(gettid());
    sem_post(&g_global_vars_lock); 

    // printf("curr_thread: %d\n", curr_thread);

    // If highest priority thread is the current thread or there are no runnable threads
    // Continue running current thread
    // if (curr_priority == curr_thread || curr_priority == -1) {
    //   // printf("CONTINUE: %d\n", curr_thread);  
    //   return;
    // }

    // Loop to find highest priority thread that can be unblocked
    while (curr_priority != -1) {
      // // If highest priority thread is the current thread
      // if (curr_priority == curr_thread) {
      //   return;
      // }
      // Check if next thread is waiting on a mutex
      if (g_mutexes[curr_priority] != NULL) {
        // Try to lock mutex
        // If not successful, try the next highest priority index
        if (orig_mutex_trylock(g_mutexes[curr_priority]) != 0) {
          sem_wait(&g_global_vars_lock);
          curr_priority = get_next_highest_priority_index(curr_priority);
          sem_post(&g_global_vars_lock);
          continue;
        }
        else { // Trylock successful
          sem_wait(&g_global_vars_lock);
          // Unblock mutex
          orig_mutex_unlock(g_mutexes[curr_priority]);
          sem_post(&g_global_vars_lock);
          // Start next thread
          sem_post(&g_semaphores[curr_priority]);
          // Stop current thread unless terminating
          sem_wait(&g_global_vars_lock);
          int state = g_threads[curr_thread].state;
          sem_post(&g_global_vars_lock);

          if (state != THREAD_TERMINATED) {
            sem_wait(&g_semaphores[curr_thread]);
          }
          break;
        }
      }
      else { // Next thread is not waiting on a mutex, no need to check
        // printf("NOT WAITING ON A MUTEX\n");
        // Start next thread
        sem_post(&g_semaphores[curr_priority]);
        // Stop current thread unless terminating
        sem_wait(&g_global_vars_lock);
        int state = g_threads[curr_thread].state;
        sem_post(&g_global_vars_lock);

        if (state != THREAD_TERMINATED) {
          sem_wait(&g_semaphores[curr_thread]);
        }
        break;
      }
    }
  }
  // else algorithm = none, do nothing special
}

//////////////////////////////////////////////////////////////////////
/////////////////////// THREADING FUNCTIONS //////////////////////////
//////////////////////////////////////////////////////////////////////
/*
 * Wrapper for starting the actual function meant to execute in the thread.
 * Allows for logging and other scheduling to be done before the thread starts.
 */
void *interpose_start_routine(void *argument) {
  sem_wait(&g_global_vars_lock);
  g_thread_count++;
  // Storing thread id at index g_thread_count
  g_thread_ids[g_thread_count] = gettid();

  // Initialize struct for current thread
  g_threads[g_thread_count].tid = gettid();
  g_threads[g_thread_count].state = THREAD_RUNNABLE;
  int thread_index = get_thread_index(gettid());
  sem_post(&g_global_vars_lock);

  run_scheduling_algorithm();

  // Deconstruct the struct into [ function to execute, arg ]
  sem_wait(&g_global_vars_lock);
  struct arg_struct *arguments = argument;
  void *(*start_routine) (void *) = arguments->struct_func;
  void *arg = arguments->struct_arg;
  // Tell which semaphore this thread should wait on
  sem_post(&g_global_vars_lock);
  
  // Execute the function for the thread as normal
  void *return_val = start_routine(arg);

  sem_wait(&g_global_vars_lock);
  g_threads[thread_index].state = THREAD_TERMINATED;
  sem_post(&g_global_vars_lock);

  run_scheduling_algorithm();
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
  sem_wait(&g_global_vars_lock);
  int thread_index = get_thread_index(gettid());
  g_threads[thread_index].state = THREAD_TERMINATED;
  sem_post(&g_global_vars_lock);

  run_scheduling_algorithm();

  sem_wait(&g_print_lock);  
  INFO("CALL pthread_exit(%p)\n", retval);
  fflush(stdout);
  STACKTRACE_THREAD_ID = gettid();
  stacktrace();
  sem_post(&g_print_lock);

  run_scheduling_algorithm();

  sem_wait(&g_print_lock);
  INFO("THREAD EXITED (%d, %ld)\n", thread_index, gettid());
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
  
  // If algorithm is PCT, check if mutex will block
  if (get_algorithm_ID() == kAlgorithmPCT) {
    sem_wait(&g_global_vars_lock);
    int curr_thread = get_thread_index(gettid());
    sem_post(&g_global_vars_lock);

    // If mutex can be aquired and won't block
    if (orig_mutex_trylock(mutex) != 0) {
      // printf("BLOCKING\n");
      sem_wait(&g_global_vars_lock);
      // Put mutex in mutex array
      g_mutexes[curr_thread] = mutex;
      // Set thread state to blocked
      g_threads[curr_thread].state = THREAD_BLOCKED;
      sem_post(&g_global_vars_lock);
    }
    else {
      // Unblock mutex
      orig_mutex_unlock(mutex);
      // printf("NOT BLOCKING\n");
    }
  }
  
  run_scheduling_algorithm();

  sem_wait(&g_print_lock);
  INFO("CALL pthread_mutex_lock(%p)\n", mutex);
  fflush(stdout);
  STACKTRACE_THREAD_ID = gettid();
  stacktrace();
  sem_post(&g_print_lock);
  
  int return_val = orig_mutex_lock(mutex);

  if (get_algorithm_ID() == kAlgorithmPCT) {
    sem_wait(&g_global_vars_lock);
    int curr_thread = get_thread_index(gettid());
    sem_post(&g_global_vars_lock);

    sem_wait(&g_global_vars_lock);
    if (g_mutexes[curr_thread] != NULL) {
      printf("RESET %p\n", g_mutexes[curr_thread]);
      // Reset mutex back to null
      g_mutexes[curr_thread] = NULL;
      // Reset thread state 
      g_threads[curr_thread].state = THREAD_RUNNABLE;
    }
    sem_post(&g_global_vars_lock);
  }

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

  // Initialize main
  g_threads[0].tid = gettid();
  g_threads[0].state = THREAD_RUNNABLE;
  g_thread_ids[0] = gettid();

  // Initialize semaphore array
  for (int i = 0; i < MAX_THREADS; i++) {
    sem_init(&g_semaphores[i], 0, 0);
  }

  // Initialize mutex array
  g_mutexes = malloc(sizeof(pthread_mutex_t *) * MAX_MUTEXES);

  //////////////////////////////////////////////////
  ////////////////// SEMAPHORES ////////////////////
  sem_init(&g_print_lock, 0, 1);
  sem_init(&g_global_vars_lock, 0, 1);
  sem_init(&g_cond_lock, 0, 1);
  sem_init(&g_queue_lock, 0, 1);

  sem_init(&g_threads_lock, 0, 1);
  sem_init(&g_semaphores_lock, 0, 1);
  sem_init(&g_mutexes_lock, 0, 1);

  //////////////////////////////////////////////////
  ////////////// CONDITION VARIABLES ///////////////
  EMPTY_THREAD_STRUCT.tid = -1;
  EMPTY_THREAD_STRUCT.state = -1;

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