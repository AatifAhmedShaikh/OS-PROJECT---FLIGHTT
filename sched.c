#include "aerocore.h"
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
