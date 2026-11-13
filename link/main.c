// main.c: flags, the thread pool, errors, the phase table, and the order of the phases.

// ---- errors: collected from any thread, reported at the end of a phase
static pthread_mutex_t errMu = PTHREAD_MUTEX_INITIALIZER;
static char *errList[30];
static int nerrList;
static long nerrs;

static void errorf(const char *fmt, ...) {
	if (__atomic_add_fetch(&nerrs, 1, __ATOMIC_RELAXED) > 30) return;
	va_list ap;
	va_start(ap, fmt);
	char *s = NULL;
	if (vasprintf(&s, fmt, ap) < 0) s = NULL;
	va_end(ap);
	pthread_mutex_lock(&errMu);
	if (nerrList < 30) errList[nerrList++] = s;
	pthread_mutex_unlock(&errMu);
}

static void removeOutput(void) {
	if (flagOut && *flagOut) unlink(flagOut);
}

static void exitIfErrors(void) {
	if (__atomic_load_n(&nerrs, __ATOMIC_RELAXED) == 0) return;
	for (int i = 0; i < nerrList; i++) fprintf(stderr, "hlink: %s\n", errList[i] ? errList[i] : "?");
	if (nerrs > nerrList) fprintf(stderr, "hlink: ... %ld errors in all\n", nerrs);
	removeOutput();
	fflush(stdout);
	_exit(2);
}

static pthread_mutex_t fatalMu = PTHREAD_MUTEX_INITIALIZER;
static void fatalf(const char *fmt, ...) {
	pthread_mutex_lock(&fatalMu); // (a second thread with a fatal error waits here until the process is gone)
	va_list ap;
	va_start(ap, fmt);
	fprintf(stderr, "hlink: ");
	vfprintf(stderr, fmt, ap);
	fprintf(stderr, "\n");
	va_end(ap);
	removeOutput();
	fflush(stdout);
	_exit(2);
}

static void refError(const object *o, u32 pkg) {
	str p = pkg < o->npkg ? oPkg(o, pkg) : cstr("?");
	errorf("%s: reference to package %.*s, which is not in the importcfg", o->file, (int)p.n, p.p);
}

// ---- the thread pool: nthreads-1 persistent workers; the calling thread is worker 0
// The pool. A link is a few dozen parallel passes, some of them (the levels of the reachability flood)
// microseconds long, so a worker waiting for the next job spins for a while before it sleeps: waking a
// sleeping thread costs tens of microseconds here, a spinning one sees the new job in nanoseconds. It must
// still sleep eventually: on a busy machine eight spinning threads take the CPU from the one that has work.
static struct {
	parfn fn;
	void *ctx;
	long n;
	int nw;
	_Alignas(64) long next;
	_Alignas(64) u32 gen;     // bumped once per job (a futex word)
	_Alignas(64) u32 done;    // workers that have finished the current job (a futex word)
	_Alignas(64) u32 sleepers; // workers asleep on gen
	u32 mainSleeps;
} job;
#include <linux/futex.h>
#include <sys/syscall.h>
#include <immintrin.h>
static inline void futexWait(u32 *addr, u32 val) { syscall(SYS_futex, addr, FUTEX_WAIT_PRIVATE, val, NULL, NULL, 0); }
static inline void futexWake(u32 *addr, int n) { syscall(SYS_futex, addr, FUTEX_WAKE_PRIVATE, n, NULL, NULL, 0); }
#define spinRounds 20000 // of _mm_pause: on the order of 100 microseconds

static void runJob(parfn fn, void *ctx, long n, int w) {
	for (;;) {
		long i = __atomic_fetch_add(&job.next, 1, __ATOMIC_RELAXED);
		if (i >= n) return;
		fn(ctx, w, i);
	}
}

static void *poolWorker(void *arg) {
	int w = (int)(intptr_t)arg;
	u32 seen = 0;
	for (;;) {
		u32 g;
		for (int spin = 0; (g = __atomic_load_n(&job.gen, __ATOMIC_ACQUIRE)) == seen; spin++) {
			if (spin < spinRounds) {
				_mm_pause();
				continue;
			}
			__atomic_fetch_add(&job.sleepers, 1, __ATOMIC_SEQ_CST);
			if (__atomic_load_n(&job.gen, __ATOMIC_SEQ_CST) == seen) futexWait(&job.gen, seen);
			__atomic_fetch_sub(&job.sleepers, 1, __ATOMIC_SEQ_CST);
		}
		seen = g;
		if (w < job.nw) runJob(job.fn, job.ctx, job.n, w);
		__atomic_fetch_add(&job.done, 1, __ATOMIC_SEQ_CST);
		if (__atomic_load_n(&job.mainSleeps, __ATOMIC_SEQ_CST)) futexWake(&job.done, 1);
	}
	return NULL;
}

static void startPool(void) {
	for (int w = 1; w < nthreads; w++) {
		pthread_t t;
		pthread_attr_t at;
		pthread_attr_init(&at);
		pthread_attr_setstacksize(&at, 64 << 20); // typePtrs and the sorts recurse
		if (pthread_create(&t, &at, poolWorker, (void *)(intptr_t)w) != 0) fatalf("pthread_create failed");
		pthread_detach(t);
	}
}

// par runs fn(ctx, w, i) for i in [0,n) on all threads; the order of the calls is not defined. Not reentrant.
static void par(long n, parfn fn, void *ctx) {
	if (n <= 0) return;
	int nw = nthreads;
	if (nw > n) nw = (int)n;
	if (nw <= 1) {
		for (long i = 0; i < n; i++) fn(ctx, 0, i);
		return;
	}
	job.fn = fn;
	job.ctx = ctx;
	job.n = n;
	job.nw = nw;
	__atomic_store_n(&job.next, 0, __ATOMIC_RELAXED);
	__atomic_store_n(&job.done, 0, __ATOMIC_RELAXED);
	__atomic_fetch_add(&job.gen, 1, __ATOMIC_SEQ_CST);
	if (__atomic_load_n(&job.sleepers, __ATOMIC_SEQ_CST)) futexWake(&job.gen, nthreads);
	runJob(fn, ctx, n, 0);
	// every worker passes through every job (those beyond nw only acknowledge it), so that none can still be
	// reading this job's fields when the next one is set up
	u32 want = (u32)nthreads - 1, d;
	for (int spin = 0; (d = __atomic_load_n(&job.done, __ATOMIC_ACQUIRE)) != want; spin++) {
		if (spin < spinRounds) {
			_mm_pause();
			continue;
		}
		__atomic_store_n(&job.mainSleeps, 1, __ATOMIC_SEQ_CST);
		if ((d = __atomic_load_n(&job.done, __ATOMIC_SEQ_CST)) != want) futexWait(&job.done, d);
		__atomic_store_n(&job.mainSleeps, 0, __ATOMIC_SEQ_CST);
	}
}

// ---- the phase table
typedef struct {
	const char *name;
	double wall, cpu;
	char *note;
} phase;
static VEC(phase) phases;
static double phaseT0, phaseC0, phaseS0, linkT0, linkCPU0;
static long long outSize;

static double nowSeconds(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}
static void rusageSeconds(double *cpu, double *sys) {
	struct rusage ru;
	getrusage(RUSAGE_SELF, &ru);
	*sys = ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6;
	*cpu = ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6 + *sys;
}

static void lap(const char *name, const char *fmt, ...) {
	double now = nowSeconds(), c, s;
	rusageSeconds(&c, &s);
	char *note = NULL, *full = NULL;
	va_list ap;
	va_start(ap, fmt);
	if (vasprintf(&note, fmt, ap) < 0) note = NULL;
	va_end(ap);
	if (flagStats) {
		if (asprintf(&full, "[kernel %5.1f] %s", (s - phaseS0) * 1000, note ? note : "") < 0) full = NULL;
		free(note);
		note = full;
	}
	phase p = {name, (now - phaseT0) * 1000, (c - phaseC0) * 1000, note};
	VPUSH(phases, p);
	phaseT0 = now;
	phaseC0 = c;
	phaseS0 = s;
}

// ---- flags, as Go's flag package parses them: -name, --name, -name=value, -name value (not for bools)
typedef struct {
	const char *name;
	int kind; // 0 string, 1 bool, 2 int, 3 the repeated -X
	void *p;
} flagdef;

static VEC(const char *) flagXDefs;
static const char *ignoredStr;
static bool ignoredBool;

static const flagdef flagdefs[] = {
	{"o", 0, &flagOut},
	{"importcfg", 0, &flagImportcfg},
	{"buildid", 0, &flagBuildid},
	{"stats", 1, &flagStats},
	{"j", 2, &flagJ},
	{"nodce", 1, &flagNoDCE},
	{"dumplive", 0, &flagDumpLive},
	{"dumpmap", 0, &flagDumpMap},
	{"E", 0, &flagEntry},
	{"X", 3, NULL},
	// accepted for compatibility with the go command's link line
	{"buildmode", 0, &ignoredStr},
	{"s", 1, &ignoredBool},
	{"w", 1, &ignoredBool},
	{"extld", 0, &ignoredStr},
	{"installsuffix", 0, &ignoredStr},
	{"linkmode", 0, &ignoredStr},
	{"B", 0, &ignoredStr},
	{"tmpdir", 0, &ignoredStr},
};

static void usage(void) {
	fprintf(stderr, "usage: hlink -o out -importcfg importcfg.link [-buildid id] [-X pkg.name=val]... [-stats] [-j N] [-nodce] "
	                "[-dumplive file] [-dumpmap file] [-E entry] main.o\n");
	_exit(2);
}

static bool parseBool(const char *v, bool *out) {
	static const char *const t[] = {"1", "t", "T", "TRUE", "true", "True"}, *const f[] = {"0", "f", "F", "FALSE", "false", "False"};
	for (int i = 0; i < 6; i++) {
		if (strcmp(v, t[i]) == 0) { *out = true; return true; }
		if (strcmp(v, f[i]) == 0) { *out = false; return true; }
	}
	return false;
}

// parseFlags returns the index of the first argument that is not a flag.
static int parseFlags(int argc, char **argv) {
	int i = 1;
	while (i < argc) {
		const char *a = argv[i];
		if (a[0] != '-' || a[1] == 0) break;
		const char *name = a + 1;
		if (name[0] == '-') {
			name++;
			if (name[0] == 0) { i++; break; } // "--": the end of the flags
		}
		if (name[0] == '-' || name[0] == '=') {
			fprintf(stderr, "bad flag syntax: %s\n", a);
			usage();
		}
		i++;
		const char *eq = strchr(name, '=');
		size_t nlen = eq ? (size_t)(eq - name) : strlen(name);
		const flagdef *fd = NULL;
		for (size_t k = 0; k < sizeof flagdefs / sizeof flagdefs[0]; k++)
			if (strlen(flagdefs[k].name) == nlen && memcmp(flagdefs[k].name, name, nlen) == 0) fd = &flagdefs[k];
		if (!fd) {
			if (nlen == 1 && (name[0] == 'h') ) usage();
			if (nlen == 4 && memcmp(name, "help", 4) == 0) usage();
			fprintf(stderr, "flag provided but not defined: -%.*s\n", (int)nlen, name);
			usage();
		}
		const char *val = eq ? eq + 1 : NULL;
		if (fd->kind == 1) {
			bool b = true;
			if (val && !parseBool(val, &b)) {
				fprintf(stderr, "invalid boolean value \"%s\" for -%s\n", val, fd->name);
				usage();
			}
			*(bool *)fd->p = b;
			continue;
		}
		if (!val) {
			if (i >= argc) {
				fprintf(stderr, "flag needs an argument: -%s\n", fd->name);
				usage();
			}
			val = argv[i++];
		}
		switch (fd->kind) {
		case 0: *(const char **)fd->p = val; break;
		case 2: {
			char *end;
			errno = 0;
			long v = strtol(val, &end, 0);
			if (*val == 0 || *end != 0 || errno) {
				fprintf(stderr, "invalid value \"%s\" for flag -%s: parse error\n", val, fd->name);
				usage();
			}
			*(int *)fd->p = (int)v;
			break;
		}
		case 3: VPUSH(flagXDefs, val); break;
		}
	}
	return i;
}

static char *buildVersion(void) {
	const char *v = GO_VERSION, *x = GO_EXPERIMENT;
	char *s;
	if (*x) {
		const char *sep = strchr(v, '-') ? " " : "-";
		if (asprintf(&s, "%s%sX:%s", v, sep, x) < 0) fatalf("out of memory");
	} else {
		s = strdup(v);
	}
	return s;
}

static void load(const char *cfgFile, const char *mainFile);
static void resolve(void);
static void inittasks(void);
static void deadcode(void);
static void layout(void);
static void writeOut(void);
static void finish(void);
static void dumpMap(void);

int main(int argc, char **argv) {
	setlocale(LC_CTYPE, "C.UTF-8");
	linkT0 = phaseT0 = nowSeconds();
	rusageSeconds(&linkCPU0, &phaseS0);
	phaseC0 = linkCPU0;
	int first = parseFlags(argc, argv);
	if (argc - first != 1 || !*flagImportcfg) fatalf("usage: hlink -o out -importcfg importcfg.link [-buildid id] [-stats] main.o");
	nthreads = flagJ;
	if (nthreads <= 0) {
		cpu_set_t cs;
		if (sched_getaffinity(0, sizeof cs, &cs) == 0) nthreads = CPU_COUNT(&cs);
		if (nthreads <= 0) nthreads = (int)sysconf(_SC_NPROCESSORS_ONLN);
		if (nthreads <= 0) nthreads = 1;
	}
	startPool();

	setStrdata("runtime.buildVersion", cstr(buildVersion()));
	const char *goroot = getenv("GOROOT"); // buildcfg.GOROOT
	if (goroot && *goroot) setStrdata("runtime.defaultGOROOT", cstr(goroot));
	for (size_t i = 0; i < flagXDefs.n; i++) {
		const char *x = flagXDefs.a[i], *eq = strchr(x, '=');
		if (!eq) fatalf("-X flag requires argument of the form importpath.name=value");
		char *name = strndup(x, (size_t)(eq - x));
		setStrdata(name, cstr(eq + 1));
		free(name);
	}

	removeOutput();
	load(flagImportcfg, argv[first]);
	exitIfErrors();
	resolve();
	exitIfErrors();
	inittasks();
	deadcode();
	exitIfErrors();
	layout();
	exitIfErrors();
	writeOut();
	exitIfErrors();
	finish();
	exitIfErrors();
	if (*flagDumpMap) dumpMap();

	if (flagStats) {
		double c, s;
		rusageSeconds(&c, &s);
		double total = (nowSeconds() - linkT0) * 1000, totalCPU = (c - linkCPU0) * 1000;
		printf("%-10s %8s %8s\n", "phase", "wall ms", "cpu ms");
		for (size_t i = 0; i < phases.n; i++)
			printf("%-10s %8.1f %8.1f  %s\n", phases.a[i].name, phases.a[i].wall, phases.a[i].cpu, phases.a[i].note ? phases.a[i].note : "");
		printf("%-10s %8.1f %8.1f  %d threads, output %.1f MB\n", "total", total, totalCPU, nthreads, (double)outSize / 1e6);
	}
	fflush(stdout);
	_exit(0); // the workers never return
}
