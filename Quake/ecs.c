/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ecs.h"

#include <stdlib.h>
#include <string.h>

#define ECS_MAX_COMPONENTS 64

typedef struct ecs_slot_s
{
    uint64_t components;
    uint32_t generation;
    bool alive;
} ecs_slot_t;

typedef struct ecs_storage_s
{
    void *data;
    size_t stride;
} ecs_storage_t;

struct ecs_world_s
{
    uint64_t id;
    uint32_t capacity;
    uint32_t component_count;
    size_t bytes_allocated;
    ecs_slot_t *slots;
    ecs_storage_t components[ECS_MAX_COMPONENTS];
};

/* Worlds are created and destroyed on the main thread. Never reuse an ID. */
static uint64_t ecs_next_world_id = 1;
static const ecs_entity_t ecs_invalid_entity = {0, ECS_INVALID_INDEX, 0};

ecs_world_t *ECS_CreateWorld(uint32_t capacity)
{
    if (!capacity || !ecs_next_world_id || (SIZE_MAX - sizeof(ecs_world_t)) / capacity < sizeof(ecs_slot_t))
        return NULL;
    ecs_world_t *world = calloc(1, sizeof(*world));
    if (!world)
        return NULL;
    world->slots = calloc(capacity, sizeof(*world->slots));
    if (!world->slots)
    {
        free(world);
        return NULL;
    }
    world->id = ecs_next_world_id++;
    world->bytes_allocated = sizeof(*world) + (size_t)capacity * sizeof(*world->slots);
    world->capacity = capacity;
    for (uint32_t i = 0; i < capacity; ++i)
        world->slots[i].generation = 1;
    return world;
}

void ECS_DestroyWorld(ecs_world_t *world)
{
    if (!world)
        return;
    for (uint32_t i = 0; i < world->component_count; ++i)
        free(world->components[i].data);
    free(world->slots);
    free(world);
}

size_t ECS_BytesAllocated(const ecs_world_t *world)
{
    return world ? world->bytes_allocated : 0;
}

ecs_component_t ECS_Register(ecs_world_t *world, size_t size)
{
    if (!world || !size || world->component_count == ECS_MAX_COMPONENTS)
        return ECS_INVALID_COMPONENT;
    const size_t alignment = _Alignof(max_align_t);
    if (size > SIZE_MAX - (alignment - 1))
        return ECS_INVALID_COMPONENT;
    const size_t stride = ((size + alignment - 1) / alignment) * alignment;
    if (stride > (SIZE_MAX - world->bytes_allocated) / world->capacity)
        return ECS_INVALID_COMPONENT;
    void *data = calloc(world->capacity, stride);
    if (!data)
        return ECS_INVALID_COMPONENT;
    const ecs_component_t id = world->component_count++;
    world->components[id].data = data;
    world->components[id].stride = stride;
    world->bytes_allocated += stride * world->capacity;
    return id;
}

bool ECS_Alive(const ecs_world_t *world, ecs_entity_t entity)
{
    return world && entity.world == world->id && entity.index < world->capacity &&
           world->slots[entity.index].alive && world->slots[entity.index].generation == entity.generation;
}

ecs_entity_t ECS_At(const ecs_world_t *world, uint32_t index)
{
    if (!world || index >= world->capacity || !world->slots[index].alive)
        return ecs_invalid_entity;
    ecs_entity_t entity = {world->id, index, world->slots[index].generation};
    return entity;
}

ecs_entity_t ECS_Spawn(ecs_world_t *world, uint32_t index)
{
    if (!world || index >= world->capacity || !world->slots[index].generation)
        return ecs_invalid_entity;
    world->slots[index].alive = true;
    return ECS_At(world, index);
}

void ECS_Despawn(ecs_world_t *world, ecs_entity_t entity)
{
    if (!ECS_Alive(world, entity))
        return;
    ecs_slot_t *slot = &world->slots[entity.index];
    slot->alive = false;
    slot->components = 0;
    /* Generation zero retires the slot permanently rather than resurrect a
     * stale handle on wraparound. */
    ++slot->generation;
}

void *ECS_Get(const ecs_world_t *world, ecs_entity_t entity, ecs_component_t component)
{
    if (!ECS_Alive(world, entity) || component >= world->component_count ||
        !(world->slots[entity.index].components & ECS_MASK(component)))
        return NULL;
    const ecs_storage_t *storage = &world->components[component];
    return (char *)storage->data + (size_t)entity.index * storage->stride;
}

void *ECS_Add(ecs_world_t *world, ecs_entity_t entity, ecs_component_t component)
{
    if (!ECS_Alive(world, entity) || component >= world->component_count)
        return NULL;
    ecs_slot_t *slot = &world->slots[entity.index];
    const uint64_t mask = ECS_MASK(component);
    ecs_storage_t *storage = &world->components[component];
    void *data = (char *)storage->data + (size_t)entity.index * storage->stride;
    if (!(slot->components & mask))
    {
        memset(data, 0, storage->stride);
        slot->components |= mask;
    }
    return data;
}

void ECS_Remove(ecs_world_t *world, ecs_entity_t entity, ecs_component_t component)
{
    if (ECS_Alive(world, entity) && component < world->component_count)
        world->slots[entity.index].components &= ~ECS_MASK(component);
}

void *ECS_NextComponentUntil(const ecs_world_t *world, uint32_t start, uint32_t end,
                             ecs_component_t component, uint32_t *out_index)
{
    if (out_index)
        *out_index = ECS_INVALID_INDEX;
    if (!world || component >= world->component_count)
        return NULL;
    if (end > world->capacity)
        end = world->capacity;
    const uint64_t mask = ECS_MASK(component);
    for (uint32_t i = start; i < end; ++i)
    {
        const ecs_slot_t *slot = &world->slots[i];
        if (slot->alive && (slot->components & mask))
        {
            const ecs_storage_t *storage = &world->components[component];
            if (out_index)
                *out_index = i;
            return (char *)storage->data + (size_t)i * storage->stride;
        }
    }
    return NULL;
}

void *ECS_NextComponentRun(const ecs_world_t *world, uint32_t start, uint32_t end,
                           ecs_component_t component, uint32_t *out_index,
                           uint32_t *out_count, size_t *out_stride)
{
    if (out_index) *out_index = ECS_INVALID_INDEX;
    if (out_count) *out_count = 0;
    if (out_stride) *out_stride = 0;
    if (!world || component >= world->component_count) return NULL;
    if (end > world->capacity) end = world->capacity;
    const uint64_t mask = ECS_MASK(component);
    uint32_t first = start;
    while (first < end && (!world->slots[first].alive || !(world->slots[first].components & mask)))
        ++first;
    if (first == end || first > end) return NULL;
    uint32_t last = first + 1;
    while (last < end && world->slots[last].alive && (world->slots[last].components & mask))
        ++last;
    const ecs_storage_t *storage = &world->components[component];
    if (out_index) *out_index = first;
    if (out_count) *out_count = last - first;
    if (out_stride) *out_stride = storage->stride;
    return (char *)storage->data + (size_t)first * storage->stride;
}

void *ECS_NextComponentEntityUntil(const ecs_world_t *world, uint32_t start, uint32_t end,
                                   ecs_component_t component, ecs_entity_t *out_entity)
{
    if (out_entity) *out_entity = ecs_invalid_entity;
    if (!world || component >= world->component_count) return NULL;
    if (end > world->capacity) end = world->capacity;
    const uint64_t mask = ECS_MASK(component);
    for (uint32_t i = start; i < end; ++i)
    {
        const ecs_slot_t *slot = &world->slots[i];
        if (slot->alive && (slot->components & mask))
        {
            const ecs_storage_t *storage = &world->components[component];
            if (out_entity) *out_entity = (ecs_entity_t){world->id, i, slot->generation};
            return (char *)storage->data + (size_t)i * storage->stride;
        }
    }
    return NULL;
}

uint32_t ECS_NextUntil(const ecs_world_t *world, uint32_t start, uint32_t end, uint64_t required_mask)
{
    if (!world)
        return ECS_INVALID_INDEX;
    if (end > world->capacity)
        end = world->capacity;
    for (uint32_t i = start; i < end; ++i)
    {
        const ecs_slot_t *slot = &world->slots[i];
        if (slot->alive && (slot->components & required_mask) == required_mask)
            return i;
    }
    return ECS_INVALID_INDEX;
}

uint32_t ECS_Next(const ecs_world_t *world, uint32_t start, uint64_t required_mask)
{
    return ECS_NextUntil(world, start, UINT32_MAX, required_mask);
}
