/*
 * rt-migrate-test.c
 *
 * Copyright (C) 2007-2009 Steven Rostedt <srostedt@redhat.com>
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License (not later!)
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 */
#define _GNU_SOURCE
#include <stdio.h>
#ifndef __USE_XOPEN2K
# define __USE_XOPEN2K
#endif
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <stdarg.h>
#include <unistd.h>
#include <ctype.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/time.h>
#include <linux/unistd.h>
#include <sys/syscall.h>
#include <errno.h>
#include <sched.h>
#include <pthread.h>

#define gettid() syscall(__NR_gettid)

int nr_tasks;
int nr_locks;
int sleep_time = 5;
int diffprio = 1;
int nopi;
int numprio;
int can_migrate = 1;
int cpus;

static int read_ctx_switches(int pid, int *vol, int *nonvol, int *migrate);

static int mark_fd = -1;
static __thread char buff[BUFSIZ+1];
static int **threadmem;

static void setup_ftrace_marker(void)
{
	struct stat st;
	char *files[] = {
		"/sys/kernel/debug/tracing/trace_marker",
		"/debug/tracing/trace_marker",
		"/debugfs/tracing/trace_marker",
	};
	int ret;
	int i;

	for (i = 0; i < (sizeof(files) / sizeof(char *)); i++) {
		ret = stat(files[i], &st);
		if (ret >= 0)
			goto found;
	}
	/* todo, check mounts system */
	return;
found:
	mark_fd = open(files[i], O_WRONLY);
}

static void ftrace_write(const char *fmt, ...)
{
	va_list ap;
	int n;

	if (mark_fd < 0)
		return;

	va_start(ap, fmt);
	n = vsnprintf(buff, BUFSIZ, fmt, ap);
	va_end(ap);

	write(mark_fd, buff, n);
}

#define nano2sec(nan) (nan / 1000000000ULL)
#define nano2ms(nan) (nan / 1000000ULL)
#define nano2usec(nan) (nan / 1000ULL)
#define usec2nano(sec) (sec * 1000ULL)
#define ms2nano(ms) (ms * 1000000ULL)
#define sec2nano(sec) (sec * 1000000000ULL)
#define RUN_INTERVAL ms2nano(1ULL)
#define NR_RUNS 50
#define PRIO_START 2

#define PROGRESS_CHARS 70

static int nr_runs = NR_RUNS;
static int prio_start = PRIO_START;

static unsigned long long now;
static unsigned long long end;

static pthread_barrier_t start_barrier;
static pthread_barrier_t end_barrier;
static pthread_mutex_t *locks;
static unsigned long long *lock_time;
static unsigned long long **lock_wait_time;
static int *iterations;
static int *loops;

static int *vol_switches;
static int *nonvol_switches;
static int *migrated;

static long *thread_pids;

static int done;

static char buffer[BUFSIZ];

static void perr(char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buffer, BUFSIZ, fmt, ap);
	va_end(ap);

	perror(buffer);
	fflush(stderr);
	exit(-1);
}

static void usage(char **argv)
{
	char *arg = argv[0];
	char *p = arg+strlen(arg);

	while (p >= arg && *p != '/')
		p--;
	p++;

	printf("Usage:\n"
	       "%s <options> nr_tasks\n\n"
	       "-p prio --prio  prio        base priority to start RT tasks with (2) \n"
	       "-l locks --locks locks      number of locks to have\n"
	       "-r time --run-time time     Run time (secs)\n"
	       "-o #  --limitprio #      Limit RT priority tasks to #\n"
	       "-s    --same                Keep threads at same prio\n"
	       "-n    --nopi                Do not use prio inheritance mutexes\n"
	       "-m    --nomigrate           CPU/2 low prio tasks are pinned to CPU\n"
	       "  () above are defaults \n",
		p);
	exit(0);
}

static void parse_options (int argc, char *argv[])
{
	for (;;) {
		int option_index = 0;
		/** Options for getopt */
		static struct option long_options[] = {
			{"prio", required_argument, NULL, 'p'},
			{"time", required_argument, NULL, 'r'},
			{"locks", required_argument, NULL, 'l'},
			{"same", no_argument, NULL, 's'},
			{"nopi", no_argument, NULL, 'n'},
			{"limitprio", no_argument, NULL, 'o'},
			{"nomigrate", no_argument, NULL, 'm'},
			{"help", no_argument, NULL, '?'},
			{NULL, 0, NULL, 0}
		};
		int c = getopt_long (argc, argv, "p:r:l:o:snmh",
			long_options, &option_index);
		if (c == -1)
			break;
		switch (c) {
		case 'p': prio_start = atoi(optarg); break;
		case 'l': nr_locks = atoi(optarg); break;
		case 'r': sleep_time = atoi(optarg); break;
		case 'o': numprio = atoi(optarg); break;
		case 's': diffprio = 0; break;
		case 'n': nopi = 1; break;
		case 'm': can_migrate = 0; break;
		case '?':
		case 'h':
			usage(argv);
			break;
		}
	}

}

static unsigned long long get_time(void)
{
	struct timeval tv;
	unsigned long long time;

	gettimeofday(&tv, NULL);

	time = sec2nano(tv.tv_sec);
	time += usec2nano(tv.tv_usec);

	return time;
}

static unsigned long busy_loop(int x, int id)
{
	unsigned long long start_time;
	unsigned long long time;
	unsigned long l = 0, i;
	/*
	 * 5KB buffer, 3 arrays, 4 bytes an array
	 */
	int nr_int = (5 * 1024) / (4 * 3);
	int *tmem = threadmem[id];

	if (x <= 0)
		return 0;

	start_time = get_time();
	do {
		/* Number of times we were able to process arrays */
		for (i = 0; i < nr_int; i++) {
			tmem[i] = tmem[nr_int + i] + tmem[(2 * nr_int) + i];
		}
		l++;
		time = get_time();
	} while ((time - start_time) < RUN_INTERVAL * (x + 1));

	return l;
}

static void ms_sleep(int ms)
{
	struct timespec ts;
	unsigned long sec = 0;
	unsigned long nsec;

	if (ms <= 0)
		return;

	if (ms > 1000) {
		sec = ms / 1000;
		ms -= sec * 100;
	}

	nsec = ms * 1000000;
	ts.tv_sec = sec;
	ts.tv_nsec = nsec;

	nanosleep(&ts, NULL);
}

static void print_results(void)
{
	long total_vol = 0;
	long total_nonvol = 0;
	long total_migrate = 0;
	long total_iterations = 0;
	long total_loops = 0;
	int i;

	printf("Task        vol    nonvol   migrated     iterations    loops\n"
	       "----        ---    ------   --------     ----------    -----\n");

	for (i = 0; i < nr_tasks; i++) {
		printf("  %d:     %6d  %8d   %8d       %8d     %5d\n",
		       i, vol_switches[i], nonvol_switches[i],
		       migrated[i], iterations[i], loops[i]);
		total_vol += vol_switches[i];
		total_nonvol += nonvol_switches[i];
		total_migrate += migrated[i];
		total_iterations += iterations[i];
		total_loops += loops[i];
	}
	printf("\ntotal:   %6ld   %8ld  %8ld       %8ld     %5ld\n",
	       total_vol, total_nonvol, total_migrate,
	       total_iterations, total_loops);
}

static int grab_lock(long id, int iter, int l)
{
	unsigned long long try_time;
	unsigned long long time;
	unsigned long long delta;
	int ret;

	ftrace_write("thread %ld iter %d, taking lock %d\n",
		     id, iter, l);
	try_time = get_time();
	pthread_mutex_lock(&locks[l]);
	time = get_time();
	delta = time - try_time;
	if (iter < nr_runs)
		lock_wait_time[l][iter] = time - try_time;
	ftrace_write("thread %ld iter %d, took lock %d in %llu us\n",
		     id, iter, delta / 1000);

	ret = busy_loop(nr_tasks - id, id);

	ftrace_write("thread %ld iter %d, unlock lock %d\n",
		     id, iter, l);

	pthread_mutex_unlock(&locks[l]);

	return ret;
}

void *start_task(void *data)
{
	long id = (long)data;
	unsigned long long start_time;
	struct sched_param param = {
		.sched_priority = id * diffprio + prio_start,
	};
	int ret;
	long pid;
	int i, l, lp;

	pid = gettid();

	if (!numprio || id >= (nr_tasks - numprio)) {
		ret = sched_setscheduler(0, SCHED_FIFO, &param);

		if (ret < 0 && !id)
			fprintf(stderr, "Warning, can't set priorities\n");
	}

	/* some processes may need to be pinned */
	if (!can_migrate && id < cpus) {
		cpu_set_t cpumask;
		int cpu;

		/* if possible, do not pin the lowest task */
		if (!id && nr_tasks > 2)
			goto skip;

		if (nr_tasks < 3)
			cpu = id;
		else
			cpu = id - 1;

		if (cpu >= cpus/2)
			goto skip;

		CPU_ZERO(&cpumask);
		CPU_SET(id, &cpumask);
		/* bind to CPU */
		sched_setaffinity(0, sizeof(cpumask), &cpumask);
	}
 skip:

	pthread_barrier_wait(&start_barrier);
	start_time = get_time();
	ftrace_write("Thread %d: started %lld diff %lld\n",
		     pid, start_time, start_time - now);
	i = 0;
	lp = 0;
	while (!done) {
		for (l = 0; l < nr_locks; l++) {
			lp += grab_lock(id, i, l);
			ftrace_write("thread %ld iter %d sleeping\n",
				     id, i);
			ms_sleep(id);
		}
		i++;
	}

	loops[id] = lp;
	iterations[id] = i;
	read_ctx_switches(gettid(), &vol_switches[id], &nonvol_switches[id],
			  &migrated[id]);

	pthread_barrier_wait(&end_barrier);

	return (void*)pid;
}

static void stop_log(int sig)
{
	done = 1;
}

static int get_value(const char *line)
{
	const char *p;

	for (p = line; isspace(*p); p++)
		;
	if (*p != ':')
		return -1;
	p++;
	for (; isspace(*p); p++)
		;
	return atoi(p);
}

static int update_value(const char *line, int *val, const char *name)
{
	int ret;

	if (strncmp(line, name, strlen(name)) == 0) {
		ret = get_value(line + strlen(name));
		if (ret < 0)
			return 0;
		*val = ret;
		return 1;
	}
	return 0;
}

static int read_ctx_switches(int pid, int *vol, int *nonvol, int *migrate)
{
	static int vol_once, nonvol_once;
	const char *vol_name = "nr_voluntary_switches";
	const char *nonvol_name = "nr_involuntary_switches";
	const char *migrate_name = "se.nr_migrations";
	char file[1024];
	char buf[1024];
	char *pbuf;
	size_t *pn;
	size_t n;
	FILE *fp;
	int r;

	snprintf(file, 1024, "/proc/%d/sched", pid);
	fp = fopen(file, "r");
	if (!fp) {
		snprintf(file, 1024, "/proc/%d/status", pid);
		fp = fopen(file, "r");
		if (!fp)
			perr("could not open %s", file);
		vol_name = "voluntary_ctxt_switches";
		nonvol_name = "nonvoluntary_ctxt_switches";
	}

	*vol = *nonvol = *migrate = -1;

	n = 1024;
	pn = &n;
	pbuf = buf;

	while ((r = getline(&pbuf, pn, fp)) >= 0) {

		if (update_value(buf, vol, vol_name))
			continue;

		if (update_value(buf, nonvol, nonvol_name))
			continue;

		if (update_value(buf, migrate, migrate_name))
			continue;
	}
	fclose(fp);

	if (!vol_once && *vol == -1) {
		vol_once++;
		fprintf(stderr, "Warning, could not find voluntary ctx switch count\n");
	}
	if (!nonvol_once && *nonvol == -1) {
		nonvol_once++;
		fprintf(stderr, "Warning, could not find nonvoluntary ctx switch count\n");
	}

	return 0;
}

static int count_cpus(void)
{
	return sysconf(_SC_NPROCESSORS_ONLN);
}


static void *__do_malloc(int size, const char *str)
{
	void *ret;

	ret = malloc(size);
	if (!ret)
		perr("malloc %s", str);
	memset(ret, 0, size);
	return ret;
}

#define do_malloc(obj, size)			\
	__do_malloc(size * sizeof(*obj), #obj)

int main (int argc, char **argv)
{
	pthread_t *threads;
	long i;
	int ret;
	struct sched_param param;
	pthread_mutexattr_t attr;
	pthread_mutexattr_t *pattr;

	parse_options(argc, argv);

	signal(SIGINT, stop_log);

	cpus = count_cpus();

	if (cpus < 2)
		perr("Test must be run on SMP box (more than 1 CPU)");

	if (argc >= (optind + 1))
		nr_tasks = atoi(argv[optind]);
	else
		nr_tasks = cpus * 2;

	if (!nr_locks)
		nr_locks = cpus;

	if (nr_tasks < 1)
		perr("Need at least 1 task to run");

	threads = do_malloc(threads, nr_tasks);
	locks = do_malloc(locks, nr_locks);
	iterations = do_malloc(iterations, nr_tasks);
	loops = do_malloc(loops, nr_tasks);

	if (nopi)
		pattr = NULL;
	else {
		if (pthread_mutexattr_init(&attr))
			perr("pthread_mutexattr_init");
		if (pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT))
			perr("pthread_mutexattr_setprotocol");
		pattr = &attr;
	}

	for (i = 0; i < nr_locks; i++) {
		ret = pthread_mutex_init(&locks[i], pattr);
		if (ret < 0)
			perr("pthread_mutex_init");
	}

	lock_time = do_malloc(lock_time, nr_locks);

	lock_wait_time = do_malloc(lock_wait_time, nr_locks);
	for (i = 0; i < nr_locks; i++) {
		lock_wait_time[i] = malloc(sizeof(**lock_wait_time) * nr_runs);
		if (!lock_wait_time[i])
			perr("malloc");
		memset(lock_wait_time[i], 0, sizeof(**lock_wait_time) * nr_runs);
	}

	vol_switches = do_malloc(vol_switches, nr_tasks);
	nonvol_switches = do_malloc(nonvol_switches, nr_tasks);
	migrated = do_malloc(migrated, nr_tasks);
	threadmem = do_malloc(threadmem, nr_tasks);

	ret = pthread_barrier_init(&start_barrier, NULL, nr_tasks + 1);
	if (ret < 0)
		perr("pthread_barrier_init");
	ret = pthread_barrier_init(&end_barrier, NULL, nr_tasks + 1);
	if (ret < 0)
		perr("pthread_barrier_init");


	thread_pids = malloc(sizeof(long) * nr_tasks);
	if (!thread_pids)
		perr("malloc thread_pids");

	for (i=0; i < nr_tasks; i++) {
		if (pthread_create(&threads[i], NULL, start_task, (void *)i))
			perr("pthread_create");

		/*
		 * 8 tasks, 2 tasks per CPU, 32KB per l1-cache, 16 kb per task,
		 * each task has 3 buffers ~5.33kb, Assume 5kb. 
		 */
		threadmem[i] = aligned_alloc(4096, 1024 * 5);
		if (!threadmem[i]) {
			perr("threadmem %d alloc error\n", i);
		}
	}

	/* up our prio above all tasks */
	memset(&param, 0, sizeof(param));
	param.sched_priority = nr_tasks + prio_start;
	if (sched_setscheduler(0, SCHED_FIFO, &param))
		fprintf(stderr, "Warning, can't set priority of main thread!\n");


	setup_ftrace_marker();

	pthread_barrier_wait(&start_barrier);
	ftrace_write("All running!!!\n");

	sleep(sleep_time);
	end = get_time();
	ftrace_write("End=%lld now=%lld diff=%lld\n", end, end - now);

	done = 1;

	pthread_barrier_wait(&end_barrier);
	putc('\n', stderr);

	for (i=0; i < nr_tasks; i++) {
		pthread_join(threads[i], (void*)&thread_pids[i]);
		free(threadmem[i]);
	}

	print_results();

	return 0;
}
