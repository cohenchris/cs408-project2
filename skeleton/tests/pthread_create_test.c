#include<stdio.h>
#include<pthread.h>
#include<unistd.h>

struct multipliers {
    int x;
    int y;
}

void* multiply(void * args) {
  int result = (args->x) * (args->y);
  printf("%d * %d = %d", x, y, result);
  return (void*)0;
}

int main() {
  pthread_t thread;
  
  struct multipliers args;
  args.x = 5;
  args.y = 7;

  pthread_create(&thread, NULL, &foo, (void *)&args);
  sleep(1);
  return 0;
}