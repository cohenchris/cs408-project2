#include <stdio.h>
#include <pthread.h>
#define _GNU_SOURCE

/*
 * PTHREAD_YIELD TEST
 * This test creates 2 threads. The first thread yields the CPU, giving a chance for the second thread to execute.
 * It does not guarantee that the second thread finishes first, but it happens more often than not.
 */

void *t1(void *arg)
{
 pthread_yield();
 printf("Thread 1 executing\n");
 pthread_exit(NULL);
}

void *t2(void *arg)
{
 printf("Thread 2 executing\n");
 pthread_exit(NULL);
}

int main()
{
 pthread_t thread1;
 pthread_t thread2;
 pthread_create(&thread1, NULL, &t1, NULL);
 pthread_create(&thread2, NULL, &t2, NULL);

 pthread_exit(NULL);

 // This will always succeed.
 return 0;
}