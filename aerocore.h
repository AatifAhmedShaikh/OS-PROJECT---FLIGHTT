#ifndef AEROCORE_H
#define AEROCORE_H

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
