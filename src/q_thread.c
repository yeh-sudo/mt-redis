//
// Created by chunlei zhang on 2019/07/18.
//

#include "fmacros.h"
#include "q_thread.h"
#include "server.h"
#include <pthread.h>
#include <sched.h>

int q_thread_init(q_thread *thread)
{
    if (thread == NULL) {
        return C_ERR;
    }

    thread->id = 0;
    thread->thread_id = 0;
    thread->fun_run = NULL;
    thread->data = NULL;

    return C_OK;
}

void q_thread_deinit(q_thread *thread)
{
    if (thread == NULL) {
        return;
    }

    thread->id = 0;
    thread->thread_id = 0;
    thread->fun_run = NULL;
    thread->data = NULL;
}

static void *q_thread_run(void *data)
{
    q_thread *thread = data;
    int cpu_id = thread->cpu_id;
    srand(ustime() ^ (int) pthread_self());

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu_id, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
    printf("thread sched_getcpu = %d\n", sched_getcpu());

    return thread->fun_run(thread->data);
}

int q_thread_start(q_thread *thread, int cpu_id)
{
    thread->cpu_id = cpu_id;
    pthread_attr_t attr;
    pthread_attr_init(&attr);

    if (thread == NULL || thread->fun_run == NULL) {
        return C_ERR;
    }

    pthread_create(&thread->thread_id, &attr, q_thread_run, thread);

    return C_OK;
}
