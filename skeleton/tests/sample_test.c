#include<stdio.h>
#include<pthread.h>
#include<unistd.h>

void* foo(void * arg) {
  printf("Hello from a thread\n");
  return (void*)0;
}

int main() {
  pthread_t thread;
  pthread_create(&thread, NULL, &foo, NULL);
  sleep(1);
  return 0;
}
