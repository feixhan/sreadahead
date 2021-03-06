/*
 * (C) Copyright 2008 Intel Corporation
 *
 * Author: Arjan van de Ven <arjan@linux.intel.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; version 2
 * of the License.
 */
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/times.h>
#include <string.h>
#include <pthread.h>

#include <fcntl.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <sys/mount.h>
#include <sys/signal.h>
#include <fcntl.h>
#include <errno.h>

#include <getopt.h>

#define VERSION "1.0"

#undef HAVE_IO_PRIO
#if defined(__i386__)
#  define HAVE_IO_PRIO
#  define __NR_ioprio_set 289
#elif defined(__x86_64__)
#  define HAVE_IO_PRIO
#  define __NR_ioprio_set 251
#elif defined(__powerpc__)
#  define HAVE_IO_PRIO
#  define __NR_ioprio_set 273
#else /* not fatal */
#  warning "Architecture does not support ioprio modification"
#endif
#define IOPRIO_WHO_PROCESS 1
#define IOPRIO_CLASS_IDLE 3
#define IOPRIO_CLASS_SHIFT 13
#define IOPRIO_IDLE_LOWEST (7 | (IOPRIO_CLASS_IDLE << IOPRIO_CLASS_SHIFT))

#define PACK_PATH	"/var/lib/sreadahead"
#define DEBUGFS_MNT	"/var/lib/sreadahead/debugfs"
#define PACK_FILE	"/var/lib/sreadahead/pack"

#define MAXR 40000	/* trace file can be long */
#define MAXFL 128
#define MAXRECS 6	/* reduce nr of fragments to this amount */

#define DEFAULT_MAX_TIME 15 /* should be enough for every OS to boot */

/*
 * By default, the kernel reads ahead for 128kb. This throws off our
 * measurements since we don't need the extra 128kb for each file.
 * On top of that, at the accelerated boot, we would be reading another
 * 128kb too much potentially, wasting a lot of time.
 *
 * By lowering the read_ahead_kb, we get more fragments (since they
 * are not glued together by the artifical kernel readahead). So
 * lowering this number too much doesn't actually gain much.
 *
 * XX kb seems to be a good balance with not too many fragments, but
 * keeping the total size low enough to make a difference.
 *
 * 8-16kb seems to be a good median value, with good total size savings
 * over anything higher. Lower sizes result in more separate blocks
 * and only minimal total size savings.
 */
#define RA_NORMAL 128	/* default read_ahead_kb size */
#define RA_SMALL  16	/* our tuned down value */

struct ra_record {
	uint32_t		offset;
	uint32_t		len;
};

/* disk format used, when reading pack */
struct ra_disk {
	char			filename[MAXFL];
	struct ra_record	data[MAXRECS];
};

/* memory format used with sorting/filtering */
struct ra_struct {
	char			filename[MAXFL];
	struct ra_record	data[MAXRECS];
	struct ra_struct	*next;
	struct ra_struct	*prev;
	int			number;
};

static struct ra_struct *ra[MAXR];
static struct ra_disk rd[MAXR];
static struct ra_struct *first_ra;
static int racount = 0;
static int rdcount = 0;
static int fcount = 0;
static int rdsize = 0;

static unsigned int total_files = 0;
static unsigned int cursor = 0;

static int debug = 0;


static void readahead_set_len(int size)
{
	int unmount;
	int i = 0;
	char ractl[100];
	/* changes readahead size to "size" for local block devices */

	unmount = chdir("/sys/block");
	if (unmount != 0) {
		if (mount("sysfs", "/sys", "sysfs", 0, NULL) != 0) {
			perror("Unable to mount sysfs\n");
			/* non-fatal */
			return;
		}
		chdir("/sys/block");
	}

	sprintf(ractl, "sda/queue/read_ahead_kb");
	while (i <= 3) {
		/* check first 4 sata discs */
		FILE *file = fopen(ractl, "w");
		if (file) {
			fprintf(file, "%d", size);
			fclose(file);
		}
		ractl[2]++; /* a -> b, etc */
		i++;
	}

	chdir("/");

	if (unmount != 0)
		umount("/sys");
}

static void readahead_one(int index)
{
	int fd;
	int i;
	char buf[128];

	fd = open(rd[index].filename, O_RDONLY|O_NOATIME);
	if (fd < 0)
		fd = open(rd[index].filename, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "%s: open failed (%s)\n",
			rd[index].filename, strerror_r(errno, buf, sizeof buf));
		return;
	}

	for (i = 0; i < MAXRECS; i++) {
		if (rd[index].data[i].len)
			readahead(fd, rd[index].data[i].offset,
				  rd[index].data[i].len);
	}
	close(fd);
}

static void *one_thread(void *ptr)
{
	while (1) {
		unsigned int mine;

		mine = __sync_fetch_and_add(&cursor, 1);
		if (mine < total_files)
			readahead_one(mine);
		else
			break;
	}
	return NULL;
}

static void sort_ra_by_name(void)
{
	int delta = 1;

	while (delta > 0) {
		int i;
		delta = 0;
		for (i = 0; i < racount - 1; i++) {
			int c;

			c = strcmp(ra[i]->filename, ra[i+1]->filename);
			if (c > 0) {
				struct ra_struct *tmp;
				tmp = ra[i];
				ra[i] = ra[i+1];
				ra[i+1] = tmp;
				delta++;
			}
		}
	}
}

static void remove_dupes(void)
{
	int i;
	int j;

	for (i = 0; i < racount - 1; i++) {
		for (j = i + 1; j < racount; j++) {
			if (!ra[i])
				break;

			if (strcmp(ra[i]->filename, ra[j]->filename) != 0) {
				i = j - 1;
				break;
			}
			if (ra[j]->next)
				ra[j]->next->prev = ra[j]->prev;
			if (ra[j]->prev)
				ra[j]->prev->next = ra[j]->next;
			free(ra[j]);
			ra[j] = NULL;
		}
	}
}

static int smallest_gap(struct ra_record *record, int count)
{
	int i;
	int cur = 0, maxgap;

	maxgap = 1024*1024*512;
	
	for (i = 0; i < count; i++, record++) {
		if ((i + 1) < count) {
			int gap;
			gap = (record + 1)->offset - record->offset - record->len;
			if (gap < maxgap) {
				maxgap = gap;
				cur = i;
			}
		}
	}
	return cur;
}

static int merge_record(struct ra_record *record, int count, int to_merge)
{
	record[to_merge].len = record[to_merge+1].offset
			       + record[to_merge+1].len - record[to_merge].offset;
	memcpy(&record[to_merge+1], &record[to_merge+2],
		sizeof(struct ra_record) * (count-to_merge - 2));
	return count - 1;
}

static int reduce_blocks(struct ra_record *record, int count, int target)
{
	while (count > target) {
		int tomerge;
		tomerge = smallest_gap(record, count);
		count = merge_record(record, count, tomerge);
	}
	return count;
}

static int get_blocks(struct ra_struct *r)
{
	FILE *file;
	int fd;
	struct stat statbuf;
	void *mmapptr;
	unsigned char *mincorebuf;
	struct ra_record record[4096];
	int rcount = 0;
	int phase;
	uint32_t start = 0;
	int there = 0;
	int notthere = 0;
	int i;

	if (!r)
		return 0;

	file = fopen(r->filename, "r");
	if (!file)
		return 0;

	fd = fileno(file);
	fstat(fd, &statbuf);
	/* prevent accidentally reading from a pipe */
	if (!(S_ISREG(statbuf.st_mode))) {
		fclose(file);
		return 0;
	}

	memset(record, 0, sizeof(record));

	mmapptr = mmap(NULL, statbuf.st_size, PROT_READ, MAP_SHARED, fd, 0);

	mincorebuf = malloc(statbuf.st_size/4096 + 1);
	mincore(mmapptr, statbuf.st_size, mincorebuf);

	if (mincorebuf[0]) {
		phase = 1;
		start = 0;
	} else {
		phase = 0;
	}

	for (i = 0; i <= statbuf.st_size; i += 4096) {
		if (mincorebuf[i / 4096])
			there++;
		else
			notthere++;
		if (phase == 1 && !mincorebuf[i / 4096]) {
			phase = 0;
			if (i > statbuf.st_size)
				i = statbuf.st_size + 1;
			record[rcount].offset = start;
			record[rcount].len = i - 1 - start;
			rcount++;
			if (rcount >= 4000) rcount = 4000;
		} else if (phase == 0 && mincorebuf[i / 4096]) {
			phase = 1;
			start = i;
		}
	}

	if (phase == 1) {
		if (i > statbuf.st_size)
			i = statbuf.st_size + 1;
		record[rcount].offset = start;
		record[rcount].len = i - 1 - start;
		rcount++;
	}

	free(mincorebuf);
	munmap(mmapptr, statbuf.st_size);
	fclose(file);
	
	rcount = reduce_blocks(record, rcount, MAXRECS);
	if (rcount > 0) {
		/* some empty files slip through */
		if (record[0].len == 0)
			return 0;

		if (debug) {
			int tlen = 0;
			int tc = 0;
			while (tc < rcount) {
				tlen += record[tc].len;
				tc++;
				fcount++;
			}
			rdsize += (tlen <= 0 ? 1024 : tlen);
			printf("%s: %d fragment(s), %dkb, %3.1f%%\n",
			       r->filename, rcount,
			       (tlen <= 1024 ? 1024 : tlen) / 1024,
			       100.0 * there / (there + notthere));
		}

		memcpy(r->data, record, sizeof(r->data));
		return 1;
	}
}

static void get_ra_blocks(void)
{
	struct ra_struct *r = first_ra;

	while (r) {
		if (!get_blocks(r)) {
			/* no blocks, remove from list */
			if (r->next)
				r->next->prev = r->prev;
			if (r->prev)
				r->prev->next = r->next;
		}
		r = r->next;
	}
}

static void trace_start(void)
{
	int ret;
	FILE *file;
	char buf[4096];

	/*
	 * at this time during boot we can guarantee that things like
	 * debugfs, sysfs are not mounted yet (at least they should be)
	 * so we mount it temporarily to enable tracing, and umount
	 */
	ret = mount("debugfs", DEBUGFS_MNT, "debugfs", 0, NULL);
	if (ret != 0) {
		perror("Unable to mount debugfs\n");
		exit(EXIT_FAILURE);
	}

	chdir(DEBUGFS_MNT);

	file = fopen("tracing/current_tracer", "w");
	if (!file) {
		perror("Unable to select tracer\n");
		exit(EXIT_FAILURE);
	}
	fprintf(file, "open");
	fclose(file);

	file = fopen("tracing/current_tracer", "r");
	fgets(buf, 4096, file);
	fclose(file);
	if (strcmp(buf, "open\n") != 0) {
		perror("Unable to select open tracer\n");
		exit(EXIT_FAILURE);
	}

	file = fopen("tracing/tracing_enabled", "w");
	if (!file) {
		perror("Unable to enable tracing\n");
		exit(EXIT_FAILURE);
	}
	fprintf(file, "1");
	fclose(file);

	file = fopen("tracing/tracing_enabled", "r");
	fgets(buf, 4096, file);
	fclose(file);
	if (strcmp(buf, "1\n") != 0) {
		perror("Enabling tracing failed\n");
		exit(EXIT_FAILURE);
	}

	chdir("/");

	umount(DEBUGFS_MNT);

	/* set this low, so we don't readahead way too much */
	readahead_set_len(RA_SMALL);
}

static void trace_stop(int signal)
{
	int unmount;
	int ret;
	char buf[4096];
	char filename[4096];
	FILE *file;
	struct ra_struct *r;
	struct tms start_time;
	struct tms stop_time;

	if (debug)
		times(&start_time);

	nice(20);

	/* return readahead size to normal */
	readahead_set_len(RA_NORMAL);

	/*
	 * by now the init process should have mounted debugfs on a logical
	 * location like /sys/kernel/debug, but if not then we temporarily
	 * re-mount it ourselves
	 */
	unmount = chdir("/sys/kernel/debug/tracing");
	if (unmount != 0) {
		ret = mount("debugfs", DEBUGFS_MNT, "debugfs", 0, NULL);
		if (ret != 0) {
			perror("Unable to mount debugfs\n");
			exit(EXIT_FAILURE);
		}
		chdir(DEBUGFS_MNT);
	} else {
		chdir("..");
	}

	/* stop tracing */
	file = fopen("tracing/tracing_enabled", "w");
	if (!file) {
		perror("Unable to disable tracing\n");
		/* non-fatal */
	} else {
		fprintf(file, "0");
		fclose(file);
	}

	file = fopen("tracing/trace", "r");
	if (!file) {
		perror("Unable to open trace file\n");
		exit(EXIT_FAILURE);
	}

	while (fgets(buf, 4095, file) != NULL) {
		char *start;
		char *len;

		if (buf[0] == '#')
			continue;

		start = strchr(buf, '"') + 1;
		if (start == buf)
			continue;

		len = strrchr(start, '"');
		strncpy(filename, start, len - start);

		filename[len - start] = '\0';

		/* ignore sys, dev, proc stuff */
		if (strncmp(filename, "/dev/", 5) == 0)
			continue;
		if (strncmp(filename, "/sys/", 5) == 0)
			continue;
		if (strncmp(filename, "/proc/", 6) == 0)
			continue;

		if (racount >= MAXR) {
			perror("Max records exceeded!");
			break;
		}

		if (strlen(filename) <= MAXFL) {
			struct ra_struct *tmp;
			tmp = malloc(sizeof(struct ra_struct));

			if (!tmp) {
				perror("Out of memory\n");
				exit(EXIT_FAILURE);
			}
			memset(tmp, 0, sizeof(struct ra_struct));

			ra[racount] = tmp;

			strcpy(ra[racount]->filename, filename);
			if (racount > 0) {
				ra[racount]->prev = ra[racount - 1];
				ra[racount - 1]->next = ra[racount];
			}
			ra[racount]->number = racount;
			racount++;
		}
	}
	fclose(file);

	if (debug)
		printf("Trace contained %d records\n", racount);

	first_ra = ra[0];

	chdir("/");
	if (unmount != 0) {
		umount(DEBUGFS_MNT);
	}

	/*
	 * sort and filter duplicates, and get memory blocks
	 */
	sort_ra_by_name();
	remove_dupes();
	get_ra_blocks();

	/*
	 * and write out the new pack file
	 */
	file = fopen(PACK_FILE, "w");
	if (!file) {
		perror("Unable to open output file\n");
		exit(EXIT_FAILURE);
	}

	r = first_ra;
	while (r) {
		fwrite(r->filename, MAXFL, 1, file);
		fwrite(r->data, sizeof(r->data), 1, file);
		r = r->next;
		rdcount++;
	}
	fclose(file);
	if (debug) {
		times(&stop_time);
		printf("Took %.3f seconds\n", (double)(stop_time.tms_utime -
		       start_time.tms_utime) / 1000.0f);
		printf("Total %d files, %dkb, %d fragments\n", rdcount,

		       rdsize / 1024, fcount);
	}

	exit(EXIT_SUCCESS);
}

static void print_usage(const char *name)
{
	printf("Usage: %s [OPTION...]\n", name);
	printf("  -t N, --time=N        Wait for N seconds before creating new\n");
	printf("                        pack file (default %d)\n", DEFAULT_MAX_TIME);
	printf("  -d,   --debug         Print debug output to stdout\n");
	printf("  -h,   --help          Show this help message\n");
	printf("  -v,   --version       Show version information and exit\n");
	exit(EXIT_SUCCESS);
}

static void print_version(void)
{
	printf("sreadahead version %s\n", VERSION);
	printf("Copyright (C) 2008, 2009 Intel Corporation\n");
	exit(EXIT_SUCCESS);
}

int main(int argc, char **argv)
{
	FILE *file;
	int pid = 0;
	pthread_t one, two, three, four;
	int max_time = DEFAULT_MAX_TIME;

	while (1) {
		static struct option opts[] = {
			{ "debug", 0, NULL, 'd' },
			{ "help", 0, NULL, 'h' },
			{ "version", 0, NULL, 'v' },
			{ "time", 1, NULL, 't' },
			{ 0, 0, NULL, 0 }
		};
		int c;
		int index = 0;

		c = getopt_long(argc, argv, "dhvt:", opts, &index);
		if (c == -1)
			break;
		switch (c) {
		case 'd':
			debug = 1;
			break;
		case 'v':
			print_version();
			break;
		case 'h':
			print_usage(argv[0]);
			break;
		case 't':
			max_time = atoi(optarg);
			break;
		default:
			;
		}
	}

	file = fopen(PACK_FILE, "r");
	if (!file) {
		/* enable tracing open calls before we fork! */
		trace_start();
	
		if (!fork()) {
			/* child */
			signal(SIGUSR1, trace_stop);
			sleep(max_time);
			/*
			 * abort if we don't get a signal, so we can stop
			 * the tracing and minimize the trace buffer size
			 */
			signal(SIGUSR1, NULL);
			trace_stop(0);
		} else {
			return EXIT_SUCCESS;
		}
	}

	total_files = fread(&rd, sizeof(struct ra_disk), MAXR, file);

	if (ferror(file)) {
		perror("Can't open sreadahead pack file");
		return 1;
	}
	fclose(file);

#ifdef HAVE_IO_PRIO
	if (syscall(__NR_ioprio_set, IOPRIO_WHO_PROCESS, pid,
		    IOPRIO_IDLE_LOWEST) == -1)
		perror("Can not set IO priority to idle class");
#endif

	readahead_set_len(RA_SMALL);

	daemon(0,0);

	pthread_create(&one, NULL, one_thread, NULL);
	pthread_create(&two, NULL, one_thread, NULL);
	pthread_create(&three, NULL, one_thread, NULL);
	pthread_create(&four, NULL, one_thread, NULL);

	pthread_join(one, NULL);
	pthread_join(two, NULL);
	pthread_join(three, NULL);
	pthread_join(four, NULL);

	readahead_set_len(RA_NORMAL);

	return EXIT_SUCCESS;
}
