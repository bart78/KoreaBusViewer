#ifndef LEARNER_H
#define LEARNER_H

/*
 * Portable learned-schedule model — no ESP-IDF dependencies.
 * Port of tools/learn_schedule.py (validated on a week of real data).
 *
 * Per route and day-type (weekday / weekend+holiday), keep a ring of the
 * last LEARNER_RING_DAYS days of confirmed arrivals (minutes of day).
 * Expected arrivals = aligned medians across the ring (i-th arrival vs
 * i-th arrival), merged when the ordinal alignment splits one real
 * arrival (a day missed a bus). Confidence = reproducibility: the
 * fraction of (day x slot) pairs where the day's nearest arrival landed
 * within LEARNER_TOLERANCE_MIN of the slot median.
 */

#define LEARNER_RING_DAYS  10
#define LEARNER_MAX_ARR    96
#define LEARNER_MAX_SLOTS  90

#define LEARNER_DT_WEEKDAY 0
#define LEARNER_DT_WEEKEND 1
#define LEARNER_DT_COUNT   2

#define LEARNER_DEDUPE_MIN    3   /* dedupe arrivals closer than this */
#define LEARNER_MERGE_GAP_MIN 6   /* merge aligned columns closer than this */
#define LEARNER_TOLERANCE_MIN 3   /* confidence match tolerance */
#define LEARNER_MIN_DAY_FRAC  0.6 /* anomaly scoring day-completeness gate */
#define LEARNER_ANOM_DEV_MIN  8   /* consistent shift threshold (min) */
#define LEARNER_ANOM_FRAC     0.6 /* fraction of ring days agreeing in sign */
#define LEARNER_MIN_RING      3   /* min ring days to score / confidence */
#define LEARNER_MIN_SLOT_N    3   /* min samples for a 'learned' slot */

typedef struct {
    int n;                        /* arrivals this day */
    int arr[LEARNER_MAX_ARR];     /* minutes of day, ascending */
} learner_day_t;

typedef struct {
    learner_day_t days[LEARNER_RING_DAYS];
    int n_days;
} learner_ring_t;

typedef struct {
    int med;                      /* rounded median arrival minute */
    int n;                        /* column sample count */
} learner_slot_t;

typedef struct {
    learner_ring_t ring[LEARNER_DT_COUNT];
    int anomaly_days;             /* consecutive anomalous days (any type) */
    int route;                    /* route number, for logging */
} learner_t;

void learner_init(learner_t* l, int route);

/* daytype: LEARNER_DT_WEEKDAY or LEARNER_DT_WEEKEND (holiday -> weekend).
 * arrivals: minutes of day, any order (deduped + sorted internally). */
void learner_learn_day(learner_t* l, int daytype, const int* arrivals, int n);

/* Returns 1 if the day was judged anomalous (consistent shift vs the ring),
 * 0 otherwise. Updates l->anomaly_days. Gated: a partial day never flags. */
int learner_score_day(learner_t* l, int daytype, const int* arrivals, int n);

/* Expected arrivals: aligned+merged medians. Returns the slot count. */
int learner_slots(learner_t* l, int daytype, learner_slot_t* out, int max);

/* Reproducibility confidence in [0,1]; 0 if not enough data. */
double learner_confidence(learner_t* l, int daytype);
double learner_slot_quality(learner_t* l, int daytype, int med);

/* Next and next-next expected arrivals after now_min. Returns 1 if found. */
int learner_next(learner_t* l, int daytype, int now_min, int* next1, int* next2);

#endif