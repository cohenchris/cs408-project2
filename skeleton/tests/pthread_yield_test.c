#include<stdio.h>
#include<string.h>
#include<pthread.h>
#include<unistd.h>
#include<malloc.h>

void *t1(void * args) {
  pthread_yield();
  for (int i = 0; i < 100000; i++)
    ;
  printf("Thread 1 executing\n");
  pthread_exit(NULL);
}

void *t2(void * args) {
  printf("Thread 2 executing\n");
  pthread_exit(NULL);
}

int main() {
  pthread_t thread1;      
  pthread_t thread2;

  pthread_create(&thread1, NULL, &t1, NULL);
  pthread_create(&thread2, NULL, &t2, NULL);

  pthread_join(thread1, NULL);
  pthread_join(thread2, NULL);
  return 0;
}