/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Opt-in benchmark instrumentation: never compiled into normal engine builds. */
#include "../Quake/quakedef.h"
#include "../Quake/sys.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define LEVEL_STRESS_WARMUP 8
#define LEVEL_STRESS_TICKS 128

typedef struct level_stress_flag_s { bool value; } level_stress_flag_t;
static uint64_t world_id;
static ecs_component_t flag_type = ECS_INVALID_COMPONENT;
static uint32_t first_slot, expected_entities, ticks;
static bool expected_value;
static size_t component_bytes;
static double setup_ms;
static double samples[LEVEL_STRESS_TICKS];

static int double_compare(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static uint32_t target_for_map(void)
{
    if (!strcmp(sv.name, "ecs1024")) return 1024;
    if (!strcmp(sv.name, "ecs8192")) return 8192;
    if (!strcmp(sv.name, "ecs31998")) return 31998;
    if (!strcmp(sv.name, "ecs32768")) return 32768;
    return 0;
}

/* Called once after a completed server physics step, outside sv_speeds'
 * physics timing. World identity detects reset/reload even if malloc reuses an
 * address. Never hold a pointer into ECS storage across engine callbacks. */
void ECS_LevelStressTick(void)
{
    if (!COM_CheckParm("-ecs-level-stress") || !qcvm || !qcvm->ecs_world || qcvm != &sv.qcvm)
        return;
    uint32_t target = target_for_map();
    if (!target) return;
    ecs_entity_t world_entity = ECS_At(qcvm->ecs_world, 0);
    if (!ECS_Alive(qcvm->ecs_world, world_entity))
        Sys_Error("ECS_NATIVE_STRESS_FAIL missing world entity");
    if (world_id != world_entity.world) {
        world_id = world_entity.world;
        flag_type = ECS_INVALID_COMPONENT;
        expected_entities = ticks = 0;
        expected_value = false;
    }
    first_slot = (uint32_t)qcvm->reserved_edicts;
    if (flag_type == ECS_INVALID_COMPONENT) {
        if ((uint32_t)qcvm->num_edicts < first_slot + target)
            return; /* fixture creates entities in batches */
        if ((uint32_t)qcvm->num_edicts != first_slot + target)
            Sys_Error("ECS_NATIVE_STRESS_FAIL expected exactly %u fixture entities", target);
        double start = Sys_DoubleTime();
        size_t before = ECS_BytesAllocated(qcvm->ecs_world);
        flag_type = ECS_Register(qcvm->ecs_world, sizeof(level_stress_flag_t));
        if (flag_type == ECS_INVALID_COMPONENT) Sys_Error("ECS_NATIVE_STRESS_FAIL component registration");
        component_bytes = ECS_BytesAllocated(qcvm->ecs_world) - before;
        for (uint32_t i = first_slot; i < first_slot + target; ++i) {
            ecs_entity_t e = ECS_At(qcvm->ecs_world, i);
            edict_ecs_binding_t *binding = ECS_Get(qcvm->ecs_world, e, qcvm->ecs_edict_component);
            level_stress_flag_t *flag = ECS_Add(qcvm->ecs_world, e, flag_type);
            if (!binding || binding->edict != EDICT_NUM((int)i) || !flag || flag->value)
                Sys_Error("ECS_NATIVE_STRESS_FAIL invalid initial binding %u", i);
        }
        setup_ms = (Sys_DoubleTime() - start) * 1000.0;
        expected_entities = target;
        Con_Printf("ECS_NATIVE_STRESS_BEGIN entities=%u component_bytes=%zu setup_ms=%.6f\n", target, component_bytes, setup_ms);
    }
    if (ticks >= LEVEL_STRESS_WARMUP + LEVEL_STRESS_TICKS) return;
    uint32_t cursor = first_slot, slot, visited = 0;
    level_stress_flag_t *flag;
    double start = Sys_DoubleTime();
    while ((flag = ECS_NextComponentUntil(qcvm->ecs_world, cursor, first_slot + target, flag_type, &slot)) != NULL) {
        if (slot != first_slot + visited) Sys_Error("ECS_NATIVE_STRESS_FAIL ordering");
        flag->value = !flag->value;
        ++visited;
        cursor = slot + 1;
    }
    double elapsed_ms = (Sys_DoubleTime() - start) * 1000.0;
    if (visited != expected_entities) Sys_Error("ECS_NATIVE_STRESS_FAIL visit count %u/%u", visited, expected_entities);
    expected_value = !expected_value;
    /* Full verification is intentionally outside the timed toggle pass. */
    for (uint32_t i = first_slot; i < first_slot + target; ++i) {
        ecs_entity_t e = ECS_At(qcvm->ecs_world, i);
        flag = ECS_Get(qcvm->ecs_world, e, flag_type);
        edict_ecs_binding_t *binding = ECS_Get(qcvm->ecs_world, e, qcvm->ecs_edict_component);
        if (!flag || flag->value != expected_value || !binding || binding->edict != EDICT_NUM((int)i))
            Sys_Error("ECS_NATIVE_STRESS_FAIL verification at slot %u", i);
    }
    if (ticks >= LEVEL_STRESS_WARMUP)
        samples[ticks - LEVEL_STRESS_WARMUP] = elapsed_ms;
    ++ticks;
    if (ticks == LEVEL_STRESS_WARMUP + LEVEL_STRESS_TICKS) {
        double sum = 0;
        for (unsigned i = 0; i < LEVEL_STRESS_TICKS; ++i) sum += samples[i];
        qsort(samples, LEVEL_STRESS_TICKS, sizeof(*samples), double_compare);
        double median = (samples[LEVEL_STRESS_TICKS/2 - 1] + samples[LEVEL_STRESS_TICKS/2]) * 0.5;
        double p95 = samples[(LEVEL_STRESS_TICKS - 1) * 95 / 100];
        Con_Printf("ECS_NATIVE_STRESS_PASS entities=%u warmup=%u ticks=%u mean_ms=%.6f median_ms=%.6f p95_ms=%.6f component_bytes=%zu setup_ms=%.6f verified_ticks=%u\n",
                   target, LEVEL_STRESS_WARMUP, LEVEL_STRESS_TICKS, sum / LEVEL_STRESS_TICKS,
                   median, p95, component_bytes, setup_ms, ticks);
    }
}
