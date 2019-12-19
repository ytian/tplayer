#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <execinfo.h>
#include <signal.h>

/* Obtain a backtrace and print it to stdout. */
#define ARRAY_SIZE(x) (sizeof(x) / sizeof(x[0]))
void dump_stack(void)
{
    void *array[30] = {0};
    size_t size = backtrace(array, ARRAY_SIZE(array));
    char **strings = backtrace_symbols(array, size);
    size_t i;

    if (strings == NULL)
    {
        perror("backtrace_symbols.");
        exit(EXIT_FAILURE);
    }

    printf("Obtained %zd stack frames.\n", size);

    for (i = 0; i < size; i++)
        printf("%s\n", strings[i]);

    free(strings);
    strings = NULL;

    exit(EXIT_SUCCESS);
}

void sighandler_dump_stack(int sig)
{
    psignal(sig, "handler");
    dump_stack();
    signal(sig, SIG_DFL);
    raise(sig);
}


//线程安全队列
typedef struct SafeElem
{
    void *data;
    struct SafeElem *next;
} SafeElem;

typedef struct
{
    SafeElem *head;
    SafeElem *tail;
    int size;
    int capacity;
    pthread_mutex_t mutex;
} SafeQueue;

SafeQueue *mkqueue(int capacity)
{
    SafeQueue *q = (SafeQueue *)malloc(sizeof(SafeQueue));
    q->head = NULL;
    q->tail = NULL;
    q->size = 0;
    q->capacity = capacity;
    int ret = pthread_mutex_init(&q->mutex, NULL);
    if (ret != 0)
    {
        printf("mutex error(%d)!\n", ret);
        return NULL;
    }
    return q;
}

void *dequeue(SafeQueue *q)
{
    pthread_mutex_lock(&q->mutex);
    if (q->size == 0)
    {
        pthread_mutex_unlock(&q->mutex);
        return NULL;
    }
    SafeElem *elem = q->head;
    q->head = q->head->next;
    if (q->head == NULL)
    {
        q->tail = NULL;
    }
    q->size--;
    void *data = elem->data;
    free(elem);
    pthread_mutex_unlock(&q->mutex);
    return data;
}

int qsize(SafeQueue *q) {
    int size;
    pthread_mutex_lock(&q->mutex);
    size = q->size;
    pthread_mutex_unlock(&q->mutex);
    return size;
}

int enqueue(SafeQueue *q, void *data)
{
    pthread_mutex_lock(&q->mutex);
    if (q->size == q->capacity)
    {
        pthread_mutex_unlock(&q->mutex);
        return -1;
    }
    SafeElem *elem = (SafeElem *)malloc(sizeof(SafeElem));
    elem->data = data;
    elem->next = NULL;
    if (q->head == NULL)
    {
        q->head = elem;
    }
    else
    {
        q->tail->next = elem; 
    }
    q->tail = elem;
    q->size++;
    pthread_mutex_unlock(&q->mutex);
    return 0;
}
