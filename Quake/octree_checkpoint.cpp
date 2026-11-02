/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <bit>
#include <cmath>
#include <cstring>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
#include "quakedef.h"
#include "octree_checkpoint.h"
#include "octree_engine.h"
#include "octree_qc.h"
#include "octree_paging.h"
#include "world_protocol.h"
#include "world_string_pool.h"

namespace
{
using namespace quiver;
constexpr size_t   Limit = 64 * 1024 * 1024;
constexpr uint32_t LegacyMagic = 0x31535751; // QWS1
constexpr uint32_t Magic = 0x32535751;		 // QWS2: stable entity page identities.
constexpr uint32_t PageMagic = 0x31504551;	 // QEP1
struct Value
{
	std::string				name, text;
	uint16_t				type = 0;
	std::array<uint32_t, 3> words{};
};
WorldStringPool restoredStrings;
struct Entity
{
	bool			   live = false;
	uint8_t			   alpha = 0;
	uint64_t		   pageId = 0, pageRevision = 0;
	WorldPosition	   position;
	std::vector<Value> values;
};
struct Snapshot
{
	uint32_t								 hash = 0, size = 0, clients = 0, flags = 0;
	double									 time = 0;
	float									 skill = 1, coop = 0, deathmatch = 0;
	bool									 player = false;
	std::array<float, NUM_TOTAL_SPAWN_PARMS> parms{};
	std::vector<Value>						 globals;
	std::vector<Entity>						 entities;
	std::vector<std::string>				 models, sounds, lights;
	std::vector<uint8_t>					 handles;
};
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
void Check (bool condition, const char *message)
{
	if (!condition)
		throw std::runtime_error (message);
}
const char *PrecacheString (std::string_view text)
{
	auto stable = [] (const char *value)
	{
		for (int i = 0; i < qcvm->numknownstrings; ++i)
			if (qcvm->knownstrings[i] == value && qcvm->knownstringsowned[i])
				return true;
		return false;
	};
	for (const char *value : sv.model_precache)
		if (value && stable (value) && text == value)
			return value;
	for (const char *value : sv.sound_precache)
		if (value && stable (value) && text == value)
			return value;
	return nullptr;
}
bool StringMatches (int id, std::string_view expected)
{
	const char *value = nullptr;
	if (id >= 0 && id < qcvm->stringssize)
	{
		value = qcvm->strings + id;
		if (expected.size () >= size_t (qcvm->stringssize - id))
			return false;
		return std::string_view (value, expected.size ()) == expected && value[expected.size ()] == '\0';
	}
	const int64_t index = -int64_t (id) - 1;
	if (id < 0 && index < qcvm->numknownstrings && qcvm->knownstrings[index] && qcvm->knownstringsowned[index])
		return expected == qcvm->knownstrings[index];
	return false;
}
std::optional<int> ExistingString (std::string_view text)
{
	if (text.empty ())
		return 0;
	const std::string_view literals (qcvm->strings, qcvm->stringssize);
	for (size_t offset = literals.find (text); offset != std::string_view::npos; offset = literals.find (text, offset + 1))
		if (offset + text.size () < literals.size () && literals[offset + text.size ()] == '\0')
			return int (offset);
	if (const char *precache = PrecacheString (text))
		for (int i = 0; i < qcvm->numknownstrings; ++i)
			if (qcvm->knownstrings[i] == precache)
				return -1 - i;
	return std::nullopt;
}
void InternStrings (std::span<const std::string_view> texts)
{
	for (std::string_view text : texts)
		if (const auto id = restoredStrings.Find (text))
			Check (StringMatches (*id, text), "restored QC string ID was freed or modified outside its VM lifecycle");
	std::string error;
	const bool	success = restoredStrings.Intern (
		 texts, ExistingString,
		 [] (const std::string &value)
		 {
			 // Only VM-owned precache strings are reused. Other text receives a VM-owned
			 // allocation so aliases remain valid after the original entity pages out.
			 if (const char *precache = PrecacheString (value))
				 return PR_SetEngineString (precache);
			 char	  *text;
			 const int id = PR_AllocString (int (value.size ()) + 1, &text);
			 memcpy (text, value.c_str (), value.size () + 1);
			 return id;
		 },
		 error);
	Check (success, error.c_str ());
}
int InternedString (const std::string &text)
{
	const auto id = restoredStrings.Find (text);
	Check (id.has_value (), "restored QC string was not preflighted");
	Check (StringMatches (*id, text), "restored QC string ID was freed or modified outside its VM lifecycle");
	return *id;
}
void CollectStrings (const std::vector<Value> &values, std::vector<std::string_view> &texts)
{
	for (const auto &value : values)
		if (value.type == ev_string)
			texts.push_back (value.text);
}
void ReadPageHeader (wire::Reader &reader, double &savedTime, uint8_t &alpha, WorldPosition &position)
{
	uint32_t magic, hash, size;
	uint64_t timestamp;
	Check (
		reader.U32 (magic) && magic == PageMagic && reader.U32 (hash) && reader.U32 (size) && hash == qcvm->progshash && size == qcvm->progssize &&
			reader.U64 (timestamp) && reader.U8 (alpha) && reader.Position (position),
		"entity page format/progs.dat mismatch");
	savedTime = std::bit_cast<double> (timestamp);
	Check (std::isfinite (savedTime) && savedTime >= 0, "invalid entity page time");
}
size_t Width (unsigned type)
{
	switch (type)
	{
	case ev_vector:
		return 3;
	case ev_ext_sint64:
	case ev_ext_uint64:
	case ev_ext_double:
		return 2;
	case ev_string:
	case ev_float:
	case ev_entity:
	case ev_field:
	case ev_function:
	case ev_pointer:
	case ev_ext_integer:
	case ev_ext_uint32:
		return 1;
	default:
		return 0;
	}
}
bool Component (const ddef_t &definition, bool globals)
{
	const char	*name = PR_GetString (definition.s_name);
	const size_t length = strlen (name);
	if (length <= 2 || name[length - 2] != '_')
		return false;
	static constexpr char axes[] = "xyz";
	const char			 *axis = strchr (axes, name[length - 1]);
	if (!axis)
		return false;
	const std::string parent (name, length - 2);
	const ddef_t	 *vector = globals ? ED_FindGlobal (parent.c_str ()) : ED_FindField (parent.c_str ());
	return vector && (vector->type & ~DEF_SAVEGLOBAL) == ev_vector && definition.ofs == vector->ofs + (axis - axes);
}
bool PositionField (const std::string &name)
{
	return name == "origin" || name == "oldorigin" || name == "absmin" || name == "absmax";
}
void PutString (wire::Writer &writer, const std::string &text)
{
	Check (
		text.size () <= 1024 * 1024 && writer.U32 (uint32_t (text.size ())) && writer.Bytes ({reinterpret_cast<const uint8_t *> (text.data ()), text.size ()}),
		"checkpoint string/size limit exceeded");
}
void CheckNumbers (const Value &value)
{
	if (value.type == ev_float || value.type == ev_vector)
		for (size_t i = 0; i < Width (value.type); ++i)
			Check (std::isfinite (std::bit_cast<float> (value.words[i])), "nonfinite checkpoint float/vector");
	if (value.type == ev_ext_double)
	{
		double number;
		memcpy (&number, value.words.data (), sizeof (number));
		Check (std::isfinite (number), "nonfinite checkpoint double");
	}
	if (value.type == ev_float && (value.name == "flags" || value.name == "solid" || value.name == "movetype" || value.name == "modelindex"))
	{
		const double number = std::bit_cast<float> (value.words[0]);
		Check (number >= INT32_MIN && number <= INT32_MAX, "checkpoint native field exceeds integer range");
		if (value.name == "modelindex")
			Check (number >= 0 && number < MAX_MODELS && std::floor (number) == number, "invalid checkpoint model index");
	}
}
std::string GetString (wire::Reader &reader, size_t maximum = 1024 * 1024)
{
	uint32_t length;
	Check (reader.U32 (length) && length <= maximum && length <= reader.Remaining (), "invalid checkpoint string length");
	std::string result (length, '\0');
	Check (reader.Bytes ({reinterpret_cast<uint8_t *> (result.data ()), length}) && result.find ('\0') == std::string::npos, "invalid checkpoint string");
	return result;
}
void PutValues (wire::Writer &writer, const std::vector<Value> &values)
{
	Check (writer.U32 (uint32_t (values.size ())), "checkpoint size limit exceeded");
	for (const auto &value : values)
	{
		PutString (writer, value.name);
		Check (writer.U16 (value.type), "checkpoint size limit exceeded");
		if (value.type == ev_string)
			PutString (writer, value.text);
		else
			for (size_t i = 0; i < Width (value.type); ++i)
				Check (writer.U32 (value.words[i]), "checkpoint size limit exceeded");
	}
}
std::vector<Value> GetValues (wire::Reader &reader, bool globals, uint32_t entities)
{
	uint32_t count;
	Check (reader.U32 (count) && count <= uint32_t (globals ? qcvm->progs->numglobaldefs : qcvm->progs->numfielddefs), "invalid checkpoint field count");
	std::vector<Value>	  result;
	std::set<std::string> names;
	std::set<uint32_t>	  offsets;
	for (uint32_t i = 0; i < count; ++i)
	{
		Value value;
		value.name = GetString (reader, 1024);
		Check (names.insert (value.name).second && reader.U16 (value.type) && Width (value.type), "invalid checkpoint field");
		ddef_t *definition = globals ? ED_FindGlobal (value.name.c_str ()) : ED_FindField (value.name.c_str ());
		Check (
			definition && (definition->type & ~DEF_SAVEGLOBAL) == value.type &&
				size_t (definition->ofs) + Width (value.type) <= size_t (globals ? qcvm->progs->numglobals : qcvm->progs->entityfields),
			"checkpoint field schema differs from current progs.dat");
		Check (!Component (*definition, globals), "checkpoint contains an aliased vector component");
		for (size_t word = 0; word < Width (value.type); ++word)
			Check (offsets.insert (definition->ofs + uint32_t (word)).second, "overlapping checkpoint fields");
		if (globals)
			Check (definition->type & DEF_SAVEGLOBAL, "checkpoint contains nonpersistent global");
		else
			Check (!(definition->type & DEF_SAVEGLOBAL) && !PositionField (value.name), "checkpoint contains raw position or synthetic field");
		if (value.type == ev_string)
			value.text = GetString (reader);
		else
			for (size_t word = 0; word < Width (value.type); ++word)
				Check (reader.U32 (value.words[word]), "truncated checkpoint field");
		if (value.type == ev_entity)
			Check (value.words[0] < entities, "invalid checkpoint entity reference");
		if (value.type == ev_function)
			Check (value.words[0] < uint32_t (qcvm->progs->numfunctions), "invalid checkpoint function");
		if (value.type == ev_field)
			Check (value.words[0] < uint32_t (qcvm->progs->entityfields), "invalid checkpoint field reference");
		if (value.type == ev_pointer)
			Check (!value.words[0], "checkpoint contains a VM pointer");
		CheckNumbers (value);
		result.push_back (std::move (value));
	}
	return result;
}
std::vector<Value> CaptureValues (const float *base, bool globals)
{
	const ddef_t	  *definitions = globals ? qcvm->globaldefs : qcvm->fielddefs;
	const int		   count = globals ? qcvm->progs->numglobaldefs : qcvm->progs->numfielddefs;
	std::vector<Value> result;
	for (int i = globals ? 0 : 1; i < count; ++i)
	{
		const auto	  &definition = definitions[i];
		const unsigned type = definition.type & ~DEF_SAVEGLOBAL;
		const char	  *name = PR_GetString (definition.s_name);
		if (!Width (type) || !*name || Component (definition, globals) || (globals && !(definition.type & DEF_SAVEGLOBAL)) ||
			(!globals && ((definition.type & DEF_SAVEGLOBAL) || PositionField (name))))
			continue;
		Check (size_t (definition.ofs) + Width (type) <= size_t (globals ? qcvm->progs->numglobals : qcvm->progs->entityfields), "invalid QC field definition");
		Value value;
		value.name = name;
		value.type = uint16_t (type);
		memcpy (value.words.data (), base + definition.ofs, Width (type) * sizeof (uint32_t));
		CheckNumbers (value);
		if (!globals && value.words == std::array<uint32_t, 3>{})
			continue;
		if (type == ev_string)
			value.text = PR_GetString (int32_t (value.words[0]));
		if (type == ev_pointer)
			Check (!value.words[0], "nonzero VM pointers cannot be checkpointed");
		if (type == ev_entity)
		{
			Check (value.words[0] % qcvm->edict_size == 0 && value.words[0] / qcvm->edict_size < uint32_t (qcvm->num_edicts), "invalid QC entity reference");
			value.words[0] /= qcvm->edict_size;
		}
		result.push_back (std::move (value));
	}
	return result;
}
void ApplyValues (float *base, const std::vector<Value> &values, bool globals)
{
	for (const auto &value : values)
	{
		ddef_t *definition = globals ? ED_FindGlobal (value.name.c_str ()) : ED_FindField (value.name.c_str ());
		auto	words = value.words;
		if (value.type == ev_string)
			words[0] = InternedString (value.text);
		if (value.type == ev_entity)
			words[0] *= qcvm->edict_size;
		memcpy (base + definition->ofs, words.data (), Width (value.type) * sizeof (uint32_t));
	}
}
template <size_t N> std::vector<std::string> CaptureStrings (const char *const (&strings)[N])
{
	std::vector<std::string> result (N);
	for (size_t i = 0; i < N; ++i)
		if (strings[i])
			result[i] = strings[i];
	while (!result.empty () && result.back ().empty ())
		result.pop_back ();
	return result;
}
void PutStrings (wire::Writer &writer, const std::vector<std::string> &strings)
{
	Check (writer.U32 (uint32_t (strings.size ())), "checkpoint size limit exceeded");
	for (const auto &text : strings)
		PutString (writer, text);
}
std::vector<std::string> GetStrings (wire::Reader &reader, size_t maximum)
{
	uint32_t count;
	Check (reader.U32 (count) && count <= maximum, "invalid checkpoint precache count");
	std::vector<std::string> strings;
	for (uint32_t i = 0; i < count; ++i)
		strings.push_back (GetString (reader, 4096));
	return strings;
}
const char *EngineString (const std::string &value)
{
	return PR_GetString (InternedString (value));
}
} // namespace

void OW_CheckpointResetStrings ()
{
	if (qcvm == &sv.qcvm)
		restoredStrings.Clear ();
}

bool OW_CheckpointCapture (quiver::World &world, std::vector<uint8_t> &bytes, std::string &error)
{
	VMGuard guard;
	try
	{
		Check (!qcvm->depth && sv.active, "checkpoint requires an idle active server");
		Check (!sv.num_statics && !sv.num_ambients, "checkpointing static entities/ambient sounds is not supported yet");
		if (!OW_PagingFlush (error) || !world.Flush (&error))
			return false;
		world.Pump (1024);
		OW_SyncEntities ();
		wire::Writer writer (Limit);
		Check (
			writer.U32 (Magic) && writer.U32 (qcvm->progshash) && writer.U32 (qcvm->progssize) && writer.U32 (svs.maxclients) &&
				writer.U32 (qcvm->num_edicts) && writer.U64 (std::bit_cast<uint64_t> (qcvm->time)) && writer.U32 (uint32_t (pr_global_struct->serverflags)) &&
				writer.F32 (skill.value) && writer.F32 (coop.value) && writer.F32 (deathmatch.value) &&
				writer.U8 (svs.maxclients == 1 && svs.clients[0].active && svs.clients[0].spawned),
			"checkpoint size limit exceeded");
		for (float parm : svs.clients[0].spawn_parms)
			Check (writer.F32 (parm), "invalid player spawn parameter");
		PutValues (writer, CaptureValues (qcvm->globals, true));
		for (int index = 0; index < qcvm->num_edicts; ++index)
		{
			edict_t	  *entity = EDICT_NUM (index);
			// Multiplayer clients reconnect through normal spawn; identity/account persistence is separate.
			const bool live = !entity->free && !(svs.maxclients > 1 && index > 0 && index <= svs.maxclients);
			Check (writer.U8 (live), "checkpoint size limit exceeded");
			if (!live)
				continue;
			WorldPosition position;
			OW_GetEntityPosition (entity, position);
			Check (
				writer.U64 (OW_EntityPageId (entity)) && writer.U64 (OW_EntityPageRevision (entity)) && writer.U8 (entity->alpha) && writer.Position (position),
				"invalid canonical entity position");
			PutValues (writer, CaptureValues (reinterpret_cast<float *> (&entity->v), false));
		}
		PutStrings (writer, CaptureStrings (sv.model_precache));
		PutStrings (writer, CaptureStrings (sv.sound_precache));
		PutStrings (writer, CaptureStrings (sv.lightstyles));
		std::vector<uint8_t> handles;
		if (!OW_QCExportPositions (handles, error, svs.maxclients > 1 ? uint32_t (svs.maxclients) : 0))
			return false;
		Check (writer.U32 (uint32_t (handles.size ())) && writer.Bytes (handles), "checkpoint size limit exceeded");
		bytes = writer.Data ();
		return true;
	}
	catch (const std::exception &exception)
	{
		error = exception.what ();
		return false;
	}
}

bool OW_CheckpointSave (quiver::World &world, std::string &error)
{
	std::vector<uint8_t> bytes;
	return OW_CheckpointCapture (world, bytes, error) && world.SaveCheckpoint (bytes, &error);
}

static bool CheckpointLoad (quiver::World &world, bool &restored, std::string &error, const std::vector<uint8_t> *validateOnly = nullptr)
{
	VMGuard guard;
	restored = false;
	try
	{
		std::vector<uint8_t> bytes;
		if (validateOnly)
			bytes = *validateOnly;
		else if (!world.LoadCheckpoint (bytes, &error))
			return false;
		if (bytes.empty ())
			return true;
		wire::Reader reader (bytes);
		Snapshot	 snapshot;
		uint32_t	 magic, count;
		uint64_t	 time;
		uint8_t		 player;
		Check (
			reader.U32 (magic) && (magic == Magic || magic == LegacyMagic) && reader.U32 (snapshot.hash) && reader.U32 (snapshot.size) &&
				snapshot.hash == qcvm->progshash && snapshot.size == qcvm->progssize,
			"checkpoint requires the same progs.dat");
		Check (reader.U32 (snapshot.clients) && snapshot.clients == uint32_t (svs.maxclients), "checkpoint requires the same maxclients setting");
		Check (reader.U32 (count) && count >= snapshot.clients + 1 && count <= uint32_t (qcvm->max_edicts), "checkpoint entity capacity exceeded");
		Check (
			reader.U64 (time) && reader.U32 (snapshot.flags) && reader.F32 (snapshot.skill) && reader.F32 (snapshot.coop) && reader.F32 (snapshot.deathmatch) &&
				reader.U8 (player) && player <= 1,
			"invalid checkpoint header");
		Check (snapshot.skill >= 0 && snapshot.skill <= 3, "invalid checkpoint skill");
		snapshot.time = std::bit_cast<double> (time);
		snapshot.player = player != 0;
		Check (std::isfinite (snapshot.time) && snapshot.time >= 0 && (!snapshot.player || snapshot.clients == 1), "invalid checkpoint time/player state");
		for (auto &parm : snapshot.parms)
			Check (reader.F32 (parm), "invalid checkpoint spawn parameter");
		snapshot.globals = GetValues (reader, true, count);
		snapshot.entities.resize (count);
		std::set<uint64_t> pageIds;
		for (auto &entity : snapshot.entities)
		{
			uint8_t live;
			Check (reader.U8 (live) && live <= 1, "invalid checkpoint entity");
			entity.live = live;
			if (!live)
				continue;
			if (magic == Magic)
			{
				Check (
					reader.U64 (entity.pageId) && reader.U64 (entity.pageRevision) && entity.pageRevision <= uint64_t (INT64_MAX) &&
						(entity.pageId || !entity.pageRevision),
					"invalid checkpoint page identity");
				if (entity.pageId)
					Check (pageIds.insert (entity.pageId).second, "duplicate checkpoint page identity");
			}
			Check (reader.U8 (entity.alpha) && reader.Position (entity.position), "invalid checkpoint position");
			entity.values = GetValues (reader, false, count);
		}
		Check (snapshot.entities[0].live && (!snapshot.player || snapshot.entities[1].live), "checkpoint is missing world/player");
		for (size_t i = 0; i <= snapshot.clients; ++i)
			Check (!snapshot.entities[i].pageId, "world/client entities cannot be paged");
		snapshot.models = GetStrings (reader, MAX_MODELS);
		snapshot.sounds = GetStrings (reader, MAX_SOUNDS);
		snapshot.lights = GetStrings (reader, MAX_LIGHTSTYLES);
		Check (snapshot.models.size () > 1 && snapshot.models[1] == "maps/__octree.bsp", "invalid checkpoint world model");
		uint32_t handleBytes;
		Check (reader.U32 (handleBytes) && handleBytes <= 16 * 1024 * 1024 && handleBytes == reader.Remaining (), "invalid checkpoint handle payload");
		snapshot.handles.resize (handleBytes);
		Check (reader.Bytes (snapshot.handles), "truncated checkpoint handles");
		std::vector<uint8_t> liveEntities;
		liveEntities.reserve (count);
		for (const auto &entity : snapshot.entities)
			liveEntities.push_back (entity.live);
		if (!OW_QCValidatePositions (snapshot.handles, count, error, liveEntities))
			return false;
		std::vector<std::string_view> texts;
		CollectStrings (snapshot.globals, texts);
		for (const auto &entity : snapshot.entities)
			CollectStrings (entity.values, texts);
		for (const auto &value : snapshot.models)
			texts.push_back (value);
		for (const auto &value : snapshot.sounds)
			texts.push_back (value);
		for (const auto &value : snapshot.lights)
			texts.push_back (value);
		if (validateOnly)
			return true;	   // Schema, references, progs identity and position handles validated without mutation.
		InternStrings (texts); // Budget/preflight before checkpoint or live entity mutation.
		if (!world.RestoreEntityPagesCheckpoint (&error))
			return false;

		memset (sv.model_precache, 0, sizeof (sv.model_precache));
		memset (sv.models, 0, sizeof (sv.models));
		for (size_t i = 0; i < snapshot.models.size (); ++i)
		{
			sv.model_precache[i] = EngineString (snapshot.models[i]);
			if (i && !snapshot.models[i].empty ())
				sv.models[i] = Mod_ForName (sv.model_precache[i], true);
		}
		memset (sv.sound_precache, 0, sizeof (sv.sound_precache));
		for (size_t i = 0; i < snapshot.sounds.size (); ++i)
			sv.sound_precache[i] = EngineString (snapshot.sounds[i]);
		memset (sv.lightstyles, 0, sizeof (sv.lightstyles));
		for (size_t i = 0; i < snapshot.lights.size (); ++i)
			sv.lightstyles[i] = EngineString (snapshot.lights[i]);

		// All serialized fields are validated before changing live server state.
		// This runs during server startup, before clients receive the new baseline.
		for (int i = 1; i < qcvm->num_edicts; ++i)
			SV_UnlinkEdict (EDICT_NUM (i));
		OW_QCReset ();
		OW_PagingReset ();
		for (int i = 0; i < qcvm->num_edicts; ++i)
			OW_ForgetEntity (EDICT_NUM (i));
		memset (qcvm->edicts, 0, qcvm->max_edicts * qcvm->edict_size);
		qcvm->num_edicts = int (count);
		SV_ClearWorld ();
		ApplyValues (qcvm->globals, snapshot.globals, true);
		for (uint32_t index = 0; index < count; ++index)
		{
			edict_t *entity = EDICT_NUM (int (index));
			entity->free = !snapshot.entities[index].live && index > uint32_t (svs.maxclients);
			entity->baseline = nullentitystate;
			entity->alpha = snapshot.entities[index].alpha;
			if (snapshot.entities[index].live)
				ApplyValues (reinterpret_cast<float *> (&entity->v), snapshot.entities[index].values, false);
		}
		std::vector<WorldPosition> savedPositions;
		savedPositions.reserve (count);
		for (const auto &entity : snapshot.entities)
			savedPositions.push_back (entity.position);
		Check (OW_RestoreEntityPositions (savedPositions, liveEntities), "failed to restore canonical entity positions");
		for (uint32_t i = 0; i < count; ++i)
			if (snapshot.entities[i].live)
				Check (
					OW_RestoreEntityPageId (EDICT_NUM (int (i)), snapshot.entities[i].pageId, snapshot.entities[i].pageRevision),
					"failed to restore stable entity page identity");
		ED_RebuildFreeList (true);
		if (!OW_QCImportPositions (snapshot.handles, error))
			return false;
		qcvm->time = snapshot.time;
		pr_global_struct->time = float (snapshot.time);
		svs.serverflags = int (snapshot.flags);
		Cvar_SetValueQuick (&skill, snapshot.skill);
		current_skill = int (snapshot.skill);
		Cvar_SetValueQuick (&coop, snapshot.coop);
		Cvar_SetValueQuick (&deathmatch, snapshot.deathmatch);
		for (size_t i = 0; i < snapshot.parms.size (); ++i)
			svs.clients[0].spawn_parms[i] = snapshot.parms[i];
		sv.loadgame = snapshot.player;
		sv.paused = snapshot.player;
		if (snapshot.player)
			OW_BeginEntity (EDICT_NUM (1));
		// Restoring the terrain root invalidates the startup chunk cache. Rebuild
		// collision residency for the saved player or the fresh multiplayer spawn.
		const WorldPosition p = snapshot.player ? snapshot.entities[1].position : WorldPosition{{}, {128, 128, 256}};
		for (int z = -1; z <= 1; ++z)
			for (int y = -1; y <= 1; ++y)
				for (int x = -1; x <= 1; ++x)
				{
					WorldPosition neighbor;
					if (Add (p, {double (x * ChunkSize), double (y * ChunkSize), double (z * ChunkSize)}, neighbor))
						Check (world.Require (neighbor.chunk, &error), "failed to restore player collision neighborhood");
				}
		Con_Printf ("World checkpoint restored: %u entity slots, time %.3f%s\n", count, snapshot.time, snapshot.player ? ", saved player" : "");
		restored = true;
		return true;
	}
	catch (const std::exception &exception)
	{
		error = exception.what ();
		return false;
	}
}

bool OW_CheckpointLoad (quiver::World &world, bool &restored, std::string &error)
{
	return CheckpointLoad (world, restored, error);
}

bool OW_CheckpointValidate (quiver::World &world, const std::vector<uint8_t> &bytes, std::string &error)
{
	if (bytes.empty ())
	{
		error = "snapshot has no game-state checkpoint";
		return false;
	}
	bool restored = false;
	return CheckpointLoad (world, restored, error, &bytes);
}

bool OW_CheckpointRestoreSnapshot (quiver::World &world, const std::string &name, std::string &error)
{
	VMGuard guard;
	if (qcvm->depth || !sv.active)
	{
		error = "snapshot restore requires an idle active server";
		return false;
	}
	std::vector<uint8_t> bytes;
	// Paging callbacks inspect edicts, so retain the server VM selection through
	// the drain and commit as well as through checkpoint validation.
	return world.LoadSnapshot (name, bytes, &error) && OW_CheckpointValidate (world, bytes, error) && OW_PagingFlush (error) &&
		   world.RestoreSnapshot (name, &error);
}

bool OW_EntityPageEncode (edict_t *entity, double snapshotTime, std::vector<uint8_t> &bytes, std::string &error)
{
	try
	{
		Check (qcvm == &sv.qcvm && !qcvm->depth && !entity->free, "entity paging requires an idle server VM");
		WorldPosition position;
		Check (OW_GetEntityPosition (entity, position), "entity position unavailable");
		auto values = CaptureValues (reinterpret_cast<float *> (&entity->v), false);
		for (auto &value : values)
			if (value.type == ev_entity && value.words[0])
			{
				Check (value.words[0] == uint32_t (NUM_FOR_EDICT (entity)), "entity page contains an external entity reference");
				value.words[0] = 1; // Relative self, never a recyclable edict slot.
			}
		std::vector<uint8_t> handles;
		if (!OW_QCExportEntityPositions (entity, handles, error))
			return false;
		wire::Writer writer (MaxEntityPageBytes);
		Check (
			writer.U32 (PageMagic) && writer.U32 (qcvm->progshash) && writer.U32 (qcvm->progssize) && writer.U64 (std::bit_cast<uint64_t> (snapshotTime)) &&
				writer.U8 (entity->alpha) && writer.Position (position),
			"invalid entity page header");
		PutValues (writer, values);
		Check (writer.U32 (uint32_t (handles.size ())) && writer.Bytes (handles), "entity page exceeds 1 MiB");
		bytes = writer.Data ();
		return true;
	}
	catch (const std::exception &exception)
	{
		error = exception.what ();
		return false;
	}
}

bool OW_EntityPageDecode (std::span<const uint8_t> bytes, edict_t *&entity, std::string &error)
{
	edict_t *allocated = nullptr;
	try
	{
		Check (qcvm == &sv.qcvm && !qcvm->depth && bytes.size () <= MaxEntityPageBytes, "invalid entity page context/size");
		wire::Reader  reader (bytes);
		uint32_t	  handleSize;
		uint8_t		  alpha;
		double		  savedTime;
		WorldPosition position;
		ReadPageHeader (reader, savedTime, alpha, position);
		auto values = GetValues (reader, false, 2);
		Check (reader.U32 (handleSize) && handleSize == reader.Remaining (), "invalid entity page handles");
		std::vector<uint8_t> handles (handleSize);
		Check (reader.Bytes (handles), "truncated entity page handles");
		if (!OW_QCValidateEntityPositions (handles, error))
			return false;
		// Map model indices through the current precache. Mods must precache all
		// streamed actor assets during worldspawn, including after process restart.
		int modelIndex = 0;
		for (const auto &value : values)
			if (value.name == "model" && !value.text.empty ())
			{
				for (int i = 1; i < MAX_MODELS && sv.model_precache[i]; ++i)
					if (value.text == sv.model_precache[i])
					{
						modelIndex = i;
						break;
					}
				Check (modelIndex != 0, "streamed entity model must be precached by worldspawn");
			}
		std::vector<std::string_view> texts;
		CollectStrings (values, texts);
		InternStrings (texts); // An exhausted pool leaves the entity dormant, before ED_Alloc.
		allocated = ED_Alloc ();
		for (auto &value : values)
		{
			if (value.type == ev_entity && value.words[0])
				value.words[0] = NUM_FOR_EDICT (allocated);
			if (value.name == "nextthink" && value.type == ev_float)
			{
				const float time = std::bit_cast<float> (value.words[0]);
				if (time > 0)
					value.words[0] = std::bit_cast<uint32_t> (float (qcvm->time + std::max (0.001, double (time) - savedTime)));
			}
		}
		ApplyValues (reinterpret_cast<float *> (&allocated->v), values, false);
		allocated->v.modelindex = float (modelIndex);
		allocated->alpha = alpha;
		allocated->oldframe = allocated->v.frame;
		allocated->oldthinktime = allocated->lastthink = 0;
		allocated->sendinterval = false;
		if (!OW_QCImportEntityPositions (allocated, handles, error))
		{
			ED_Free (allocated);
			return false;
		}
		Check (OW_SetEntityPosition (allocated, position), "invalid restored entity position");
		entity = allocated;
		return true;
	}
	catch (const std::exception &exception)
	{
		if (allocated)
			ED_Free (allocated);
		error = exception.what ();
		return false;
	}
}

bool OW_EntityPagePosition (std::span<const uint8_t> bytes, WorldPosition &position, std::string &error)
{
	try
	{
		Check (qcvm == &sv.qcvm && qcvm->progs && bytes.size () <= MaxEntityPageBytes, "invalid entity page context/size");
		wire::Reader  reader (bytes);
		double		  savedTime;
		uint8_t		  alpha;
		WorldPosition decoded;
		ReadPageHeader (reader, savedTime, alpha, decoded);
		position = decoded;
		return true;
	}
	catch (const std::exception &exception)
	{
		error = exception.what ();
		return false;
	}
}
