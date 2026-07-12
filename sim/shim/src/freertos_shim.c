/* sim shim — FreeRTOS tasks/semaphores/queues on pthreads. */
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/* ---------------- tasks ---------------- */

typedef struct {
    TaskFunction_t fn;
    void* arg;
} task_tramp_t;

static void* task_trampoline(void* p) {
    task_tramp_t t = *(task_tramp_t*)p;
    free(p);
    t.fn(t.arg); /* FreeRTOS tasks exit via vTaskDelete(NULL); a plain return is also fine here */
    return NULL;
}

static BaseType_t task_create(TaskFunction_t fn, void* arg, TaskHandle_t* out_handle) {
    task_tramp_t* t = (task_tramp_t*)malloc(sizeof(*t));
    if (t == NULL) return pdFAIL;
    t->fn = fn;
    t->arg = arg;
    pthread_t th;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    int rc = pthread_create(&th, &attr, task_trampoline, t);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        free(t);
        return pdFAIL;
    }
    if (out_handle != NULL) *out_handle = (TaskHandle_t)th;
    return pdPASS;
}

BaseType_t xTaskCreate(TaskFunction_t fn, const char* name, uint32_t stack_depth, void* arg,
                       UBaseType_t priority, TaskHandle_t* out_handle) {
    (void)name;
    (void)stack_depth;
    (void)priority;
    return task_create(fn, arg, out_handle);
}

BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char* name, uint32_t stack_depth,
                                   void* arg, UBaseType_t priority, TaskHandle_t* out_handle,
                                   BaseType_t core_id) {
    (void)name;
    (void)stack_depth;
    (void)priority;
    (void)core_id;
    return task_create(fn, arg, out_handle);
}

void vTaskDelay(TickType_t ticks) { usleep((useconds_t)ticks * 1000u); }

void vTaskDelete(TaskHandle_t handle) {
    (void)handle; /* self-delete only */
    pthread_exit(NULL);
}

/* ---------------- deadline helper ---------------- */

static void abs_deadline(struct timespec* ts, uint32_t ms) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    uint64_t nsec = (uint64_t)tv.tv_usec * 1000u + (uint64_t)(ms % 1000u) * 1000000u;
    ts->tv_sec = tv.tv_sec + ms / 1000u + nsec / 1000000000u;
    ts->tv_nsec = (long)(nsec % 1000000000u);
}

/* ---------------- semaphores ---------------- */

struct sim_sem {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    unsigned count;
    unsigned max;
};

static SemaphoreHandle_t sem_new(unsigned initial, unsigned max) {
    struct sim_sem* s = (struct sim_sem*)calloc(1, sizeof(*s));
    if (s == NULL) return NULL;
    pthread_mutex_init(&s->mu, NULL);
    pthread_cond_init(&s->cv, NULL);
    s->count = initial;
    s->max = max;
    return s;
}

SemaphoreHandle_t xSemaphoreCreateMutex(void) { return sem_new(1, 1); }
SemaphoreHandle_t xSemaphoreCreateBinary(void) { return sem_new(0, 1); }

BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t ticks_to_wait) {
    if (s == NULL) return pdFALSE;
    pthread_mutex_lock(&s->mu);
    if (ticks_to_wait == portMAX_DELAY) {
        while (s->count == 0) pthread_cond_wait(&s->cv, &s->mu);
    } else {
        struct timespec ts;
        abs_deadline(&ts, ticks_to_wait);
        while (s->count == 0) {
            if (pthread_cond_timedwait(&s->cv, &s->mu, &ts) == ETIMEDOUT) break;
        }
        if (s->count == 0) {
            pthread_mutex_unlock(&s->mu);
            return pdFALSE;
        }
    }
    s->count--;
    pthread_mutex_unlock(&s->mu);
    return pdTRUE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t s) {
    if (s == NULL) return pdFALSE;
    pthread_mutex_lock(&s->mu);
    if (s->count < s->max) s->count++;
    pthread_cond_signal(&s->cv);
    pthread_mutex_unlock(&s->mu);
    return pdTRUE;
}

void vSemaphoreDelete(SemaphoreHandle_t s) {
    if (s == NULL) return;
    pthread_mutex_destroy(&s->mu);
    pthread_cond_destroy(&s->cv);
    free(s);
}

/* ---------------- queues ---------------- */

struct sim_queue {
    pthread_mutex_t mu;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
    uint8_t* buf;
    size_t item_size;
    size_t cap;
    size_t count;
    size_t head; /* index of the oldest item */
};

QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t item_size) {
    struct sim_queue* q = (struct sim_queue*)calloc(1, sizeof(*q));
    if (q == NULL) return NULL;
    q->buf = (uint8_t*)malloc((size_t)length * item_size);
    if (q->buf == NULL) {
        free(q);
        return NULL;
    }
    pthread_mutex_init(&q->mu, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    pthread_cond_init(&q->not_full, NULL);
    q->item_size = item_size;
    q->cap = length;
    return q;
}

BaseType_t xQueueSend(QueueHandle_t q, const void* item, TickType_t ticks_to_wait) {
    if (q == NULL || item == NULL) return pdFALSE;
    pthread_mutex_lock(&q->mu);
    if (q->count == q->cap) {
        if (ticks_to_wait == 0) {
            pthread_mutex_unlock(&q->mu);
            return pdFALSE;
        }
        if (ticks_to_wait == portMAX_DELAY) {
            while (q->count == q->cap) pthread_cond_wait(&q->not_full, &q->mu);
        } else {
            struct timespec ts;
            abs_deadline(&ts, ticks_to_wait);
            while (q->count == q->cap) {
                if (pthread_cond_timedwait(&q->not_full, &q->mu, &ts) == ETIMEDOUT) break;
            }
            if (q->count == q->cap) {
                pthread_mutex_unlock(&q->mu);
                return pdFALSE;
            }
        }
    }
    size_t slot = (q->head + q->count) % q->cap;
    memcpy(q->buf + slot * q->item_size, item, q->item_size);
    q->count++;
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->mu);
    return pdTRUE;
}

BaseType_t xQueueReceive(QueueHandle_t q, void* item, TickType_t ticks_to_wait) {
    if (q == NULL || item == NULL) return pdFALSE;
    pthread_mutex_lock(&q->mu);
    if (q->count == 0) {
        if (ticks_to_wait == 0) {
            pthread_mutex_unlock(&q->mu);
            return pdFALSE;
        }
        if (ticks_to_wait == portMAX_DELAY) {
            while (q->count == 0) pthread_cond_wait(&q->not_empty, &q->mu);
        } else {
            struct timespec ts;
            abs_deadline(&ts, ticks_to_wait);
            while (q->count == 0) {
                if (pthread_cond_timedwait(&q->not_empty, &q->mu, &ts) == ETIMEDOUT) break;
            }
            if (q->count == 0) {
                pthread_mutex_unlock(&q->mu);
                return pdFALSE;
            }
        }
    }
    memcpy(item, q->buf + q->head * q->item_size, q->item_size);
    q->head = (q->head + 1) % q->cap;
    q->count--;
    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->mu);
    return pdTRUE;
}

void vQueueDelete(QueueHandle_t q) {
    if (q == NULL) return;
    pthread_mutex_destroy(&q->mu);
    pthread_cond_destroy(&q->not_empty);
    pthread_cond_destroy(&q->not_full);
    free(q->buf);
    free(q);
}
