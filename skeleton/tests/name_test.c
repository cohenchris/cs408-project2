#include<stdio.h>
#include<string.h>
#include<pthread.h>
#include<unistd.h>
#include<malloc.h>

pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
char *name = NULL;
int return_code;

void *t1(void * args) {
    name = malloc(6);
    name = "Chris";
  pthread_exit(NULL);
}

void *t2(void * args) {
    printf("Your name is %s\n", name);
    if (strcmp("Chris", name) == 0) {
        return_code = 0;
    }
    else {
        return_code = 1;
    }
    pthread_exit(NULL);
}

int main() {
  pthread_t thread1;
  pthread_t thread2;

  pthread_create(&thread1, NULL, &t1, NULL);
  pthread_join(thread1, NULL);
  pthread_create(&thread2, NULL, &t2, NULL);
  pthread_join(thread2, NULL);

  return return_code;
}