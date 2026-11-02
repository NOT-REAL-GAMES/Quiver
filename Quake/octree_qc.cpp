#include "quakedef.h"
#include "octree_engine.h"
#include "octree_qc.h"
#include "world_position_handles.h"

#include <cmath>
#include <cstddef>

namespace
{
quiver::PositionHandles handles;
using Token = quiver::PositionHandles::Token;
using quiver::PositionCheckpoint;
using quiver::WorldPosition;
struct Registration
{
	PositionCheckpoint::Binding binding;
	int							offset = 0;
};
std::vector<Registration> registrations;

bool CheckVM (std::string &error)
{
	if (qcvm != &sv.qcvm || !qcvm->progs)
	{
		error = "world position checkpoint requires the loaded server QCVM";
		return false;
	}
	return true;
}
bool ResolveRegistration (const PositionCheckpoint::Binding &binding, Registration &registration, std::string &error)
{
	if (!PositionCheckpoint::ValidName (binding.name))
	{
		error = "invalid world position registration name";
		return false;
	}
	const ddef_t *definition = binding.global ? ED_FindGlobal (binding.name.c_str ()) : ED_FindField (binding.name.c_str ());
	const int	  limit = binding.global ? qcvm->progs->numglobals : qcvm->progs->entityfields;
	const size_t  native = binding.global ? sizeof (globalvars_t) : sizeof (entvars_t);
	if (!definition || (definition->type & ~DEF_SAVEGLOBAL) != ev_vector || definition->ofs + 3 > limit || definition->ofs < native / sizeof (float))
	{
		error = "world position registration must name a custom vector " + std::string (binding.global ? "global: " : "field: ") + binding.name;
		return false;
	}
	// Native extensions are also engine-owned even when their offsets follow the
	// classic ABI. Do not let handles overwrite movement or input vectors.
	bool reserved = false;
	if (binding.global)
	{
#define QCEXTGLOBAL_FLOAT(n)
#define QCEXTGLOBAL_INT(n)
#define QCEXTGLOBAL_VECTOR(n)                                                                                                                       \
	if (qcvm->extglobals.n && definition->ofs < qcvm->extglobals.n - qcvm->globals + 3 && definition->ofs + 3 > qcvm->extglobals.n - qcvm->globals) \
		reserved = true;
		QCEXTGLOBALS_GAME
#undef QCEXTGLOBAL_FLOAT
#undef QCEXTGLOBAL_INT
#undef QCEXTGLOBAL_VECTOR
	}
	else
	{
#define QCEXTFIELD(n, t)                                                                                                                             \
	if (qcvm->extfields.n >= 0 && definition->ofs < qcvm->extfields.n + (!strcmp (t, ".vector") ? 3 : 1) && definition->ofs + 3 > qcvm->extfields.n) \
		reserved = true;
		QCEXTFIELDS_ALL
		QCEXTFIELDS_GAME
		QCEXTFIELDS_SS
#undef QCEXTFIELD
	}
	if (reserved)
	{
		error = "cannot register an engine-owned world position vector: " + binding.name;
		return false;
	}
	registration = {binding, definition->ofs};
	return true;
}
bool ResolveSchema (const PositionCheckpoint &checkpoint, std::vector<Registration> &resolved, std::string &error)
{
	for (const auto &binding : checkpoint.schema)
	{
		Registration registration;
		if (!ResolveRegistration (binding, registration, error))
			return false;
		for (const auto &previous : resolved)
			if (previous.binding.global == binding.global && registration.offset < previous.offset + 3 && registration.offset + 3 > previous.offset)
			{
				error = "overlapping world position registrations";
				return false;
			}
		resolved.push_back (std::move (registration));
	}
	return true;
}
float *RegisteredVector (const Registration &registration, uint32_t entity)
{
	return registration.binding.global ? qcvm->globals + registration.offset : reinterpret_cast<float *> (&EDICT_NUM (entity)->v) + registration.offset;
}
void ClearRegisteredVectors (const std::vector<Registration> &schema)
{
	for (const auto &registration : schema)
	{
		const uint32_t count = registration.binding.global ? 1 : qcvm->num_edicts;
		for (uint32_t entity = 0; entity < count; ++entity)
			std::fill_n (RegisteredVector (registration, entity), 3, 0.f);
	}
}

Token ReadToken (const float *value)
{
	return {value[0], value[1], value[2]};
}
bool BuildEntityPositionPage (edict_t *entity, PositionCheckpoint &page, std::string &error)
{
	if (qcvm->depth)
	{
		error = "entity paging requires an idle QuakeC VM";
		return false;
	}
	const uint32_t owner = NUM_FOR_EDICT (entity);
	if (owner <= uint32_t (svs.maxclients) || owner >= uint32_t (qcvm->num_edicts) || entity->free)
	{
		error = "only live nonplayer entities can page";
		return false;
	}
	// Only this owner's referenced generations affect page identity. Unrelated
	// allocations must not make an async durable snapshot appear to have changed.
	page.generation = 0;
	std::set<int>	   ownedFields;
	std::vector<Token> tokens;
	for (const auto &registration : registrations)
	{
		if (registration.binding.global)
			continue;
		const uint32_t binding = page.schema.size ();
		page.schema.push_back (registration.binding);
		ownedFields.insert (registration.offset);
		const Token token = ReadToken (RegisteredVector (registration, owner));
		if (token == Token{})
			continue;
		if (handles.Owner (token) != owner)
		{
			error = "registered position is stale or owned by another entity";
			return false;
		}
		page.generation = std::max (page.generation, uint64_t (token[1]) | (uint64_t (token[2]) << 24));
		tokens.push_back (token);
		page.references.push_back ({binding, 0, 0});
	}
	std::vector<uint32_t> ids;
	if (!handles.Export (tokens, page.values, ids, error))
		return false;
	if (handles.OwnerCount (owner) != page.values.size ())
	{
		error = "entity owns temporary handles outside its registered fields";
		return false;
	}
	for (size_t i = 0; i < ids.size (); ++i)
		page.references[i].value = ids[i];
	// Every named vector is inspected, including unregistered locals/globals. A
	// valid hidden token must pin rather than silently become stale on page-out.
	for (int i = 0; i < qcvm->progs->numfielddefs; ++i)
	{
		const auto &definition = qcvm->fielddefs[i];
		if ((definition.type & ~DEF_SAVEGLOBAL) != ev_vector)
			continue;
		if (definition.ofs + 3 > qcvm->progs->entityfields)
		{
			error = "invalid vector field in paging schema";
			return false;
		}
		const auto tokenOwner = handles.Owner (ReadToken (reinterpret_cast<const float *> (&entity->v) + definition.ofs));
		if (tokenOwner && (*tokenOwner != owner || !ownedFields.contains (definition.ofs)))
		{
			error = "entity contains an external or unregistered position handle";
			return false;
		}
	}
	if (!page.values.empty ())
	{
		for (int i = 0; i < qcvm->progs->numglobaldefs; ++i)
		{
			const auto &definition = qcvm->globaldefs[i];
			if ((definition.type & ~DEF_SAVEGLOBAL) != ev_vector)
				continue;
			if (definition.ofs + 3 > qcvm->progs->numglobals)
			{
				error = "invalid vector global in paging schema";
				return false;
			}
			if (handles.Owner (ReadToken (qcvm->globals + definition.ofs)) == owner)
			{
				error = "a global or temporary vector references this entity's position handle";
				return false;
			}
		}
		for (int index = 0; index < qcvm->num_edicts; ++index)
		{
			edict_t *other = EDICT_NUM (index);
			if (other == entity || other->free)
				continue;
			for (int i = 0; i < qcvm->progs->numfielddefs; ++i)
			{
				const auto &definition = qcvm->fielddefs[i];
				if ((definition.type & ~DEF_SAVEGLOBAL) != ev_vector)
					continue;
				if (handles.Owner (ReadToken (reinterpret_cast<const float *> (&other->v) + definition.ofs)) == owner)
				{
					error = "another entity references this entity's position handle";
					return false;
				}
			}
		}
	}
	for (auto &value : page.values)
		value.owner = 0; // Page-local self, never a durable edict index.
	return true;
}
bool DecodeEntityPositionPage (
	std::span<const uint8_t> bytes, PositionCheckpoint &page, std::vector<Registration> &resolved, std::vector<Registration> &merged, std::string &error)
{
	if (!PositionCheckpoint::Decode (bytes, 1, page, error))
		return false;
	for (const auto &binding : page.schema)
		if (binding.global)
		{
			error = "entity position page contains global registrations";
			return false;
		}
	if (!ResolveSchema (page, resolved, error))
		return false;
	PositionCheckpoint schema;
	for (const auto &registration : registrations)
		schema.schema.push_back (registration.binding);
	for (const auto &binding : page.schema)
		if (std::none_of (
				schema.schema.begin (), schema.schema.end (), [&] (const auto &existing) { return !existing.global && existing.name == binding.name; }))
			schema.schema.push_back (binding);
	if (schema.schema.size () > PositionCheckpoint::MaxSchema)
	{
		error = "entity position page exceeds registration capacity";
		return false;
	}
	if (!ResolveSchema (schema, merged, error))
		return false;
	const uint64_t floor = std::max (page.generation, handles.Generation ());
	if (page.values.size () > PositionCheckpoint::MaxValues - handles.ActiveCount () || page.values.size () > quiver::PositionHandles::MaxGeneration - floor)
	{
		error = "entity position page exceeds available handle capacity";
		return false;
	}
	return true;
}

void RequireWorld ()
{
	if (qcvm != &sv.qcvm || !OW_ServerActive ())
		PR_RunError ("world position builtins require an active octree server (check wp_active)");
}
Token Argument (int parameter)
{
	const float *value = G_VECTOR (parameter);
	return {value[0], value[1], value[2]};
}
WorldPosition &Position (int parameter)
{
	RequireWorld ();
	WorldPosition *position = handles.Get (Argument (parameter));
	if (!position)
		PR_RunError ("invalid or stale world position handle; use wp_alloc/wp_copy, not vector arithmetic");
	return *position;
}
edict_t *Entity (int parameter, bool allowWorld = true)
{
	RequireWorld ();
	const int offset = G_INT (parameter);
	if (offset < 0 || offset % qcvm->edict_size || offset / qcvm->edict_size >= qcvm->num_edicts || (!allowWorld && offset == 0))
		PR_RunError ("invalid entity in world position builtin");
	edict_t *entity = PROG_TO_EDICT (offset);
	if (entity->free)
		PR_RunError ("freed entity in world position builtin");
	return entity;
}
std::array<long double, 3> WideDelta (const WorldPosition &to, const WorldPosition &from)
{
	std::array<long double, 3> result;
	for (size_t axis = 0; axis < 3; ++axis)
	{
		const int64_t  a = to.chunk[axis], b = from.chunk[axis];
		// Subtract integer chunks before conversion, preserving adjacent far-away chunks.
		const bool	   positive = a >= b;
		const uint64_t magnitude = positive ? uint64_t (a) - uint64_t (b) : uint64_t (b) - uint64_t (a);
		result[axis] = (positive ? 1.L : -1.L) * magnitude * quiver::ChunkSize + (static_cast<long double> (to.local[axis]) - from.local[axis]);
	}
	return result;
}
void ReturnVector (const std::array<float, 3> &value)
{
	for (int axis = 0; axis < 3; ++axis)
		G_FLOAT (OFS_RETURN + axis) = value[axis];
}
} // namespace

void OW_QCReset ()
{
	handles.Reset ();
	registrations.clear ();
}
bool OW_QCCanPageEntity (edict_t *entity, std::string &reason)
{
	if (!CheckVM (reason))
		return false;
	PositionCheckpoint page;
	return BuildEntityPositionPage (entity, page, reason);
}
bool OW_QCExportEntityPositions (edict_t *entity, std::vector<uint8_t> &bytes, std::string &error)
{
	if (!CheckVM (error))
		return false;
	PositionCheckpoint page;
	return BuildEntityPositionPage (entity, page, error) && page.Encode (1, bytes, error);
}
bool OW_QCValidateEntityPositions (std::span<const uint8_t> bytes, std::string &error)
{
	if (!CheckVM (error))
		return false;
	PositionCheckpoint		  page;
	std::vector<Registration> resolved, merged;
	return DecodeEntityPositionPage (bytes, page, resolved, merged, error);
}
bool OW_QCImportEntityPositions (edict_t *entity, std::span<const uint8_t> bytes, std::string &error)
{
	if (!CheckVM (error))
		return false;
	if (qcvm->depth)
	{
		error = "entity page restore requires an idle QuakeC VM";
		return false;
	}
	const uint32_t owner = NUM_FOR_EDICT (entity);
	if (owner <= uint32_t (svs.maxclients) || owner >= uint32_t (qcvm->num_edicts) || entity->free || handles.OwnerCount (owner))
	{
		error = "entity position page requires a fresh nonplayer entity";
		return false;
	}
	PositionCheckpoint		  page;
	std::vector<Registration> resolved, merged;
	if (!DecodeEntityPositionPage (bytes, page, resolved, merged, error))
		return false;
	for (auto &value : page.values)
		value.owner = owner;
	std::vector<Token> tokens;
	if (!handles.Append (page.values, page.generation, tokens, error))
		return false;
	for (const auto &registration : merged)
		if (!registration.binding.global)
			std::fill_n (RegisteredVector (registration, owner), 3, 0.f);
	for (const auto &reference : page.references)
		std::copy (tokens[reference.value - 1].begin (), tokens[reference.value - 1].end (), RegisteredVector (resolved[reference.binding], owner));
	registrations = std::move (merged);
	return true;
}
bool OW_QCExportPositions (std::vector<uint8_t> &bytes, std::string &error, uint32_t excludedPlayerCount)
{
	if (!CheckVM (error))
		return false;
	PositionCheckpoint checkpoint;
	checkpoint.generation = handles.Generation ();
	std::vector<Token> tokens;
	for (const auto &registration : registrations)
	{
		const uint32_t binding = checkpoint.schema.size ();
		checkpoint.schema.push_back (registration.binding);
		const uint32_t count = registration.binding.global ? 1 : qcvm->num_edicts;
		for (uint32_t entity = 0; entity < count; ++entity)
		{
			if (!registration.binding.global && EDICT_NUM (entity)->free)
				continue;
			if (!registration.binding.global && entity && entity <= excludedPlayerCount)
				continue;
			const float *value = RegisteredVector (registration, entity);
			const Token	 token{value[0], value[1], value[2]};
			if (token == Token{})
				continue;
			const auto owner = handles.Owner (token);
			if (owner && *owner && *owner <= excludedPlayerCount)
				continue;
			if (tokens.size () == PositionCheckpoint::MaxReferences)
			{
				error = "world position checkpoint exceeds reference budget";
				return false;
			}
			tokens.push_back (token);
			checkpoint.references.push_back ({binding, entity, 0});
		}
	}
	std::vector<uint32_t> ids;
	if (!handles.Export (tokens, checkpoint.values, ids, error))
		return false;
	for (const auto &value : checkpoint.values)
		if (value.owner >= uint32_t (qcvm->num_edicts) || EDICT_NUM (value.owner)->free)
		{
			error = "world position checkpoint contains a freed owner";
			return false;
		}
	for (size_t index = 0; index < ids.size (); ++index)
		checkpoint.references[index].value = ids[index];
	return checkpoint.Encode (qcvm->num_edicts, bytes, error);
}
bool OW_QCValidatePositions (std::span<const uint8_t> bytes, uint32_t entityCount, std::string &error, std::span<const uint8_t> liveEntities)
{
	if (!CheckVM (error))
		return false;
	PositionCheckpoint		  checkpoint;
	std::vector<Registration> resolved;
	if (!PositionCheckpoint::Decode (bytes, entityCount, checkpoint, error) || !ResolveSchema (checkpoint, resolved, error))
		return false;
	const uint64_t floor = std::max (checkpoint.generation, handles.Generation ());
	if (checkpoint.values.size () > quiver::PositionHandles::MaxGeneration - floor)
	{
		error = "world position checkpoint exhausts allocation generations";
		return false;
	}
	if (!liveEntities.empty ())
	{
		if (liveEntities.size () != entityCount)
		{
			error = "world position checkpoint entity mask has wrong size";
			return false;
		}
		for (const auto &value : checkpoint.values)
			if (!liveEntities[value.owner])
			{
				error = "world position checkpoint owner was not saved";
				return false;
			}
		for (const auto &reference : checkpoint.references)
			if (!checkpoint.schema[reference.binding].global && !liveEntities[reference.entity])
			{
				error = "world position checkpoint reference targets an entity that was not saved";
				return false;
			}
	}
	return true;
}
bool OW_QCImportPositions (std::span<const uint8_t> bytes, std::string &error)
{
	if (!CheckVM (error))
		return false;
	PositionCheckpoint		  checkpoint;
	std::vector<Registration> resolved;
	if (!PositionCheckpoint::Decode (bytes, qcvm->num_edicts, checkpoint, error) || !ResolveSchema (checkpoint, resolved, error))
		return false;
	for (const auto &value : checkpoint.values)
		if (EDICT_NUM (value.owner)->free)
		{
			error = "world position checkpoint owner is a freed entity";
			return false;
		}
	for (const auto &reference : checkpoint.references)
		if (!resolved[reference.binding].binding.global && EDICT_NUM (reference.entity)->free)
		{
			error = "world position checkpoint reference targets a freed entity";
			return false;
		}
	std::vector<Token> tokens;
	if (!handles.Import (checkpoint.values, checkpoint.generation, tokens, error))
		return false;
	// All fallible work has completed. Clear nulls and replay aliases using fresh
	// generations; no pre-load or unregistered temporary handle remains live.
	ClearRegisteredVectors (registrations);
	ClearRegisteredVectors (resolved);
	for (const auto &reference : checkpoint.references)
		std::copy (tokens[reference.value - 1].begin (), tokens[reference.value - 1].end (), RegisteredVector (resolved[reference.binding], reference.entity));
	registrations = std::move (resolved);
	return true;
}
void OW_QCForgetEntity (edict_t *entity)
{
	if (qcvm == &sv.qcvm)
		handles.ForgetOwner (NUM_FOR_EDICT (entity));
}
void OW_QCCheckField (edict_t *entity, int field, int width, int statement)
{
	if (qcvm != &sv.qcvm || !OW_ServerActive () || entity == qcvm->edicts)
		return;
	constexpr size_t offsets[] = {offsetof (entvars_t, origin), offsetof (entvars_t, oldorigin), offsetof (entvars_t, absmin), offsetof (entvars_t, absmax)};
	for (size_t offset : offsets)
	{
		const int first = static_cast<int> (offset / sizeof (float));
		if (field < first + 3 && int64_t (field) + width > first && !OW_EntityVisible (entity))
		{
			qcvm->xstatement = statement;
			PR_RunError ("entity position is outside the current local frame; use wp_getentity and wp_delta/wp_distance instead of origin/absmin/absmax");
		}
	}
}
void PF_wp_active ()
{
	G_FLOAT (OFS_RETURN) = qcvm == &sv.qcvm && OW_ServerActive ();
}
void PF_wp_alloc ()
{
	edict_t *owner = Entity (OFS_PARM0);
	auto	 token = handles.Allocate (NUM_FOR_EDICT (owner));
	if (!token)
		PR_RunError ("world position handle capacity exhausted; free unused handles with wp_free");
	OW_GetEntityPosition (owner, *handles.Get (*token));
	ReturnVector (*token);
}
void PF_wp_free ()
{
	RequireWorld ();
	if (!handles.Free (Argument (OFS_PARM0)))
		PR_RunError ("wp_free: invalid or stale world position handle");
}
void PF_wp_valid ()
{
	G_FLOAT (OFS_RETURN) = qcvm == &sv.qcvm && OW_ServerActive () && handles.Get (Argument (OFS_PARM0));
}
void PF_wp_copy ()
{
	const WorldPosition source = Position (OFS_PARM1);
	Position (OFS_PARM0) = source;
}
void PF_wp_getentity ()
{
	const Token destination = Argument (OFS_PARM0);
	Position (OFS_PARM0);
	WorldPosition source;
	if (!OW_GetEntityPosition (Entity (OFS_PARM1), source))
		PR_RunError ("wp_getentity: position unavailable");
	*handles.Get (destination) = source;
}
void PF_wp_setentity ()
{
	const WorldPosition source = Position (OFS_PARM1);
	if (!OW_SetEntityPosition (Entity (OFS_PARM0, false), source))
		PR_RunError ("wp_setentity: invalid world position");
}
void PF_wp_add ()
{
	const WorldPosition source = Position (OFS_PARM1);
	WorldPosition		result;
	const float		   *delta = G_VECTOR (OFS_PARM2);
	if (!quiver::Add (source, {delta[0], delta[1], delta[2]}, result))
		PR_RunError ("wp_add: position overflow or nonfinite offset");
	Position (OFS_PARM0) = result;
}
void PF_wp_delta ()
{
	const WorldPosition	  to = Position (OFS_PARM0), from = Position (OFS_PARM1);
	std::array<double, 3> delta;
	if (!quiver::Delta (to, from, delta))
		PR_RunError ("wp_delta: distance exceeds precise local vector range; use wp_distance/wp_direction");
	ReturnVector ({float (delta[0]), float (delta[1]), float (delta[2])});
}
void PF_wp_distance ()
{
	const auto delta = WideDelta (Position (OFS_PARM0), Position (OFS_PARM1));
	G_FLOAT (OFS_RETURN) = float (std::hypot (delta[0], delta[1], delta[2]));
}
void PF_wp_direction ()
{
	const auto		  delta = WideDelta (Position (OFS_PARM0), Position (OFS_PARM1));
	const long double length = std::hypot (delta[0], delta[1], delta[2]);
	ReturnVector (length ? std::array<float, 3>{float (delta[0] / length), float (delta[1] / length), float (delta[2] / length)} : std::array<float, 3>{});
}
void PF_wp_contents ()
{
	const WorldPosition position = Position (OFS_PARM0);
	G_FLOAT (OFS_RETURN) = OW_GetWorld ()->CollisionContentsCached (position).contents;
}
void PF_wp_tracebox_terrain ()
{
	const Token destination = Argument (OFS_PARM0);
	Position (OFS_PARM0);
	const WorldPosition start = Position (OFS_PARM1);
	const float		   *offset = G_VECTOR (OFS_PARM2), *mins = G_VECTOR (OFS_PARM3), *maxs = G_VECTOR (OFS_PARM4);
	for (int axis = 0; axis < 3; ++axis)
		if (!std::isfinite (offset[axis]) || std::abs (offset[axis]) > 1048576 || !std::isfinite (mins[axis]) || !std::isfinite (maxs[axis]) ||
			mins[axis] > maxs[axis] || std::abs (mins[axis]) > 4096 || std::abs (maxs[axis]) > 4096)
			PR_RunError ("wp_tracebox_terrain: invalid or excessive local trace bounds");
	const auto trace = OW_TraceTerrain (start, {offset[0], offset[1], offset[2]}, {mins[0], mins[1], mins[2]}, {maxs[0], maxs[1], maxs[2]});
	*handles.Get (destination) = trace.end;
	pr_global_struct->trace_fraction = float (trace.fraction);
	pr_global_struct->trace_startsolid = trace.startSolid;
	pr_global_struct->trace_allsolid = trace.allSolid;
	pr_global_struct->trace_ent = 0;
	pr_global_struct->trace_inopen = trace.cell.contents == quiver::Empty;
	pr_global_struct->trace_inwater = trace.cell.contents <= quiver::Water && trace.cell.contents >= quiver::Lava;
	pr_global_struct->trace_plane_dist = 0;
	for (int axis = 0; axis < 3; ++axis)
	{
		pr_global_struct->trace_plane_normal[axis] = trace.normal[axis];
		pr_global_struct->trace_endpos[axis] = 0; // End position is the destination handle, never an absolute float vector.
	}
	G_FLOAT (OFS_RETURN) = float (trace.fraction);
}
void PF_wp_findradius ()
{
	const WorldPosition center = Position (OFS_PARM0);
	const float			radius = G_FLOAT (OFS_PARM1);
	const int			after = NUM_FOR_EDICT (Entity (OFS_PARM2));
	if (!std::isfinite (radius) || radius < 0 || radius > 1048576)
		PR_RunError ("wp_findradius: radius outside supported local search range");
	G_INT (OFS_RETURN) = 0;
	for (int index = after + 1; index < qcvm->num_edicts; ++index)
	{
		edict_t *entity = EDICT_NUM (index);
		if (entity->free || entity->v.solid == float (SOLID_NOT))
			continue;
		WorldPosition position;
		if (!OW_GetEntityPosition (entity, position))
			continue;
		std::array<double, 3> delta;
		if (quiver::Delta (position, center, delta, radius) && std::hypot (delta[0], delta[1], delta[2]) <= radius)
		{
			G_INT (OFS_RETURN) = EDICT_TO_PROG (entity);
			return;
		}
	}
}

static void RegisterPosition (bool global)
{
	RequireWorld ();
	const PositionCheckpoint::Binding binding{PR_GetString (G_INT (OFS_PARM0)), global};
	for (const auto &registration : registrations)
		if (registration.binding.global == global && registration.binding.name == binding.name)
			return;
	if (registrations.size () == PositionCheckpoint::MaxSchema)
		PR_RunError ("world position registration capacity exhausted");
	PositionCheckpoint checkpoint;
	for (const auto &registration : registrations)
		checkpoint.schema.push_back (registration.binding);
	checkpoint.schema.push_back (binding);
	std::vector<Registration> resolved;
	std::string				  error;
	if (!ResolveSchema (checkpoint, resolved, error))
		PR_RunError ("%s", error.c_str ());
	registrations = std::move (resolved);
}
void PF_wp_register_field ()
{
	RegisterPosition (false);
}
void PF_wp_register_global ()
{
	RegisterPosition (true);
}
void PF_wp_streamable ()
{
	edict_t	   *entity = Entity (OFS_PARM0, false);
	const float enabled = G_FLOAT (OFS_PARM1);
	if (!std::isfinite (enabled))
		PR_RunError ("wp_streamable requires a finite enable flag");
	G_FLOAT (OFS_RETURN) = OW_SetStreamable (entity, enabled != 0);
}
