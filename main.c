#include "aerocore.h"
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
