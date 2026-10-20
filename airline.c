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
#include "aerocore.h"
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
