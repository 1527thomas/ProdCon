#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <memory.h>
#include "prodcon.h"
#include <pthread.h>

//initialize thread lock and condition variables
pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t empty = PTHREAD_COND_INITIALIZER;

struct llist_node {
    struct llist_node *next;
    char *str;
};

/**
 * pop a node off the start of the list.
 *
 * @param phead the head of the list. this will be modified by the call unless the list is empty
 * (*phead == NULL).
 * @return NULL if list is empty or a pointer to the string at the top of the list. the caller is
 * incharge of calling free() on the pointer when finished with the string.
 */
char *pop(struct llist_node **phead)
{
    if (*phead == NULL) {
        return NULL;
    }
    char *s = (*phead)->str;
    struct llist_node *next = (*phead)->next;
    free(*phead);
    *phead = next;
    return s;
}

/**
 * push a node onto the start of the list. a copy of the string will be made.
 * @param phead the head of the list. this will be modified by this call to point to the new node
 * being added for the string.
 * @param s the string to add. a copy of the string will be made and placed at the beginning of
 * the list.
 */
void push(struct llist_node **phead, const char *s)
{
    struct llist_node *new_node = malloc(sizeof(*new_node));
    new_node->next = *phead;
    new_node->str = strdup(s);
    *phead = new_node;
}

// the array of list heads. the size should be equal to the number of consumers
static struct llist_node **heads;

static assign_consumer_f assign_consumer;
static int producer_count;
static int consumer_count;

static int my_consumer_number;

void queue(int consumer, const char *str)
{
    push(&heads[consumer], str);
}

void produce(const char *buffer)
{
    int hash = assign_consumer(consumer_count, buffer);
    queue(hash, buffer);
}

char *consume() {
    char *str = pop(&heads[my_consumer_number]);
    return str;
}

void do_usage(char *prog)
{
    printf("USAGE: %s shared_lib consumer_count producer_count ....\n", prog);
    exit(1);
}

//pStruct is the producer structure containing the arguments needed for run_consumer
struct producer_struct {
    int num;
    int producer_count;
    produce_f produce;
    int argc;
    char **argv;
    char *lib;
} pStruct;

//cStruct is the consumer structure containing the arguments needed for run_consumer
struct consumer_struct {
    int num;
    consume_f consume;
    int argc;
    char **argv;
    char *lib;
} cStruct;


void *startProducer(void *_args) {
    pthread_mutex_lock(&lock);
    //load library so producer can run_producer
    void *dh = dlopen(pStruct.lib, RTLD_LAZY);
    run_producer_f run_producer = dlsym(dh, "run_producer");
    run_producer(pStruct.num, pStruct.producer_count, pStruct.produce, pStruct.argc, pStruct.argv);
    pthread_cond_signal(&empty); //signals that the producers are finished
    pthread_mutex_unlock(&lock);
    pthread_exit(NULL);
}

void *startConsumer(void *_args) {
    pthread_mutex_lock(&lock);
    //load library so consumer can run_consumer
    void *dh = dlopen(cStruct.lib, RTLD_LAZY);
    run_consumer_f run_consumer = dlsym(dh, "run_consumer");
    run_consumer(cStruct.num, cStruct.consume, cStruct.argc, cStruct.argv);

    pthread_cond_signal(&empty); //signals that consumers are finished
    pthread_mutex_unlock(&lock);
    pthread_exit(NULL);
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        do_usage(argv[0]);
    }

    char *shared_lib = argv[1];
    producer_count = strtol(argv[2], NULL, 10);
    consumer_count = strtol(argv[3], NULL, 10);

    char **new_argv = &argv[4];
    int new_argc = argc - 4;
    setlinebuf(stdout);

    if (consumer_count <= 0 || producer_count <= 0) {
        do_usage(argv[0]);
    }

    void *dh = dlopen(shared_lib, RTLD_LAZY);

    // load the producer, consumer, and assignment functions from the library
    run_producer_f run_producer = dlsym(dh, "run_producer");
    run_consumer_f run_consumer = dlsym(dh, "run_consumer");
    assign_consumer = dlsym(dh, "assign_consumer");
    if (run_producer == NULL || run_consumer == NULL || assign_consumer == NULL) {
        printf("Error loading functions: prod %p cons %p assign %p\n", run_producer,
                run_consumer, assign_consumer);
        exit(2);
    }

    heads = calloc(consumer_count, sizeof(*heads));

    //Create array of threads for producer and consumer
    pthread_t pThread[producer_count];
    pthread_t cThread[consumer_count];

    const char *sentinel = "S";

    //start up threads for each producer
    for (int i = 0; i < producer_count; i++) {
	pStruct.argc = new_argc;
   	pStruct.producer_count = producer_count;
	pStruct.argv = new_argv;
	pStruct.produce = produce;
	pStruct.lib = shared_lib;
	pStruct.num = i;

	pthread_create(&pThread[i],  NULL, startProducer, &pStruct);
    	pthread_join(pThread[i], NULL);

    }

    //start up threads for consumers
    for (int i = 0; i < consumer_count; i++) {
	//push sentinels to head of consumer list
	push(heads, sentinel);
	cStruct.argc = new_argc;
        cStruct.argv = new_argv;
        cStruct.consume = consume;
        cStruct.lib = shared_lib;
	cStruct.num = i;
	my_consumer_number = i;

	//checks if end of list is the sentinel
	//if it is, then signal the condition that the consumer is done
	if(&cThread[i] != NULL) {
	    char *check = pop(heads);
	    if(strcmp(check, sentinel) == 0) {
		pthread_cond_signal(&empty);
	    }
	}

	pthread_create(&cThread[i], NULL, startConsumer, &cStruct);
   	pthread_join(pThread[i], NULL);
    }

    return 0;
}
