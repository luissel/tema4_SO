#include "util/so_scheduler.h"
#include "util/utils.h"
#include <stdio.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdlib.h>

#define MAXSIZE 1000
#define TRUE 1
#define FALSE 0
#define NEW 0
#define READY 1
#define RUNNING 2
#define WAITING 3
#define TERMINATED 4

typedef struct {
	tid_t tid;
	int priority;
	int time;
	int status;

    so_handler *handler;
	sem_t thread_sem;
} thread;

typedef struct {
	int initialized;
	unsigned int io_events;
	int quantum;
	int threads_no;
	int queue_size;

	thread *running_thread;
	thread **threads;
	thread **prio_queue;
} scheduler;

static void add_in_queue(thread *thread);
static void thread_start(void *args);
static void call_scheduler(void);
static void delete_from_queue(void);
static void run_next_thread(void);
static int find_next(void);

static scheduler my_scheduler;

int so_init(unsigned int time_quantum, unsigned int io)
{
	/*
	 * Ma asigur ca planificatorul nu a fost deja initializat si ca
	 * parametrii sunt valizi.
	 */
	if (my_scheduler.initialized || time_quantum < 1 ||
			io > SO_MAX_NUM_EVENTS)
		return -1;

	/*
	 * Initializez structura planificatorului si aloc memorie pentru
	 * vectorul de thread-uri si pentru coada.
	 */
	my_scheduler.initialized = TRUE;
	my_scheduler.io_events = io;
	my_scheduler.threads_no = 0;
	my_scheduler.queue_size = 0;
	my_scheduler.quantum = time_quantum;
	my_scheduler.running_thread = NULL;

	my_scheduler.threads = malloc(sizeof(thread *) * MAXSIZE);
	my_scheduler.prio_queue = malloc(sizeof(thread *) * MAXSIZE);

	return 0;
}

tid_t so_fork(so_handler *func, unsigned int priority)
{
	thread *new_thread;
	int ret;

	/*
	 * Verfic parametrii sa fie valizi
	 */
	if (!func || priority > SO_MAX_PRIO)
		return INVALID_TID;

	/*
	 * Creez un thread nou si il initializez
	 */
	new_thread = malloc(sizeof(thread));
	DIE(new_thread == NULL, "malloc error");

	new_thread->tid = INVALID_TID;
	new_thread->status = NEW;
	new_thread->time = my_scheduler.quantum;
	new_thread->priority = priority;
	new_thread->handler = func;

	ret = sem_init(&new_thread->thread_sem, 0, 0);
	DIE(ret != 0, "sem init");

	/*
	 * Pornesc thread-ul
	 */
	ret = pthread_create(&new_thread->tid, NULL, (void *)thread_start,
							(void *)new_thread);
	DIE(ret != 0, "new thread create");

	/*
	 * Adaug noul thread in coada cu prioritati si in vectorul de
	 * thread-uri.
	 */
	add_in_queue(new_thread);

	my_scheduler.threads[my_scheduler.threads_no] = new_thread;
	my_scheduler.threads_no++;

	/*
	 * Daca nu exista niciun thread care ruleaza, atunci il alegem pe
	 * urmatorul
	 */
	if (!my_scheduler.running_thread)
		run_next_thread();
	/*
	 * Daca ruleaza deja un thread, atunci consuma timp si verificam daca
	 * trebuie sa fie preemptat
	 */
	else
		so_exec();

	return new_thread->tid;
}

int so_wait(unsigned int io)
{
	int ret;

	/*
	 * Verfic parametrii sa fie valizi
	 */
	if (io >= my_scheduler.io_events)
		return -1;

	/*
	 * Actualizez starea thread-ului pentru a-l bloca
	 * si pentru a planifica urmatorul thread
	 */
	my_scheduler.running_thread->status = WAITING;

	call_scheduler();

	ret = sem_wait(&my_scheduler.running_thread->thread_sem);
	DIE(ret != 0, "thread sem wait");

	return 0;
}

int so_signal(unsigned int io)
{
	/*
	 * Verfic parametrii sa fie valizi
	 */
	if (io >= my_scheduler.io_events)
		return -1;

	return 0;
}

void so_exec(void)
{
	int ret;
	thread *current_thread = my_scheduler.running_thread;

	/*
	 * Scad timpul thread-ului aflat in executie si apelez
	 * call_scheduler pentru a planifica care este urmatorul
	 * thread care va rula.
	 */
	current_thread->time--;
	call_scheduler();

	ret = sem_wait(&current_thread->thread_sem);
	DIE(ret != 0, "thread sem wait");
}

/*
 * Asteapta terminarea tututor thread-urilor, distruge semafoarele acestora
 * si elibereaza memoria alocata.
 */
void so_end(void)
{
	int i, ret;

	if (my_scheduler.initialized == FALSE)
		return;

	for (i = 0; i < my_scheduler.threads_no; i++) {
		ret = pthread_join(my_scheduler.threads[i]->tid, NULL);
		DIE(ret != 0, "thread join");
	}

	for (i = 0; i < my_scheduler.threads_no; i++) {
		ret = sem_destroy(&my_scheduler.threads[i]->thread_sem);
		DIE(ret != 0, "thread sem destroy");

		free(my_scheduler.threads[i]);
	}

	for (i = 0; i < my_scheduler.queue_size; i++)
		free(my_scheduler.prio_queue[i]);

	free(my_scheduler.threads);
	free(my_scheduler.prio_queue);
	my_scheduler.initialized = FALSE;
}

/*
 * Functia care determina contextul în care se va executa noul thread
 */
static void thread_start(void *args)
{
	int ret;
	thread *current_thread = (thread *)args;

	/*
	 * Asteapta thread-ul sa fie planificat si apoi executa
	 * handler(priority)
	 */
	ret = sem_wait(&current_thread->thread_sem);
	DIE(ret != 0, "sem wait");

	current_thread->handler(current_thread->priority);

	/*
	 * Actualizez starea thread-ului
	 */
	current_thread->status = TERMINATED;

	/*
	 * Actualizez planificatorul
	 */
	call_scheduler();
}

/*
 * Planifica urmatorul thread
 */
static void call_scheduler(void)
{
	int ret;
	thread *current_thread;

	current_thread = my_scheduler.running_thread;

	/*
	 * In cazul in care nu exista alte thread-uri in coada sau nu exista
	 * alt thread care sa fie pus in executie, eliberez semaforul
	 * thread-ului curent pentru pentru ca acesta sa poata rula
	 */
	if ((current_thread && my_scheduler.queue_size == 0) || !find_next()) {
		/*
		 * Daca a expirat timpul thread-ului curent, il resetez.
		 */
		if (current_thread->time <= 0)
			current_thread->time = my_scheduler.quantum;

		ret = sem_post(&current_thread->thread_sem);
		DIE(ret != 0, "thread sem post");
	}
}

/*
 * Intoarce TRUE si pune in executie urmatorul thread, daca exista
 * Altfel, intoarce FALSE.
 */
static int find_next(void)
{
	thread *next_thread, *current_thread;

	current_thread = my_scheduler.running_thread;
	next_thread = my_scheduler.prio_queue[0];

	/*
	 * Daca thread-ul curent este blocat sau a terminat, atunci rulez
	 * urmatorul thread.
	 */
	if (current_thread->status == WAITING ||
			current_thread->status == TERMINATED) {
		run_next_thread();
		return TRUE;
	}

	/*
	 * Daca este un thread cu prioritatea mai mare,
	 * thread-ul curent este preemptat si urmatorul thread
	 * este executat.
	 */
	if (next_thread->priority > current_thread->priority) {
		run_next_thread();
		add_in_queue(current_thread);
		return TRUE;
	}

	/*
	 * Daca a expirat timpul thread-ului curent si exista
	 * un thread cu aceeasi prioritate, acesta este adaugat
	 * din nou in coada iar urmatorul thread este executat.
	 */
	if (current_thread->time <= 0) {
		if (current_thread->priority == next_thread->priority) {
			run_next_thread();
			add_in_queue(current_thread);
			return TRUE;
		}
	}

	return FALSE;
}

/*
 * Executa thred-ul cu prioritatea cea mai mare
 */
static void run_next_thread(void)
{
	int ret;
	thread *next_thread = my_scheduler.prio_queue[0];

	my_scheduler.running_thread = next_thread;
	/*
	 * Se elimina thread-ul din coada, se retine trecerea
	 * in starea RUNNING si se reseteaza timpul.
	 */
	delete_from_queue();

	next_thread->status = RUNNING;
	next_thread->time = my_scheduler.quantum;

	/*
	 * Se elibereaza semaforul thread-ului, pentru a putea rula
	 */
	ret = sem_post(&next_thread->thread_sem);
	DIE(ret != 0, "thread sem post");
}

/*
 * Elimin elementul de pe prima pozitie si le shiftez pe restul.
 */
static void delete_from_queue(void)
{
	int i;

	for (i = 0; i < my_scheduler.queue_size - 1; ++i)
		my_scheduler.prio_queue[i] = my_scheduler.prio_queue[i + 1];

	my_scheduler.prio_queue[my_scheduler.queue_size - 1] = NULL;
	my_scheduler.queue_size--;
}

/*
 * Adauga thread-ul in coada (ordonata descrescator in functie prioritate):
 * inainte de thread-ul care are prioritatea mai mica decat a lui
 */
static void add_in_queue(thread *thread)
{
	int i, j = 0;

	for (i = 0; i < my_scheduler.queue_size; ++i, ++j)
		if (my_scheduler.prio_queue[i]->priority < thread->priority)
			break;

	if (j != my_scheduler.queue_size)
		for (i = my_scheduler.queue_size; i > j; --i)
			my_scheduler.prio_queue[i] =
			my_scheduler.prio_queue[i - 1];

	my_scheduler.queue_size++;
	my_scheduler.prio_queue[j] = thread;
	my_scheduler.prio_queue[j]->status = READY;
}
