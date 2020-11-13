#include<stdio.h>
#include<string.h>
#include<pthread.h>
#include<unistd.h>
#include<malloc.h>

pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

int g_shared_var = 0;

void *t1(void * args) {
  //pthread_mutex_lock(&lock);

  g_shared_var += 2;
  g_shared_var *= 3;
  g_shared_var += 5;
  g_shared_var *= 7;
  g_shared_var -= 2;

  //pthread_mutex_unlock(&lock);
  pthread_exit(NULL);
}

void *t2(void * args) {
  //pthread_mutex_lock(&lock);

  g_shared_var += 6;
  g_shared_var *= 5;
  g_shared_var += 2;
  g_shared_var *= 9;
  g_shared_var -= 6;  

  //pthread_mutex_unlock(&lock);
  pthread_exit(NULL);
}

int main() {
  pthread_t thread1;
  pthread_t thread2;

  pthread_create(&thread1, NULL, &t1, NULL);
  //sleep(0.5);
  pthread_create(&thread2, NULL, &t2, NULL);

  pthread_join(thread1, NULL);
  pthread_join(thread2, NULL);

  printf("g_shared_var = %d\n", g_shared_var);
  if (g_shared_var == 3657) {
    return 0;
  }
  else {
    return 1;
  }
}