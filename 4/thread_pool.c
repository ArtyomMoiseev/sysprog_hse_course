#include "thread_pool.h"
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <pthread.h>

enum task_status {
    TASK_WAITING = 0,
    TASK_RUNNING,
    TASK_FINISHED
};

struct thread_task {
    thread_task_f function;
    void *arg;
    void *result;

    int status;
    int pushed;
    pthread_mutex_t mutex;
    pthread_cond_t  done;
    struct thread_task *next;
};


struct thread_pool {
    pthread_t *threads;
    int max_threads;
    int current_threads;
    int idle_threads;

    struct thread_task *task_head;
    struct thread_task *task_tail;
    int task_count;
    int active_tasks;

    pthread_mutex_t pool_mutex;
    pthread_cond_t  pool_cond;

    int shutdown;
};

static void *worker_thread(void *arg)
{
    struct thread_pool *pool = (struct thread_pool *)arg;

    while (1) {
        pthread_mutex_lock(&pool->pool_mutex);

        while (pool->task_head == NULL && !pool->shutdown) {
            pool->idle_threads++;
            pthread_cond_wait(&pool->pool_cond, &pool->pool_mutex);
            pool->idle_threads--;
        }


        if (pool->shutdown && pool->task_head == NULL) {
            pthread_mutex_unlock(&pool->pool_mutex);
            break;
        }

        struct thread_task *task = pool->task_head;
        if (task) {
            pool->task_head = task->next;
            if (pool->task_head == NULL)
                pool->task_tail = NULL;
            pool->task_count--;

            pool->active_tasks++;
        }
        pthread_mutex_unlock(&pool->pool_mutex);

        if (task) {
            pthread_mutex_lock(&task->mutex);
            task->status = TASK_RUNNING;
            pthread_mutex_unlock(&task->mutex);

            void *ret = task->function(task->arg);

            pthread_mutex_lock(&task->mutex);
            task->result = ret;
            task->status = TASK_FINISHED;
            pthread_cond_broadcast(&task->done);
            pthread_mutex_unlock(&task->mutex);

            pthread_mutex_lock(&pool->pool_mutex);
            pool->active_tasks--;
            pthread_mutex_unlock(&pool->pool_mutex);
        }
    }
    return NULL;
}

int
thread_pool_new(int max_thread_count, struct thread_pool **pool)
{
    if (max_thread_count <= 0 || max_thread_count > TPOOL_MAX_THREADS)
        return TPOOL_ERR_INVALID_ARGUMENT;

    struct thread_pool *p = malloc(sizeof(struct thread_pool));
    if (!p)
        return ENOMEM;

    p->threads = malloc(sizeof(pthread_t) * max_thread_count);
    if (!p->threads) {
        free(p);
        return ENOMEM;
    }

    p->max_threads = max_thread_count;
    p->current_threads = 0;
    p->idle_threads = 0;
    p->task_head = NULL;
    p->task_tail = NULL;
    p->task_count = 0;
    p->active_tasks = 0;
    p->shutdown = 0;

    if (pthread_mutex_init(&p->pool_mutex, NULL) != 0) {
        free(p->threads);
        free(p);
        return ENOMEM;
    }
    if (pthread_cond_init(&p->pool_cond, NULL) != 0) {
        pthread_mutex_destroy(&p->pool_mutex);
        free(p->threads);
        free(p);
        return ENOMEM;
    }

    *pool = p;
    return 0;
}

int
thread_pool_thread_count(const struct thread_pool *pool)
{
    if (!pool)
        return TPOOL_ERR_INVALID_ARGUMENT;
    return pool->current_threads;
}

int
thread_pool_push_task(struct thread_pool *pool, struct thread_task *task)
{
    if (!pool || !task)
        return TPOOL_ERR_INVALID_ARGUMENT;

    pthread_mutex_lock(&pool->pool_mutex);
    if (pool->task_count >= TPOOL_MAX_TASKS) {
        pthread_mutex_unlock(&pool->pool_mutex);
        return TPOOL_ERR_TOO_MANY_TASKS;
    }

    pthread_mutex_lock(&task->mutex);
    if (task->pushed) {
        if (task->status != TASK_FINISHED) {
            pthread_mutex_unlock(&task->mutex);
            pthread_mutex_unlock(&pool->pool_mutex);
            return TPOOL_ERR_TASK_IN_POOL;
        }

        task->status = TASK_WAITING;
        task->result = NULL;
    } else {
        task->pushed = 1;
    }
    pthread_mutex_unlock(&task->mutex);

    task->next = NULL;
    if (pool->task_tail == NULL)
        pool->task_head = pool->task_tail = task;
    else {
        pool->task_tail->next = task;
        pool->task_tail = task;
    }
    pool->task_count++;

    pthread_cond_signal(&pool->pool_cond);

    if (pool->idle_threads == 0 && pool->current_threads < pool->max_threads) {
        int ret = pthread_create(&pool->threads[pool->current_threads], NULL, worker_thread, pool);
        if(ret == 0) {
            pool->current_threads++;
        }
    }

    pthread_mutex_unlock(&pool->pool_mutex);
    return 0;
}

int
thread_pool_delete(struct thread_pool *pool)
{
    if (!pool)
        return TPOOL_ERR_INVALID_ARGUMENT;

    pthread_mutex_lock(&pool->pool_mutex);
    /* Now also check that no task is in progress */
    if (pool->task_head != NULL || pool->active_tasks > 0) {
        pthread_mutex_unlock(&pool->pool_mutex);
        return TPOOL_ERR_HAS_TASKS;
    }
    pool->shutdown = 1;
    pthread_cond_broadcast(&pool->pool_cond);
    pthread_mutex_unlock(&pool->pool_mutex);

    for (int i = 0; i < pool->current_threads; i++)
        pthread_join(pool->threads[i], NULL);

    pthread_mutex_destroy(&pool->pool_mutex);
    pthread_cond_destroy(&pool->pool_cond);
    free(pool->threads);
    free(pool);
    return 0;
}

int
thread_task_new(struct thread_task **ptask, thread_task_f function, void *arg)
{
    if (!ptask || !function)
        return TPOOL_ERR_INVALID_ARGUMENT;

    struct thread_task *task = malloc(sizeof(struct thread_task));
    if (!task)
        return ENOMEM;

    task->function = function;
    task->arg = arg;
    task->result = NULL;
    task->status = TASK_WAITING;
    task->pushed = 0;
    task->next = NULL;

    if (pthread_mutex_init(&task->mutex, NULL) != 0) {
        free(task);
        return ENOMEM;
    }
    if (pthread_cond_init(&task->done, NULL) != 0) {
        pthread_mutex_destroy(&task->mutex);
        free(task);
        return ENOMEM;
    }
    *ptask = task;
    return 0;
}

bool
thread_task_is_finished(const struct thread_task *task)
{
    if (!task)
        return false;
    bool finished;
    pthread_mutex_lock((pthread_mutex_t *)&task->mutex);
    finished = (task->status == TASK_FINISHED);
    pthread_mutex_unlock((pthread_mutex_t *)&task->mutex);
    return finished;
}

bool
thread_task_is_running(const struct thread_task *task)
{
    if (!task)
        return false;
    bool running;
    pthread_mutex_lock((pthread_mutex_t *)&task->mutex);
    running = (task->status == TASK_RUNNING);
    pthread_mutex_unlock((pthread_mutex_t *)&task->mutex);
    return running;
}

int
thread_task_join(struct thread_task *task, void **result)
{
    if (!task)
        return TPOOL_ERR_INVALID_ARGUMENT;

    pthread_mutex_lock(&task->mutex);
    if (!task->pushed) {
        pthread_mutex_unlock(&task->mutex);
        return TPOOL_ERR_TASK_NOT_PUSHED;
    }
    while (task->status != TASK_FINISHED)
        pthread_cond_wait(&task->done, &task->mutex);
    if (result)
        *result = task->result;
    pthread_mutex_unlock(&task->mutex);
    return 0;
}


#if NEED_TIMED_JOIN
#include <time.h>

int
thread_task_timed_join(struct thread_task *task, double timeout, void **result)
{
  	/* IMPLEMENT THIS FUNCTION */
    (void)task;
    (void)timeout;
    (void)result;
    return TPOOL_ERR_NOT_IMPLEMENTED;
}
#endif

int
thread_task_delete(struct thread_task *task)
{
    if (!task)
        return TPOOL_ERR_INVALID_ARGUMENT;
    pthread_mutex_lock(&task->mutex);
    if (task->pushed && task->status != TASK_FINISHED) {
        pthread_mutex_unlock(&task->mutex);
        return TPOOL_ERR_TASK_IN_POOL;
    }
    pthread_mutex_unlock(&task->mutex);
    pthread_mutex_destroy(&task->mutex);
    pthread_cond_destroy(&task->done);
    free(task);
    return 0;
}


#if NEED_DETACH
int
thread_task_detach(struct thread_task *task)
{
    /* IMPLEMENT THIS FUNCTION */
    (void)task;
    return TPOOL_ERR_NOT_IMPLEMENTED;
}
#endif