#include<stdio.h>
#include<string.h>
#include<pthread.h>
#include<unistd.h>
#include<malloc.h>

#define THREAD_NUM 10

pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

int g_shared_var = 1;

void *t1(void * args) {
  g_shared_var += 1;
  g_shared_var *= 2;

  pthread_exit(NULL);
}

// void *t2(void * args) {
//   g_shared_var *= 6;

//   pthread_exit(NULL);
// }

int main() {
  pthread_t threads[THREAD_NUM];

  for (int i = 0; i < THREAD_NUM; i++) {
    pthread_create(&threads[i], NULL, &t1, NULL);
  }

  for (int i = 0; i < THREAD_NUM; i++) {
    pthread_join(threads[i], NULL);
  }

//   pthread_create(&thread1, NULL, &t1, NULL);
//   pthread_create(&thread2, NULL, &t2, NULL);

//   pthread_join(thread1, NULL);
//   pthread_join(thread2, NULL);

  printf("g_shared_var = %d\n", g_shared_var);
  return 0;
}