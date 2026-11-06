/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef QUAKE_ECS_H
#define QUAKE_ECS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Worlds and component storage are owned and mutated by the engine's main
 * thread. A handle is valid only in the exact world that created it. */
typedef struct ecs_world_s ecs_world_t;
typedef uint32_t ecs_component_t;
typedef struct ecs_entity_s
{
    uint64_t world;
    uint32_t index;
    uint32_t generation;
} ecs_entity_t;

#define ECS_INVALID_INDEX UINT32_MAX
#define ECS_INVALID_COMPONENT UINT32_MAX
/* Only apply to a successfully registered component. */
#define ECS_MASK(component) (UINT64_C(1) << (component))

/* Capacity is fixed. Component pointers remain stable until world teardown. */
ecs_world_t *ECS_CreateWorld(uint32_t capacity);
void ECS_DestroyWorld(ecs_world_t *world);
/* Bytes owned by the world, including component storage; excludes allocator overhead. */
size_t ECS_BytesAllocated(const ecs_world_t *world);
ecs_component_t ECS_Register(ecs_world_t *world, size_t component_size);
ecs_entity_t ECS_Spawn(ecs_world_t *world, uint32_t index);
void ECS_Despawn(ecs_world_t *world, ecs_entity_t entity);
bool ECS_Alive(const ecs_world_t *world, ecs_entity_t entity);
ecs_entity_t ECS_At(const ecs_world_t *world, uint32_t index);
void *ECS_Add(ecs_world_t *world, ecs_entity_t entity, ecs_component_t component);
void *ECS_Get(const ecs_world_t *world, ecs_entity_t entity, ecs_component_t component);
void ECS_Remove(ecs_world_t *world, ecs_entity_t entity, ecs_component_t component);
/* Find the next entity with component and return its storage directly.
 * The optional out_index receives ECS_INVALID_INDEX when no match exists. */
void *ECS_NextComponentUntil(const ecs_world_t *world, uint32_t start, uint32_t end,
                             ecs_component_t component, uint32_t *out_index);
/* As above, also capture the generation-checked entity handle in the same
 * lookup. Re-query after lifecycle callbacks before advancing. */
void *ECS_NextComponentEntityUntil(const ecs_world_t *world, uint32_t start, uint32_t end,
                                   ecs_component_t component, ecs_entity_t *out_entity);
/* Return the next contiguous run of live slots holding the component. The
 * returned pointer addresses the first element; advance by out_stride bytes
 * per slot for out_count elements. The run is valid only until any entity or
 * component lifecycle mutation in this world. Query again after mutation.
 * No match returns NULL and clears the optional output parameters. */
void *ECS_NextComponentRun(const ecs_world_t *world, uint32_t start, uint32_t end,
                           ecs_component_t component, uint32_t *out_index,
                           uint32_t *out_count, size_t *out_stride);
/* Find a matching live slot >= start. Re-query after mutations; iteration is
 * ordered by slot. A zero mask matches every live entity. */
uint32_t ECS_NextUntil(const ecs_world_t *world, uint32_t start, uint32_t end, uint64_t required_mask);
uint32_t ECS_Next(const ecs_world_t *world, uint32_t start, uint64_t required_mask);

#endif
