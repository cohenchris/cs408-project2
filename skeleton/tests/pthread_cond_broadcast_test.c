#include<stdio.h>
#include<string.h>
#include<pthread.h>
#include<unistd.h>
#include<malloc.h>

#define THREAD_NUM 10

/*
 * PTHREAD_COND_BROADCAST TEST
 * This test has 10 threads wait on 1 condition variable, and have another thread broadcast a signal for that condition variable
 * to make all of the threads start executing again.
 */

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t dispatch_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static pthread_cond_t wait_cond = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t id_lock = PTHREAD_MUTEX_INITIALIZER;

int g_waiting_count = 0;

struct my_thread {
    int id;
};

void *t1(void * args) {
  struct my_thread *arguments = (struct my_thread *)args;

  printf("THREAD %d WAITING FOR CONDITION\n", arguments->id);
  fflush(stdout);

  pthread_mutex_lock(&lock);
  g_waiting_count += 1;
  pthread_cond_wait(&cond, &lock);
  pthread_mutex_unlock(&lock);
  printf("THREAD %d TERMINATING\n", arguments->id);
  pthread_exit(NULL);
}

void *t2(void * args) {
  pthread_mutex_lock(&dispatch_lock);
  pthread_mutex_unlock(&dispatch_lock);
  printf("\nBROADCASTING CONDITION SIGNAL\n\n");
  pthread_cond_broadcast(&cond);
  pthread_exit(NULL);
}

int main() {
  pthread_t threads[THREAD_NUM];
  struct my_thread args[THREAD_NUM];

  for (int i = 0; i < THREAD_NUM; i++) {
    pthread_mutex_lock(&id_lock);
    args[i].id = i + 1;
    pthread_mutex_unlock(&id_lock);
    pthread_create(&threads[i], NULL, &t1, (void *)&args[i]);
  }


  sleep(1);
  pthread_t dispatcher;
  pthread_create(&dispatcher, NULL, &t2, NULL);

  pthread_exit(NULL);
  // If the program gets down to here, it succeeds. The program will hang if it fails.
  return 0;
}
