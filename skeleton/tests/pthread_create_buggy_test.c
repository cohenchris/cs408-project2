#include<stdio.h>
#include<pthread.h>
#include<unistd.h>

#define THREAD_NUM 10

struct multipliers {
    int x;
    int y;
};

void* multiply(void * args) {
  struct multipliers *arguments = (struct multipliers *)args;
  int result = (arguments->x) * (arguments->y);
  printf("%d * %d = %d\n", arguments->x, arguments->y, result);
  fflush(stdout);
  return (void*)0;
}

int main() {
  pthread_t threads[THREAD_NUM];

  for (int i = 0; i < THREAD_NUM; i++) {
    struct multipliers args;
    args.x = i;
    args.y = i;

    pthread_create(&threads[i], NULL, &multiply, (void *)&args);
  }

  for (int i = 0; i < THREAD_NUM; i++) {
    pthread_join(threads[i], NULL);
  }
  return 0;
}
