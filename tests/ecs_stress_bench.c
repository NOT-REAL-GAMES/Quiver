/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Standalone stress benchmark of real ECS storage and ordered queries.
 * Each independent synthetic world is a level-sized workload; no map,
 * edicts, QuakeC, renderer, or network processing is included. */
#include "ecs.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static double now_seconds(void)
{
    LARGE_INTEGER counter, frequency;
    if (!QueryPerformanceCounter(&counter) || !QueryPerformanceFrequency(&frequency)) {
        fputs("QueryPerformanceCounter failed\n", stderr); exit(1);
    }
    return (double)counter.QuadPart / (double)frequency.QuadPart;
}
#else
#include <time.h>
static double now_seconds(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) {
        perror("clock_gettime"); exit(1);
    }
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}
#endif

typedef struct toggle_component_s { bool value; } toggle_component_t;

static void die(const char *message)
{
    fprintf(stderr, "FAIL: %s\n", message);
    exit(1);
}

static void flip_tick(ecs_world_t *world, ecs_component_t type, uint32_t count)
{
    uint32_t cursor = 0, slot = 0, updated = 0;
    toggle_component_t *component;
    while ((component = ECS_NextComponentUntil(world, cursor, count, type, &slot)) != NULL) {
        if (slot != updated) die("query omitted, duplicated, or reordered an entity");
        component->value = !component->value;
        ++updated;
        cursor = slot + 1;
    }
    if (updated != count) die("tick updated the wrong number of entities");
}

/* This full pass runs outside the timed region on every warmup and measured
 * tick. It checks every component, not a checksum that could hide errors. */
static void verify_tick(const ecs_world_t *world, ecs_component_t type,
                        uint32_t count, bool expected)
{
    for (uint32_t i = 0; i < count; ++i) {
        const ecs_entity_t entity = ECS_At(world, i);
        const toggle_component_t *component = ECS_Get(world, entity, type);
        if (!component || component->value != expected) die("incorrect component value or missing entity");
    }
    if (ECS_NextUntil(world, count, UINT32_MAX, ECS_MASK(type)) != ECS_INVALID_INDEX)
        die("unexpected entity beyond the requested count");
}

static int compare_double(const void *a, const void *b)
{
    const double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static unsigned parse_count(const char *s)
{
    char *end;
    errno = 0;
    unsigned long n = strtoul(s, &end, 10);
    if (errno || !*s || *end || n < 1 || n > 10000) die("counts must be integers in 1..10000");
    return (unsigned)n;
}

int main(int argc, char **argv)
{
    unsigned ticks = 200, warmup = 20, repeats = 5;
    if (argc > 1) ticks = parse_count(argv[1]);
    if (argc > 2) warmup = parse_count(argv[2]);
    if (argc > 3) repeats = parse_count(argv[3]);
    if (argc > 4 || (size_t)ticks > SIZE_MAX / repeats / sizeof(double)) {
        fprintf(stderr, "usage: %s [ticks=200] [warmup=20] [repeats=5]\n", argv[0]); return 1;
    }
    const uint32_t sizes[] = {1024, 8192, 32768};
    printf("Synthetic ECS worlds: %u warmup + %u measured ticks, %u fresh worlds per size\n", warmup, ticks, repeats);
    puts("Timing excludes validation; every component value is verified after every tick.");
    for (size_t n = 0; n < sizeof(sizes)/sizeof(sizes[0]); ++n) {
        const uint32_t count = sizes[n];
        const size_t sample_count = (size_t)ticks * repeats;
        double *samples = malloc(sample_count * sizeof(*samples));
        double *setup = malloc(repeats * sizeof(*setup));
        if (!samples || !setup) die("sample allocation failed");
        size_t bytes = 0;
        for (unsigned rep = 0; rep < repeats; ++rep) {
            double start = now_seconds();
            ecs_world_t *world = ECS_CreateWorld(count);
            if (!world) die("world allocation failed");
            ecs_component_t component_type = ECS_Register(world, sizeof(toggle_component_t));
            if (component_type == ECS_INVALID_COMPONENT) die("component registration failed");
            for (uint32_t i = 0; i < count; ++i) {
                const ecs_entity_t entity = ECS_Spawn(world, i);
                toggle_component_t *component = ECS_Add(world, entity, component_type);
                if (!component || component->value) die("component creation failed or not initially false");
            }
            setup[rep] = (now_seconds() - start) * 1000.0;
            bytes = ECS_BytesAllocated(world);
            verify_tick(world, component_type, count, false);
            bool expected = false;
            for (unsigned tick = 0; tick < warmup + ticks; ++tick) {
                start = now_seconds();
                flip_tick(world, component_type, count);
                double elapsed = now_seconds() - start;
                if (elapsed < 0) die("monotonic clock moved backward");
                if (tick >= warmup) samples[(size_t)rep * ticks + tick - warmup] = elapsed * 1000.0;
                expected = !expected;
                verify_tick(world, component_type, count, expected);
            }
            ECS_DestroyWorld(world);
        }
        qsort(samples, sample_count, sizeof(*samples), compare_double);
        qsort(setup, repeats, sizeof(*setup), compare_double);
        const double median_ms = sample_count % 2 ? samples[sample_count/2] : (samples[sample_count/2-1]+samples[sample_count/2])*0.5;
        const double p95 = samples[(sample_count-1)*95/100];
        const double median_setup = repeats % 2 ? setup[repeats/2] : (setup[repeats/2-1]+setup[repeats/2])*0.5;
        printf("entities=%u setup_ms=%.6f ecs_bytes=%zu tick_median_ms=%.6f tick_p95_ms=%.6f ns_per_entity=%.3f samples=%zu verified_ticks=%u\n",
               count, median_setup, bytes, median_ms, p95, median_ms * 1000000.0 / count,
               sample_count, (warmup + ticks) * repeats);
        fflush(stdout);
        free(setup); free(samples);
    }
    puts("PASS: every entity flipped exactly once and matched expected state on every tick");
    return 0;
}
