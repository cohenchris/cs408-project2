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
pthread_mutex_t g_mutexes[MAX_MUTEXES];

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
  for (int i = 0; i < MAX_THREADS; i++) {
    if (g_thread_ids[i] == tid) {
      return i;
    }
  }
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
char omit_functions[11][30] = {
  "interpose_start_routine",
  "omit",
  "stacktrace",
  "get_thread_index",
  "compare_thread_structs",
  "get_highest_priority_index",
  "run_scheduling_algorithm",
  "dequeue",
  "queue",
  "find_cond_index",
  "print_queue"
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
  sem_wait(&g_print_lock);
  INFO("\nPRIORITIES\n");
  fflush(stdout);
  for (int i = 0; i < MAX_THREADS; i++) {
    if (g_threads[i].state == THREAD_INVALID) {
      break;
    }
    INFO("thread num: %d, tid: %lu, priority: %d, state: %d\n", get_thread_index(g_threads[i].tid), g_threads[i].tid, get_priorities()[i], g_threads[i].state);
    fflush(stdout);
  }
  sem_post(&g_print_lock);

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
    sem_wait(&g_global_vars_lock);
    // run pct scheduling algorithm
    int highest_priority = get_highest_priority_index();
    int curr_thread = get_thread_index(gettid());
    sem_post(&g_global_vars_lock);

    sem_wait(&g_print_lock);
    INFO("HIGHEST PRIORITY THREAD %d STATE: %d\n", highest_priority, g_threads[highest_priority].state);
    fflush(stdout);
    sem_post(&g_print_lock);
 
    // If highest priority thread is the current thread or there are no runnable threads
    // Continue running current thread
    if (highest_priority == curr_thread || highest_priority == -1) {
      sem_wait(&g_print_lock);
      INFO("CONTINUING THREAD %d\n", curr_thread);
      fflush(stdout);
      sem_post(&g_print_lock);
    }
    else {
      sem_wait(&g_print_lock);
      INFO("\nSTART THREAD %d\n", highest_priority);
      fflush(stdout);
      sem_post(&g_print_lock);

      // start new highest thread
      sem_post(&g_semaphores[highest_priority]);

      // stop current thread unless it is in terminated state
      sem_wait(&g_global_vars_lock);
      int curr_state = g_threads[curr_thread].state;
      sem_post(&g_global_vars_lock);
  
      if (curr_state != THREAD_TERMINATED) {
        sem_wait(&g_print_lock);
        INFO("\nBLOCK THREAD %d\n", curr_thread);
        fflush(stdout);
        sem_post(&g_print_lock);

        sem_wait(&g_semaphores[curr_thread]);
      } else {
        sem_wait(&g_print_lock);
        INFO("CONTINUING THREAD %d\n", curr_thread);
        fflush(stdout);
        sem_post(&g_print_lock);
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

  // sem_wait(&g_threads_lock);
  // Initialize struct for current thread
  g_threads[g_thread_count].tid = gettid();
  g_threads[g_thread_count].state = THREAD_RUNNABLE;
  // sem_post(&g_threads_lock);
  int thread_index = get_thread_index(gettid());
  sem_post(&g_global_vars_lock);

  // Block immediately
  sem_wait(&g_semaphores[thread_index]);

  sem_wait(&g_print_lock);
  sem_wait(&g_global_vars_lock);
  INFO("THREAD CREATED (%d, %ld)\n", thread_index, g_threads[thread_index].tid);
  fflush(stdout);
  sem_post(&g_global_vars_lock);
  sem_post(&g_print_lock);

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

  sem_wait(&g_print_lock);
  INFO("THREAD EXITED IN INTERPOSE (%d, %ld)\n", thread_index, gettid());
  fflush(stdout);
  sem_post(&g_print_lock);

  run_scheduling_algorithm();
  sem_wait(&g_print_lock);
  INFO("THREAD %d GOT TO THE END OF INTERPOSE\n", thread_index);
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

  sem_wait(&g_global_vars_lock);
  sem_wait(&g_print_lock);
  INFO("THREAD %d GOT TO THE END OF CREATE\n", get_thread_index(gettid()));
  fflush(stdout);
  sem_post(&g_print_lock);
  sem_post(&g_global_vars_lock);
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
  INFO("CALL pthread_exit(%p) from THREAD %d\n", retval, thread_index);
  fflush(stdout);
  STACKTRACE_THREAD_ID = gettid();
  stacktrace();
  sem_post(&g_print_lock);

  run_scheduling_algorithm();

  sem_wait(&g_print_lock);
  INFO("THREAD EXITED IN PTHREAD EXIT (%d, %ld)\n", thread_index, gettid());
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
/*
 * Prints the current contents of the queue for each condition variable
 * MUST be locked with g_global_vars_lock semaphore beforehand
 */
void print_queue() {
  sem_wait(&g_print_lock);
  INFO("######################################\n");
  fflush(stdout);
  INFO("There are %d condition variables!\n", num_cond_vars);
  fflush(stdout);

  // Print queue for each condition var
  for (int i = 0; i < num_cond_vars; i++) {
    INFO("QUEUE FOR COND VAR %d: [ ", i);
    fflush(stdout);
    for (int j = 0; j < MAX_THREADS; j++) {
      if (compare_thread_structs(&cond_vars_queue[i][j], &EMPTY_THREAD_STRUCT)) {
        break;
      }
      else {
        INFO("%lu, ", cond_vars_queue[i][j].tid);
        fflush(stdout);
      }
    }
    INFO("]\n");
    fflush(stdout);
  }

  INFO("######################################\n");
  fflush(stdout);
  sem_post(&g_print_lock);
}

/*
 * Finds index of condition variable, if it exists.
 * Returns queue_rear if it doesn't exist - makes the program insert at back
 * MUST be locked with g_global_vars_lock semaphore beforehand
 */
int find_cond_index(pthread_cond_t *cond) {
  for (int i = 0; i < MAX_THREADS; i++) {
    if (cond_vars[i] == cond) {
      return i;
    }
  }
  // cond doesn't exist in the cond_vars array
  // insert new cond at back of array
  // return new index at back of array
  cond_vars[queue_rear] = cond;
  num_cond_vars++;
  return queue_rear++;
}

/*
 * Searches cond_vars for the cond variable. If it doesn't exist, it inserts the new cond var at the end.
 * After that, it looks through cond_vars_queue to figure out where to queue the unique thread_id.
 * The unique thread is queued in the queue now.
 */
void queue(pthread_cond_t *cond) {
  sem_wait(&g_global_vars_lock);
  // Finds place that the condition variable resides/should reside
  int cond_var_index = find_cond_index(cond);

  // Loops until it finds place that it should insert this next thread
  int queue_index = -1;
  for (int i = 0; i < MAX_THREADS; i++) {
    if (compare_thread_structs(&cond_vars_queue[cond_var_index][i], &EMPTY_THREAD_STRUCT)) {
      queue_index = i;
      break;
    }
  }

  int current_thread = get_thread_index(gettid());
  cond_vars_queue[cond_var_index][queue_index] = g_threads[current_thread];
  // Make sure to mark the current thread as blocked
  g_threads[current_thread].state = THREAD_BLOCKED;

  if (DEBUG) {
    sem_wait(&g_print_lock);
    INFO("QUEUING!\n");
    fflush(stdout);
    sem_post(&g_print_lock);
    print_queue();
  }
  sem_post(&g_global_vars_lock);
}

/*
 * Dequeues highest priority runnable thread from condition variable cond.
 * Returns thread number of thread dequeued, -1 if no runnable threads
 */
int dequeue(pthread_cond_t *cond) {
  sem_wait(&g_global_vars_lock);
  // Finds place that the condition variable resides/should reside
  int cond_var_index = find_cond_index(cond);

  // find first runnable thread in the queue
  int dequeue_index = -1;
  for (int i = 0; i < MAX_THREADS; i++) {
    if (cond_vars_queue[cond_var_index][i].state == THREAD_RUNNABLE) {
      // if thread is runnable, dequeue it
      dequeue_index = i;
      break;
    }
  }

  // index corresponding to this thread in every global array
  int thread_index = get_thread_index(cond_vars_queue[cond_var_index][dequeue_index].tid);

  if (dequeue_index == -1) {
    // return NULL if there is nothing to dequeue
    sem_post(&g_global_vars_lock);
    return -1;
  }

  // dequeue variable and shift everything
  for (int i = dequeue_index + 1; i < MAX_THREADS; i++) {
    if (!compare_thread_structs(&cond_vars_queue[cond_var_index][i], &EMPTY_THREAD_STRUCT)) {
      // if it's not empty, then there's a value that needs to be shifted
      cond_vars_queue[cond_var_index][i-1] = cond_vars_queue[cond_var_index][i];
    } 
    else {
      // if it is NULL, make sure to overwrite the value before it to prevent duplicates
      cond_vars_queue[cond_var_index][i-1] = EMPTY_THREAD_STRUCT;
      break;
    }
  }

  // Edge case - if you dequeue, the last index will always be empty
  cond_vars_queue[cond_var_index][MAX_THREADS - 1] = EMPTY_THREAD_STRUCT;

  if (DEBUG) {
    sem_wait(&g_print_lock);
    INFO("DEQUEUING!\n");
    fflush(stdout);
    sem_post(&g_print_lock);
    print_queue();
  }

  // Mark newly dequeued thread as runnable
  g_threads[thread_index].state = THREAD_RUNNABLE;

  sem_post(&g_global_vars_lock);
  return thread_index;
}

////////////////////////////
// PTHREAD_COND FUNCTIONS //
////////////////////////////

/*
 * Blocks until a condition variable is signaled. Must be called enclosed in 'mutex'
 * in order to ensure that the program doesn't hang.
 */
int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex) {
  run_scheduling_algorithm();

  sem_wait(&g_print_lock);
  INFO("CALL pthread_cond_wait(%p, %p)\n", cond, mutex);
  fflush(stdout);
  STACKTRACE_THREAD_ID = gettid();
  stacktrace();
  sem_post(&g_print_lock);

  sem_wait(&g_global_vars_lock);
  int current_thread = get_thread_index(gettid());
  sem_post(&g_global_vars_lock);

  int return_val = -1;
  if (get_algorithm_ID() == kAlgorithmPCT) {
    // Run special version of pthread_cond_wait for PCT
  
    // Queue current thread into the queue for 'cond'
    sem_wait(&g_print_lock);
    INFO("PTHREAD_COND_WAIT --> Queuing TID %d\n", gettid());
    fflush(stdout);
    sem_post(&g_print_lock);


    queue(cond);  // Queues the thread into "cond's" queue, also sets state to blocked

    sem_wait(&g_print_lock);
    INFO("UNLOCKING MUTEX AND WAITING\n");
    sem_post(&g_print_lock);

    // Temporarily release the mutex to prevent deadlocks
    orig_mutex_unlock(mutex);
    // Reschedule to switch this thread out while it waits for the condition signal
    run_scheduling_algorithm();
    // Once we're back, take hold of the mutex again
    orig_mutex_lock(mutex);
    // by now, thread will be dequeued from conditional variable queue
    return_val = 0;
  }
  else {
    // no special PCT case - just run original function
    return_val = orig_cond_wait(cond, mutex);
    run_scheduling_algorithm();
  }

  sem_wait(&g_print_lock);  
  INFO("RETURN pthread_cond_wait(%p, %p) = %d\n", cond, mutex, return_val);
  fflush(stdout);
  sem_post(&g_print_lock);

  return return_val;
}

/*
 * Suspends the current thread, starts the highest priority runnable thread.
 */
int pthread_cond_signal(pthread_cond_t *cond) {
  run_scheduling_algorithm();

  sem_wait(&g_print_lock);
  INFO("CALL pthread_cond_signal(%p)\n", cond);
  fflush(stdout);
  STACKTRACE_THREAD_ID = gettid();
  stacktrace();
  sem_post(&g_print_lock);

  int return_val = -1;
  if (get_algorithm_ID() == kAlgorithmPCT) {
    // Run special version of pthread_cond_signal for PCT

    // find highest priority runnable thread thread_id
    int dequeued_index = dequeue(cond);
    if (dequeued_index != -1) {
      // run (post) thread that was just dequeued
      sem_post(&g_semaphores[dequeued_index]);
      return_val = 0;
    }
  }
  else {
    // no special PCT case - just run original function
    return_val = orig_cond_signal(cond);
  }

  run_scheduling_algorithm();

  sem_wait(&g_print_lock);
  INFO("RETURN pthread_cond_signal(%p) = %d\n", cond, return_val);
  fflush(stdout);
  sem_post(&g_print_lock);

  return return_val;
}

/*
 * Suspends the current thread, starts running ALL of the runnable threads.
 */
int pthread_cond_broadcast(pthread_cond_t *cond) {
  run_scheduling_algorithm();

  sem_wait(&g_print_lock);
  INFO("CALL pthread_cond_broadcast(%p)\n", cond);
  fflush(stdout);
  STACKTRACE_THREAD_ID = gettid();
  stacktrace();
  sem_post(&g_print_lock);

  int return_val = -1;
  if (get_algorithm_ID() == kAlgorithmPCT) {
    // Run special version of pthread_cond_broadcast for PCT
    while (true) {
      int dequeued_index = dequeue(cond);
      if (dequeued_index != -1) {
        // run thread that was just dequeued
        sem_post(&g_semaphores[dequeued_index]);
      }
      else {
        break;
      }
    }
    return_val = 0;
  }
  else {
    // no special PCT case - just run original function
    return_val = orig_cond_broadcast(cond);
  }

  run_scheduling_algorithm();

  sem_wait(&g_print_lock);
  INFO("RETURN pthread_cond_broadcast(%p) = %d\n", cond, return_val);
  fflush(stdout);
  sem_post(&g_print_lock);

  return return_val;
}



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

  // Initialize main
  g_threads[0].tid = gettid();
  g_threads[0].state = THREAD_RUNNABLE;
  g_thread_ids[0] = gettid();

  // Initialize semaphore array
  for (int i = 0; i < MAX_THREADS; i++) {
    sem_init(&g_semaphores[i], 0, 0);
  }

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