/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>
#include "quakedef.h"
#include "octree_engine.h"
#include "octree_checkpoint.h"
#include "octree_paging.h"
#include "octree_qc.h"

namespace
{
using namespace quiver;
constexpr size_t MaxPendingEvictions = 64;
struct Slot
{
	uint64_t id = 0, revision = 0, generation = 0;
	double	 distantSince = -1, retry = 0;
	bool	 writing = false;
};
struct Deletion
{
	uint64_t revision = 0;
	double	 retry = 0;
	bool	 busy = false;
};
struct Query
{
	ChunkKey center;
	uint64_t after = 0;
	double	 retry = 0;
	bool	 busy = false;
};
struct Committed
{
	uint64_t			 id, generation;
	double				 snapshotTime;
	std::vector<uint8_t> bytes;
};
struct ReferenceField
{
	int	 offset;
	bool pointer;
};
std::map<size_t, Committed>		  committed;
std::vector<ReferenceField>		  globalReferences, fieldReferences;
bool							  referenceSchemaReady = false;
size_t							  pendingEvictions = 0;
std::vector<Slot>				  slots;
std::vector<Query>				  queries;
std::unordered_map<uint64_t, int> active;
std::map<uint64_t, Deletion>	  deletions;
std::set<uint64_t>				  loading;
uint64_t						  epoch = 1, generation = 1, loaded = 0, evicted = 0, cancelled = 0;
size_t							  scan = 1, queryClient = 0, pinned = 0;
bool							  evicting = false, flushing = false;
cvar_t							  world_entity_paging = {"world_entity_paging", "1", CVAR_ARCHIVE};
cvar_t							  world_entity_radius = {"world_entity_radius", "2048", CVAR_ARCHIVE};
cvar_t							  world_entity_idle = {"world_entity_idle", "2", CVAR_ARCHIVE};
cvar_t							  world_entity_budget = {"world_entity_budget", "4", CVAR_ARCHIVE};
cvar_t							  world_entity_limit = {"world_entity_limit", "1024", CVAR_ARCHIVE};

struct VMGuard
{
	qcvm_t *previous = qcvm;
	VMGuard ()
	{
		PR_SwitchQCVM (nullptr);
		PR_SwitchQCVM (&sv.qcvm);
	}
	~VMGuard ()
	{
		PR_SwitchQCVM (nullptr);
		PR_SwitchQCVM (previous);
	}
};
void Resize ()
{
	slots.resize (qcvm->max_edicts);
	queries.resize (svs.maxclients);
	if (!referenceSchemaReady)
	{
		auto collect = [] (const ddef_t *definitions, int count, int limit, std::vector<ReferenceField> &result)
		{
			for (int i = 0; i < count; ++i)
			{
				const auto &definition = definitions[i];
				const auto	type = definition.type & ~DEF_SAVEGLOBAL;
				if ((type == ev_entity || type == ev_pointer) && definition.ofs < limit)
					result.push_back ({definition.ofs, type == ev_pointer});
			}
		};
		collect (qcvm->globaldefs, qcvm->progs->numglobaldefs, qcvm->progs->numglobals, globalReferences);
		collect (qcvm->fielddefs, qcvm->progs->numfielddefs, qcvm->progs->entityfields, fieldReferences);
		referenceSchemaReady = true;
	}
}
double BoundedCvar (const cvar_t &value, double minimum, double maximum, double fallback)
{
	return std::isfinite (value.value) ? std::clamp (double (value.value), minimum, maximum) : fallback;
}
double Radius ()
{
	return BoundedCvar (world_entity_radius, 1024, 4096, 2048);
}
bool Near (const WorldPosition &position)
{
	for (int i = 0; i < svs.maxclients; ++i)
	{
		if (!svs.clients[i].active || !svs.clients[i].spawned)
			continue;
		WorldPosition		  player;
		std::array<double, 3> delta;
		if (OW_GetEntityPosition (svs.clients[i].edict, player) && WorldDelta (OW_GetWorld ()->Config (), position, player, delta, Radius ()))
			return true;
	}
	return false;
}
std::vector<uint8_t> IncomingReferences ()
{
	std::vector<uint8_t> incoming (qcvm->num_edicts);
	auto				 mark = [&] (int offset, int source, bool pointer)
	{
		if (offset > 0 && (pointer || offset % qcvm->edict_size == 0) && offset / qcvm->edict_size < qcvm->num_edicts && offset / qcvm->edict_size != source)
			incoming[offset / qcvm->edict_size] = 1;
	};
	for (const auto &field : globalReferences)
		mark (G_INT (field.offset), -1, field.pointer);
	for (int i = 0; i < qcvm->num_edicts; ++i)
	{
		edict_t *entity = EDICT_NUM (i);
		if (entity->free)
			continue;
		for (const auto &field : fieldReferences)
			mark (E_INT (entity, field.offset), i, field.pointer);
	}
	return incoming;
}
bool Eligible (edict_t *entity, const std::vector<uint8_t> &incoming, std::string &error)
{
	const int index = NUM_FOR_EDICT (entity);
	if (entity->v.movetype == float (MOVETYPE_PUSH))
	{
		error = "moving brush entities must remain resident";
		return false;
	}
	if (entity->free || index <= svs.maxclients || incoming[index])
	{
		error = "entity is referenced by another entity/global";
		return false;
	}
	for (const auto &field : fieldReferences)
	{
		if (field.pointer)
			continue;
		const int reference = E_INT (entity, field.offset);
		if (reference && reference != EDICT_TO_PROG (entity))
		{
			error = "entity references another entity";
			return false;
		}
	}
	// Encode validates owned handles and external aliases once, immediately
	// after these entity-reference checks; do not repeat its full graph scan.
	return true;
}
void CancelCommitted ()
{
	for (const auto &[index, candidate] : committed)
		if (index < slots.size () && slots[index].id == candidate.id && slots[index].generation == candidate.generation)
			slots[index].retry = realtime + 1;
	pendingEvictions -= committed.size ();
	cancelled += committed.size ();
	committed.clear ();
}
void QueueDeletion (uint64_t id, uint64_t revision, bool busy)
{
	if (!revision && !busy)
		return; // Reserved ID was never written.
	if (!deletions.contains (id) && deletions.size () >= 8192)
		Host_Error ("Entity page deletion backlog exceeded; storage is unavailable");
	deletions[id] = {revision, 0, busy};
}
void ProcessDeletions (size_t budget)
{
	World &world = *OW_GetWorld ();
	for (auto &[id, deletion] : deletions)
	{
		if (!budget)
			break;
		if (deletion.busy || (!flushing && realtime < deletion.retry))
			continue;
		--budget;
		const auto	currentEpoch = epoch;
		std::string error;
		if (world.DeleteEntityPageAsync (
				id, deletion.revision,
				[id, currentEpoch] (bool success, uint64_t, const std::string &message)
				{
					if (epoch != currentEpoch)
						return;
					auto found = deletions.find (id);
					if (found == deletions.end ())
						return;
					if (success)
						deletions.erase (found);
					else
					{
						found->second.busy = false;
						found->second.retry = realtime + 1;
						Con_DPrintf ("Entity page deletion: %s\n", message.c_str ());
					}
				},
				&error))
		{
			deletion.busy = true;
		}
		else
			deletion.retry = realtime + 1;
	}
}
bool HaveEdictSlot ()
{
	if (qcvm->num_edicts < qcvm->max_edicts)
		return true;
	if (!qcvm->free_list.size)
		return false;
	edict_t *head = qcvm->free_list.circular_buffer[qcvm->free_list.head_index];
	return head->freetime < MAX_EDICT_FREETIME_ALWAYS_REUSE || qcvm->time - head->freetime > MIN_EDICT_AGE_FOR_REUSE;
}
void QueueLoad (const EntityPageInfo &info)
{
	if (active.contains (info.id) || deletions.contains (info.id) || loading.contains (info.id) || loading.size () >= 16 || flushing)
		return;
	const auto currentEpoch = epoch;
	loading.insert (info.id);
	std::string error;
	if (!OW_GetWorld ()->LoadEntityPageAsync (
			info.id,
			[id = info.id, currentEpoch] (std::optional<EntityPage> page, const std::string &message)
			{
				if (epoch != currentEpoch)
					return;
				VMGuard guard;
				loading.erase (id);
				if (!message.empty ())
					Con_DPrintf ("Entity page %llu load: %s\n", (unsigned long long)id, message.c_str ());
				if (!page || flushing || active.contains (id) || deletions.contains (id))
					return;
				if (active.size () >= size_t (BoundedCvar (world_entity_limit, 16, 8192, 1024)) || !HaveEdictSlot ())
					return;
				WorldPosition position;
				std::string	  error;
				if (!OW_EntityPagePosition (page->bytes, position, error) || !(CanonicalWorldKey (OW_GetWorld ()->Config (), position.chunk) == page->info.key))
				{
					Con_DPrintf ("Entity page %llu position: %s\n", (unsigned long long)id, error.empty () ? "chunk index mismatch" : error.c_str ());
					return;
				}
				if (!Near (position))
					return;
				if (!OW_GetWorld ()->Find (page->info.key))
				{
					OW_GetWorld ()->Request (page->info.key);
					return;
				}
				edict_t *entity = nullptr;
				if (!OW_EntityPageDecode (page->bytes, entity, error))
				{
					Con_DPrintf ("Entity page %llu: %s\n", (unsigned long long)id, error.c_str ());
					return;
				}
				if (!OW_GetEntityPosition (entity, position) || !(CanonicalWorldKey (OW_GetWorld ()->Config (), position.chunk) == page->info.key) ||
					!OW_RestoreEntityPageId (entity, id, page->info.revision))
				{
					ED_Free (entity);
					return;
				}
				++loaded;
				Con_DPrintf ("Entity page %llu loaded in slot %d\n", (unsigned long long)id, NUM_FOR_EDICT (entity));
			},
			&error))
		loading.erase (info.id);
}
void QueryNearPlayers (size_t budget)
{
	for (size_t visited = 0; visited < queries.size () && budget; ++visited)
	{
		const size_t index = queryClient++ % queries.size ();
		auto		&query = queries[index];
		if (query.busy || realtime < query.retry || !svs.clients[index].active || !svs.clients[index].spawned)
			continue;
		WorldPosition player;
		if (!OW_GetEntityPosition (svs.clients[index].edict, player))
			continue;
		player.chunk = CanonicalWorldKey (OW_GetWorld ()->Config (), player.chunk);
		if (!(query.center == player.chunk))
		{
			query.center = player.chunk;
			query.after = 0;
		}
		ChunkKey	  minimum = player.chunk, maximum = player.chunk;
		const int64_t radius = int64_t (std::ceil (Radius () / ChunkSize));
		for (size_t axis = 0; axis < 3; ++axis)
		{
			minimum[axis] = player.chunk[axis] < INT64_MIN + radius ? INT64_MIN : player.chunk[axis] - radius;
			maximum[axis] = player.chunk[axis] > INT64_MAX - radius ? INT64_MAX : player.chunk[axis] + radius;
		}
		const auto	currentEpoch = epoch;
		const auto	center = player.chunk;
		std::string error;
		--budget;
		if (OW_GetWorld ()->ListEntityPagesAsync (
				minimum, maximum, 16,
				[index, currentEpoch, center] (std::vector<EntityPageInfo> pages, const std::string &error)
				{
					if (epoch != currentEpoch)
						return;
					auto &query = queries[index];
					query.busy = false;
					if (!(query.center == center) || flushing)
						return;
					query.after = pages.size () == 16 ? pages.back ().id : 0;
					query.retry = realtime + (pages.size () == 16 ? 0.05 : 0.5);
					if (!error.empty ())
					{
						query.retry = realtime + 1;
						return;
					}
					for (const auto &page : pages)
						QueueLoad (page);
				},
				&error, query.after))
		{
			query.busy = true;
		}
		else
			query.retry = realtime + 1;
	}
}
} // namespace

void OW_PagingInit ()
{
	Cvar_RegisterVariable (&world_entity_paging);
	Cvar_RegisterVariable (&world_entity_radius);
	Cvar_RegisterVariable (&world_entity_idle);
	Cvar_RegisterVariable (&world_entity_budget);
	Cvar_RegisterVariable (&world_entity_limit);
}
void OW_PagingReset ()
{
	++epoch;
	slots.clear ();
	queries.clear ();
	active.clear ();
	deletions.clear ();
	loading.clear ();
	committed.clear ();
	globalReferences.clear ();
	fieldReferences.clear ();
	referenceSchemaReady = false;
	pendingEvictions = 0;
	loaded = evicted = cancelled = pinned = 0;
	scan = 1;
	queryClient = 0;
	evicting = flushing = false;
}
bool OW_SetStreamable (edict_t *entity, bool enabled)
{
	if (!OW_ServerActive () || qcvm != &sv.qcvm || entity->free || NUM_FOR_EDICT (entity) <= svs.maxclients)
		return false;
	Resize ();
	auto &slot = slots[NUM_FOR_EDICT (entity)];
	if (!enabled)
	{
		OW_PagingForget (entity);
		return true;
	}
	if (slot.id)
		return true;
	std::string	   error;
	const uint64_t id = OW_GetWorld ()->AllocateEntityPageId (&error);
	if (!id)
	{
		Con_Printf ("Cannot register streamed entity: %s\n", error.c_str ());
		return false;
	}
	slot = {id, 0, generation++};
	active[id] = NUM_FOR_EDICT (entity);
	return true;
}
uint64_t OW_EntityPageId (edict_t *entity)
{
	const size_t index = NUM_FOR_EDICT (entity);
	return index < slots.size () ? slots[index].id : 0;
}
uint64_t OW_EntityPageRevision (edict_t *entity)
{
	const size_t index = NUM_FOR_EDICT (entity);
	return index < slots.size () ? slots[index].revision : 0;
}
bool OW_RestoreEntityPageId (edict_t *entity, uint64_t id, uint64_t revision)
{
	if (!id)
		return revision == 0;
	if (active.contains (id) || entity->free || NUM_FOR_EDICT (entity) <= svs.maxclients)
		return false;
	Resize ();
	slots[NUM_FOR_EDICT (entity)] = {id, revision, generation++};
	active[id] = NUM_FOR_EDICT (entity);
	return true;
}
void OW_PagingForget (edict_t *entity)
{
	if (!OW_ServerActive () || qcvm != &sv.qcvm)
		return;
	const size_t index = NUM_FOR_EDICT (entity);
	if (index >= slots.size () || !slots[index].id)
		return;
	const auto slot = slots[index];
	if (committed.erase (index))
		--pendingEvictions;
	active.erase (slot.id);
	slots[index] = {};
	if (!evicting)
		QueueDeletion (slot.id, slot.revision, slot.writing);
}
void OW_PagingFrame ()
{
	if (!OW_ServerActive () || qcvm != &sv.qcvm || qcvm->depth || sv.state != ss_active || flushing)
		return;
	Resize ();
	ProcessDeletions (16);
	if (!world_entity_paging.value)
	{
		CancelCommitted ();
		return;
	}
	QueryNearPlayers (4);
	if (active.empty ())
		return;
	std::vector<uint8_t> incoming;
	auto				 references = [&] () -> const std::vector<uint8_t> &
	{
		if (incoming.empty ())
			incoming = IncomingReferences ();
		return incoming;
	};
	size_t budget = size_t (BoundedCvar (world_entity_budget, 1, 16, 4));
	pinned = 0;
	// No QC executes during this pass. One lazy incoming-reference snapshot is
	// shared by every candidate; freeing an entity only makes it conservative.
	// Worker callbacks publish revisions, but never scan/free game entities.
	while (!committed.empty () && budget)
	{
		--budget;
		auto		 first = committed.begin ();
		const size_t index = first->first;
		auto		 candidate = std::move (first->second);
		committed.erase (first);
		--pendingEvictions;
		if (index >= slots.size () || slots[index].id != candidate.id || slots[index].generation != candidate.generation)
			continue;
		auto &slot = slots[index];
		slot.retry = realtime + 1;
		edict_t		 *entity = EDICT_NUM (int (index));
		WorldPosition position;
		OW_GetEntityPosition (entity, position);
		std::string			 reason;
		std::vector<uint8_t> current;
		if (Near (position) || !Eligible (entity, references (), reason) || !OW_EntityPageEncode (entity, candidate.snapshotTime, current, reason) ||
			candidate.bytes != current)
		{
			++cancelled;
			continue;
		}
		evicting = true;
		ED_Free (entity);
		evicting = false;
		++evicted;
		Con_DPrintf ("Entity page %llu evicted from slot %zu\n", (unsigned long long)candidate.id, index);
	}
	for (size_t visited = 0; visited < size_t (qcvm->num_edicts) && budget; ++visited)
	{
		if (pendingEvictions >= MaxPendingEvictions)
			break;
		if (scan >= size_t (qcvm->num_edicts))
			scan = svs.maxclients + 1;
		if (scan >= size_t (qcvm->num_edicts))
			break;
		const size_t index = scan++;
		auto		&slot = slots[index];
		if (!slot.id || slot.writing || committed.contains (index) || realtime < slot.retry)
			continue;
		edict_t		 *entity = EDICT_NUM (int (index));
		WorldPosition position;
		OW_GetEntityPosition (entity, position);
		if (Near (position))
		{
			slot.distantSince = -1;
			continue;
		}
		if (slot.distantSince < 0)
			slot.distantSince = realtime;
		if (realtime - slot.distantSince < BoundedCvar (world_entity_idle, 0.1, 60, 2))
			continue;
		// Pinned, unencodable and queue-rejected candidates consume an attempt too.
		--budget;
		std::string error;
		if (!Eligible (entity, references (), error))
		{
			++pinned;
			slot.retry = realtime + 0.5;
			continue;
		}
		std::vector<uint8_t> bytes;
		const double		 snapshotTime = qcvm->time;
		if (!OW_EntityPageEncode (entity, snapshotTime, bytes, error))
		{
			slot.retry = realtime + 1;
			Con_DPrintf ("Entity page encode: %s\n", error.c_str ());
			continue;
		}
		const uint64_t id = slot.id, token = slot.generation, currentEpoch = epoch, previousRevision = slot.revision;
		if (OW_GetWorld ()->StoreEntityPageAsync (
				id, position.chunk, bytes, slot.revision,
				[index, id, token, currentEpoch, previousRevision, snapshotTime,
				 saved = bytes] (bool success, uint64_t revision, const std::string &error) mutable
				{
					if (currentEpoch != epoch)
						return;
					VMGuard guard;
					if (index >= slots.size () || slots[index].id != id || slots[index].generation != token)
					{
						--pendingEvictions;
						if (success)
							QueueDeletion (id, revision, false);
						else if (previousRevision)
							QueueDeletion (id, previousRevision, false);
						else
							deletions.erase (id);
						return;
					}
					auto &slot = slots[index];
					slot.writing = false;
					slot.retry = realtime + 1;
					if (!success)
					{
						--pendingEvictions;
						Con_DPrintf ("Entity page commit: %s\n", error.c_str ());
						return;
					}
					slot.revision = revision;
					if (flushing || !world_entity_paging.value || committed.size () >= MaxPendingEvictions)
					{
						--pendingEvictions;
						++cancelled;
						return;
					}
					committed.emplace (index, Committed{id, token, snapshotTime, std::move (saved)});
				},
				&error))
		{
			slot.writing = true;
			++pendingEvictions;
		}
		else
			slot.retry = realtime + 0.25;
	}
}
bool OW_PagingFlush (std::string &error)
{
	if (!OW_ServerActive ())
		return true;
	flushing = true;
	CancelCommitted ();
	World		&world = *OW_GetWorld ();
	// Each pass can submit 64 deletions; stores already in flight can create
	// another batch when their callbacks observe that the entity was removed.
	const size_t passes = 3 + (deletions.size () + 63) / 64;
	for (size_t attempt = 0; attempt < passes; ++attempt)
	{
		ProcessDeletions (64);
		if (!world.Flush (&error))
		{
			flushing = false;
			return false;
		}
		world.Pump (1024);
		if (deletions.empty () && !world.Stats ().pendingEntityPages)
		{
			flushing = false;
			return true;
		}
	}
	flushing = false;
	error = "entity paging operations are still pending or storage failed; retry saving";
	return false;
}
void OW_PagingStats ()
{
	Con_Printf (
		"World entities: %zu resident streamable, %llu paged out, %llu paged in, %zu pinned, %zu loading, %zu deletions, %zu awaiting eviction (%zu "
		"committing), %llu cancelled evictions\n",
		active.size (), (unsigned long long)evicted, (unsigned long long)loaded, pinned, loading.size (), deletions.size (), committed.size (),
		pendingEvictions - committed.size (), (unsigned long long)cancelled);
}
