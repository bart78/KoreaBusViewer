/*
 * Host-side golden test for the learner port: reads per-day confirmed
 * arrivals (route daytype count minutes...) from stdin, feeds them through
 * learner.c in order, and prints slots + confidence in a fixed format.
 * Compile: cc -o learner_test host_test.c espidf/src/learner/learner.c
 */
#include <stdio.h>
#include <stdlib.h>
#include "../espidf/src/learner/learner.h"

#define MAX_ROUTES 8

int main(void) {
    learner_t learners[MAX_ROUTES];
    int route_ids[MAX_ROUTES] = {0};
    int n_routes = 0;
    int route, daytype, n, minute;

    while (scanf("%d %d %d", &route, &daytype, &n) == 3) {
        int arr[LEARNER_MAX_ARR];
        for (int i = 0; i < n && i < LEARNER_MAX_ARR; i++)
            scanf("%d", &arr[i]);
        if (n > LEARNER_MAX_ARR) {
            int skip;
            for (int i = LEARNER_MAX_ARR; i < n; i++) scanf("%d", &skip);
            n = LEARNER_MAX_ARR;
        }

        int idx = -1;
        for (int i = 0; i < n_routes; i++)
            if (route_ids[i] == route) { idx = i; break; }
        if (idx < 0 && n_routes < MAX_ROUTES) {
            idx = n_routes;
            route_ids[idx] = route;
            learner_init(&learners[idx], route);
            n_routes++;
        }
        if (idx < 0) continue;
        learner_learn_day(&learners[idx], daytype, arr, n);
        (void)minute;
    }

    /* print the same format as the Python reference */
    for (int i = 0; i < n_routes; i++) {
        for (int dt = 0; dt < LEARNER_DT_COUNT; dt++) {
            learner_slot_t slots[LEARNER_MAX_SLOTS];
            int ns = learner_slots(&learners[i], dt, slots, LEARNER_MAX_SLOTS);
            double conf = learner_confidence(&learners[i], dt);
            int anom = learners[i].anomaly_days;
            printf("route %d dt %d slots %d conf %.4f anom %d",
                   route_ids[i], dt, ns, conf, anom);
            for (int s = 0; s < ns; s++)
                printf(" %d:%d", slots[s].med, slots[s].n);
            printf("\n");
        }
    }
    return 0;
}