/* SPDX-License-Identifier: BSD-3-Clause */
#include <pthread.h>
#include <stdio.h>
static _Thread_local int value = 7;
static void *worker(void *argument)
{
    int *result = argument;
    value = 23;
    *result = value;
    return NULL;
}
int main(void)
{
    pthread_t thread;
    int result = 0;
    int error = pthread_create(&thread, NULL, worker, &result);
    if (error) { fprintf(stderr, "pthread_create: %d\n", error); return 1; }
    error = pthread_join(thread, NULL);
    if (error || result != 23 || value != 7) return 2;
    puts("PASS standard SDK pthread/TLS application");
    return 0;
}
