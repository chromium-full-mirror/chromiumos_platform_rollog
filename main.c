/*
 * rollog
 *
 * Buffer arbitrary output in a circular buffer for some number of lines
 * into potentially time disjoint logging buffer(s).
 *
 * The buffer(s) will currently be dumped under the following conditions:
 *
 *	- The input stream hits EOF
 *	- A SIGHUP is received
 *	- Periodically, based on command line parameters
 *	- A SIGTERM is received
 *
 * TODO:	- Zero out all sample lines before reusing a sample container.
 *		- Non-reentrant, so not useful to convert to a library
 *		- Memory is not free on completion (also a library blocker)
 *
 * DOCUMENTATION
 *
 * 50,00 foot view of the data structure relationships:
 *
 * current >-.	,----------------- . . . --------------------------.
 *	     |	|						   |
 *	     v	v						   |
 *	sample 0:				sample N	   |
 *	,---------------.			,---------------.  |
 *	| next >------------------ . . . -----> | next >-----------'
 *	| fd            |			| fd            |
 *	| linebuf:      |			| linebuf:      |
 *	| ,-----------. |			| ,-----------. |
 *	| | ,-< index | |			| | ,-< index | |
 *	| | |   data  | |			| | |   data  | |
 *	| | |  ,---.  | |    ,-------------.	| | |  ,---.  | |
 *	| | |  | 0 | >-----> | data line\0 |
 *	| | |  +---+  | |    `-------------'		...
 *	| | |  |   |  | |           .
 *	| | |    .    | |           .
 *	| | `->  .    | |           .
 *	| |      .    | |
 *	| |    ,---.  | |
 *	| |    | N |  | |
 *	| |    `---'  | |
 *	| `-----------' |
 *	`---------------'
 *
 * We allocate a group of N sample containers, and link them into a circular
 * list.  In each one, we allocate a pointer vector to contain pointers to
 * sample lines.  The number of both containers and lines per container is
 * controlled by command line arguments.
 *
 * We start in the first sample container and populate its linebuf with
 * lines of data up to the max; the index is the next line to be populated.
 * When we hit the max, we roll back over to the first index.
 *
 * As a result of an event (currently, only timers or SIGUSR1), we trigger a
 * switch to the next sample container, and then repeat the process for that
 * container.  If we trigger more times than there are sample containers, we
 * roll back to the first one and start over.
 *
 * When we receive a termination event (currently, EOF on input, SIGTERM,
 * SIGHUP, SIGINT), we take advantage of the pre-traversal of the index and
 * current context, and dump out our logs in the order they were collected.
 *
 * By default, logs are written to stdout; they may also be sent to a specific
 * file via command line option.  If that file has a suffix of "XXXXXX", then
 * it is used as a template for mkstemp(3), and one file is written for each
 * sample container instead.  When using this mode, file names will end with
 * (effectively) a random string of characters; collection order can be
 * inferred by file time stamp.
 */
#include <stdio.h>
#include <stdlib.h>	/* atoi */
#include <unistd.h>	/* exit */
#include <getopt.h>	/* getopt */
#include <string.h>	/* strcmp */
#include <ctype.h>	/* isdigit */

#include <sys/types.h>	/* open */
#include <sys/stat.h>
#include <fcntl.h>

#include <signal.h>	/* sigaction */

#include <libgen.h>	/* basename */

#include <setjmp.h>	/* setjmp/longjmp */

/* program name with base name removed */
char *progname;


/*
 * Configuration options; these can usually be left as-is
 */
#define	MAX_DATA	4096	/* largest line we allow, in bytes */

#define	VAL_BUFFER_DEF	100	/* number of lines in a container ring */
#define	VAL_BUFFER_MIN	1
#define	VAL_BUFFER_MAX	10000

#define	VAL_PERIOD_DEF	0	/*  seconds per container (0 disables) */
#define	VAL_PERIOD_MIN	0
#define	VAL_PERIOD_MAX	600

#define	VAL_SAMPLES_DEF	1	/* number of containers */
#define	VAL_SAMPLES_MIN	1
#define	VAL_SAMPLES_MAX	10



/*
 * Options we understand (the long form); there are corresponding short
 * versions for each of these.
 */
static struct option long_options[] = {
	{ "buffer",	1,	0,	'b' },
	{ "help",	0,	0,	'?' },
	{ "output",	1,	0,	'o' },
	{ "period",	1,	0,	'p' },
	{ "samples",	1,	0,	's' },
	{ 0,		0,	0,	0 }
};


/*
 * usage output of help message for the program
 */
void
usage(void)
{
	fprintf(stderr, "%s:\n", progname);
	fprintf(stderr, "\t[-b|--buffer <lines>]    lines in a circular\n");
	fprintf(stderr, "\t                         sample buffer\n");
	fprintf(stderr, "\t                         : default %d\n",
				VAL_BUFFER_DEF);
	fprintf(stderr, "\t[-h|--help]              this usage message\n");
	fprintf(stderr, "\t[-o|--output <name>]     output file name or\n");
	fprintf(stderr, "\t                         mkstemp(3) template\n");
	fprintf(stderr, "\t                         (must end in XXXXXX)\n");
	fprintf(stderr, "\t                         : default stdout\n");
	fprintf(stderr, "\t[-p|--period <seconds>]  sample interval\n");
	fprintf(stderr, "\t                         : default %d%s\n",
				VAL_PERIOD_DEF,
				VAL_PERIOD_DEF ? "" : "(disabled)");
	fprintf(stderr, "\t[-s|--samples <count>]   number of samples; this\n");
	fprintf(stderr, "\t                         indicates the number of\n");
	fprintf(stderr, "\t                         output files when using\n");
	fprintf(stderr, "\t                         a mkstemp(3) template\n");
	fprintf(stderr, "\t                         : default %d\n",
				VAL_SAMPLES_DEF);
	exit(99);
}

/*
 * Command line options and defaults.
 */
int opt_lines		= VAL_BUFFER_DEF;
int opt_period		= VAL_PERIOD_DEF;
int opt_samples		= VAL_SAMPLES_DEF;
char *opt_template	= NULL;


/*
 * Line buffers; a line buffer is a ring containing up to a number of lines
 * equal to opt_lines.  Line data elements are reused as needed to limit the
 * number of output lines we carry around.
 */
struct linebuf;
typedef struct linebuf linebuf_t;

struct linebuf {
	int line_index;
	char **line_data;
};


/*
 * Sample containers; containers are arranged in a ring and reused as needed
 * for subsequent sample runs, once the limit on the number of containers is
 * reached.
 */
struct sample;
typedef struct sample sample_t;

struct sample {
	sample_t *sample_next;
	int sample_fd;
	linebuf_t sample_linebuf;
};


sample_t *samples;


/*
 * Validate that options are within allowable ranges; error out and provide
 * a usage message if something is out of range.
 */
void
range(char *option, int min, int max, int requested)
{
	if (requested >= min && requested <= max)
		return;

	fprintf(stderr, "error: %s invalid: %d; must be in the range %d..%d\n",
				option, requested, min, max);
	usage();
}


/*
 * Save a single sample container contents. Depending on the value of
 * sample_fd, this will be going to stdout, a file previously opened, or
 * a mkstemp(3) file based on the input pattern.
 */
void
save_sample(sample_t *sample)
{
	int index;
	FILE *output = stdout;

	/*
	 * shortcut: nothing was collected for this container, or at least
	 * the first sample line would have been allocated.
	 */
	if (sample->sample_linebuf.line_data[0] == NULL)
		return;

	/* Retarget output to the appropriate file */
	if (sample->sample_fd != fileno(stdout)) {
		/* temporary file */
		if (sample->sample_fd == -1) {
			/*
			 * Why use mkstemp?  Because there's no other way to
			 * get a persistent temporary file attached to an fd
			 * so that we can use it e.g. as a log file in
			 * /var/log or otherwise keep it around to look at.
			 */
			char *template = strdup(opt_template);
			if (template == NULL) {
				perror("strdup");
				exit(97);
			}
			sample->sample_fd = mkstemp(template);
			if (sample->sample_fd == -1) {
				perror("mkstemp");
				exit(96);
			}
		}

		/* Hook the output stream to the fd */
		output = fdopen(sample->sample_fd, "w");
		if (output == NULL) {
			perror("fdopen");
			exit(95);
		}
	}

	/*
	 * Traverse the line ring buffer and dump each line out to the
	 * output file for the sample.  We take advantage of the index
	 * in the linebuf_t having been pre-incremented; this means it's
	 * either pointing to the oldest line collected in that ring, or,
	 * if only a partial ring was collected, then to an uncollected
	 * entry, which means a NULL pointer is at that index.  We run
	 * the index until we hit the same value again by preincrementing
	 * over the wrap boundary during iteration.
	 */
	index = sample->sample_linebuf.line_index;
	for(;;) {
		/*
		 * Save data, if it exists, until we wrap; this gets us the
		 * same order out as we had in.  Since this is a ring buffer,
		 * data earlier than this will have been lost; this is either
		 * OK, or the program should have been invoked with a larger
		 * ring.
		 */
		if (sample->sample_linebuf.line_data[index] != NULL) {
			/*
			 * use fprintf to avoid an extra NL; use a format
			 * string in case the data contains a '%' character.
			 */
			fprintf(output, "%s",
				sample->sample_linebuf.line_data[index]);
#if NOT_NEEDED
			/*
			 * Not strictly needed, as we are about to exit;
			 * note that this should use a temp variable if
			 * this code were intended to be reentrant, or the
			 * freed pointer could be traversed after it was
			 * freed but before it was NULL'ed.
			 */
			free(sample->sample_linebuf.line_data[index]);
			sample->sample_linebuf.line_data[index] = NULL;
#endif	/* NOT_NEEDED */
		}

		/* wrap boundary */
		if (++index == opt_lines)
			index = 0;

		/* Got all lines? */
		if (index == sample->sample_linebuf.line_index)
			break;
	}
}


/*
 * Save out all the samples we have collected; in some cases, we will have
 * hit a termination event prior to having collected into more than one
 * sample container, despite them having been specified.  In the case
 * where there is no sample to collect, we avoid creating any output.  See
 * previous function for this skip.
 */
void
save_samples(sample_t *current)
{
	sample_t *dump = current->sample_next;

	/*
	 * Traverse the circularly linked container list to dump all the
	 * containers; we cheat by pre-incrementing over the end so that
	 * we output the samples in the order they were collected when
	 * they actually go to the output stream, in case there was more
	 * than one container involved.
	 */
	for(;;) {
		save_sample(dump);
		dump = dump->sample_next;
		if (dump == current->sample_next)
			break;
	}
}


/*
 * This is a naieve algorithm which assumes that the length of an input
 * line, on average, will be much less than the maximum, and therefore any
 * allocation will be smaller.  This is a poor efficiency trade-off for
 * buffer reuse, but we can live with that.
 *
 * We collect an input line into a ring biffer, and then use the index
 * value on the read out to sync to the head of the buffer list by
 * iterating until we hit ourselves again, using the same index fold point.
 */
void
collect_sample(sample_t *sample)
{
	char linebuf[MAX_DATA];
	char *old;

	/*
	 * Read input lines until we get interrupted by something; this
	 * may be an elapsed interval, or it may be an EOF on input or
	 * a SIGHUP/SIGTERM.
	 */
	while(fgets(linebuf, sizeof(linebuf), stdin) != NULL) {
		old = sample->sample_linebuf.line_data[
				sample->sample_linebuf.line_index];
		sample->sample_linebuf.line_data[
				sample->sample_linebuf.line_index] =
							strdup(linebuf);
		if (old != NULL)
			free(old);
		if (++sample->sample_linebuf.line_index == opt_lines)
			sample->sample_linebuf.line_index = 0;
	}
}



/*
 * Signal handler context for timer expiration triggering moving to another
 * sample container, or SIGHUP/SIGTERM/EOF triggering program termination.
 */
sigjmp_buf sigjmp_env;
#define	JR_INIT		0	/* setjmp() initial return */
#define	JR_TERM		1	/* done taking samples */
#define JR_NEXT		2	/* switch to next sample container */

/*
 * We're exiting; we need to dump out out collected samples for all the
 * containers for which we've collected samples.
 *
 * Note: Context is carried around for debugging purposes
 */
void
sa_term(int signo, siginfo_t *info, void *context)
{
	info = info;		/* portable __unused */
	signo = signo;		/* portable __unused */
	context = context;	/* portable __unused */

	siglongjmp(sigjmp_env, JR_TERM);
}


/*
 * We're into the next interval; we need to move onto collecting the next
 * sample container worth of data.
 *
 * Note: Context is carried around for debugging purposes
 */
void
sa_next(int signo, siginfo_t *info, void *context)
{
	info = info;		/* portable __unused */
	signo = signo;		/* portable __unused */
	context = context;	/* portable __unused */

	siglongjmp(sigjmp_env, JR_NEXT);
}


struct handler {
	int handler_signal;
	void (*handler_func)(int, siginfo_t *, void *);
} handlers[] = {
	{ SIGHUP, 	sa_term },
	{ SIGINT, 	sa_term },
	{ SIGTERM, 	sa_term },
	{ SIGALRM, 	sa_next },	/* timer triggered */
	{ SIGUSR1, 	sa_next }	/* user triggered */
};
int numhandlers = sizeof(handlers)/sizeof(struct handler);


/*
 * Set up intervals and other conditions for sample collection and trigger
 * sampling.  We will jump out of the signal handler into either the next
 * iteration or our termination condition, if we received an EOF.
 */
void
collect_all(void)
{
	struct sigaction sa;
	sample_t *current = samples;	/* start at the top */
	int i;

	sa.sa_flags = SA_SIGINFO;
	for(i = 0; i < numhandlers; i++) {
		sa.sa_sigaction = handlers[i].handler_func;
		if (sigaction(handlers[i].handler_signal, &sa, NULL) == -1) {
			perror("sigaction");
			exit(98);
		}
	}

	fprintf(stderr, "Sampling %d sample sets of %d lines at period %d\n",
		opt_samples, opt_lines, opt_period);

reset_timer:
	/* if we periodically switch containers, arm the alarm here */
	if (opt_period)
		alarm(opt_period);

	/*
	 *
	 */
	switch (sigsetjmp(sigjmp_env, 1)) {
	case JR_INIT:	/* initial run through */
		collect_sample(current);
		/* fallsthrough: EOF */

	case JR_TERM:	/* termination by signal */
		collect_sample(current);
		save_samples(current);
		break;

	case JR_NEXT:	/* interval timer fired; move to next container */
		current = current->sample_next;
		goto reset_timer;
	}
}



/*
 * rollog main program
 */
int
main(int ac, char *av[])
{
	int option_index = 0;
	int ofd = fileno(stdout);
	int i;
	sample_t *next_sample;

	progname = basename(av[0]);

	/*
	 * Process input options.  All options have single character aliases
	 * and do not use the default value or variable pointer functionality.
	 */
	for(;;) {
		int c = getopt_long(ac, av, "b:ho:p:s:",
					long_options, &option_index);

		if (c == -1)
			break;

		switch (c) {
		case 'b':	/* buffer size, in lines */
			if (!isdigit(optarg[0])) {
				c = '?';
				break;
			}
			opt_lines = atoi(optarg);
			range("buffer", VAL_BUFFER_MIN, VAL_BUFFER_MAX,
								opt_lines);
			break;

		case 'o':	/* output file, if not stdout */
			/* '-' is an alias for stdout */
			if (!strcmp("-", optarg))
				break;

			/*
			 * If it looks like a template for a temp file name,
			 * it is.
			 */
			if (strstr(optarg, "XXXXXX") != NULL) {
				opt_template = optarg;
				ofd = -1;	/* distinguish open error -1 */
				break;
			}

			/* otherwise, it's just a file name */
			ofd = open(optarg, O_RDWR|O_CREAT|O_TRUNC|O_EXCL, 0600);
			if (ofd == -1) {
				perror("open");
				fprintf(stderr, "can not open '%s'\n", optarg);
				exit(1);
			}
			break;

		case 'p':	/* sample period, in seconds */
			/*
			 * If specified, we will dump a sample every this
			 * many seconds.  If there is a template file, then
			 * the sample will be dumped consecutively to the
			 * temp file.  If there is a sample count, that many
			 * sample files will be involved.
			 */
			if (!isdigit(optarg[0])) {
				c = '?';
				break;
			}
			opt_period = atoi(optarg);
			range("period", VAL_PERIOD_MIN, VAL_PERIOD_MAX,
								opt_period);
			break;

		case 's':	/* number of samples */
			/*
			 * If specified, this will be the number of sample
			 * files we will create, if a template was given for
			 * the name using the output file name option.
			 */
			if (!isdigit(optarg[0])) {
				c = '?';
				break;
			}
			opt_samples = atoi(optarg);
			range("samples", VAL_SAMPLES_MIN, VAL_SAMPLES_MAX,
								opt_samples);
			break;

		case '?':
		default:
			c = '?';	/* Trigger usage/help message */
			break;
		}

		/*
		 * Explicit request for help, or silent cry for help due to
		 * improper option usage.
		 */
		if (c == '?')
			usage();	/* no return */
	}

	/*
	 * Meat of the code... collect sample liness from our input a line
	 * at a time, and buffer them up.  At the sample period
	 */

	samples = calloc(opt_samples, sizeof(sample_t));
	if (samples == NULL) {
		perror("calloc");
		fprintf(stderr,
			"can not allocate %d sample containers", opt_samples);
		exit(1);
	}

	/*
	 * Circularly link the sample containers; for each container,
	 * initialize the sample fd and the linebuf; initializing the
	 * linebuf means allocating a line vector and an initial index.
	 */
	next_sample = samples;
	for(i = opt_samples - 1; i >= 0; i--) {
		samples[i].sample_next = next_sample;
		next_sample = &samples[i];
		samples[i].sample_fd = ofd;		/* stdout or alloc */
		samples[i].sample_linebuf.line_data =
				calloc(opt_lines, sizeof(linebuf_t));
		if (samples[i].sample_linebuf.line_data == NULL) {
			/* hope they take the hint and use a smaller #... */
			perror("calloc");
			fprintf(stderr,
				"can not allocate lines  for sample %d",
				opt_samples - i);
			exit(2);
		}
	}

	collect_all();

	exit(0);
}
