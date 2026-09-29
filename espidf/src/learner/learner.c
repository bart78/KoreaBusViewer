/*
 * Learned-schedule model — port of tools/learn_schedule.py.
 * Pure computation: no ESP-IDF includes, host-testable.
 */
#include "learner.h"
#include <stdlib.h>
#include <string.h>

static int cmp_int(const void* a, const void* b) {
    return *(const int*)a - *(const int*)b;
}

static int cmp_dbl(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b;
    return (x > y) - (x < y);
}

/* Python round(): half-to-even */
static int round_banker(double x) {
    int lo = (int)x;
    double frac = x - lo;
    if (frac < 0.5) return lo;
    if (frac > 0.5) return lo + 1;
    return (lo % 2 == 0) ? lo : lo + 1;
}

static int median_int(int* buf, int n) {
    qsort(buf, n, sizeof(int), cmp_int);
    return buf[n / 2];
}

static double median_dbl(double* buf, int n) {
    /* cheap selection via qsort on indices */
    int idx[LEARNER_MAX_ARR];
    for (int i = 0; i < n; i++) idx[i] = i;
    /* insertion sort by buf[] */
    for (int i = 1; i < n; i++) {
        int j = i;
        while (j > 0 && buf[idx[j]] < buf[idx[j - 1]]) {
            int t = idx[j]; idx[j] = idx[j - 1]; idx[j - 1] = t;
            j--;
        }
    }
    return buf[idx[n / 2]];
}

void learner_init(learner_t* l, int route) {
    memset(l, 0, sizeof(*l));
    l->route = route;
}

static void day_dedupe(learner_day_t* d) {
    if (d->n <= 1) return;
    qsort(d->arr, d->n, sizeof(int), cmp_int);
    int out = 0, last = -9999;
    for (int i = 0; i < d->n; i++) {
        if (d->arr[i] - last >= LEARNER_DEDUPE_MIN) {
            d->arr[out++] = d->arr[i];
            last = d->arr[i];
        }
    }
    d->n = out;
}

/* consistent shift of today vs each ring day (ordinal alignment) */
static int shifted_vs_ring(learner_ring_t* ring, const int* arr, int n) {
    if (ring->n_days < LEARNER_MIN_RING) return 0;

    /* completeness gate: a partial day can't be judged */
    int lens[LEARNER_RING_DAYS];
    for (int i = 0; i < ring->n_days; i++) lens[i] = ring->days[i].n;
    int med_len = median_int(lens, ring->n_days);
    if (n < (int)(LEARNER_MIN_DAY_FRAC * med_len)) return 0;

    double shifts[LEARNER_RING_DAYS];
    int ns = 0;
    for (int i = 0; i < ring->n_days && ns < LEARNER_RING_DAYS; i++) {
        int m = ring->days[i].n < n ? ring->days[i].n : n;
        if (m < 5) continue;
        double col[LEARNER_MAX_ARR];
        for (int k = 0; k < m; k++) col[k] = arr[k] - ring->days[i].arr[k];
        shifts[ns++] = median_dbl(col, m);
    }
    if (ns < LEARNER_MIN_RING) return 0;
    double med = median_dbl(shifts, ns);
    int same = 0;
    for (int i = 0; i < ns; i++)
        if ((shifts[i] > 0) == (med > 0)) same++;
    return (med > LEARNER_ANOM_DEV_MIN || med < -LEARNER_ANOM_DEV_MIN) &&
           same >= LEARNER_ANOM_FRAC * ns;
}

void learner_learn_day(learner_t* l, int daytype, const int* arrivals, int n) {
    if (daytype < 0 || daytype >= LEARNER_DT_COUNT) return;
    if (n <= 0) return;
    if (n > LEARNER_MAX_ARR) n = LEARNER_MAX_ARR;

    learner_ring_t* ring = &l->ring[daytype];

    /* dedupe the input first so the gate and the ring see the same count */
    learner_day_t in;
    in.n = n;
    memcpy(in.arr, arrivals, n * sizeof(int));
    day_dedupe(&in);
    n = in.n;

    /* completeness gate: a partial day (board unplugged, capture gap)
     * cannot be ordinally aligned — its few arrivals land in arbitrary
     * columns and skew every slot's median. Skip it once the ring has a
     * reference to judge against. The COUNT gate alone misses days that
     * reach 60% of the arrivals but STARTED late (their compressed
     * ordinals shift every evening column) — so also require the day to
     * SPAN most of the service window. */
    if (ring->n_days >= LEARNER_MIN_RING) {
        int lens[LEARNER_RING_DAYS];
        int spans[LEARNER_RING_DAYS];
        for (int i = 0; i < ring->n_days; i++) {
            lens[i] = ring->days[i].n;
            spans[i] = ring->days[i].arr[ring->days[i].n - 1] -
                       ring->days[i].arr[0];
        }
        int med_len = median_int(lens, ring->n_days);
        if (n < LEARNER_MIN_DAY_FRAC * med_len) return;
        int med_span = median_int(spans, ring->n_days);
        if (in.arr[n - 1] - in.arr[0] < LEARNER_MIN_DAY_FRAC * med_span) return;
    }

    if (shifted_vs_ring(ring, arrivals, n))
        l->anomaly_days++;
    else
        l->anomaly_days = 0;

    if (ring->n_days >= LEARNER_RING_DAYS) {
        /* slide the ring */
        for (int i = 1; i < LEARNER_RING_DAYS; i++)
            ring->days[i - 1] = ring->days[i];
        ring->n_days = LEARNER_RING_DAYS - 1;
    }
    learner_day_t* d = &ring->days[ring->n_days++];
    *d = in;
}

int learner_score_day(learner_t* l, int daytype, const int* arrivals, int n) {
    if (daytype < 0 || daytype >= LEARNER_DT_COUNT) return 0;
    return shifted_vs_ring(&l->ring[daytype], arrivals, n);
}

/* time-anchored alignment buffers (module-static: the learner is single-
 * threaded; keeps big arrays off the caller's stack) */
static int scol[LEARNER_MAX_ARR][LEARNER_RING_DAYS];
static int scnt[LEARNER_MAX_ARR];

int learner_slots(learner_t* l, int daytype, learner_slot_t* out, int max) {
    if (daytype < 0 || daytype >= LEARNER_DT_COUNT) return 0;
    learner_ring_t* ring = &l->ring[daytype];
    if (ring->n_days == 0 || max <= 0) return 0;

    /* reference: the longest day (densest) */
    int longest = 0;
    for (int i = 1; i < ring->n_days; i++)
        if (ring->days[i].n > ring->days[longest].n) longest = i;
    int nref = ring->days[longest].n;
    const int* refs = ring->days[longest].arr;

    /* TIME-ANCHORED alignment: each day's arrivals match the nearest
     * unused reference point within LEARNER_ALIGN_TOL_MIN, one-to-one.
     * Ordinal alignment drifts when day counts vary (capture noise) and
     * smears evening columns (medians landing between real trips, q -> 0);
     * time-anchoring survives the drift. */
    for (int j = 0; j < nref; j++) scnt[j] = 0;
    for (int k = 0; k < ring->n_days; k++) {
        int used[LEARNER_MAX_ARR] = {0};
        for (int i = 0; i < ring->days[k].n; i++) {
            int a = ring->days[k].arr[i];
            int best = 9999, bi = -1;
            for (int j = 0; j < nref; j++) {
                if (used[j]) continue;
                int dist = a - refs[j];
                if (dist < 0) dist = -dist;
                if (dist < best) { best = dist; bi = j; }
            }
            if (bi >= 0 && best <= LEARNER_ALIGN_TOL_MIN)
                scol[bi][scnt[bi]++] = a, used[bi] = 1;
        }
    }

    double mmeds[LEARNER_MAX_SLOTS];
    int mcnt[LEARNER_MAX_SLOTS];
    int nslots = 0;
    for (int j = 0; j < nref && nslots < LEARNER_MAX_SLOTS; j++) {
        if (scnt[j] == 0) continue;
        qsort(scol[j], scnt[j], sizeof(int), cmp_int);
        int nc = scnt[j];
        mmeds[nslots] = (scol[j][(nc - 1) / 2] + scol[j][nc / 2]) / 2.0;
        mcnt[nslots] = nc;
        nslots++;
    }

    /* merge close columns (one arrival can match two references); keep the
     * medians as doubles, sorted, and round only at the end */
    int order[LEARNER_MAX_SLOTS];
    for (int i = 0; i < nslots; i++) order[i] = i;
    for (int i = 1; i < nslots; i++) {
        int j = i;
        while (j > 0 && mmeds[order[j]] < mmeds[order[j - 1]]) {
            int t = order[j]; order[j] = order[j - 1]; order[j - 1] = t;
            j--;
        }
    }
    int nsl = 0;
    for (int oi = 0; oi < nslots; oi++) {
        int i = order[oi];
        if (nsl == 0 || mmeds[i] - mmeds[nsl - 1] > LEARNER_MERGE_GAP_MIN) {
            mmeds[nsl] = mmeds[i];
            mcnt[nsl] = mcnt[i];
            nsl++;
        } else {
            int total = mcnt[nsl - 1] + mcnt[i];
            mmeds[nsl - 1] =
                (mmeds[nsl - 1] * mcnt[nsl - 1] + mmeds[i] * mcnt[i]) / total;
            mcnt[nsl - 1] = total;
        }
        if (nsl >= max) break;
    }
    for (int s = 0; s < nsl; s++) {
        out[s].med = round_banker(mmeds[s]);
        out[s].n = mcnt[s];
    }
    return nsl;
}

double learner_confidence(learner_t* l, int daytype) {
    if (daytype < 0 || daytype >= LEARNER_DT_COUNT) return 0;
    learner_ring_t* ring = &l->ring[daytype];
    if (ring->n_days < LEARNER_MIN_RING) return 0;

    learner_slot_t slots[LEARNER_MAX_SLOTS];
    int ns = learner_slots(l, daytype, slots, LEARNER_MAX_SLOTS);
    if (ns == 0) return 0;

    int matched = 0, total = 0;
    for (int s = 0; s < ns; s++) {
        for (int k = 0; k < ring->n_days; k++) {
            /* days with no arrival near the slot (a missed bus) are
             * skipped, not punished; near arrivals must be tight */
            int best = 9999;
            for (int i = 0; i < ring->days[k].n; i++) {
                int diff = ring->days[k].arr[i] - slots[s].med;
                if (diff < 0) diff = -diff;
                if (diff < best) best = diff;
            }
            if (best > LEARNER_MERGE_GAP_MIN) continue;
            total++;
            if (best <= LEARNER_TOLERANCE_MIN) matched++;
        }
    }
    return total ? (double)matched / total : 0;
}

/* per-slot quality: the same tightness measure as learner_confidence, but
 * for ONE slot median. The route confidence is an average — a loose 3pm
 * slot hides behind a tight 7am one. This lets the display gate claims
 * per slot: a slot whose days scatter >3 min withholds itself. */
double learner_slot_quality(learner_t* l, int daytype, int med) {
    if (daytype < 0 || daytype >= LEARNER_DT_COUNT) return 0;
    learner_ring_t* ring = &l->ring[daytype];
    if (ring->n_days < LEARNER_MIN_RING) return 0;
    int matched = 0, total = 0;
    for (int k = 0; k < ring->n_days; k++) {
        int best = 9999;
        for (int i = 0; i < ring->days[k].n; i++) {
            int diff = ring->days[k].arr[i] - med;
            if (diff < 0) diff = -diff;
            if (diff < best) best = diff;
        }
        if (best > LEARNER_MERGE_GAP_MIN) continue;
        total++;
        if (best <= LEARNER_TOLERANCE_MIN) matched++;
    }
    return total ? (double)matched / total : 0;
}

int learner_next(learner_t* l, int daytype, int now_min, int* next1, int* next2) {
    if (daytype < 0 || daytype >= LEARNER_DT_COUNT) return 0;
    learner_slot_t slots[LEARNER_MAX_SLOTS];
    int ns = learner_slots(l, daytype, slots, LEARNER_MAX_SLOTS);
    if (ns == 0) return 0;

    int found = 0;
    for (int s = 0; s < ns; s++) {
        if (slots[s].med > now_min && slots[s].n >= LEARNER_MIN_SLOT_N) {
            *next1 = slots[s].med;
            for (int t = s + 1; t < ns; t++) {
                if (slots[t].med > *next1 + 1 && slots[t].n >= LEARNER_MIN_SLOT_N) {
                    *next2 = slots[t].med;
                    break;
                }
            }
            found = 1;
            break;
        }
    }
    return found;
}