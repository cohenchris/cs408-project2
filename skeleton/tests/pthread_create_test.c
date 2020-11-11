#include<stdio.h>
#include<pthread.h>
#include<unistd.h>

struct multipliers {
    int x;
    int y;
};

void* multiply(void * args) {
  struct multipliers *arguments = (struct multipliers *)args;
  int result = (arguments->x) * (arguments->y);
  printf("%d * %d = %d\n", arguments->x, arguments->y, result);
  return (void*)0;
}

int main() {
  pthread_t thread;

  struct multipliers args;
  args.x = 5;
  args.y = 7;

  pthread_create(&thread, NULL, &multiply, (void *)&args);
  sleep(1);
  return 0;
}