/* make test: scheduler maths, aging/preemption, booking rules, journal recovery incl. a real torn write. */
#include "aerocore.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/wait.h>
#endif
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static Req rq(int id, int arr, int burst) {
    Req r = {0}; r.id = id; r.type = BOOK; r.arrival = arr; r.burst = burst; r.prio = 3; return r;
}

static Req op(int type, int pax, const char *fl, const char *seat, const char *pnr) {
    Req r = {0}; r.type = type; r.pax = pax;
    if (fl) strcpy(r.flight, fl);
    if (seat) strcpy(r.seat, seat);
    if (pnr) strcpy(r.pnr, pnr);
    return r;
}

static int max_gap(const Sim *s, int pid) {              /* longest stretch P<pid> went without CPU */
    int gap = 0, end = 0;
    for (int i = 0; i < s->ng; i++)
        if (s->g[i].pid == pid) { if (s->g[i].start - end > gap) gap = s->g[i].start - end; end = s->g[i].end; }
    return gap;
}

static void test_baselines(void) {
    static Sim s;
    Req w[] = {rq(1, 0, 5), rq(2, 1, 3), rq(3, 2, 1)};
    Cfg c = cfg_single(FCFS, 2); sched_run(w, 3, &c, &s);
    CHECK(s.p[0].finish == 5 && s.p[1].finish == 8 && s.p[2].finish == 9);
    c = cfg_single(SJF, 2); sched_run(w, 3, &c, &s);
    CHECK(s.p[0].finish == 5 && s.p[2].finish == 6 && s.p[1].finish == 9);
    c = cfg_single(RR, 2); sched_run(w, 3, &c, &s);                   /* P1 P1 P2 P2 P3 P1 P1 P2 P1 */
    CHECK(s.p[2].finish == 5 && s.p[1].finish == 8 && s.p[0].finish == 9);
    Met m = sched_met(&s);
    CHECK(m.cpu == 100.0 && m.wait > 3.33 && m.wait < 3.34);          /* waits 4,4,2 */
}

static void test_mlfq(void) {
    static Sim s;
    Cfg c = cfg_mlfq(2, 15);
    /* A burns its Q1 allotment (4), drops to Q2, then B lands in Q1 and preempts it */
    Req w[] = {rq(1, 0, 20), rq(2, 6, 1)};
    sched_run(w, 2, &c, &s);
    CHECK(s.p[1].finish == 7 && s.npreempt >= 1 && s.ndemote >= 1);

    /* urgent request jumps the Q1 line in MLFQ, but not in the plain RR baseline */
    Req u[] = {rq(1, 0, 4), rq(2, 0, 4), rq(3, 0, 2)};
    u[2].urgent = 1;
    sched_run(u, 3, &c, &s);
    CHECK(s.p[2].first == 0 && s.p[2].finish == 2);
    Cfg rr = cfg_single(RR, 2); sched_run(u, 3, &rr, &s);
    CHECK(s.p[2].first == 4);

    /* starvation: long job + a stream of short ones that keeps Q1 busy */
    Req st[MAXREQ]; int n = 0, sum = 0;
    st[n] = rq(1, 0, 30); n++; sum += 30;
    for (int t = 1; t < 42; t += 2) { st[n] = rq(n + 1, t, 2); n++; sum += 2; }
    c = cfg_mlfq(2, 0); sched_run(st, n, &c, &s);
    int noage = max_gap(&s, 1);
    CHECK(s.npromo == 0 && s.busy == sum && noage > 30);              /* starved ~40 ticks without aging */
    c = cfg_mlfq(2, 15); sched_run(st, n, &c, &s);
    CHECK(s.npromo > 0 && s.busy == sum && max_gap(&s, 1) <= 20);    /* aging caps the wait near age limit */
    for (int i = 0; i < n; i++) CHECK(s.p[i].finish >= s.p[i].arrival + s.p[i].burst && s.p[i].first >= s.p[i].arrival);
    int ticks = 0;
    for (int i = 0; i < s.ng; i++) if (s.g[i].pid) ticks += s.g[i].end - s.g[i].start;
    CHECK(ticks == sum);
}

static void test_airline(void) {
    const char *path = "test_aerocore.journal";
    unlink(path);
    int ok, bad;
    CHECK(db_open(path, &ok, &bad) && bad == 0);
    int a = pax_add("0300-1", "Ali Khan"), b = pax_add("0300-2", "Bea Lee");
    int cy = pax_add("0300-3", "Cy Dee");                                  /* bystander for seat checks */
    CHECK(a == 1 && b == 2 && cy == 3);

    Req r = op(BOOK, a, "AC101", "3B", 0);
    CHECK(apply_req(&r));
    char pnr[8]; strcpy(pnr, r.pnr);
    Req dup = op(BOOK, b, "AC101", "3B", 0);  CHECK(!apply_req(&dup));   /* same seat */
    Req two = op(BOOK, a, "AC101", "4B", 0);  CHECK(!apply_req(&two));   /* same passenger, same flight */
    Req ref = op(REFUND, 0, 0, 0, pnr);       CHECK(!apply_req(&ref));   /* refund before cancel */
    Req ci = op(CHECKIN, 0, 0, 0, pnr);       CHECK(apply_req(&ci));
    Req cx = op(CANCEL, 0, 0, 0, pnr);        CHECK(!apply_req(&cx));    /* no cancel after check-in */

    Req r2 = op(BOOK, a, "AC202", "1A", 0);   CHECK(apply_req(&r2));
    char pnr2[8]; strcpy(pnr2, r2.pnr);
    Req cx2 = op(CANCEL, 0, 0, 0, pnr2);      CHECK(apply_req(&cx2));
    CHECK(book_check(b, "AC202", "1A") == NULL);                          /* seat released */
    Req rf2 = op(REFUND, 0, 0, 0, pnr2);      CHECK(apply_req(&rf2));
    Req rf3 = op(REFUND, 0, 0, 0, pnr2);      CHECK(!apply_req(&rf3));    /* double refund */
    Req r3 = op(BOOK, b, "AC202", "1A", 0);   CHECK(apply_req(&r3));
    char pnr3[8]; strcpy(pnr3, r3.pnr);
    db_close();

    FILE *f = fopen(path, "a");                                           /* torn tail, no newline */
    fputs("B|ZZZZZZ|1|AC101|9D|0|250|99|0|250|1*deadb", f); fclose(f);
    CHECK(db_open(path, &ok, &bad) && bad == 1);
    CHECK(pnr_status(pnr) == CHECKED_IN && pnr_status(pnr2) == REFUNDED && pnr_status(pnr3) == BOOKED);
    CHECK(pnr_status("ZZZZZZ") < 0);
    CHECK(book_check(a, "AC202", "1A") != NULL);                          /* r3 still holds it */
    Req r4 = op(BOOK, a, "AC303", "2C", 0);   CHECK(apply_req(&r4));      /* append after repair is clean */
    db_close();
    CHECK(db_open(path, &ok, &bad) && bad == 0 && pnr_status(r4.pnr) == BOOKED);
    db_close();

    /* real crash: the child's 2nd write is torn mid-line and the process dies */
#ifndef _WIN32                                           /* needs fork(): skipped on Windows */
    fflush(stdout);
    pid_t pid = fork();
    if (!pid) {
        CHECK(db_open(path, &ok, &bad));
        db_crash_in(1);
        Req x = op(BOOK, a, "AC202", "2A", 0); CHECK(apply_req(&x));     /* write 1: survives */
        Req y = op(BOOK, b, "AC303", "2B", 0); apply_req(&y);            /* write 2: torn, _exit(137) */
        _exit(1);                                                         /* not reached */
    }
    int st; waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 137);
    CHECK(db_open(path, &ok, &bad) && bad == 1);
    CHECK(book_check(cy, "AC202", "2A") != NULL);                         /* committed before the crash */
    CHECK(book_check(cy, "AC303", "2B") == NULL);                         /* torn one never happened */
#endif
    db_close();
    unlink(path);
}

int main(void) {
    WIN_INIT();
    test_baselines();
    test_mlfq();
    test_airline();
    puts("ALL TESTS PASSED");
    return 0;
}
