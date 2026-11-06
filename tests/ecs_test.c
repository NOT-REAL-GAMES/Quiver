/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ecs.h"

#include <assert.h>
#include <stdalign.h>
#include <stdio.h>
#include <stdint.h>

typedef struct { int x, y; } position_t;
typedef struct { long double value; } aligned_t;

int main(void)
{
    assert(!ECS_CreateWorld(0));
    ecs_world_t *a = ECS_CreateWorld(16), *b = ECS_CreateWorld(16);
    assert(a && b);
    assert(ECS_BytesAllocated(NULL) == 0);
    size_t initial_bytes = ECS_BytesAllocated(a);
    assert(initial_bytes > 0);
    ecs_component_t pos = ECS_Register(a, sizeof(position_t));
    ecs_component_t aligned = ECS_Register(a, sizeof(aligned_t));
    ecs_component_t other = ECS_Register(b, sizeof(position_t));
    assert(pos == 0 && aligned == 1 && other == 0);
    assert(ECS_BytesAllocated(a) >= initial_bytes + 16 * (sizeof(position_t) + sizeof(aligned_t)));
    size_t registered_bytes = ECS_BytesAllocated(a);
    assert(ECS_Register(a, 0) == ECS_INVALID_COMPONENT);
    assert(ECS_Register(a, SIZE_MAX) == ECS_INVALID_COMPONENT);
    assert(ECS_BytesAllocated(a) == registered_bytes);
    ecs_entity_t e = ECS_Spawn(a, 3), foreign = ECS_Spawn(b, 3);
    assert(ECS_Alive(a, e) && ECS_Alive(b, foreign));
    assert(!ECS_Alive(a, foreign) && !ECS_Alive(b, e));
    assert(!ECS_Get(b, e, other) && !ECS_Add(a, foreign, pos));
    assert(ECS_Spawn(a, 16).index == ECS_INVALID_INDEX);
    position_t *p = ECS_Add(a, e, pos);
    assert(p && p->x == 0 && p->y == 0);
    p->x = 123;
    assert(ECS_Add(a, e, pos) == p && p->x == 123);
    void *ap = ECS_Add(a, e, aligned);
    assert(ap && (uintptr_t)ap % alignof(max_align_t) == 0);
    assert(ECS_Next(a, 0, ECS_MASK(pos)) == 3);
    assert(ECS_Next(a, 4, 0) == ECS_INVALID_INDEX);
    assert(ECS_NextUntil(a, 0, 3, 0) == ECS_INVALID_INDEX);
    assert(ECS_NextUntil(a, 0, 4, 0) == 3);
    assert(ECS_NextUntil(a, 0, UINT32_MAX, 0) == 3);
    uint32_t direct_slot = 77;
    assert(ECS_NextComponentUntil(a, 0, 4, pos, &direct_slot) == p && direct_slot == 3);
    assert(ECS_NextComponentUntil(a, 0, 3, pos, &direct_slot) == NULL && direct_slot == ECS_INVALID_INDEX);
    assert(ECS_NextComponentUntil(a, 0, 99, pos, NULL) == p);
    assert(ECS_NextComponentUntil(a, 0, 99, ECS_INVALID_COMPONENT, &direct_slot) == NULL && direct_slot == ECS_INVALID_INDEX);
    assert(ECS_NextComponentUntil(NULL, 0, 99, pos, &direct_slot) == NULL && direct_slot == ECS_INVALID_INDEX);
    ecs_entity_t direct_entity;
    assert(ECS_NextComponentEntityUntil(a, 0, 4, pos, &direct_entity) == p);
    assert(direct_entity.world == e.world && direct_entity.index == e.index && direct_entity.generation == e.generation);
    assert(!ECS_NextComponentEntityUntil(a, 0, 3, pos, &direct_entity));
    assert(direct_entity.index == ECS_INVALID_INDEX);
    ECS_Remove(a, e, pos);
    assert(!ECS_Get(a, e, pos));
    assert(ECS_Next(a, 0, ECS_MASK(pos)) == ECS_INVALID_INDEX);
    p = ECS_Add(a, e, pos);
    assert(p && p->x == 0 && p->y == 0);
    ECS_Despawn(a, e);
    assert(!ECS_Alive(a, e) && !ECS_Get(a, e, aligned));
    ecs_entity_t replacement = ECS_Spawn(a, 3);
    assert(ECS_Alive(a, replacement) && replacement.generation != e.generation);
    assert(!ECS_Add(a, e, pos));
    assert(((position_t *)ECS_Add(a, replacement, pos))->x == 0);
    ECS_Despawn(a, e); /* stale handle must not destroy replacement */
    assert(ECS_Alive(a, replacement));

    /* Re-query after each mutation; newly created later slots are visible. */
    ecs_entity_t e1 = ECS_Spawn(a, 1), e5 = ECS_Spawn(a, 5);
    assert(ECS_Add(a, e1, pos) && ECS_Add(a, e5, pos));
    uint32_t order[4], count = 0;
    for (uint32_t i = ECS_Next(a, 0, ECS_MASK(pos)); i != ECS_INVALID_INDEX;
         i = ECS_Next(a, i + 1, ECS_MASK(pos)))
    {
        order[count++] = i;
        if (i == 1)
        {
            ECS_Despawn(a, e5);
            ecs_entity_t e7 = ECS_Spawn(a, 7);
            assert(ECS_Add(a, e7, pos));
        }
    }
    assert(count == 3 && order[0] == 1 && order[1] == 3 && order[2] == 7);
    count = 0;
    uint32_t cursor = 0;
    position_t *current;
    while ((current = ECS_NextComponentUntil(a, cursor, 16, pos, &direct_slot)))
    {
        order[count++] = direct_slot;
        assert(current == ECS_Get(a, ECS_At(a, direct_slot), pos));
        cursor = direct_slot + 1;
        if (direct_slot == 1)
        {
            ECS_Despawn(a, replacement);
            ecs_entity_t e9 = ECS_Spawn(a, 9);
            assert(ECS_Add(a, e9, pos));
        }
    }
    assert(count == 3 && order[0] == 1 && order[1] == 7 && order[2] == 9);
    replacement = ECS_Spawn(a, 3);
    for (unsigned i = 2; i < 64; ++i)
        assert(ECS_Register(a, 1) == i);
    assert(ECS_Register(a, 1) == ECS_INVALID_COMPONENT);
    ecs_component_t last = 63;
    assert(ECS_Add(a, replacement, last));
    assert(ECS_Next(a, 0, ECS_MASK(last)) == 3);

    /* Batched queries preserve slot order, split at missing components and
     * despawns, and expose a correctly aligned component stride. */
    ecs_world_t *runs = ECS_CreateWorld(12);
    assert(runs);
    ecs_component_t run_pos = ECS_Register(runs, sizeof(position_t));
    for (uint32_t i = 2; i < 8; ++i)
    {
        ecs_entity_t h = ECS_Spawn(runs, i);
        position_t *value = ECS_Add(runs, h, run_pos);
        assert(value); value->x = (int)i;
    }
    ECS_Remove(runs, ECS_At(runs, 5), run_pos);
    ECS_Despawn(runs, ECS_At(runs, 7));
    uint32_t begin, length; size_t stride;
    position_t *block = ECS_NextComponentRun(runs, 0, 12, run_pos, &begin, &length, &stride);
    assert(block && begin == 2 && length == 3 && stride >= sizeof(position_t));
    for (uint32_t i = 0; i < length; ++i)
        assert(((position_t *)((char *)block + i * stride))->x == (int)(begin + i));
    block = ECS_NextComponentRun(runs, 5, UINT32_MAX, run_pos, &begin, &length, &stride);
    assert(block && begin == 6 && length == 1 && block->x == 6);
    assert(!ECS_NextComponentRun(runs, 11, 3, run_pos, &begin, &length, &stride));
    assert(begin == ECS_INVALID_INDEX && length == 0 && stride == 0);
    assert(!ECS_NextComponentRun(runs, 0, 12, ECS_INVALID_COMPONENT, &begin, &length, &stride));
    assert(begin == ECS_INVALID_INDEX && length == 0 && stride == 0);
    ECS_DestroyWorld(runs);
    ECS_DestroyWorld(a);
    ecs_world_t *c = ECS_CreateWorld(16);
    assert(c && !ECS_Alive(c, replacement));
    ECS_DestroyWorld(c);
    ECS_DestroyWorld(b);
    ECS_DestroyWorld(NULL);
    puts("ECS lifecycle and iteration passed");
    return 0;
}
