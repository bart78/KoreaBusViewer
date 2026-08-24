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
    d->n = n;
    memcpy(d->arr, arrivals, n * sizeof(int));
    day_dedupe(d);
}

int learner_score_day(learner_t* l, int daytype, const int* arrivals, int n) {
    if (daytype < 0 || daytype >= LEARNER_DT_COUNT) return 0;
    return shifted_vs_ring(&l->ring[daytype], arrivals, n);
}

int learner_slots(learner_t* l, int daytype, learner_slot_t* out, int max) {
    if (daytype < 0 || daytype >= LEARNER_DT_COUNT) return 0;
    learner_ring_t* ring = &l->ring[daytype];
    if (ring->n_days == 0 || max <= 0) return 0;

    int longest = 0;
    for (int i = 1; i < ring->n_days; i++)
        if (ring->days[i].n > ring->days[longest].n) longest = i;

    double meds[LEARNER_MAX_SLOTS];
    int ns = 0;
    for (int i = 0; i < ring->days[longest].n && ns < LEARNER_MAX_SLOTS; i++) {
        double col[LEARNER_RING_DAYS];
        int nc = 0;
        for (int k = 0; k < ring->n_days; k++)
            if (i < ring->days[k].n) col[nc++] = ring->days[k].arr[i];
        if (nc > 0) {
            /* median, matching statistics.median (even counts average) */
            qsort(col, nc, sizeof(double), cmp_dbl);
            meds[ns++] = (col[(nc - 1) / 2] + col[nc / 2]) / 2.0;
        }
    }

    /* merge close columns (ordinal alignment splits one arrival); keep the
     * medians as doubles, sorted (columns are not monotonic when a day
     * drifts), and round only at the end */
    int order[LEARNER_MAX_SLOTS];
    for (int i = 0; i < ns; i++) order[i] = i;
    for (int i = 1; i < ns; i++) {
        int j = i;
        while (j > 0 && meds[order[j]] < meds[order[j - 1]]) {
            int t = order[j]; order[j] = order[j - 1]; order[j - 1] = t;
            j--;
        }
    }
    double mmeds[LEARNER_MAX_SLOTS];
    int mcnt[LEARNER_MAX_SLOTS];
    int nslots = 0;
    for (int oi = 0; oi < ns; oi++) {
        int i = order[oi];
        int cn = 0;
        for (int k = 0; k < ring->n_days; k++)
            if (i < ring->days[k].n) cn++;
        if (nslots == 0 || meds[i] - mmeds[nslots - 1] > LEARNER_MERGE_GAP_MIN) {
            mmeds[nslots] = meds[i];
            mcnt[nslots] = cn;
            nslots++;
        } else {
            int total = mcnt[nslots - 1] + cn;
            mmeds[nslots - 1] =
                (mmeds[nslots - 1] * mcnt[nslots - 1] + meds[i] * cn) / total;
            mcnt[nslots - 1] = total;
        }
        if (nslots >= max) break;
    }
    for (int s = 0; s < nslots; s++) {
        out[s].med = round_banker(mmeds[s]);
        out[s].n = mcnt[s];
    }
    return nslots;
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