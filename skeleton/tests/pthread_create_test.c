#include<stdio.h>
#include<pthread.h>
#include<unistd.h>
#include<malloc.h>

#define THREAD_NUM 10

/*
 * PTHREAD_CREATE TEST
 * This test creates 10 threads and makes sure to pass a unique 'args' struct, preventing data races.
 * The threads will print out i*i for every i in range 0-9.
 */

struct multipliers {
    int x;
    int y;
};

pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

void *multiply(void * args) {
  struct multipliers *arguments = (struct multipliers *)args;
  int result = (arguments->x) * (arguments->y);
  printf("%d * %d = %d\n", arguments->x, arguments->y, result);
  fflush(stdout);
  return result;
}

int main() {
  pthread_t threads[THREAD_NUM];
  struct multipliers args[THREAD_NUM];

  for (int i = 0; i < THREAD_NUM; i++) {
    pthread_mutex_lock(&lock);
    args[i].x = i;
    args[i].y = i;
    pthread_mutex_unlock(&lock);

    pthread_create(&threads[i], NULL, &multiply, (void *)&args[i]);
  }

  pthread_exit(NULL);
  return 0;
}
