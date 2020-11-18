//////////////////////////////////////////////////////////////////////
/////////////////////// CONDITION FUNCTIONS //////////////////////////
//////////////////////////////////////////////////////////////////////
/*
 * Prints the current contents of the queue for each condition variable
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
        INFO("%lu, ", cond_vars_queue[i][j].thread_id);
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
 * The unique g_current_thread is queued in the queue now.
 */
void queue(pthread_cond_t *cond) {
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

  cond_vars_queue[cond_var_index][queue_index] = g_threads[g_current_thread];

  if (DEBUG) {
    INFO("QUEUING!\n");
    fflush(stdout);
    print_queue();
  }
}

/*
 * Dequeues highest priority runnable thread from condition variable cond.
 * Returns priority of thread dequeued, -1 if no runnable threads
 */
struct thread_struct dequeue(pthread_cond_t *cond) {
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

  if (dequeue_index == -1) {
    // return NULL if there is nothing to dequeue
    return EMPTY_THREAD_STRUCT;
  }

  struct thread_struct dequeued_thread = cond_vars_queue[cond_var_index][dequeue_index];

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

  // Edge cases - if you dequeue, the last index will always be empty
  cond_vars_queue[cond_var_index][MAX_THREADS - 1] = EMPTY_THREAD_STRUCT;

  if (DEBUG) {
    INFO("DEQUEUING!\n");
    fflush(stdout);
    print_queue();
  }
  return dequeued_thread;
}

////////////////////////////
// PTHREAD_COND FUNCTIONS //
////////////////////////////

/*
 * Blocks until a condition variable is signaled. Must be called enclosed in 'mutex'
 * in order to ensure that the program doesn't hang.
 */
int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex) {
  printf("WAITING\n");
  fflush(stdout);
  run_scheduling_algorithm(PCT_THREAD_CALL);

  sem_wait(&g_print_lock);
  INFO("CALL pthread_cond_wait(%p, %p)\n", cond, mutex);
  fflush(stdout);
  STACKTRACE_THREAD_ID = gettid();
  stacktrace();
  sem_post(&g_print_lock);

  printf("PTHREAD_COND_WAIT DONE STACKTRACE\n");
  fflush(stdout);
  int return_val = -1;
  if (get_algorithm_ID() == kAlgorithmPCT) {
    // Run special version of pthread_cond_wait for PCT
    // unlock mutex?
    pthread_mutex_unlock(mutex);
    printf("PTHREAD_COND_WAIT UNLOCKED MUTEX\n");
    fflush(stdout);
    // Queue current thread into the queue for 'cond'
    queue(cond);
    printf("PTHREAD_COND_WAIT QUEUED\n");
    fflush(stdout);
    printf("G_CURR_THD = %lu", g_current_thread);
    sem_wait(&(g_semaphores[g_current_thread])); // will block until unlocked by PCT
    // lock mutex?
    printf("PTHREAD_COND_WAIT LOCKED MUTEX AGAIN\n");
    fflush(stdout);
    pthread_mutex_lock(mutex);

    // by now, thread will be dequeued from conditional variable queue
    return_val = 0;
  }
  else {
    // no special PCT case - just run original function
    return_val = orig_cond_wait(cond, mutex);
  }

  run_scheduling_algorithm(PCT_DO_NOTHING);

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
  //sem_wait(&g_cond_lock);
  run_scheduling_algorithm(PCT_THREAD_CALL);

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
    struct thread_struct dequeued = dequeue(cond);
    if (!compare_thread_structs(&dequeued, &EMPTY_THREAD_STRUCT)) {
      // run thread
      int thread_index = dequeued.index;
      INFO("DEQUEUING INDEX %d\n", thread_index);
      fflush(stdout);
      sem_post(&(g_semaphores[thread_index]));
      return_val = 0;
    }
  }
  else {
    // no special PCT case - just run original function
    return_val = orig_cond_signal(cond);
  }

  run_scheduling_algorithm(PCT_DO_NOTHING);

  sem_wait(&g_print_lock);
  INFO("RETURN pthread_cond_signal(%p) = %d\n", cond, return_val);
  fflush(stdout);
  sem_post(&g_print_lock);

  //sem_post(&g_cond_lock);
  return return_val;
}

/*
 * Suspends the current thread, starts running ALL of the runnable threads.
 */
int pthread_cond_broadcast(pthread_cond_t *cond) {
  //sem_wait(&g_cond_lock);
  run_scheduling_algorithm(PCT_THREAD_CALL);

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
      struct thread_struct dequeued = dequeue(cond);
      if (!compare_thread_structs(&dequeued, &EMPTY_THREAD_STRUCT)) {
        // run thread
        int thread_index = dequeued.index;
        sem_post(&(g_semaphores[thread_index]));
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

  run_scheduling_algorithm(PCT_DO_NOTHING);

  sem_wait(&g_print_lock);
  INFO("RETURN pthread_cond_broadcast(%p) = %d\n", cond, return_val);
  fflush(stdout);
  sem_post(&g_print_lock);

  //sem_post(&g_cond_lock);

  return return_val;
}