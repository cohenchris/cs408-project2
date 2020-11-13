#include<stdio.h>
#include<pthread.h>
#include<unistd.h>
#include<malloc.h>

#define THREAD_NUM 10

struct multipliers {
    int x;
    int y;
};

void *multiply(void * args) {
  struct multipliers *arguments = (struct multipliers *)args;
  int result = (arguments->x) * (arguments->y);
  printf("%d * %d = %d\n", arguments->x, arguments->y, result);
  pthread_exit(NULL);
}

int main() {
  pthread_t threads[THREAD_NUM];
  struct multipliers args[THREAD_NUM];

  for (int i = 0; i < THREAD_NUM; i++) {
    args[i].x = i;
    args[i].y = i;

    pthread_create(&threads[i], NULL, &multiply, (void *)&args[i]);
  }

  for (int i = 0; i < THREAD_NUM; i++) {
    pthread_join(threads[i], NULL);
  }
  return 0;
}