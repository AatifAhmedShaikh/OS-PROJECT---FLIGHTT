/*
 * AeroCore: flight booking driven by a 5-level MLFQ scheduler (single-file version).
 *
 * Build and run:
 *   Linux / macOS / WSL / MSYS2:  gcc -std=gnu11 -O2 -o aerocore aerocore_single.c && ./aerocore
 *   Windows (MinGW gcc):          gcc -std=gnu11 -O2 -o aerocore.exe aerocore_single.c && aerocore.exe
 *   Online compiler:              paste this whole file, choose C, run, then type: demo / run / compare
 *
 * Sections: 1 shared types, 2 scheduler (sched.c), 3 bookings + journal (airline.c), 4 command line (main.c)
 */

/* ================================================================
 * 1. Shared types (aerocore.h)
 * ================================================================ */
#ifndef AEROCORE_H
#define AEROCORE_H

#ifdef _WIN32                                       /* Windows (MinGW): map the few POSIX calls the code uses */
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
__declspec(dllimport) void __stdcall Sleep(unsigned long ms);
static inline int win_truncate(const char *p, long n) {
    int fd = _open(p, _O_RDWR | _O_BINARY), r;
    if (fd < 0) return -1;
    r = _chsize(fd, n); _close(fd); return r;
}
#define fsync _commit
#define isatty _isatty
#define getpid _getpid
#define usleep(us) Sleep((us) / 1000)
#define truncate win_truncate
#define WIN_INIT() (_fmode = _O_BINARY)             /* journal in binary mode: no CRLF translation */
#else
#define WIN_INIT() ((void)0)
#endif

#define MAXREQ 64
#define MAXSEG 1024
#define MAXEV  512
#define INF    1000000

enum { RR, SJF, PRIO, SRTF, FCFS };                 /* per-queue algorithms */
enum { BOOK, CANCEL, CHECKIN, REFUND };             /* request types */
enum { BOOKED, CHECKED_IN, CANCELLED, REFUNDED };   /* booking status */
extern const char *TYPE_NAME[], *STATUS_NAME[];

/* One request == one scheduler process. Input fields first, scheduler state after. */
typedef struct {
    int id, type, urgent, arrival, burst, prio;
    int pax; char flight[8], seat[4], pnr[8];
    int rem, q, seq, q_used, slice, wait_q, first, finish;
    char msg[96];                                   /* outcome, filled by apply_req */
} Req;

/* nq queues, queue i runs alg[i]; a process leaves queue i after allot[i] ticks of CPU.
   urgent_first: RR queue serves urgent requests before regular ones (MLFQ only). */
typedef struct { int nq, alg[5], allot[5], quantum, age, trace, urgent_first; char name[16]; } Cfg;
typedef struct { int pid, q, start, end; } Seg;     /* pid 0 = idle */
typedef struct {
    Req p[MAXREQ]; int n;
    Seg g[MAXSEG]; int ng;
    char ev[MAXEV][72]; int nev;
    int busy, makespan, npromo, ndemote, npreempt;
} Sim;
typedef struct { double wait, tat, resp, thr, cpu; } Met;

/* sched.c */
Cfg  cfg_mlfq(int quantum, int age);
Cfg  cfg_single(int alg, int quantum);              /* FCFS / SJF / RR baseline */
void sched_run(const Req *in, int n, const Cfg *c, Sim *s);
Met  sched_met(const Sim *s);
void sched_report(const Sim *s, const Cfg *c);

/* airline.c */
int  db_open(const char *path, int *replayed, int *discarded);
void db_close(void);
void db_crash_in(int k);                            /* k writes succeed, the next is torn + exit */
int  pax_add(const char *phone, const char *name);  /* -> id, 0 on failure */
int  pax_count(void);
const char *pax_active_pnr(int pax, const char *flight);
int  pnr_status(const char *pnr);                   /* -1 if unknown */
int  seat_norm(char *out, const char *in);          /* "3b" -> "3B"; 0 if invalid */
const char *book_check(int pax, const char *flight, const char *seat);  /* NULL = ok */
int  apply_req(Req *r);                             /* 1 ok / 0 failed, sets r->msg */
void show_flights(void);
void show_seats(const char *flight);
void show_pax(void);
void show_history(int pax);
void show_pnr(const char *pnr);
void show_txns(void);

#endif

/* ================================================================
 * 2. Scheduler (sched.c)
 * ================================================================ */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const char *ALG[] = {"RR", "SJF", "PRIO", "SRTF", "FCFS"};

Cfg cfg_mlfq(int quantum, int age) {
    return (Cfg){5, {RR, SJF, PRIO, SRTF, FCFS}, {4, 4, 6, 8, INF}, quantum, age, 0, 1, "MLFQ-5Q"};
}

Cfg cfg_single(int alg, int quantum) {
    Cfg c = {1, {alg}, {INF}, quantum, 0, 0, 0, ""};
    if (alg == RR) snprintf(c.name, sizeof c.name, "RR(q=%d)", quantum);
    else snprintf(c.name, sizeof c.name, "%s", ALG[alg]);
    return c;
}

static void ev(Sim *s, int t, const char *fmt, ...) {
    if (s->nev >= MAXEV) return;
    char *d = s->ev[s->nev++];
    int n = snprintf(d, sizeof s->ev[0], "t=%-3d ", t);
    va_list ap; va_start(ap, fmt); vsnprintf(d + n, sizeof s->ev[0] - n, fmt, ap); va_end(ap);
}

static void seg(Sim *s, int pid, int q, int t) {
    Seg *g = s->ng ? &s->g[s->ng - 1] : 0;
    if (g && g->end == t && g->pid == pid && g->q == q) { g->end++; return; }
    if (s->ng < MAXSEG) s->g[s->ng++] = (Seg){pid, q, t, t + 1};  /* chart stops at MAXSEG segments */
}

/* Smaller key runs first; ties go to the earliest queue entry (seq). FCFS uses seq only. */
static int key(const Req *r, int alg, const Cfg *c) {
    return alg == SJF ? r->burst : alg == PRIO ? r->prio : alg == SRTF ? r->rem
         : alg == RR && c->urgent_first ? !r->urgent : 0;
}

static void trace(const Sim *s, const Cfg *c, int t, int rq, const Req *r) {
    printf("t=%-3d run P%-2d Q%d/%-4s rem %-2d |", t, r->id, rq + 1, ALG[c->alg[rq]], r->rem);
    for (int q = 0; q < c->nq; q++) {
        printf(" Q%d[", q + 1);
        int last = -1, first = 1;
        for (;;) {                                   /* walk queue q in entry order */
            int b = -1;
            for (int i = 0; i < s->n; i++) {
                const Req *x = &s->p[i];
                if (x->rem && x->q == q && x->seq > last && (b < 0 || x->seq < s->p[b].seq)) b = i;
            }
            if (b < 0) break;
            printf("%sP%d:%d", first ? "" : " ", s->p[b].id, s->p[b].rem);
            first = 0; last = s->p[b].seq;
        }
        putchar(']');
    }
    putchar('\n');
    if (c->trace > 1) { fflush(stdout); usleep(200000); }
}

/*
 * One tick = one time unit. Queues are not data structures: each request carries its queue (q)
 * and entry order (seq); picking a process is a scan. Higher queue always preempts lower.
 */
void sched_run(const Req *in, int n, const Cfg *c, Sim *s) {
    memset(s, 0, sizeof *s);
    s->n = n; memcpy(s->p, in, n * sizeof *in);
    Req *p = s->p;
    for (int i = 0; i < n; i++) { p[i].rem = p[i].burst; p[i].q = 0; p[i].seq = -1; p[i].first = -1; }
    int cur = -1, rq = -1, done = 0, seqc = 0;       /* rq: job to re-enter its queue tail next tick */

    for (int t = 0; done < n && t < 100000; t++) {
        for (int i = 0; i < n; i++)
            if (p[i].arrival == t) {
                p[i].seq = seqc++;
                ev(s, t, "P%d arrives (%s, prio %d, burst %d)", p[i].id, TYPE_NAME[p[i].type], p[i].prio, p[i].burst);
            }
        if (rq >= 0) { p[rq].seq = seqc++; rq = -1; }  /* after arrivals: textbook RR order */

        if (c->age)                                  /* aging: waiting too long -> one queue up, priority +1 */
            for (int i = 0; i < n; i++) {
                Req *r = &p[i];
                if (r->seq < 0 || !r->rem || i == cur || r->q == 0) continue;
                if (++r->wait_q >= c->age) {
                    ev(s, t, "P%d Q%d->Q%d promoted (aged %d ticks)", r->id, r->q + 1, r->q, r->wait_q);
                    r->q--; r->q_used = r->wait_q = 0; r->seq = seqc++;
                    if (r->prio > 1) r->prio--;
                    s->npromo++;
                }
            }

        int top = c->nq;
        for (int i = 0; i < n; i++) if (p[i].seq >= 0 && p[i].rem && p[i].q < top) top = p[i].q;
        if (top == c->nq) {                          /* nothing ready */
            seg(s, 0, -1, t); cur = -1;
            if (c->trace) printf("t=%-3d idle\n", t);
            continue;
        }

        int alg = c->alg[top], nxt = -1;
        if (cur >= 0 && p[cur].q == top && alg != SRTF) nxt = cur;   /* RR slice / non-preemptive: keep going */
        else {
            for (int i = 0; i < n; i++) {
                Req *r = &p[i];
                if (r->seq < 0 || !r->rem || r->q != top) continue;
                if (nxt < 0 || key(r, alg, c) < key(&p[nxt], alg, c) ||
                    (key(r, alg, c) == key(&p[nxt], alg, c) && r->seq < p[nxt].seq)) nxt = i;
            }
            if (alg == SRTF && cur >= 0 && p[cur].q == top && key(&p[nxt], alg, c) >= key(&p[cur], alg, c)) nxt = cur;
        }
        if (nxt != cur) {
            if (cur >= 0) {                          /* cur neither finished nor demoted: it was preempted */
                s->npreempt++;
                ev(s, t, "P%d preempted by P%d (Q%d/%s)", p[cur].id, p[nxt].id, top + 1, ALG[alg]);
            }
            p[nxt].slice = 0;
        }
        cur = nxt;

        Req *r = &p[cur]; int ci = cur, rqn = r->q;
        if (r->first < 0) r->first = t;
        seg(s, r->id, r->q, t);
        r->rem--; r->q_used++; r->slice++; r->wait_q = 0; s->busy++;
        if (!r->rem) {
            r->finish = t + 1; done++; cur = -1;
            ev(s, t + 1, "P%d finished", r->id);
        } else {
            if (alg == RR && r->slice >= c->quantum) { r->slice = 0; cur = -1; rq = ci; }
            if (r->q < c->nq - 1 && r->q_used >= c->allot[r->q]) {
                ev(s, t + 1, "P%d Q%d->Q%d demoted (used %d ticks)", r->id, r->q + 1, r->q + 2, r->q_used);
                r->q++; r->q_used = r->wait_q = 0; cur = -1; rq = ci; s->ndemote++;
            }
        }
        if (c->trace) trace(s, c, t, rqn, r);
    }
    for (int i = 0; i < n; i++) if (p[i].finish > s->makespan) s->makespan = p[i].finish;
}

Met sched_met(const Sim *s) {
    Met m = {0};
    for (int i = 0; i < s->n; i++) {
        const Req *r = &s->p[i];
        m.wait += r->finish - r->arrival - r->burst;
        m.tat += r->finish - r->arrival;
        m.resp += r->first - r->arrival;
    }
    if (s->n) { m.wait /= s->n; m.tat /= s->n; m.resp /= s->n; }
    if (s->makespan) { m.thr = (double)s->n / s->makespan; m.cpu = 100.0 * s->busy / s->makespan; }
    return m;
}

static void gflush(char r[3][300], int end) {
    if (!r[0][0]) return;
    printf("%s|\n%s|\n%s%d\n\n", r[0], r[1], r[2], end);
    r[0][0] = r[1][0] = r[2][0] = 0;
}

static void gantt(const Sim *s) {
    char r[3][300] = {"", "", ""};
    int end = 0;
    puts("\nGantt chart  (row 1: request, row 2: queue, row 3: tick)");
    for (int i = 0; i < s->ng; i++) {
        const Seg *g = &s->g[i];
        int w = (g->end - g->start) * 2;
        if (w < 4) w = 4;
        if (w > 40) w = 40;
        if (strlen(r[0]) + w + 2 > 96) gflush(r, end);   /* wrap at ~100 columns */
        char a[16] = "idle", b[16] = "";
        if (g->pid) { snprintf(a, sizeof a, "P%d", g->pid); snprintf(b, sizeof b, "Q%d", g->q + 1); }
        size_t l;
        l = strlen(r[0]); snprintf(r[0] + l, 300 - l, "|%-*s", w, a);
        l = strlen(r[1]); snprintf(r[1] + l, 300 - l, "|%-*s", w, b);
        l = strlen(r[2]); snprintf(r[2] + l, 300 - l, "%-*d", w + 1, g->start);
        end = g->end;
    }
    gflush(r, end);
}

void sched_report(const Sim *s, const Cfg *c) {
    puts("\n ID  Type     Urg Arr Burst Prio(end) First Finish Wait  TAT Resp");
    for (int i = 0; i < s->n; i++) {
        const Req *r = &s->p[i];
        printf("%3d  %-8s %-3s %3d %5d %9d %5d %6d %4d %4d %4d\n", r->id, TYPE_NAME[r->type],
               r->urgent ? "yes" : "-", r->arrival, r->burst, r->prio, r->first, r->finish,
               r->finish - r->arrival - r->burst, r->finish - r->arrival, r->first - r->arrival);
    }
    gantt(s);
    puts("Queue transitions / preemptions:");
    for (int i = 0; i < s->nev; i++)
        if (strstr(s->ev[i], "promoted") || strstr(s->ev[i], "demoted") || strstr(s->ev[i], "preempted"))
            printf("  %s\n", s->ev[i]);
    Met m = sched_met(s);
    printf("\n%s: avg wait %.2f | avg turnaround %.2f | avg response %.2f | throughput %.3f req/tick | CPU %.1f%%\n",
           c->name, m.wait, m.tat, m.resp, m.thr, m.cpu);
    printf("promotions %d, demotions %d, preemptions %d\n", s->npromo, s->ndemote, s->npreempt);
}

/* ================================================================
 * 3. Flights, passengers, bookings and the journal (airline.c)
 * ================================================================ */
/*
 * Flights, passengers, bookings, transactions. All state lives in memory and is rebuilt at
 * startup by replaying an append-only journal (one checksummed line per record). Every live
 * write goes journal-first, then through the same replay() that recovery uses.
 *
 *   F|id|from|to|departure|fare
 *   P|id|name|phone
 *   B|pnr|pax|flight|seat|status|fare|txid|txtype|amount|ts   (booking upsert + its transaction)
 *
 * A B line carries the booking change and its transaction together, so they are atomic.
 * A line counts only if it ends in '\n' and its trailing "*hash" matches: a torn write fails both.
 * Seat occupancy is derived from active bookings, so cancelling releases the seat by itself.
 */
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

const char *TYPE_NAME[] = {"BOOK", "CANCEL", "CHECKIN", "REFUND"};
const char *STATUS_NAME[] = {"BOOKED", "CHECKED_IN", "CANCELLED", "REFUNDED"};

#define ROWS 10
#define COLS 4
#define MAXF 16
#define MAXP 256
#define MAXB 1024
#define MAXT 4096

typedef struct { char id[8], from[4], to[4], depart[20]; int fare; } Flight;
typedef struct { int id; char name[40], phone[20]; } Pax;
typedef struct { char pnr[8]; int pax; char flight[8], seat[4]; int status, fare; } Booking;
typedef struct { int id, type, amount; char pnr[8]; long ts; } Txn;

static Flight fl[MAXF]; static Pax px[MAXP]; static Booking bk[MAXB]; static Txn tx[MAXT];
static int nf, npx, nb, nt, nextp = 1, nextt = 1;
static FILE *jf;
static int crash_in = -1;

/* ---- lookups ---- */
static Flight *flight(const char *id) { for (int i = 0; i < nf; i++) if (!strcmp(fl[i].id, id)) return &fl[i]; return 0; }
static Pax *pax(int id) { for (int i = 0; i < npx; i++) if (px[i].id == id) return &px[i]; return 0; }
static Booking *booking(const char *pnr) { for (int i = 0; i < nb; i++) if (!strcmp(bk[i].pnr, pnr)) return &bk[i]; return 0; }
static int active(const Booking *b) { return b->status == BOOKED || b->status == CHECKED_IN; }
static Booking *holder(const char *f, const char *seat) {
    for (int i = 0; i < nb; i++)
        if (active(&bk[i]) && !strcmp(bk[i].flight, f) && !strcmp(bk[i].seat, seat)) return &bk[i];
    return 0;
}
static int taken(const char *f) { int k = 0; for (int i = 0; i < nb; i++) k += active(&bk[i]) && !strcmp(bk[i].flight, f); return k; }

/* ---- journal ---- */
static unsigned fnv(const char *s) { unsigned h = 2166136261u; for (; *s; s++) h = (h ^ (unsigned char)*s) * 16777619u; return h; }

static int replay(const char *l) {
    Flight f; Pax x; Booking b; Txn t; int type;
    if (l[0] == 'F' && nf < MAXF &&
        sscanf(l, "F|%7[^|]|%3[^|]|%3[^|]|%19[^|]|%d", f.id, f.from, f.to, f.depart, &f.fare) == 5)
        fl[nf++] = f;
    else if (l[0] == 'P' && npx < MAXP && sscanf(l, "P|%d|%39[^|]|%19[^|]", &x.id, x.name, x.phone) == 3) {
        px[npx++] = x;
        if (x.id >= nextp) nextp = x.id + 1;
    } else if (l[0] == 'B' && nt < MAXT &&
               sscanf(l, "B|%7[^|]|%d|%7[^|]|%3[^|]|%d|%d|%d|%d|%d|%ld", b.pnr, &b.pax, b.flight, b.seat,
                      &b.status, &b.fare, &t.id, &type, &t.amount, &t.ts) == 10 &&
               (unsigned)b.status <= REFUNDED && (unsigned)type <= REFUND) {
        Booking *e = booking(b.pnr);
        if (e) *e = b; else if (nb < MAXB) bk[nb++] = b; else return 0;
        t.type = type; strcpy(t.pnr, b.pnr); tx[nt++] = t;
        if (t.id >= nextt) nextt = t.id + 1;
    } else return 0;
    return 1;
}

static int jwrite(const char *body) {
    char line[440];
    int n = snprintf(line, sizeof line, "%s*%08x\n", body, fnv(body));
    int torn = crash_in == 0;
    if (crash_in > 0) crash_in--;
    size_t len = torn ? n / 2 : n;                   /* simulated power loss: half a line, no newline */
    if (fwrite(line, 1, len, jf) != len || fflush(jf) || fsync(fileno(jf))) return 0;
    if (torn) { puts("*** simulated power loss mid-write ***"); fflush(stdout); _exit(137); }
    return 1;
}

static int put(const char *fmt, ...) {
    char body[400];
    va_list ap; va_start(ap, fmt); vsnprintf(body, sizeof body, fmt, ap); va_end(ap);
    return jwrite(body) && replay(body);
}

void db_crash_in(int k) { crash_in = k; }

int db_open(const char *path, int *replayed, int *discarded) {
    if (jf) fclose(jf);
    nf = npx = nb = nt = 0; nextp = nextt = 1; crash_in = -1;
    srand(time(0) ^ getpid());
    *replayed = *discarded = 0;
    FILE *f = fopen(path, "r");
    if (f) {
        char line[512];
        long good = 0;
        while (fgets(line, sizeof line, f)) {
            size_t n = strlen(line);
            char *star = strrchr(line, '*');
            unsigned ck;
            if (line[n - 1] != '\n' || !star || sscanf(star + 1, "%8x", &ck) != 1) { (*discarded)++; continue; }
            *star = 0;
            if (fnv(line) != ck || !replay(line)) { (*discarded)++; continue; }
            (*replayed)++; good = ftell(f);
        }
        fclose(f);
        if (truncate(path, good)) return 0;          /* cut the torn tail so new appends start clean */
    }
    if (!(jf = fopen(path, "a"))) return 0;
    if (!nf) {                                       /* first run: seed flights (also journaled) */
        put("F|AC101|KHI|DXB|2026-11-01 08:30|250");
        put("F|AC202|LHE|IST|2026-11-01 14:10|420");
        put("F|AC303|ISB|LHR|2026-11-02 02:45|610");
    }
    return 1;
}

void db_close(void) { if (jf) fclose(jf); jf = 0; }

/* ---- passengers ---- */
static void clean(char *s) { for (; *s; s++) if (*s == '|' || *s == '\n') *s = ' '; }

int pax_add(const char *phone, const char *name) {
    char n[40], p[20];
    snprintf(n, sizeof n, "%s", name); snprintf(p, sizeof p, "%s", phone);
    clean(n); clean(p);
    if (npx >= MAXP || !*n || !*p) return 0;
    int id = nextp;
    return put("P|%d|%s|%s", id, n, p) ? id : 0;
}

int pax_count(void) { return npx; }

const char *pax_active_pnr(int id, const char *f) {
    for (int i = 0; i < nb; i++)
        if (bk[i].pax == id && active(&bk[i]) && !strcmp(bk[i].flight, f)) return bk[i].pnr;
    return 0;
}

int pnr_status(const char *pnr) { Booking *b = booking(pnr); return b ? b->status : -1; }

/* ---- booking rules ---- */
int seat_norm(char *out, const char *in) {
    char *e;
    long r = strtol(in, &e, 10);
    int c = toupper((unsigned char)*e);
    if (r < 1 || r > ROWS || c < 'A' || c >= 'A' + COLS || e[1]) return 0;
    snprintf(out, 4, "%d%c", (int)r, c);
    return 1;
}

const char *book_check(int id, const char *f, const char *seat) {
    if (!pax(id)) return "unknown passenger";
    if (!flight(f)) return "unknown flight";
    if (holder(f, seat)) return "seat already taken";
    if (pax_active_pnr(id, f)) return "passenger already holds a booking on this flight";
    return 0;
}

static void new_pnr(char *out) {
    static const char A[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    do { for (int i = 0; i < 6; i++) out[i] = A[rand() % 32]; out[6] = 0; } while (booking(out));
}

static int commit(const Booking *b, int type, int amount) {
    if (nb >= MAXB || nt >= MAXT) return 0;
    return put("B|%s|%d|%s|%s|%d|%d|%d|%d|%d|%ld", b->pnr, b->pax, b->flight, b->seat, b->status, b->fare,
               nextt, type, amount, (long)time(0));
}

static int fail(Req *r, const char *why) { snprintf(r->msg, sizeof r->msg, "FAILED: %s", why); return 0; }

int apply_req(Req *r) {
    Booking b;
    char ok[96], why[80];
    int amt = 0;
    if (r->type == BOOK) {
        const char *err = book_check(r->pax, r->flight, r->seat);
        if (err) return fail(r, err);
        memset(&b, 0, sizeof b);
        new_pnr(b.pnr);
        b.pax = r->pax; b.status = BOOKED; b.fare = flight(r->flight)->fare; amt = b.fare;
        snprintf(b.flight, sizeof b.flight, "%s", r->flight);
        snprintf(b.seat, sizeof b.seat, "%s", r->seat);
        snprintf(ok, sizeof ok, "PNR %s seat %s confirmed, charged $%d", b.pnr, b.seat, b.fare);
    } else {
        Booking *e = booking(r->pnr);
        if (!e) return fail(r, "unknown PNR");
        b = *e;
        int need = r->type == REFUND ? CANCELLED : BOOKED;
        if (b.status != need) {
            snprintf(why, sizeof why, "PNR %s is %s (needs %s)", b.pnr, STATUS_NAME[b.status], STATUS_NAME[need]);
            return fail(r, why);
        }
        if (r->type == CANCEL) {
            b.status = CANCELLED;
            snprintf(ok, sizeof ok, "PNR %s cancelled, seat %s released", b.pnr, b.seat);
        } else if (r->type == CHECKIN) {
            b.status = CHECKED_IN;
            snprintf(ok, sizeof ok, "PNR %s checked in, boarding pass flight %s seat %s", b.pnr, b.flight, b.seat);
        } else {
            b.status = REFUNDED; amt = -(b.fare * 80 / 100);
            snprintf(ok, sizeof ok, "PNR %s refunded $%d (20%% cancellation fee kept)", b.pnr, -amt);
        }
    }
    if (!commit(&b, r->type, amt)) return fail(r, "journal write failed, nothing changed");
    snprintf(r->pnr, sizeof r->pnr, "%s", b.pnr);
    snprintf(r->msg, sizeof r->msg, "%s", ok);
    return 1;
}

/* ---- display ---- */
void show_flights(void) {
    puts("Flight  Route     Departure          Fare   Taken");
    for (int i = 0; i < nf; i++)
        printf("%-7s %s->%s  %-17s $%-5d %d/%d\n", fl[i].id, fl[i].from, fl[i].to, fl[i].depart, fl[i].fare,
               taken(fl[i].id), ROWS * COLS);
}

void show_seats(const char *id) {
    Flight *f = flight(id);
    if (!f) { puts("unknown flight"); return; }
    printf("%s %s->%s  %s  fare $%d  (%d/%d taken)\n     ", f->id, f->from, f->to, f->depart, f->fare, taken(id), ROWS * COLS);
    for (int c = 0; c < COLS; c++) printf(" %c ", 'A' + c);
    putchar('\n');
    for (int r = 1; r <= ROWS; r++) {
        printf("%3d  ", r);
        for (int c = 0; c < COLS; c++) {
            char s[4]; snprintf(s, sizeof s, "%d%c", r, 'A' + c);
            Booking *b = holder(id, s);
            printf("[%c]", !b ? '.' : b->status == CHECKED_IN ? '#' : 'X');
        }
        putchar('\n');
    }
    puts(".=free  X=booked  #=checked in");
}

void show_pax(void) {
    for (int i = 0; i < npx; i++) printf("#%-3d %-24s %s\n", px[i].id, px[i].name, px[i].phone);
    if (!npx) puts("no passengers yet (pax add <phone> <name>)");
}

static void txn_row(const Txn *t) {
    char ts[20]; time_t tt = t->ts;
    strftime(ts, sizeof ts, "%m-%d %H:%M:%S", localtime(&tt));
    printf("      tx#%-4d %s  %-8s %+d\n", t->id, ts, TYPE_NAME[t->type], t->amount);
}

static void print_booking(const Booking *b) {
    printf("  PNR %s  %s seat %-3s %-10s fare $%d\n", b->pnr, b->flight, b->seat, STATUS_NAME[b->status], b->fare);
    for (int i = 0; i < nt; i++) if (!strcmp(tx[i].pnr, b->pnr)) txn_row(&tx[i]);
}

void show_history(int id) {
    Pax *p = pax(id);
    if (!p) { puts("unknown passenger"); return; }
    printf("Passenger #%d %s (%s)\n", p->id, p->name, p->phone);
    int any = 0;
    for (int i = 0; i < nb; i++) if (bk[i].pax == id) { print_booking(&bk[i]); any = 1; }
    if (!any) puts("  no tickets");
}

void show_pnr(const char *pnr) {
    Booking *b = booking(pnr);
    if (!b) { puts("unknown PNR"); return; }
    printf("Passenger #%d %s\n", b->pax, pax(b->pax) ? pax(b->pax)->name : "?");
    print_booking(b);
}

void show_txns(void) {
    for (int i = 0; i < nt; i++) { printf("  %s", tx[i].pnr); txn_row(&tx[i]); }
    if (!nt) puts("no transactions yet");
}

/* ================================================================
 * 4. Command line (main.c)
 * ================================================================ */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CLAMP(x, lo, hi) ((x) < (lo) ? (lo) : (x) > (hi) ? (hi) : (x))

static Req pend[MAXREQ], last[MAXREQ];               /* pending batch / last executed batch (for compare) */
static int np, nl, quantum = 2, age = 25, tracing;
static const char *CMD[] = {"book", "cancel", "checkin", "refund"};
static const int BURST[] = {6, 3, 2, 8};             /* CPU ticks per request type */
static const int PRIOR[] = {3, 4, 2, 5};             /* 1 = most important; urgent forces 1 */

static char *up(char *s) { for (char *p = s; *p; p++) *p = toupper((unsigned char)*p); return s; }

static Req mk(int type) { Req r = {0}; r.type = type; r.arrival = -1; return r; }

static const char *what(const Req *r, char *b) {
    if (r->type == BOOK) snprintf(b, 40, "pax %d %s %s", r->pax, r->flight, r->seat);
    else snprintf(b, 40, "PNR %s", r->pnr);
    return b;
}

static void submit(Req r) {
    if (np == MAXREQ) { puts("queue full, 'run' it first"); return; }
    char b[40];
    r.id = np + 1;
    if (r.arrival < 0) r.arrival = np;               /* default: one new request per tick */
    if (r.burst <= 0) r.burst = BURST[r.type];
    r.prio = r.urgent ? 1 : PRIOR[r.type];
    pend[np++] = r;
    printf("queued REQ %d: %-8s %-22s arrival t=%-3d burst %-2d prio %d%s\n", r.id, TYPE_NAME[r.type],
           what(&r, b), r.arrival, r.burst, r.prio, r.urgent ? " URGENT" : "");
}

static void opts(int argc, char **argv, int from, Req *r) {
    for (int i = from; i < argc; i++)
        if (!strcmp(argv[i], "urgent")) r->urgent = 1;
        else if (argv[i][0] == '@') r->arrival = CLAMP(atoi(argv[i] + 1), 0, 9999);
        else if (!strncmp(argv[i], "b=", 2)) r->burst = CLAMP(atoi(argv[i] + 2), 1, 99);
}

static void pending(void) {
    char b[40];
    for (int i = 0; i < np; i++)
        printf("  REQ %-2d %-8s %-22s arrival %-3d burst %-2d prio %d%s\n", pend[i].id, TYPE_NAME[pend[i].type],
               what(&pend[i], b), pend[i].arrival, pend[i].burst, pend[i].prio, pend[i].urgent ? " URGENT" : "");
    if (!np) puts("  no pending requests");
}

static void run(void) {
    static Sim s;
    if (!np) { puts("nothing pending"); return; }
    Cfg c = cfg_mlfq(quantum, age);
    c.trace = tracing;
    if (tracing) puts("--- trace: tick | running | queues (P<id>:remaining burst) ---");
    sched_run(pend, np, &c, &s);

    int o[MAXREQ];                                   /* order of completion */
    for (int i = 0; i < s.n; i++) {
        int j = i;
        while (j && s.p[o[j - 1]].finish > s.p[i].finish) { o[j] = o[j - 1]; j--; }
        o[j] = i;
    }
    puts("\nCommitting in scheduler completion order:");
    for (int k = 0; k < s.n; k++) {                  /* the scheduler decides who wins a seat race */
        Req *r = &s.p[o[k]];
        char b[40];
        apply_req(r);
        printf("  [t=%-3d] REQ %-2d %-8s %-22s %s\n", r->finish, r->id, TYPE_NAME[r->type], what(r, b), r->msg);
        fflush(stdout);
    }
    sched_report(&s, &c);
    memcpy(last, pend, sizeof pend); nl = np; np = 0;
}

static void compare(void) {
    static Sim s;
    const Req *w = np ? pend : last;
    int n = np ? np : nl;
    if (!n) { puts("no workload: queue requests or run 'demo' first"); return; }
    Cfg cs[4] = {cfg_mlfq(quantum, age), cfg_single(FCFS, quantum), cfg_single(SJF, quantum), cfg_single(RR, quantum)};
    printf("Same %d requests through each scheduler:\n", n);
    puts("Algorithm   AvgWait  AvgTAT  AvgResp  Throughput  CPU%");
    for (int i = 0; i < 4; i++) {
        sched_run(w, n, &cs[i], &s);
        Met m = sched_met(&s);
        printf("%-10s %8.2f %7.2f %8.2f %10.3f %6.1f\n", cs[i].name, m.wait, m.tat, m.resp, m.thr, m.cpu);
    }
}

/* Pre-existing bookings + a batch of 13 mixed requests: seat races, urgent requests, two long jobs. */
static void demo(void) {
    static const char *NAME[] = {"Ayesha Khan", "Bilal Ahmed", "Chen Wei", "Dina Ortiz",
                                 "Emre Yilmaz", "Fatima Noor", "Gustav Berg", "Hana Sato"};
    while (pax_count() < 8) pax_add("0300-0000000", NAME[pax_count()]);
    char pn[6][8] = {{0}};
    for (int i = 1; i <= 5; i++) {                   /* pax 1-5 hold seats on AC101 */
        const char *p = pax_active_pnr(i, "AC101");
        if (!p) {
            Req r = mk(BOOK);
            r.pax = i; strcpy(r.flight, "AC101"); snprintf(r.seat, sizeof r.seat, "%d%c", i < 5 ? 1 : 2, 'A' + (i < 5 ? i - 1 : 0));
            if (apply_req(&r)) p = r.pnr;
        }
        if (p) strcpy(pn[i], p);
    }
    if (pn[5][0]) { Req r = mk(CANCEL); strcpy(r.pnr, pn[5]); apply_req(&r); }   /* so a refund is valid */

    static const struct { int at, type, who; const char *fl, *seat; int urgent, burst; } D[] = {
        {0,  BOOK,    6, "AC101", "3A", 0, 24},      /*  1 long \ seat race: the scheduler decides the winner */
        {1,  REFUND,  5, "",      "",   0, 28},      /*  2 long */
        {2,  CHECKIN, 1, "",      "",   1, 2},       /*  3 urgent */
        {3,  CANCEL,  2, "",      "",   0, 3},       /*  4 */
        {4,  BOOK,    7, "AC101", "3A", 0, 6},       /*  5 / */
        {5,  BOOK,    8, "AC202", "5C", 0, 5},       /*  6 */
        {6,  CHECKIN, 3, "",      "",   0, 2},       /*  7 */
        {8,  CANCEL,  4, "",      "",   0, 3},       /*  8 \ cancel vs check-in on one PNR */
        {9,  CHECKIN, 4, "",      "",   0, 2},       /*  9 / */
        {10, BOOK,    1, "AC202", "1A", 0, 7},       /* 10 */
        {12, BOOK,    2, "AC303", "2D", 0, 14},      /* 11 \ seat race */
        {14, BOOK,    3, "AC303", "2D", 0, 4},       /* 12 / */
        {50, BOOK,    4, "AC202", "2B", 1, 3},       /* 13 late urgent: preempts a lower-queue job */
    };
    for (size_t i = 0; i < sizeof D / sizeof *D; i++) {
        Req r = mk(D[i].type);
        r.arrival = D[i].at; r.urgent = D[i].urgent; r.burst = D[i].burst;
        if (r.type == BOOK) { r.pax = D[i].who; strcpy(r.flight, D[i].fl); strcpy(r.seat, D[i].seat); }
        else if (pn[D[i].who][0]) strcpy(r.pnr, pn[D[i].who]);
        else continue;
        submit(r);
    }
    puts("demo batch queued: try 'run', then 'compare'");
}

static void help(void) {
    puts("flights | seats <flight> | pax | pax add <phone> <name...>\n"
         "book <pax> <flight> [seat]   (no seat = pick interactively)\n"
         "cancel|checkin|refund <pnr>\n"
         "   request options: urgent   @<arrival tick>   b=<burst ticks>\n"
         "pending | run | compare | demo\n"
         "pnr <pnr> | history <pax> | txns\n"
         "trace off|on|live | set quantum <n> | set age <n>  (0 = no aging)\n"
         "crash [k]   power loss during journal write k+1 of the next run | quit");
}

int main(void) {
    WIN_INIT();
    const char *path = getenv("AEROCORE_DB");
    if (!path) path = "aerocore.journal";
    int ok, bad;
    if (!db_open(path, &ok, &bad)) { perror(path); return 1; }
    printf("AeroCore | %s: %d records recovered, %d torn/damaged discarded | 'help' for commands\n", path, ok, bad);

    char line[256];
    int tty = isatty(0);
    for (;;) {
        if (tty) { fputs("aerocore> ", stdout); fflush(stdout); }
        if (!fgets(line, sizeof line, stdin)) break;
        int argc = 0;
        char *argv[12];
        for (char *t = strtok(line, " \t\r\n"); t && argc < 12; t = strtok(0, " \t\r\n")) argv[argc++] = t;
        if (!argc) continue;
        char *c = argv[0];
        int type = -1;
        for (int i = 0; i < 4; i++) if (!strcmp(c, CMD[i])) type = i;

        if (!strcmp(c, "quit") || !strcmp(c, "exit")) break;
        else if (!strcmp(c, "help")) help();
        else if (!strcmp(c, "flights")) show_flights();
        else if (!strcmp(c, "seats") && argc > 1) show_seats(up(argv[1]));
        else if (!strcmp(c, "pax") && argc == 1) show_pax();
        else if (!strcmp(c, "pax") && argc > 3 && !strcmp(argv[1], "add")) {
            char name[64] = "";
            for (int i = 3; i < argc; i++) { if (i > 3) strcat(name, " "); strncat(name, argv[i], sizeof name - strlen(name) - 1); }
            int id = pax_add(argv[2], name);
            if (id) printf("passenger #%d created\n", id); else puts("could not add passenger");
        }
        else if (type == BOOK && argc >= 3) {
            Req r = mk(BOOK);
            char in[32] = "";
            int oi = 3;
            r.pax = atoi(argv[1]);
            snprintf(r.flight, sizeof r.flight, "%s", up(argv[2]));
            if (argc > 3 && isdigit((unsigned char)argv[3][0])) { snprintf(in, sizeof in, "%s", argv[3]); oi = 4; }
            else {                                   /* interactive seat selection */
                show_seats(r.flight);
                fputs("Pick a seat (e.g. 3B): ", stdout); fflush(stdout);
                if (!fgets(in, sizeof in, stdin)) break;
                in[strcspn(in, "\r\n")] = 0;
            }
            if (!seat_norm(r.seat, in)) { puts("invalid seat (rows 1-10, columns A-D)"); continue; }
            opts(argc, argv, oi, &r);
            const char *err = book_check(r.pax, r.flight, r.seat);
            if (err) printf("rejected: %s\n", err); else submit(r);
        }
        else if (type > BOOK && argc >= 2) {
            Req r = mk(type);
            snprintf(r.pnr, sizeof r.pnr, "%s", up(argv[1]));
            opts(argc, argv, 2, &r);
            submit(r);
        }
        else if (!strcmp(c, "pending")) pending();
        else if (!strcmp(c, "run")) run();
        else if (!strcmp(c, "compare")) compare();
        else if (!strcmp(c, "demo")) demo();
        else if (!strcmp(c, "pnr") && argc > 1) show_pnr(up(argv[1]));
        else if (!strcmp(c, "history") && argc > 1) show_history(atoi(argv[1]));
        else if (!strcmp(c, "txns")) show_txns();
        else if (!strcmp(c, "trace") && argc > 1) tracing = !strcmp(argv[1], "live") ? 2 : !strcmp(argv[1], "on");
        else if (!strcmp(c, "set") && argc > 2 && !strcmp(argv[1], "quantum")) quantum = CLAMP(atoi(argv[2]), 1, 20);
        else if (!strcmp(c, "set") && argc > 2 && !strcmp(argv[1], "age")) age = CLAMP(atoi(argv[2]), 0, 1000);
        else if (!strcmp(c, "crash")) {
            int k = argc > 1 ? CLAMP(atoi(argv[1]), 0, 1000) : 0;
            db_crash_in(k);
            printf("armed: journal write #%d from now will be torn. 'run' a batch to trigger it.\n", k + 1);
        }
        else puts("unknown command, try 'help'");
    }
    db_close();
    return 0;
}
