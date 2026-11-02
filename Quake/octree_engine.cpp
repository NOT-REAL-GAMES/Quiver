/* Quiver streamed worlds. SPDX-License-Identifier: GPL-2.0-or-later */
#include <algorithm>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <deque>
#include <map>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include "quakedef.h"
#include "octree_engine.h"
#include "octree_checkpoint.h"
#include "octree_paging.h"
#include "octree_render.h"
#include "octree_qc.h"
#include "world_protocol.h"
#include "world_peer.h"
#include "world_solids.h"
#include "jolt_stream_world.h"
#include "jolt_stream_character.h"

namespace
{
using namespace quiver;
struct KeyLess
{
	bool operator() (const ChunkKey &a, const ChunkKey &b) const
	{
		if (a.x != b.x)
			return a.x < b.x;
		if (a.y != b.y)
			return a.y < b.y;
		return a.z < b.z;
	}
};
struct EntityPosition
{
	WorldPosition position;
	WorldPosition oldPosition;
	WorldPosition predictedPosition;
	bool		  known = false;
	bool		  projected = true;
	bool		  hasPrediction = false;
};
struct PendingSignal
{
	std::vector<uint8_t> bytes;
	double				 queuedAt = 0;
};
struct Transfer
{
	bool					  peerNAT = false;
	std::set<uint64_t>		  introducedPeers;
	std::deque<PendingSignal> signals;
	size_t					  signalBytes = 0;
	double					  signalBudgetTime = 0, signalBudgetBytes = 32768, signalBudgetFragments = 64;

	bool								waitingAck = false;
	std::map<ChunkKey, double, KeyLess> retryAfter;
	ContentHash							root{};
	uint64_t							epoch = 0;
	uint16_t							peerPort = 0;
	double								peerListTime = -1000;
	bool								offered = false;
	double								offerDeadline = 0;
	TerrainProof						proof;

	std::map<ChunkKey, uint64_t, KeyLess> sent;
	std::shared_ptr<const Chunk>		  chunk;
	std::vector<uint8_t>				  bytes;
	size_t								  offset = 0;
	uint64_t							  serial = 0;
	uint64_t							  lastRequest = 0;
	uint64_t							  generation = 0;
	double								  lastEditTime = -1;
	std::vector<uint8_t>				  editReply;
	bool								  editPending = false;
	bool								  replyPending = false;
};
std::unique_ptr<World>			 serverWorld, remoteWorld;
std::unique_ptr<JoltStreamWorld> serverPhysics, remotePhysics;
JoltStreamWorld					&QueryPhysics (World &world)
{
	auto &physics = &world == serverWorld.get () ? serverPhysics : remotePhysics;
	if (!physics)
		physics = std::make_unique<JoltStreamWorld> (world, true);
	return *physics;
}
struct PlayerPhysics
{
	JoltStreamWorld		world;
	JoltStreamCharacter character;
	explicit PlayerPhysics (World &source, uint64_t id) : world (source, true), character (world, id) {}
};
std::map<int, std::unique_ptr<PlayerPhysics>>		 playerPhysics;
WorldConfig											 pendingConfig;
bool												 pending = false, clientProtocol = false;
uint64_t											 nextEditRequest = 1;
uint64_t											 nextConnection = 1, nextReservation = 1;
std::map<uint64_t, WorldPosition>					 placements;
WorldPosition										 serverFrame, clientFrame;
std::vector<EntityPosition>							 positions;
std::vector<Transfer>								 transfers;
wire::RootState										 remoteAuthority;
std::deque<ContentHash>								 remoteRoots;
// A bounded transfer pins the root authenticated when its first fragment arrived,
// so unrelated rapid edits cannot invalidate a large transfer halfway through.
std::map<uint64_t, std::pair<ContentHash, uint64_t>> transferRoots;
std::map<ContentHash, double>						 offeredRoots;
bool												 KnownRoot (const ContentHash &root)
{
	if (std::find (remoteRoots.begin (), remoteRoots.end (), root) != remoteRoots.end ())
		return true;
	auto offer = offeredRoots.find (root);
	return offer != offeredRoots.end () && realtime >= offer->second && realtime - offer->second < 15;
}
peer::Service								peerService;
peer::Token									sessionPeerToken{}, sessionPeerId{};
double										sessionPeerTokenTime = -1000;
std::vector<peer::Endpoint>					peerEndpoints;
std::map<ContentHash, std::vector<uint8_t>> peerCache;
size_t										peerCacheBytes = 0;
uint64_t									peerRejectedObjects = 0;
bool										listenerAdvertised = false;
wire::Reassembler							reassembly;
wire::SignalReassembler						signalReassembly;
std::deque<PendingSignal>					outboundSignals;
size_t										outboundSignalBytes = 0;
uint64_t									localPeerId = 0, nextSignalId = 1;
peer::Token									clientPeerSession{};
bool										peerNATFailed = false, peerNATConfigured = false;
uint8_t										advertisedCapabilities = 255;
cvar_t										world_peer_nat = {"world_peer_nat", "1", CVAR_ARCHIVE};
cvar_t										world_peer_stun = {"world_peer_stun", "stun.l.google.com:19302", CVAR_ARCHIVE};
cvar_t										world_cache_chunks = {"world_cache_chunks", "2048", CVAR_ARCHIVE};
cvar_t										world_view_chunks = {"world_view_chunks", "2", CVAR_ARCHIVE};
cvar_t										world_cache_mb = {"world_cache_mb", "1024", CVAR_ARCHIVE};
cvar_t										world_editing = {"world_editing", "1", CVAR_NONE};

bool EngineDelta (const WorldPosition &to, const WorldPosition &from, std::array<double, 3> &result, double limit = 1048576.0)
{
	const World *world = serverWorld ? serverWorld.get () : remoteWorld.get ();
	return world ? WorldDelta (world->Config (), to, from, result, limit) : Delta (to, from, result, limit);
}
ChunkKey EngineKey (ChunkKey key)
{
	const World *world = serverWorld ? serverWorld.get () : remoteWorld.get ();
	return world ? CanonicalWorldKey (world->Config (), key) : key;
}
bool ParseI64 (const char *text, int64_t &value)
{
	const char *end = text + strlen (text);
	auto		result = std::from_chars (text, end, value);
	return result.ec == std::errc{} && result.ptr == end;
}
bool ValidName (const char *name, size_t maximum = 48)
{
	if (!*name || strlen (name) > maximum)
		return false;
	for (const unsigned char *p = (const unsigned char *)name; *p; ++p)
		if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_' || *p == '-'))
			return false;
	return true;
}
std::string DatabasePath (const char *name)
{
	return (std::filesystem::path (com_gamedir) / "worlds" / (std::string (name) + ".sqlite3")).string ();
}
WorldPosition FromLocal (const float *point, const WorldPosition &frame)
{
	WorldPosition result = frame;
	if (!Add (frame, {point[0], point[1], point[2]}, result))
		Host_Error ("World coordinate overflow or non-finite position");
	result.chunk = EngineKey (result.chunk);
	return result;
}
void AddNeighbors (std::set<ChunkKey, KeyLess> &keys, const ChunkKey &center, int radius)
{
	WorldPosition base{center, {}};
	for (int z = -radius; z <= radius; ++z)
		for (int y = -radius; y <= radius; ++y)
			for (int x = -radius; x <= radius; ++x)
			{
				WorldPosition p;
				if (Add (base, {double (x * ChunkSize), double (y * ChunkSize), double (z * ChunkSize)}, p))
					keys.insert (EngineKey (p.chunk));
			}
}
void RequireAround (World &world, const WorldPosition &position)
{
	std::set<ChunkKey, KeyLess> keys;
	AddNeighbors (keys, position.chunk, 1);
	std::string error;
	// Bootstrap/teleport is synchronous: release old pins until the next interest update.
	world.SetInterest (std::vector<ChunkKey> (keys.begin (), keys.end ()));
	for (const auto &key : keys)
		if (!world.Require (key, &error))
			Host_Error ("World load: %s", error.c_str ());
}
void SelectFrame (const WorldPosition &position)
{
	OW_SyncEntities ();
	WorldPosition frame{position.chunk, {}};
	if (frame.chunk == serverFrame.chunk)
		return;
	serverFrame = frame;
	// Only this temporary native/QC execution frame changes. Entity positions
	// remain canonical; separate actors can be arbitrarily far apart.
	for (int i = 1; i < qcvm->num_edicts; ++i)
		SV_UnlinkEdict (EDICT_NUM (i));
	SV_ClearWorld ();
	for (int i = 1; i < qcvm->num_edicts; ++i)
	{
		edict_t *ent = EDICT_NUM (i);
		if (ent->free)
			continue;
		auto				 &slot = positions[i];
		std::array<double, 3> delta;
		slot.projected = EngineDelta (slot.position, serverFrame, delta, 8192);
		if (!slot.projected)
			continue;
		for (int axis = 0; axis < 3; ++axis)
			ent->v.origin[axis] = float (delta[axis]);
		// Native movement recovery and platform interpolation retain positions
		// between ticks. Reproject their canonical snapshots with the origin.
		if (EngineDelta (slot.oldPosition, serverFrame, delta, 8192))
			for (int axis = 0; axis < 3; ++axis)
				ent->v.oldorigin[axis] = float (delta[axis]);
		else
		{
			slot.oldPosition = slot.position;
			VectorCopy (ent->v.origin, ent->v.oldorigin);
		}
		if (slot.hasPrediction && EngineDelta (slot.predictedPosition, serverFrame, delta, 8192))
			for (int axis = 0; axis < 3; ++axis)
				ent->predthinkpos[axis] = float (delta[axis]);
		else
		{
			slot.hasPrediction = false;
			ent->lastthink = 0;
		}
		SV_LinkEdict (ent, false);
	}
}
bool NearPlayer (const WorldPosition &position)
{
	for (int i = 0; i < svs.maxclients; ++i)
	{
		if (!svs.clients[i].active || !positions[i + 1].known)
			continue;
		std::array<double, 3> delta;
		if (EngineDelta (position, positions[i + 1].position, delta, 3072))
			return true;
	}
	return sv.state == ss_loading;
}
void WritePacket (sizebuf_t *message, int kind, const std::vector<uint8_t> &bytes)
{
	if (bytes.empty () || bytes.size () > 65535)
		return;
	MSG_WriteByte (message, svc_quiver_world);
	MSG_WriteByte (message, kind);
	MSG_WriteShort (message, int (bytes.size ()));
	SZ_Write (message, bytes.data (), int (bytes.size ()));
}
std::vector<uint8_t> ReadBytes (size_t count)
{
	if (msg_badread || msg_readcount < 0 || msg_readcount > net_message.cursize || count > size_t (net_message.cursize - msg_readcount))
		Host_Error ("Truncated Quiver world message");
	std::vector<uint8_t> result (net_message.data + msg_readcount, net_message.data + msg_readcount + count);
	msg_readcount += int (count);
	return result;
}
void WorldCreate_f ()
{
	if (cmd_source != src_command)
		return;
	if (Cmd_Argc () < 3 || Cmd_Argc () > 4 || !ValidName (Cmd_Argv (1)) ||
		(strcmp (Cmd_Argv (2), "empty") && strcmp (Cmd_Argv (2), "terrain") && strcmp (Cmd_Argv (2), "planet")))
	{
		Con_Printf ("world_create <name> <empty|terrain|planet> [seed]\n");
		return;
	}
	WorldConfig config;
	config.generator = !strcmp (Cmd_Argv (2), "empty") ? Generator::Empty : (!strcmp (Cmd_Argv (2), "planet") ? Generator::Planet : Generator::Terrain);
	if (Cmd_Argc () == 4)
	{
		int64_t seed;
		if (!ParseI64 (Cmd_Argv (3), seed))
		{
			Con_Printf ("Invalid seed\n");
			return;
		}
		config.seed = uint64_t (seed);
	}
	config.databasePath = DatabasePath (Cmd_Argv (1));
	std::error_code error;
	if (std::filesystem::exists (config.databasePath))
	{
		Con_Printf ("World already exists; use world_load\n");
		return;
	}
	std::filesystem::create_directories (std::filesystem::path (config.databasePath).parent_path (), error);
	if (error)
	{
		Con_Printf ("Cannot create worlds directory: %s\n", error.message ().c_str ());
		return;
	}
	pendingConfig = config;
	pending = true;
	Cbuf_InsertText ("map __octree\n");
}
void WorldLoad_f ()
{
	if (cmd_source != src_command)
		return;
	if (Cmd_Argc () != 2 || !ValidName (Cmd_Argv (1)))
	{
		Con_Printf ("world_load <name>\nLoads current durable terrain and the saved game-state checkpoint.\n");
		return;
	}
	const std::string path = DatabasePath (Cmd_Argv (1));
	if (!std::filesystem::is_regular_file (path))
	{
		Con_Printf ("World does not exist\n");
		return;
	}
	pendingConfig = WorldConfig{};
	pendingConfig.databasePath = path;
	pending = true;
	Cbuf_InsertText ("map __octree\n");
}
void WorldSave_f ()
{
	if (cmd_source != src_command || !serverWorld)
		return;
	std::string error;
	if (Cmd_Argc () > 2 || (Cmd_Argc () == 2 && !ValidName (Cmd_Argv (1))))
	{
		Con_Printf ("world_save [name]\n");
		return;
	}
	bool success;
	if (Cmd_Argc () == 2)
	{
		const std::string path = DatabasePath (Cmd_Argv (1));
		const bool		  current = path == serverWorld->Config ().databasePath || path + ".merkle" == serverWorld->Config ().databasePath;
		if (!current && std::filesystem::exists (path))
		{
			Con_Printf ("Destination world already exists\n");
			return;
		}
		std::error_code ec;
		std::filesystem::create_directories (std::filesystem::path (path).parent_path (), ec);
		if (ec)
		{
			Con_Printf ("Cannot create worlds directory: %s\n", ec.message ().c_str ());
			return;
		}
		success = OW_CheckpointSave (*serverWorld, error) && serverWorld->SaveAs (path, &error);
	}
	else if (serverWorld->Config ().databasePath.empty ())
	{
		Con_Printf ("Use world_save <name> to persist this world\n");
		return;
	}
	else
		success = OW_CheckpointSave (*serverWorld, error);
	Con_Printf (success ? "World saved\n" : "World save failed: %s\n", error.c_str ());
}
void WorldSnapshot_f ()
{
	if (cmd_source != src_command)
		return;
	if (!serverWorld || !sv.active)
	{
		Con_Printf ("No octree world loaded\n");
		return;
	}
	const bool list = Cmd_Argc () == 2 && !strcmp (Cmd_Argv (1), "list");
	const bool collect = Cmd_Argc () == 2 && !strcmp (Cmd_Argv (1), "gc");
	if (!list && !collect &&
		(Cmd_Argc () != 3 || !ValidName (Cmd_Argv (2), 64) ||
		 (strcmp (Cmd_Argv (1), "create") && strcmp (Cmd_Argv (1), "restore") && strcmp (Cmd_Argv (1), "delete"))))
	{
		Con_Printf (
			"world_snapshot list | create <name> | restore <name> | delete <name> | gc\n"
			"Restore rolls terrain and game state back together, then restarts the session.\n");
		return;
	}
	if (serverWorld->Config ().databasePath.empty ())
	{
		Con_Printf ("Use world_save <name> before managing snapshots\n");
		return;
	}
	std::string error;
	if (list)
	{
		const auto names = serverWorld->ListSnapshots (&error);
		if (!error.empty ())
			Con_Printf ("World snapshot list failed: %s\n", error.c_str ());
		else
		{
			Con_Printf ("World snapshots: %zu\n", names.size ());
			for (const auto &name : names)
				Con_Printf ("  %s\n", name.c_str ());
		}
		return;
	}
	if (collect)
	{
		if (serverWorld->CollectGarbage (&error))
			Con_Printf ("World snapshot garbage collection complete\n");
		else
			Con_Printf ("World snapshot garbage collection failed: %s\n", error.c_str ());
		return;
	}
	const std::string name = Cmd_Argv (2);
	bool			  success;
	if (!strcmp (Cmd_Argv (1), "create"))
	{
		// Console commands run between ticks. Capture drains and publishes all
		// pending mutations, then the snapshot transaction pins the same boundary.
		std::vector<uint8_t> bytes;
		success = OW_CheckpointCapture (*serverWorld, bytes, error) && serverWorld->CreateSnapshot (name, bytes, &error);
	}
	else if (!strcmp (Cmd_Argv (1), "delete"))
		success = serverWorld->DeleteSnapshot (name, &error);
	else
	{
		// Reject incompatible game code before moving the authoritative world root.
		success = OW_CheckpointRestoreSnapshot (*serverWorld, name, error);
		if (success)
		{
			pendingConfig = serverWorld->Config ();
			pending = true;
			// Restart immediately, before another tick or queued client edit. The new
			// authority epoch invalidates requests referring to pre-restore terrain.
			Cbuf_InsertText ("map __octree\n");
		}
	}
	if (success)
		Con_Printf ("World snapshot %s: %s\n", Cmd_Argv (1), name.c_str ());
	else
		Con_Printf ("World snapshot %s failed: %s\n", Cmd_Argv (1), error.c_str ());
}
void WorldStats_f ()
{
	World *world = OW_GetWorld ();
	if (!world)
	{
		Con_Printf ("No octree world loaded\n");
		return;
	}
	auto stats = world->Stats ();
	Con_Printf (
		"World chunks %zu/%zu, pending %zu, mesh jobs %zu, nodes %zu, quads %zu, smooth triangles %zu\n", stats.residentChunks, world->Config ().residentLimit,
		stats.pendingChunks, stats.pendingMeshes, stats.octreeNodes, stats.meshQuads, stats.meshTriangles);
	if (world->Config ().planet)
		Con_Printf (
			"Planet: %lld chunks per axis, generator v%u, %zu cached regions, %.2f MiB generation cache\n",
			(long long)world->Config ().planetSettings.extentChunks, world->Config ().planetSettings.version, world->Config ().planet->CachedRegions (),
			world->Config ().planet->ResidentBytes () / 1048576.0);
	Con_Printf ("World CPU memory: trees %.2f MiB, meshes %.2f MiB\n", stats.residentBytes / 1048576.0, stats.meshBytes / 1048576.0);
	Con_Printf (
		"Collision: %zu patches, %.2f MiB, %llu local builds\n", stats.collisionPatches, stats.collisionBytes / 1048576.0,
		(unsigned long long)stats.collisionBuilds);
	Con_Printf (
		"Collision workers: %zu pending (%.2f MiB), %llu worker builds, %llu main-thread builds, %llu stale results\n", stats.collisionPending,
		stats.collisionPendingBytes / 1048576.0, (unsigned long long)stats.collisionWorkerBuilds, (unsigned long long)stats.collisionSyncBuilds,
		(unsigned long long)stats.collisionStaleResults);
	JoltStreamStats physicsStats;
	auto			addPhysics = [&] (JoltStreamWorld *physics)
	{
		if (!physics)
			return;
		const auto s = physics->Stats ();
		physicsStats.shapes += s.shapes;
		physicsStats.shapeBytes += s.shapeBytes;
		physicsStats.bodies += s.bodies;
		physicsStats.shapeBuilds += s.shapeBuilds;
		physicsStats.cacheHits += s.cacheHits;
		physicsStats.rebases += s.rebases;
		physicsStats.pendingJobs += s.pendingJobs;
		physicsStats.pendingBytes += s.pendingBytes;
		physicsStats.workerBuilds += s.workerBuilds;
		physicsStats.mainThreadCooks += s.mainThreadCooks;
		physicsStats.deferredQueries += s.deferredQueries;
	};
	addPhysics (serverPhysics.get ());
	addPhysics (remotePhysics.get ());
	for (auto &[id, player] : playerPhysics)
		addPhysics (&player->world);
	Con_Printf (
		"World physics: Jolt, %zu player controllers, %zu bodies, %zu cached shapes (%.2f MiB), %llu builds, %llu hits, %llu rebases\n", playerPhysics.size (),
		physicsStats.bodies, physicsStats.shapes, physicsStats.shapeBytes / 1048576.0, (unsigned long long)physicsStats.shapeBuilds,
		(unsigned long long)physicsStats.cacheHits, (unsigned long long)physicsStats.rebases);
	Con_Printf (
		"World physics workers: %zu pending (%.2f MiB), %llu worker cooks, %llu main-thread cooks, %llu deferred updates\n", physicsStats.pendingJobs,
		physicsStats.pendingBytes / 1048576.0, (unsigned long long)physicsStats.workerBuilds, (unsigned long long)physicsStats.mainThreadCooks,
		(unsigned long long)physicsStats.deferredQueries);
	const auto peerStats = peerService.Stats ();
	Con_Printf (
		"World peers: served %llu fetched %llu uploaded %llu downloaded %llu cached %zu rejected %llu\n", (unsigned long long)peerStats.served,
		(unsigned long long)peerStats.fetched, (unsigned long long)peerStats.uploadedBytes, (unsigned long long)peerStats.downloadedBytes, peerCacheBytes,
		(unsigned long long)peerRejectedObjects);
	Con_Printf (
		"World peer NAT: connected %llu served %llu fetched %llu failed %llu\n", (unsigned long long)peerStats.natConnected,
		(unsigned long long)peerStats.natServed, (unsigned long long)peerStats.natFetched, (unsigned long long)peerStats.natFailed);
	Con_Printf ("World pending edits: %zu (%.2f MiB)\n", stats.pendingEdits, stats.pendingEditBytes / 1048576.0);
	const WorldPosition p = isDedicated && positions.size () > 1 && positions[1].known ? positions[1].position : OW_GetRenderPosition ();
	Con_Printf (
		"World position: chunk %lld %lld %lld local %.3f %.3f %.3f\n", (long long)p.chunk.x, (long long)p.chunk.y, (long long)p.chunk.z, p.local[0], p.local[1],
		p.local[2]);
	if (!world->LastError ().empty ())
		Con_Printf ("World worker: %s\n", world->LastError ().c_str ());
	if (serverWorld)
	{
		size_t active = 0, awaiting = 0;
		for (const auto &transfer : transfers)
		{
			active += transfer.chunk != nullptr;
			awaiting += transfer.waitingAck;
		}
		Con_Printf ("World terrain transfers: active %zu, awaiting ack %zu\n", active, awaiting);
		OW_PagingStats ();
	}
	if (!isDedicated)
		R_OctreeStats ();
}

void WorldBrush_f ()
{
	if (cmd_source != src_command || !serverWorld || !sv.active)
		return;
	if (Cmd_Argc () != 6 && Cmd_Argc () != 9)
	{
		Con_Printf ("world_brush <add|subtract> <box|wedge> <width> <depth> <height> [pitch yaw roll]\nCreates at your current world position.\n");
		return;
	}
	const bool subtract = !strcmp (Cmd_Argv (1), "subtract");
	if ((!subtract && strcmp (Cmd_Argv (1), "add")) || (strcmp (Cmd_Argv (2), "box") && strcmp (Cmd_Argv (2), "wedge")))
		return;
	SolidPoint size{}, angles{};
	for (int a = 0; a < 3; ++a)
	{
		char *end;
		size[a] = strtod (Cmd_Argv (3 + a), &end);
		if (*end || !std::isfinite (size[a]))
			return;
		if (Cmd_Argc () == 9)
		{
			angles[a] = strtod (Cmd_Argv (6 + a), &end);
			if (*end || !std::isfinite (angles[a]))
				return;
		}
	}
	SolidDefinition solid;
	std::string		error;
	const auto		origin = isDedicated ? serverFrame : OW_GetRenderPosition ();
	const bool		made =
		 !strcmp (Cmd_Argv (2), "box") ? MakeSolidBox (origin, size, angles, 3, solid, &error) : MakeSolidWedge (origin, size, angles, 3, solid, &error);
	solid.operation = subtract ? SolidOperation::Subtract : SolidOperation::Add;
	if (!made || !serverWorld->AddSolid (solid, &error))
		Con_Printf ("Solid edit failed: %s\n", error.c_str ());
	else
	{
		RequireAround (*serverWorld, origin);
		Con_Printf ("Solid edit committed.\n");
	}
}
void WorldImport_f ()
{
	if (cmd_source != src_command || !serverWorld || !sv.active)
		return;
	if (Cmd_Argc () != 2 && Cmd_Argc () != 3)
	{
		Con_Printf ("world_import <watertight.obj> [add|subtract]\n");
		return;
	}
	const bool subtract = Cmd_Argc () == 3 && !strcmp (Cmd_Argv (2), "subtract");
	if (Cmd_Argc () == 3 && !subtract && strcmp (Cmd_Argv (2), "add"))
		return;
	std::ifstream file (Cmd_Argv (1), std::ios::binary | std::ios::ate);
	if (!file || file.tellg () < 0 || file.tellg () > 8 * 1024 * 1024)
	{
		Con_Printf ("Cannot read OBJ (limit 8 MiB).\n");
		return;
	}
	std::string text (size_t (file.tellg ()), '\0');
	file.seekg (0);
	file.read (text.data (), text.size ());
	SolidDefinition solid;
	std::string		error;
	auto			origin = isDedicated ? serverFrame : OW_GetRenderPosition ();
	if (!file || !ImportSolidOBJ (text, origin, solid, &error))
	{
		Con_Printf ("OBJ import failed: %s\n", error.c_str ());
		return;
	}
	solid.operation = subtract ? SolidOperation::Subtract : SolidOperation::Add;
	if (!serverWorld->AddSolid (solid, &error))
		Con_Printf ("OBJ commit failed: %s\n", error.c_str ());
	else
	{
		RequireAround (*serverWorld, origin);
		Con_Printf ("Imported editable solid.\n");
	}
}
void WorldTeleport_f ()
{
	if (cmd_source != src_command || !serverWorld || !sv.active)
		return;
	if (Cmd_Argc () != 4 && Cmd_Argc () != 7)
	{
		Con_Printf ("world_teleport <chunk-x> <chunk-y> <chunk-z> [local-x local-y local-z]\n");
		return;
	}
	WorldPosition target;
	for (int i = 0; i < 3; ++i)
		if (!ParseI64 (Cmd_Argv (1 + i), target.chunk[i]))
		{
			Con_Printf ("Invalid chunk coordinate\n");
			return;
		}
	target.local = {128, 128, 256};
	if (Cmd_Argc () == 7)
		for (int i = 0; i < 3; ++i)
		{
			char *end;
			target.local[i] = strtof (Cmd_Argv (4 + i), &end);
			if (*end || !std::isfinite (target.local[i]))
			{
				Con_Printf ("Invalid local coordinate\n");
				return;
			}
		}
	if (!Normalize (target))
	{
		Con_Printf ("World coordinate overflow\n");
		return;
	}
	target.chunk = EngineKey (target.chunk);
	qcvm_t *previous = qcvm;
	PR_SwitchQCVM (nullptr);
	PR_SwitchQCVM (&sv.qcvm);
	edict_t *player = EDICT_NUM (isDedicated ? 1 : std::max (1, cl.viewentity));
	RequireAround (*serverWorld, target);
	OW_SetEntityPosition (player, target);
	SelectFrame (target);
	VectorCopy (vec3_origin, player->v.velocity);
	player->v.flags = int (player->v.flags) & ~FL_ONGROUND;
	PR_SwitchQCVM (nullptr);
	PR_SwitchQCVM (previous);
	Con_Printf ("Teleported to chunk %lld %lld %lld\n", (long long)target.chunk.x, (long long)target.chunk.y, (long long)target.chunk.z);
}
void WorldEdit_f ()
{
	if (cmd_source != src_command)
		return; // Remote editing needs an authenticated request, not a console command.
	const bool remove = !strcmp (Cmd_Argv (0), "world_remove");
	int		   material = 1;
	if (!remove && Cmd_Argc () == 2)
	{
		int64_t value;
		if (!ParseI64 (Cmd_Argv (1), value) || value < 1 || value > 4)
		{
			Con_Printf ("Material must be 1..4\n");
			return;
		}
		material = int (value);
	}
	if (remoteWorld && cls.state == ca_connected)
	{
		vec3_t forward, right, up;
		AngleVectors (cl.viewangles, forward, right, up);
		auto hit = QueryPhysics (*remoteWorld).Trace (OW_GetRenderPosition (), {forward[0] * 256.0, forward[1] * 256.0, forward[2] * 256.0});
		if (hit.fraction == 1 || hit.hitUnknown || hit.startSolid)
		{
			Con_Printf ("No loaded terrain within reach\n");
			return;
		}
		wire::Edit	 edit;
		const double sign = remove ? -1.0 : 1.0;
		if (!Add (hit.end, {hit.normal[0] * sign, hit.normal[1] * sign, hit.normal[2] * sign}, edit.position))
			return;
		auto chunk = remoteWorld->Find (edit.position.chunk);
		if (!chunk)
			return;
		edit.requestId = nextEditRequest++;
		edit.epoch = remoteAuthority.epoch;
		edit.expectedRevision = chunk->revision;
		edit.cell = remove ? Cell{} : Cell{uint16_t (material), Solid};
		auto bytes = wire::EncodeEdit (edit);
		if (bytes.empty () || cls.message.cursize + int (bytes.size ()) + 3 > cls.message.maxsize)
			return;
		MSG_WriteByte (&cls.message, clc_quiver_world);
		MSG_WriteShort (&cls.message, int (bytes.size ()));
		SZ_Write (&cls.message, bytes.data (), int (bytes.size ()));
		return;
	}
	if (!serverWorld || !sv.active || !world_editing.value)
		return;
	qcvm_t *previous = qcvm;
	PR_SwitchQCVM (nullptr);
	PR_SwitchQCVM (&sv.qcvm);
	edict_t *player = EDICT_NUM (isDedicated ? 1 : std::max (1, cl.viewentity));
	OW_BeginEntity (player);
	vec3_t eye, forward, right, up;
	VectorAdd (player->v.origin, player->v.view_ofs, eye);
	AngleVectors (player->v.v_angle, forward, right, up);
	WorldPosition start = FromLocal (eye, serverFrame);
	auto		  hit = QueryPhysics (*serverWorld).Trace (start, {forward[0] * 256.0, forward[1] * 256.0, forward[2] * 256.0});
	if (hit.fraction < 1 && !hit.hitUnknown && !hit.startSolid)
	{
		WorldPosition target;
		const double  sign = remove ? -1.0 : 1.0;
		Add (hit.end, {hit.normal[0] * sign, hit.normal[1] * sign, hit.normal[2] * sign}, target);
		bool overlap = false;
		if (!remove)
		{
			const double		  margin = serverWorld->Config ().generator != Generator::Empty ? VoxelSize / 2 : 0;
			std::array<double, 3> delta;
			EngineDelta (target, serverFrame, delta);
			for (int axis = 0; axis < 3; ++axis)
				delta[axis] = floor (delta[axis] / VoxelSize) * VoxelSize;
			for (int i = 1; i < qcvm->num_edicts; ++i)
			{
				edict_t *ent = EDICT_NUM (i);
				if (ent->free || !OW_EntityVisible (ent) || ent->v.solid < float (SOLID_BBOX))
					continue;
				bool inside = true;
				for (int a = 0; a < 3; ++a)
					inside &= delta[a] - margin < ent->v.absmax[a] && delta[a] + VoxelSize + margin > ent->v.absmin[a];
				overlap |= inside;
			}
		}
		std::string error;
		if (overlap)
			Con_Printf ("Placement overlaps an entity\n");
		else
		{
			const uint64_t reservation = nextReservation++;
			const bool	   queued = serverWorld->EditSurfaceAsync (
				target, remove ? Cell{} : Cell{uint16_t (material), Solid},
				[reservation, remove] (bool success, uint64_t, const std::string &error)
				{
					placements.erase (reservation);
					if (success)
						Con_Printf ("Voxel %s\n", remove ? "removed" : "placed");
					else
						Con_Printf ("Edit failed: %s\n", error.c_str ());
				},
				&error);
			if (!queued)
				Con_Printf ("Edit failed: %s\n", error.c_str ());
			else if (!remove)
			{
				for (float &axis : target.local)
					axis = floorf (axis / VoxelSize) * VoxelSize;
				placements.emplace (reservation, target);
			}
		}
	}
	else
		Con_Printf ("No loaded terrain within reach\n");
	PR_SwitchQCVM (nullptr);
	PR_SwitchQCVM (previous);
}
} // namespace

void OW_Init ()
{
	OW_PagingInit ();
	R_OctreeInit ();
	Cvar_RegisterVariable (&world_cache_chunks);
	Cvar_RegisterVariable (&world_view_chunks);
	Cvar_RegisterVariable (&world_editing);
	Cvar_RegisterVariable (&world_peer_nat);
	Cvar_RegisterVariable (&world_peer_stun);
	Cvar_RegisterVariable (&world_cache_mb);
	Cmd_AddCommand ("world_create", WorldCreate_f);
	Cmd_AddCommand ("world_load", WorldLoad_f);
	Cmd_AddCommand ("world_save", WorldSave_f);
	Cmd_AddCommand ("world_snapshot", WorldSnapshot_f);
	Cmd_AddCommand ("world_stats", WorldStats_f);
	Cmd_AddCommand ("world_teleport", WorldTeleport_f);
	Cmd_AddCommand ("world_brush", WorldBrush_f);
	Cmd_AddCommand ("world_import", WorldImport_f);
	Cmd_AddCommand ("world_remove", WorldEdit_f);
	Cmd_AddCommand ("world_place", WorldEdit_f);
}
void OW_ClientDisconnect ()
{
	if (!remoteWorld)
		return;
	peerService.Reset ();
	signalReassembly.Reset ();
	outboundSignals.clear ();
	outboundSignalBytes = 0;
	localPeerId = 0;
	nextSignalId = 1;
	clientPeerSession = {};
	peerNATFailed = false;
	peerNATConfigured = false;
	advertisedCapabilities = 255;
	peerEndpoints.clear ();
	peerCache.clear ();
	peerCacheBytes = 0;
	peerRejectedObjects = 0;
	listenerAdvertised = false;
	reassembly.Reset ();
	transferRoots.clear ();
	offeredRoots.clear ();
	remoteRoots.clear ();
	clientProtocol = false;
}
void OW_Clear ()
{
	OW_PagingReset ();
	if (!isDedicated && vulkan_globals.device)
	{
		GL_WaitForDeviceIdle ();
		R_OctreeReset ();
	}
	peerService.Reset ();
	signalReassembly.Reset ();
	outboundSignals.clear ();
	outboundSignalBytes = 0;
	localPeerId = 0;
	nextSignalId = 1;
	clientPeerSession = {};
	peerNATFailed = false;
	peerNATConfigured = false;
	advertisedCapabilities = 255;
	peerEndpoints.clear ();
	peerCache.clear ();
	peerCacheBytes = 0;
	peerRejectedObjects = 0;
	listenerAdvertised = false;
	sessionPeerToken = {};
	sessionPeerId = {};
	sessionPeerTokenTime = -1000;
	remoteAuthority = {};
	remoteRoots.clear ();
	playerPhysics.clear ();
	serverPhysics.reset ();
	remotePhysics.reset ();
	serverWorld.reset ();
	remoteWorld.reset ();
	OW_QCReset ();
	positions.clear ();
	transfers.clear ();
	placements.clear ();
	reassembly.Reset ();
	transferRoots.clear ();
	offeredRoots.clear ();
	serverFrame = {};
	clientFrame = {};
	clientProtocol = false;
	nextEditRequest = 1;
}
bool OW_Active ()
{
	return serverWorld || remoteWorld;
}
bool OW_ServerActive ()
{
	return serverWorld != nullptr;
}
bool OW_ClientProtocol ()
{
	return clientProtocol;
}
World *OW_GetWorld ()
{
	return serverWorld ? serverWorld.get () : remoteWorld.get ();
}
bool OW_IsEmptyMap ()
{
	return !strcmp (sv.name, "__empty");
}
WorldPosition OW_GetRenderOrigin ()
{
	return clientFrame;
}
WorldPosition OW_GetSimulationOrigin ()
{
	return serverFrame;
}
WorldPosition OW_GetRenderPosition ()
{
	return FromLocal (r_refdef.vieworg, clientFrame);
}
void OW_ServerLoad (const char *map)
{
	if (strcmp (map, "__octree"))
		return;
	WorldConfig config = pending ? pendingConfig : WorldConfig{};
	pending = false;
	config.generationProgress = [last = uint32_t (-1)] (uint32_t done, uint32_t total) mutable
	{
		if (!isDedicated)
			Sys_SendKeyEvents ();
		if (total)
		{
			uint32_t percent = done * 100 / total;
			if (percent != last)
			{
				Con_Printf ("Preparing planet: %u%%\n", percent);
				last = percent;
			}
		}
	};
	config.residentLimit = size_t (CLAMP (std::max (128, svs.maxclients * 32), int (world_cache_chunks.value), 8192));
	config.residentBytes = size_t (CLAMP (64, int (world_cache_mb.value), 4096)) * 1024 * 1024;
	try
	{
		Con_Printf ("Preparing world generation and loading cached terrain...\n");
		playerPhysics.clear ();
		serverPhysics.reset ();
		serverWorld = std::make_unique<World> (config);
	}
	catch (const std::exception &error)
	{
		Host_Error ("World open: %s", error.what ());
	}
	positions.resize (qcvm->max_edicts);
	transfers.resize (svs.maxclients);
	RequireAround (*serverWorld, WorldPosition{{}, {128, 128, 256}});
	Con_Printf ("Octree world: 1024-unit chunks, 8-unit cells, %zu chunk cache\n", config.residentLimit);
}
void OW_PrepareRestart ()
{
	if (serverWorld && !serverWorld->Config ().databasePath.empty ())
	{
		pendingConfig = serverWorld->Config ();
		pending = true;
	}
}
void OW_ClientConnected (client_t *client)
{
	if (!serverWorld)
		return;
	transfers[client - svs.clients] = {};
	transfers[client - svs.clients].generation = nextConnection++;
	for (auto &transfer : transfers)
		transfer.peerListTime = -1000;
	if (sv.loadgame)
	{
		OW_BeginEntity (client->edict);
		return;
	}
	SelectFrame ({});
	// SelectFrame synchronizes existing edicts before changing coordinates. Reset
	// the reused slot afterwards so its previous body cannot supply a far origin
	// or leave old position handles and collision links on the new connection.
	SV_UnlinkEdict (client->edict);
	OW_ForgetEntity (client->edict);
	VectorCopy (vec3_origin, client->edict->v.origin);
	VectorCopy (vec3_origin, client->edict->v.oldorigin);
	client->edict->lastthink = 0;
	auto &slot = positions[client - svs.clients + 1];
	slot.known = true;
	slot.projected = true;
}
void OW_SyncEntities ()
{
	if (!serverWorld || qcvm != &sv.qcvm || !qcvm->edicts)
		return;
	for (int i = 1; i < qcvm->num_edicts; ++i)
	{
		edict_t *ent = EDICT_NUM (i);
		auto	&slot = positions[i];
		if (ent->free)
		{
			slot = {};
			continue;
		}
		if (!slot.known || slot.projected)
		{
			slot.position = FromLocal (ent->v.origin, serverFrame);
			if (!Add (serverFrame, {ent->v.oldorigin[0], ent->v.oldorigin[1], ent->v.oldorigin[2]}, slot.oldPosition))
				slot.oldPosition = slot.position;
			slot.hasPrediction =
				ent->lastthink != 0 && Add (serverFrame, {ent->predthinkpos[0], ent->predthinkpos[1], ent->predthinkpos[2]}, slot.predictedPosition);
			slot.known = true;
			slot.projected = true;
		}
	}
}
void OW_ForgetEntity (edict_t *ent)
{
	OW_PagingForget (ent);
	OW_QCForgetEntity (ent);
	if (serverWorld && qcvm == &sv.qcvm)
	{
		playerPhysics.erase (NUM_FOR_EDICT (ent));
		positions[NUM_FOR_EDICT (ent)] = {};
	}
}
bool OW_EntityVisible (edict_t *ent)
{
	return !serverWorld || qcvm != &sv.qcvm || !positions[NUM_FOR_EDICT (ent)].known || positions[NUM_FOR_EDICT (ent)].projected;
}
bool OW_GetEntityPosition (edict_t *ent, WorldPosition &position)
{
	if (!serverWorld || qcvm != &sv.qcvm)
		return false;
	if (ent == qcvm->edicts)
	{
		position = {};
		return true;
	}
	auto &slot = positions[NUM_FOR_EDICT (ent)];
	if (!slot.known || slot.projected)
	{
		slot.position = FromLocal (ent->v.origin, serverFrame);
		slot.known = true;
		slot.projected = true;
	}
	position = slot.position;
	return true;
}
bool OW_SetEntityPosition (edict_t *ent, const WorldPosition &position)
{
	if (!serverWorld || qcvm != &sv.qcvm)
		return false;
	WorldPosition normalized = position;
	if (!Normalize (normalized))
		return false;
	if (auto found = playerPhysics.find (NUM_FOR_EDICT (ent)); found != playerPhysics.end ())
		found->second->character.Reset ();
	OW_SyncEntities ();
	auto &slot = positions[NUM_FOR_EDICT (ent)];
	slot.position = slot.oldPosition = normalized;
	slot.known = true;
	slot.hasPrediction = false;
	ent->lastthink = 0;
	std::array<double, 3> delta;
	slot.projected = EngineDelta (normalized, serverFrame, delta, 8192);
	if (slot.projected)
	{
		for (int a = 0; a < 3; ++a)
			ent->v.origin[a] = float (delta[a]);
		VectorCopy (ent->v.origin, ent->v.oldorigin);
		SV_LinkEdict (ent, false);
	}
	else
		SV_UnlinkEdict (ent);
	return true;
}
bool OW_RestoreEntityPositions (const std::vector<WorldPosition> &saved, const std::vector<uint8_t> &live)
{
	if (!serverWorld || qcvm != &sv.qcvm || sv.state != ss_loading || saved.size () != size_t (qcvm->num_edicts) || live.size () != saved.size ())
		return false;
	for (size_t i = 1; i < saved.size (); ++i)
	{
		WorldPosition normalized = saved[i];
		if (live[i] && !Normalize (normalized))
			return false;
	}
	for (size_t i = 1; i < saved.size (); ++i)
	{
		auto &slot = positions[i];
		slot = {};
		if (!live[i])
			continue;
		edict_t *entity = EDICT_NUM (int (i));
		slot.position = saved[i];
		Normalize (slot.position);
		slot.oldPosition = slot.position;
		slot.known = true;
		std::array<double, 3> delta;
		slot.projected = EngineDelta (slot.position, serverFrame, delta, 8192);
		entity->lastthink = 0;
		if (!slot.projected)
			continue;
		for (size_t axis = 0; axis < 3; ++axis)
			entity->v.origin[axis] = float (delta[axis]);
		VectorCopy (entity->v.origin, entity->v.oldorigin);
		SV_LinkEdict (entity, false);
	}
	return true;
}
bool OW_BeginEntity (edict_t *ent)
{
	if (!serverWorld || qcvm != &sv.qcvm || ent == qcvm->edicts)
		return true;
	OW_SyncEntities ();
	const int index = NUM_FOR_EDICT (ent);
	if (index > svs.maxclients && !NearPlayer (positions[index].position))
		return false;
	SelectFrame (positions[index].position);
	return true;
}
void OW_ServerFrame ()
{
	if (!serverWorld)
		return;
	OW_SyncEntities ();
	serverWorld->Pump (64);
	OW_PagingFrame ();
	std::set<ChunkKey, KeyLess> keys;
	for (int i = 0; i < svs.maxclients; ++i)
		if (svs.clients[i].active && positions[i + 1].known)
			AddNeighbors (keys, positions[i + 1].position.chunk, 1);
	if (!isDedicated && cl.worldmodel)
		AddNeighbors (keys, OW_GetRenderPosition ().chunk, CLAMP (1, int (world_view_chunks.value), 3));
	std::vector<ChunkKey> interest (keys.begin (), keys.end ());
	if (serverWorld->SetInterest (interest))
	{
		// Admit the chunks nearest a player first. Lexicographic order could
		// spend the small pending-job budget on the back edge of the view range.
		auto distance = [&] (ChunkKey key)
		{
			double nearest = std::numeric_limits<double>::max ();
			for (int i = 0; i < svs.maxclients; ++i)
				if (svs.clients[i].active && positions[i + 1].known)
				{
					std::array<double, 3> delta;
					if (WorldDelta (serverWorld->Config (), {key, {512, 512, 512}}, positions[i + 1].position, delta))
					{
						double squared = 0;
						for (double component : delta)
						{
							const double gap = std::max (0.0, std::abs (component) - 512);
							squared += gap * gap;
						}
						nearest = std::min (nearest, squared);
					}
				}
			return nearest;
		};
		std::stable_sort (interest.begin (), interest.end (), [&] (ChunkKey a, ChunkKey b) { return distance (a) < distance (b); });
		for (const auto &key : interest)
			serverWorld->Request (key);
	}
	else
		Con_DPrintf ("World interest exceeds cache budget (%zu chunks)\n", interest.size ());
}
static void OW_QueuePeerSignal (uint64_t peerId, const std::vector<uint8_t> &data)
{
	if (!localPeerId || !clientProtocol || data.empty () || data.size () > wire::MaxSignalBytes ||
		std::none_of (peerEndpoints.begin (), peerEndpoints.end (), [peerId] (const auto &peer) { return peer.peerId == peerId && peer.nat; }))
		return;
	const size_t count = (data.size () + wire::SignalFragmentBytes - 1) / wire::SignalFragmentBytes;
	if (outboundSignals.size () + count > 64 || outboundSignalBytes + data.size () + count * 38 > 65536 || !nextSignalId)
		return;
	const uint64_t id = nextSignalId++;
	for (size_t offset = 0; offset < data.size (); offset += wire::SignalFragmentBytes)
	{
		wire::SignalFragment fragment{remoteAuthority.epoch, peerId, id, uint16_t (data.size ()), uint16_t (offset), {}};
		const size_t		 count = std::min (wire::SignalFragmentBytes, data.size () - offset);
		fragment.payload.assign (data.begin () + offset, data.begin () + offset + count);
		auto bytes = wire::EncodeSignalFragment (fragment);
		outboundSignalBytes += bytes.size ();
		outboundSignals.push_back ({std::move (bytes), realtime});
	}
}
void OW_ClientFrame ()
{
	if (!remoteWorld)
		return;
	if (!world_peer_nat.value && (peerNATConfigured || peerNATFailed))
	{
		peerService.DisableNAT ();
		peerNATConfigured = false;
		peerNATFailed = false;
		signalReassembly.Reset ();
		outboundSignals.clear ();
		outboundSignalBytes = 0;
	}
	peerService.Pump ();
	const uint8_t capabilities = world_peer_nat.value && !peerNATFailed && peer::Service::NATAvailable () ? 1 : 0;
	if ((!listenerAdvertised || advertisedCapabilities != capabilities) && cls.signon == SIGNONS && cls.message.cursize + 7 <= cls.message.maxsize)
	{
		MSG_WriteByte (&cls.message, clc_quiver_world);
		MSG_WriteShort (&cls.message, 4);
		MSG_WriteByte (&cls.message, 0xa1);
		MSG_WriteShort (&cls.message, peerService.Port ());
		MSG_WriteByte (&cls.message, capabilities);
		listenerAdvertised = true;
		advertisedCapabilities = capabilities;
	}
	signalReassembly.Expire (uint64_t (Sys_DoubleTime () * 1000));
	while (!outboundSignals.empty () && (realtime < outboundSignals.front ().queuedAt || realtime - outboundSignals.front ().queuedAt > 5))
	{
		outboundSignalBytes -= outboundSignals.front ().bytes.size ();
		outboundSignals.pop_front ();
	}
	if (!outboundSignals.empty () && cls.signon == SIGNONS && cls.message.cursize <= 128)
	{
		const auto &bytes = outboundSignals.front ().bytes;
		if (cls.message.cursize + int (bytes.size ()) + 4 <= cls.message.maxsize)
		{
			MSG_WriteByte (&cls.message, clc_quiver_world);
			MSG_WriteShort (&cls.message, int (bytes.size ()) + 1);
			MSG_WriteByte (&cls.message, 0xa3);
			SZ_Write (&cls.message, bytes.data (), int (bytes.size ()));
			outboundSignalBytes -= bytes.size ();
			outboundSignals.pop_front ();
		}
	}
	remoteWorld->Pump (64);
	std::set<ChunkKey, KeyLess> keys;
	AddNeighbors (keys, OW_GetRenderPosition ().chunk, CLAMP (1, int (world_view_chunks.value), 3));
	std::vector<ChunkKey> interest (keys.begin (), keys.end ());
	remoteWorld->SetInterest (interest); // Authoritative snapshots arrive from the server; never guess edited terrain.
}
bool OW_MovePlayer (edict_t *ent, float deltaTime, bool allowStairs, bool stickToFloor, std::vector<int> &touched)
{
	if (!serverWorld || qcvm != &sv.qcvm)
		return false;
	touched.clear ();
	const int id = NUM_FOR_EDICT (ent);
	try
	{
		auto &state = playerPhysics[id];
		if (!state)
			state = std::make_unique<PlayerPhysics> (*serverWorld, uint64_t (id));
		JoltCharacterInput input;
		input.position = FromLocal (ent->v.origin, serverFrame);
		input.deltaTime = deltaTime;
		input.grounded = ((int)ent->v.flags & FL_ONGROUND) != 0;
		input.allowStairs = allowStairs;
		input.stickToFloor = stickToFloor;
		const eval_t *gravity = GetEdictFieldValue (ent, ED_FindFieldOffset ("gravity"));
		const float	  gravityScale = gravity && gravity->_float ? gravity->_float : 1.0f;
		input.gravity = {0, 0, -gravityScale * float (Cvar_VariableValue ("sv_gravity"))};
		std::array<double, 3> minimum, maximum;
		const double		  slopeTravel = input.grounded ? 1.1 * std::hypot (ent->v.velocity[0], ent->v.velocity[1]) * deltaTime : 0;
		for (int a = 0; a < 3; ++a)
		{
			input.mins[a] = ent->v.mins[a];
			input.maxs[a] = ent->v.maxs[a];
			input.velocity[a] = ent->v.velocity[a];
			const double margin = 64 + (a == 2 ? slopeTravel : 0);
			minimum[a] = ent->v.origin[a] + ent->v.mins[a] + std::min (0.0f, ent->v.velocity[a] * deltaTime) - margin;
			maximum[a] = ent->v.origin[a] + ent->v.maxs[a] + std::max (0.0f, ent->v.velocity[a] * deltaTime) + margin;
		}
		std::vector<JoltEntityProxy> proxies;
		std::vector<edict_t *>		 brushEntities;
		for (int i = 1; i < qcvm->num_edicts; ++i)
		{
			auto other = EDICT_NUM (i);
			if (other == ent || other->free || !OW_EntityVisible (other) || other->v.solid == (float)SOLID_NOT || other->v.solid == (float)SOLID_TRIGGER ||
				(ent->v.size[0] && !other->v.size[0]) || PROG_TO_EDICT (other->v.owner) == ent || PROG_TO_EDICT (ent->v.owner) == other)
				continue;
			if (other->v.skin < 0 && !(CONTENTMASK_ANYSOLID & (1u << -(int)other->v.skin)))
				continue;
			bool nearby = true;
			for (int a = 0; a < 3; ++a)
				nearby &= minimum[a] <= other->v.absmax[a] && maximum[a] >= other->v.absmin[a];
			if (!nearby)
				continue;
			if (other->v.solid == (float)SOLID_BSP)
			{
				brushEntities.push_back (other);
				continue;
			}
			JoltEntityProxy proxy;
			proxy.id = uint64_t (i);
			proxy.position = FromLocal (other->v.origin, serverFrame);
			for (int a = 0; a < 3; ++a)
			{
				proxy.mins[a] = other->v.mins[a];
				proxy.maxs[a] = other->v.maxs[a];
				proxy.velocity[a] = other->v.movetype == (float)MOVETYPE_PUSH ? 0 : other->v.velocity[a];
			}
			proxies.push_back (proxy);
		}
		// Durable edits reserve their cells before publication. Mirror the same
		// temporary barriers used by ordinary server traces into the controller.
		for (const auto &[reservation, lower] : placements)
		{
			std::array<double, 3> local;
			if (!EngineDelta (lower, serverFrame, local, 65536))
				continue;
			const float margin = serverWorld->Config ().generator != Generator::Empty ? VoxelSize / 2 : 0;
			bool		nearby = true;
			for (int a = 0; a < 3; ++a)
				nearby &= minimum[a] <= local[a] + VoxelSize + margin && maximum[a] >= local[a] - margin;
			if (nearby)
				proxies.push_back (
					{uint64_t (1) << 63 | reservation, lower, {-margin, -margin, -margin}, {VoxelSize + margin, VoxelSize + margin, VoxelSize + margin}, {}});
		}
		state->world.SetEntityProxies (std::move (proxies));
		auto result = state->character.Step (input);
		if (result.pending)
			return false;
		std::array<double, 3> local;
		if (!EngineDelta (result.position, serverFrame, local, 65536))
			throw std::runtime_error ("Jolt player left the local execution frame");
		vec3_t start, end;
		VectorCopy (ent->v.origin, start);
		for (int a = 0; a < 3; ++a)
		{
			end[a] = float (local[a]);
			ent->v.velocity[a] = result.velocity[a];
		}
		ent->v.flags = (int)ent->v.flags & ~FL_ONGROUND;
		if (result.grounded)
		{
			ent->v.flags = (int)ent->v.flags | FL_ONGROUND;
			const int ground = result.groundUserData < uint64_t (qcvm->num_edicts) ? int (result.groundUserData) : 0;
			ent->v.groundentity = EDICT_TO_PROG (EDICT_NUM (ground));
		}
		// Brush-model entities retain their exact Quake hulls. They are not
		// approximated by their bounding boxes in the streamed Jolt scene.
		trace_t closest{};
		closest.fraction = 1;
		for (auto brush : brushEntities)
		{
			auto trace = SV_ClipMoveToEntity (brush, start, ent->v.mins, ent->v.maxs, end, CONTENTMASK_ANYSOLID);
			if (trace.allsolid || trace.fraction < closest.fraction)
			{
				closest = trace;
				closest.ent = brush;
			}
		}
		if (closest.ent)
		{
			bool   hitWall = false;
			vec3_t requested, desired, current, initialVelocity;
			VectorCopy (end, requested);
			VectorCopy (end, desired);
			VectorCopy (start, current);
			VectorCopy (ent->v.velocity, initialVelocity);
			// Finish the unspent displacement along the hull. A downward floor
			// contact at fraction zero must not cancel horizontal walking.
			for (int bump = 0; bump < 4; ++bump)
			{
				const auto trace = bump == 0 ? closest : SV_Move (current, ent->v.mins, ent->v.maxs, desired, MOVE_NORMAL, ent);
				if (trace.allsolid)
				{
					VectorCopy (current, end);
					VectorCopy (vec3_origin, ent->v.velocity);
					break;
				}
				VectorCopy (trace.endpos, end);
				if (trace.fraction == 1)
					break;
				hitWall |= trace.plane.normal[2] < .7f;
				if (trace.ent)
				{
					touched.push_back (NUM_FOR_EDICT (trace.ent));
					if (trace.plane.normal[2] > .7f)
					{
						ent->v.flags = (int)ent->v.flags | FL_ONGROUND;
						ent->v.groundentity = EDICT_TO_PROG (trace.ent);
					}
				}
				const float into = DotProduct (ent->v.velocity, trace.plane.normal);
				if (into < 0)
					VectorMA (ent->v.velocity, -into, trace.plane.normal, ent->v.velocity);
				vec3_t remaining;
				VectorSubtract (desired, end, remaining);
				const float inward = DotProduct (remaining, trace.plane.normal);
				if (inward < 0)
					VectorMA (remaining, -inward, trace.plane.normal, remaining);
				if (DotProduct (remaining, remaining) < 0.000001f)
					break;
				VectorCopy (end, current);
				VectorAdd (current, remaining, desired);
			}
			// Brush-model stairs need exact hull sweeps because their detailed
			// surfaces are not present in CharacterVirtual's terrain scene.
			if (!closest.allsolid && hitWall && input.grounded && allowStairs)
			{
				vec3_t raised, forward, lowered;
				VectorCopy (start, raised);
				raised[2] += 18;
				auto up = SV_Move (start, ent->v.mins, ent->v.maxs, raised, MOVE_NORMAL, ent);
				if (!up.startsolid && up.fraction == 1)
				{
					VectorCopy (requested, forward);
					forward[2] = raised[2] + std::max (0.0f, requested[2] - start[2]);
					auto step = SV_Move (raised, ent->v.mins, ent->v.maxs, forward, MOVE_NORMAL, ent);
					VectorCopy (step.endpos, lowered);
					lowered[2] -= 18;
					auto		down = SV_Move (step.endpos, ent->v.mins, ent->v.maxs, lowered, MOVE_NORMAL, ent);
					const float flatDistance = (end[0] - start[0]) * (end[0] - start[0]) + (end[1] - start[1]) * (end[1] - start[1]);
					const float stepDistance =
						(down.endpos[0] - start[0]) * (down.endpos[0] - start[0]) + (down.endpos[1] - start[1]) * (down.endpos[1] - start[1]);
					if (!step.startsolid && !down.startsolid && down.fraction < 1 && down.plane.normal[2] > .7f && stepDistance > flatDistance)
					{
						VectorCopy (down.endpos, end);
						VectorCopy (initialVelocity, ent->v.velocity);
						ent->v.velocity[2] = 0;
						ent->v.flags = (int)ent->v.flags | FL_ONGROUND;
						ent->v.groundentity = EDICT_TO_PROG (down.ent);
						touched.push_back (NUM_FOR_EDICT (down.ent));
						if (step.ent)
							touched.push_back (NUM_FOR_EDICT (step.ent));
					}
				}
			}
		}
		VectorCopy (end, ent->v.origin);
		for (uint64_t contact : result.touchedUserData)
			if (contact < uint64_t (qcvm->num_edicts) && int (contact) != id)
			{
				// Jolt's proposal may extend beyond a brush that subsequently
				// clipped it. Do not deliver touches through that brush.
				if (closest.ent && contact != 0)
				{
					auto reached = SV_ClipMoveToEntity (EDICT_NUM (int (contact)), start, ent->v.mins, ent->v.maxs, end, CONTENTMASK_ANYSOLID);
					if (!reached.startsolid && reached.fraction == 1)
						continue;
				}
				touched.push_back (int (contact));
			}
		std::sort (touched.begin (), touched.end ());
		touched.erase (std::unique (touched.begin (), touched.end ()), touched.end ());
	}
	catch (const std::exception &error)
	{
		VectorCopy (vec3_origin, ent->v.velocity);
		Con_DPrintf ("Jolt player update stopped: %s\n", error.what ());
	}
	return true;
}

quiver::TraceResult
OW_TraceTerrain (const quiver::WorldPosition &start, const std::array<double, 3> &offset, const std::array<float, 3> &mins, const std::array<float, 3> &maxs)
{
	World *world = qcvm == &sv.qcvm ? serverWorld.get () : OW_GetWorld ();
	if (world)
		return QueryPhysics (*world).Trace (start, offset, mins, maxs);
	TraceResult result;
	result.end = start;
	result.fraction = 0;
	result.startSolid = result.allSolid = result.hitUnknown = true;
	result.cell = {0, Unknown};
	return result;
}

bool OW_ClipMove (const float *start, const float *mins, const float *maxs, const float *end, trace_t *trace)
{
	World *world = qcvm == &sv.qcvm ? serverWorld.get () : OW_GetWorld ();
	if (!world)
		return false;
	const WorldPosition &frame = qcvm == &sv.qcvm ? serverFrame : clientFrame;
	auto				 hit = QueryPhysics (*world).Trace (
		FromLocal (start, frame), {double (end[0]) - start[0], double (end[1]) - start[1], double (end[2]) - start[2]}, {mins[0], mins[1], mins[2]},
		{maxs[0], maxs[1], maxs[2]});
	memset (trace, 0, sizeof (*trace));
	trace->fraction = float (hit.fraction);
	trace->startsolid = hit.startSolid;
	trace->allsolid = hit.allSolid;
	trace->inopen = !hit.startSolid;
	trace->contents = hit.cell.contents == Unknown ? CONTENTS_SOLID : hit.cell.contents;
	for (int a = 0; a < 3; ++a)
	{
		trace->endpos[a] = float (start[a] + (double (end[a]) - start[a]) * hit.fraction);
		trace->plane.normal[a] = hit.normal[a];
	}
	trace->plane.dist = DotProduct (trace->endpos, trace->plane.normal);
	if (hit.fraction < 1 || hit.startSolid)
		trace->ent = qcvm->edicts;
	// Reserve accepted placements while their durable transaction is in flight.
	// A player cannot enter an initially clear cell between validation and commit.
	if (qcvm == &sv.qcvm)
		for (const auto &[id, lower] : placements)
		{
			const double		  margin = world->Config ().generator != Generator::Empty ? VoxelSize / 2 : 0;
			std::array<double, 3> box;
			if (!EngineDelta (lower, frame, box, 65536))
				continue;
			double entry = -1e100, exit = 1e100;
			int	   axis = -1;
			float  sign = 0;
			bool   inside = true, endInside = true, miss = false;
			for (int a = 0; a < 3; ++a)
			{
				const double lo = box[a] - margin - maxs[a], hi = box[a] + VoxelSize + margin - mins[a];
				const double velocity = double (end[a]) - start[a];
				const bool	 pointAxis = mins[a] == maxs[a];
				inside &= pointAxis ? start[a] >= lo && start[a] <= hi : start[a] > lo && start[a] < hi;
				endInside &= pointAxis ? end[a] >= lo && end[a] <= hi : end[a] > lo && end[a] < hi;
				if (!velocity)
				{
					if (pointAxis ? start[a] < lo || start[a] > hi : start[a] <= lo || start[a] >= hi)
					{
						miss = true;
						break;
					}
					continue;
				}
				double first = (lo - start[a]) / velocity, last = (hi - start[a]) / velocity;
				if (first > last)
					std::swap (first, last);
				if (first > entry)
				{
					entry = first;
					axis = a;
					sign = velocity > 0 ? -1 : 1;
				}
				exit = std::min (exit, last);
			}
			if (miss)
				continue;
			if (inside)
			{
				trace->startsolid = true;
				trace->allsolid |= endInside;
				trace->inopen = false;
				trace->fraction = 0;
				memset (&trace->plane, 0, sizeof (trace->plane));
				VectorCopy (start, trace->endpos);
				trace->ent = qcvm->edicts;
				trace->contents = CONTENTS_SOLID;
			}
			else if (axis >= 0 && entry >= 0 && entry < exit && entry <= 1)
			{
				const double fraction = std::max (0.0, entry - DIST_EPSILON / fabs (double (end[axis]) - start[axis]));
				if (fraction >= trace->fraction)
					continue;
				trace->fraction = float (fraction);
				VectorCopy (vec3_origin, trace->plane.normal);
				trace->plane.normal[axis] = sign;
				for (int a = 0; a < 3; ++a)
					trace->endpos[a] = float (start[a] + (double (end[a]) - start[a]) * fraction);
				trace->plane.dist = DotProduct (trace->endpos, trace->plane.normal);
				trace->ent = qcvm->edicts;
				trace->contents = CONTENTS_SOLID;
			}
		}
	return true;
}
int OW_PointContents (const float *point)
{
	World *world = qcvm == &sv.qcvm ? serverWorld.get () : OW_GetWorld ();
	if (!world)
		return CONTENTS_EMPTY;
	const WorldPosition position = FromLocal (point, qcvm == &sv.qcvm ? serverFrame : clientFrame);
	if (qcvm == &sv.qcvm)
		for (const auto &[id, lower] : placements)
		{
			std::array<double, 3> relative;
			if (!EngineDelta (position, lower, relative, ChunkSize))
				continue;
			const double margin = world->Config ().generator != Generator::Empty ? VoxelSize / 2 : 0;
			bool		 inside = true;
			for (int a = 0; a < 3; ++a)
				inside &= relative[a] >= -margin && relative[a] < VoxelSize + margin;
			if (inside)
				return CONTENTS_SOLID;
		}
	Cell cell = world->CollisionContentsCached (position);
	return cell.contents == Unknown ? CONTENTS_SOLID : cell.contents;
}
void OW_WriteServerInfo (sizebuf_t *message)
{
	if (!serverWorld)
		return;
	MSG_WriteLong (message, PROTOCOL_QUIVER_WORLD);
	wire::Config config{serverWorld->Config ().seed, serverWorld->Config ().generator};
	config.descriptor = serverWorld->DescriptorBytes ();
	config.root = serverWorld->CurrentRoot ();
	config.epoch = serverWorld->AuthorityEpoch ();
	auto bytes = wire::EncodeConfig (config);
	MSG_WriteShort (message, int (bytes.size ()));
	SZ_Write (message, bytes.data (), int (bytes.size ()));
}
void OW_ReadServerInfo ()
{
	const int	 length = (unsigned short)MSG_ReadShort ();
	wire::Config config;
	if (!wire::DecodeConfig (ReadBytes (length), config))
		Host_Error ("Unsupported or malformed Quiver world protocol");
	remoteAuthority = {config.root, config.epoch};
	remoteRoots = {config.root};
	clientProtocol = true;
	clientFrame = {};
	reassembly.Reset ();
	transferRoots.clear ();
	offeredRoots.clear ();
	if (!serverWorld)
	{
		WorldConfig worldConfig;
		worldConfig.seed = config.seed;
		worldConfig.generator = config.generator;
		std::string descriptorError;
		if (!ApplyWorldDescriptor (worldConfig, config.descriptor, &descriptorError) || worldConfig.seed != config.seed ||
			worldConfig.generator != config.generator)
			Host_Error ("World descriptor: %s", descriptorError.c_str ());
		worldConfig.residentLimit = size_t (CLAMP (128, int (world_cache_chunks.value), 8192));
		remotePhysics.reset ();
		remoteWorld = std::make_unique<World> (worldConfig);
		if (remoteWorld->DescriptorBytes () != config.descriptor)
			Host_Error ("World generator descriptor mismatch; update the client to match the server");
		peerService.Listen ();
		peerService.SetProvider (
			[] (const peer::Hash &hash, std::vector<uint8_t> &bytes)
			{
				auto it = peerCache.find (hash);
				if (it == peerCache.end ())
					return false;
				bytes = it->second;
				return true;
			});
	}
}
void OW_WriteFrame (client_t *client, sizebuf_t *message)
{
	if (!serverWorld)
		return;
	OW_BeginEntity (client->edict);
	WritePacket (message, 0, wire::EncodePosition (serverFrame));
}
static void OW_CachePeerObject (const ContentObject &object)
{
	auto bytes = wire::EncodeContentObject (object);
	if (bytes.empty () || bytes.size () > 1024 * 1024 || peerCache.contains (object.hash))
		return;
	while (!peerCache.empty () && peerCacheBytes + bytes.size () > 32 * 1024 * 1024)
	{
		peerCacheBytes -= peerCache.begin ()->second.size ();
		peerCache.erase (peerCache.begin ());
	}
	peerCacheBytes += bytes.size ();
	peerCache.emplace (object.hash, std::move (bytes));
}
static void OW_AcknowledgeProof (const TerrainProof &proof)
{
	wire::Writer writer (128);
	writer.U8 (0xa2);
	writer.Position ({proof.key, {}});
	writer.U64 (proof.revision);
	writer.Bytes (proof.overrideObject ? proof.overrideObject->hash : ContentHash{});
	if (cls.message.cursize + int (writer.Data ().size ()) + 3 > cls.message.maxsize)
		return;
	MSG_WriteByte (&cls.message, clc_quiver_world);
	MSG_WriteShort (&cls.message, int (writer.Data ().size ()));
	SZ_Write (&cls.message, writer.Data ().data (), int (writer.Data ().size ()));
}
static void OW_ReceiveProof (TerrainProof proof)
{
	if (!remoteWorld || !KnownRoot (proof.root))
		return;
	if (proof.overrideObject && proof.overrideObject->bytes.empty ())
	{
		// ICE negotiation may outlive the recent-root ring during unrelated edits.
		// Retain only roots already authenticated by a bounded server offer.
		if (!offeredRoots.contains (proof.root) && offeredRoots.size () >= 8)
			offeredRoots.erase (offeredRoots.begin ());
		offeredRoots[proof.root] = realtime;
		const auto hash = proof.overrideObject->hash;
		auto	   cached = peerCache.find (hash);
		if (cached != peerCache.end ())
		{
			ContentObject object;
			if (wire::DecodeContentObject (cached->second, object))
			{
				proof.overrideObject = std::move (object);
				OW_ReceiveProof (std::move (proof));
				return;
			}
		}
		if (peerEndpoints.empty ())
			return; // Reliable server fallback follows the offer.
		const uint64_t epoch = remoteAuthority.epoch;
		peerService.Fetch (
			peerEndpoints, hash,
			[proof = std::move (proof), epoch, hash] (bool success, std::vector<uint8_t> bytes) mutable
			{
				if (!success || !remoteWorld || epoch != remoteAuthority.epoch || !KnownRoot (proof.root))
					return;
				ContentObject object;
				if (!wire::DecodeContentObject (bytes, object) || object.hash != hash || object.type != ContentObjectType::TerrainOverride)
				{
					++peerRejectedObjects;
					Con_DPrintf ("Rejected peer terrain object; awaiting authoritative fallback\n");
					return;
				}
				proof.overrideObject = std::move (object);
				OW_ReceiveProof (std::move (proof));
			});
		return;
	}
	std::string error;
	if (!remoteWorld->InstallTerrainProof (proof, &error))
	{
		Con_DPrintf ("Rejected terrain proof: %s\n", error.c_str ());
		return;
	}
	if (proof.overrideObject)
		OW_CachePeerObject (*proof.overrideObject);
	for (const auto &asset : proof.assets)
		OW_CachePeerObject (asset);
	OW_AcknowledgeProof (proof);
}
void OW_ReadMessage ()
{
	if (!clientProtocol)
		Host_Error ("Quiver world message without protocol negotiation");
	const int  kind = MSG_ReadByte ();
	const int  length = (unsigned short)MSG_ReadShort ();
	const auto bytes = ReadBytes (length);
	if (kind == 0)
	{
		WorldPosition position;
		if (!wire::DecodePosition (bytes, position))
			Host_Error ("Invalid world frame");
		if (!(clientFrame.chunk == position.chunk))
		{
			std::array<double, 3> shift;
			const bool			  nearby = EngineDelta (clientFrame, position, shift, 65536);
			for (int i = 1; i < cl.num_entities; ++i)
			{
				entity_t *ent = cl.entities + i;
				if (nearby)
					for (int a = 0; a < 3; ++a)
					{
						ent->origin[a] += float (shift[a]);
						ent->msg_origins[0][a] += float (shift[a]);
						ent->msg_origins[1][a] += float (shift[a]);
						ent->previousorigin[a] += float (shift[a]);
						ent->currentorigin[a] += float (shift[a]);
					}
				ent->forcelink = true;
				ent->lerpflags |= LERP_RESETMOVE;
			}
			R_ClearParticles ();
			memset (cl_dlights, 0, sizeof (cl_dlights));
		}
		clientFrame = position;
	}
	else if (kind == 1)
	{
		wire::ChunkFragment fragment;
		if (!wire::DecodeFragment (bytes, fragment))
			Host_Error ("Invalid world chunk fragment");
		const uint64_t now = uint64_t (Sys_DoubleTime () * 1000);
		for (auto it = transferRoots.begin (); it != transferRoots.end ();)
			if (now < it->second.second || now - it->second.second >= 10000)
				it = transferRoots.erase (it);
			else
				++it;
		wire::CompletedChunk completed;
		auto				 result = reassembly.Receive (fragment, now, completed);
		if (result == wire::ReceiveResult::Rejected || result == wire::ReceiveResult::BudgetExceeded)
			Host_Error ("Invalid or oversized world chunk transfer");
		if (!fragment.offset && fragment.payload.size () >= 40 && result != wire::ReceiveResult::Duplicate)
		{
			ContentHash root;
			std::copy_n (fragment.payload.begin () + 8, root.size (), root.begin ());
			if (KnownRoot (root))
			{
				if (transferRoots.size () == 4)
					transferRoots.erase (transferRoots.begin ());
				transferRoots[fragment.transferId] = {root, now};
			}
		}
		if (result != wire::ReceiveResult::Duplicate)
			if (auto it = transferRoots.find (fragment.transferId); it != transferRoots.end ())
				it->second.second = now;
		if (result == wire::ReceiveResult::Complete && remoteWorld)
		{
			TerrainProof proof;
			if (!wire::DecodeTerrainProof (completed.bytes, proof) || proof.key != completed.key || proof.revision != completed.revision)
				Host_Error ("Invalid world Merkle proof data");
			if (auto it = transferRoots.find (fragment.transferId); it != transferRoots.end ())
			{
				if (it->second.first == proof.root && !KnownRoot (proof.root))
				{
					remoteRoots.push_back (proof.root);
					while (remoteRoots.size () > 64)
						remoteRoots.pop_front ();
				}
				transferRoots.erase (it);
			}
			OW_ReceiveProof (std::move (proof));
		}
	}
	else if (kind == 4)
	{
		wire::Edit edit;
		if (!wire::DecodeEdit (bytes, edit) || !edit.newRevision)
			Host_Error ("Invalid world edit acknowledgement");
		if (edit.epoch != remoteAuthority.epoch)
			return;
		// Installation waits for a proof anchored to the authoritative root.
		Con_Printf ("World edit %llu committed\n", (unsigned long long)edit.requestId);
	}
	else if (kind == 5)
	{
		wire::RootState state;
		if (!wire::DecodeRoot (bytes, state))
			Host_Error ("Invalid world root announcement");
		if (state.epoch < remoteAuthority.epoch)
			return;
		if (state.epoch != remoteAuthority.epoch)
			Host_Error ("World authority changed; reconnect to load the restored world");
		remoteAuthority = state;
		if (std::find (remoteRoots.begin (), remoteRoots.end (), state.root) == remoteRoots.end ())
			remoteRoots.push_back (state.root);
		while (remoteRoots.size () > 64)
			remoteRoots.pop_front ();
	}
	else if (kind == 7)
	{
		wire::PeerDirectory directory;
		if (!wire::DecodePeerDirectory (bytes, directory))
			Host_Error ("Invalid world peer directory");
		if (directory.epoch != remoteAuthority.epoch)
			return;
		std::vector<peer::Endpoint> endpoints;
		for (const auto &address : directory.peers)
			endpoints.push_back ({address.address, address.port, directory.token, address.peerId, address.nat});
		const bool newSession = localPeerId != directory.recipientId || clientPeerSession != directory.sessionId;
		if (newSession || (world_peer_nat.value && !peerNATConfigured && !peerNATFailed))
		{
			if (newSession)
			{
				peerService.DisableNAT ();
				peerService.RevokeAll ();
				peerNATConfigured = false;
				peerNATFailed = false;
			}
			signalReassembly.Reset ();
			outboundSignals.clear ();
			outboundSignalBytes = 0;
			if (newSession)
				nextSignalId = 1;
			localPeerId = directory.recipientId;
			clientPeerSession = directory.sessionId;
			if (world_peer_nat.value && peer::Service::NATAvailable ())
			{
				peerNATConfigured = peerService.ConfigureSession (localPeerId, clientPeerSession, directory.stunServers, OW_QueuePeerSignal);
				peerNATFailed = !peerNATConfigured;
				Con_DPrintf (
					"World peer NAT session %llu: %s\n", (unsigned long long)localPeerId,
					peerNATConfigured ? "configured" : "unavailable; using TCP/server fallback");
			}
		}
		peerService.Authorize (directory.token, 120);
		peerEndpoints = std::move (endpoints);
		peerService.SetPeers (peerEndpoints);
	}
	else if (kind == 8)
	{
		wire::SignalFragment fragment;
		if (!wire::DecodeSignalFragment (bytes, fragment))
			Host_Error ("Invalid world peer signal");
		if (fragment.epoch != remoteAuthority.epoch || !localPeerId || !world_peer_nat.value || peerNATFailed ||
			std::none_of (peerEndpoints.begin (), peerEndpoints.end (), [&] (const auto &peer) { return peer.peerId == fragment.peerId && peer.nat; }))
			return;
		wire::CompletedSignal signal;
		if (signalReassembly.Receive (fragment, uint64_t (Sys_DoubleTime () * 1000), signal) == wire::ReceiveResult::Complete)
			peerService.ReceiveSignal (signal.peerId, signal.bytes);
	}

	else if (kind == 2 || kind == 3)
	{
		wire::Reader  reader (bytes);
		WorldPosition position;
		if (!reader.Position (position))
			Host_Error ("Invalid world effect position");
		std::array<double, 3> relative{};
		const bool			  nearby = EngineDelta (position, clientFrame, relative, 8192);
		vec3_t				  point = {float (relative[0]), float (relative[1]), float (relative[2])};
		if (kind == 2)
		{
			uint16_t entity, sound;
			uint8_t	 channel, volume;
			float	 attenuation;
			if (!reader.U16 (entity) || !reader.U16 (sound) || !reader.U8 (channel) || !reader.U8 (volume) || !reader.F32 (attenuation) ||
				reader.Remaining () || entity >= cl.max_edicts || sound >= MAX_SOUNDS || attenuation < 0 || attenuation > 4)
				Host_Error ("Invalid world sound");
			if (nearby && cl.sound_precache[sound])
				S_StartSound (entity, channel, cl.sound_precache[sound], point, volume / 255.0f, attenuation);
		}
		else
		{
			vec3_t	 direction;
			uint8_t	 color;
			uint16_t count;
			if (!reader.F32 (direction[0]) || !reader.F32 (direction[1]) || !reader.F32 (direction[2]) || !reader.U8 (color) || !reader.U16 (count) ||
				reader.Remaining ())
				Host_Error ("Invalid world particles");
			if (nearby)
				R_RunParticleEffect (point, direction, color, count);
		}
	}
	else
		Host_Error ("Unknown Quiver world message %d", kind);
}

static void OW_SendEffect (int kind, const WorldPosition &position, const wire::Writer &writer)
{
	for (int i = 0; i < svs.maxclients; ++i)
	{
		client_t *client = svs.clients + i;
		if (!client->active || !client->spawned)
			continue;
		WorldPosition		  player;
		std::array<double, 3> distance;
		if (!OW_GetEntityPosition (client->edict, player) || !EngineDelta (position, player, distance, 4096))
			continue;
		if (client->datagram.cursize + int (writer.Data ().size ()) + 4 > client->datagram.maxsize)
			continue;
		WritePacket (&client->datagram, kind, writer.Data ());
	}
}
bool OW_StartSound (edict_t *entity, const float *origin, int channel, unsigned int sound, int volume, float attenuation)
{
	if (!serverWorld)
		return false;
	vec3_t point;
	for (int i = 0; i < 3; ++i)
		point[i] = origin ? origin[i] : entity->v.origin[i] + 0.5f * (entity->v.mins[i] + entity->v.maxs[i]);
	WorldPosition position = FromLocal (point, serverFrame);
	wire::Writer  writer;
	writer.Position (position);
	writer.U16 (NUM_FOR_EDICT (entity));
	writer.U16 (sound);
	writer.U8 (channel);
	writer.U8 (volume);
	writer.F32 (attenuation);
	OW_SendEffect (2, position, writer);
	return true;
}
bool OW_StartParticle (const float *origin, const float *direction, int color, int count)
{
	if (!serverWorld)
		return false;
	WorldPosition position = FromLocal (origin, serverFrame);
	wire::Writer  writer;
	writer.Position (position);
	for (int i = 0; i < 3; ++i)
		writer.F32 (direction[i]);
	writer.U8 (color);
	writer.U16 (count == 255 ? 1024 : CLAMP (0, count, 65535));
	OW_SendEffect (3, position, writer);
	return true;
}

bool OW_ReadEdit (client_t *client)
{
	if (!serverWorld || !client->spawned)
		return false;
	const int length = (unsigned short)MSG_ReadShort ();
	if (msg_badread || length > 551 || msg_readcount < 0 || length > net_message.cursize - msg_readcount)
		return false;
	const auto packet = ReadBytes (length);
	auto	  &state = transfers[client - svs.clients];
	if ((packet.size () == 3 || packet.size () == 4) && packet[0] == 0xa1)
	{
		state.peerPort = uint16_t (packet[1]) | (uint16_t (packet[2]) << 8);
		if (packet.size () == 4 && packet[3] > 1)
			return false;
		state.peerNAT = packet.size () == 4 && packet[3] == 1;
		for (auto &transfer : transfers)
			transfer.peerListTime = -1000;
		return true;
	}
	if (!packet.empty () && packet[0] == 0xa3)
	{
		wire::SignalFragment fragment;
		if (!wire::DecodeSignalFragment (std::span (packet).subspan (1), fragment))
			return false;
		if (!state.peerNAT || fragment.epoch != serverWorld->AuthorityEpoch () || !state.introducedPeers.contains (fragment.peerId))
			return true;
		Transfer *target = nullptr;
		for (size_t i = 0; i < transfers.size (); ++i)
			if (transfers[i].generation == fragment.peerId && transfers[i].peerNAT && transfers[i].introducedPeers.contains (state.generation) &&
				svs.clients[i].active && svs.clients[i].spawned && svs.clients[i].netconnection)
			{
				target = &transfers[i];
				break;
			}
		if (!target || target == &state)
			return true;
		const double elapsed = std::clamp (realtime - state.signalBudgetTime, 0.0, 2.0);
		state.signalBudgetTime = realtime;
		state.signalBudgetBytes = std::min (32768.0, state.signalBudgetBytes + elapsed * 16384);
		state.signalBudgetFragments = std::min (64.0, state.signalBudgetFragments + elapsed * 32);
		if (state.signalBudgetBytes < packet.size () || state.signalBudgetFragments < 1)
			return true;
		state.signalBudgetBytes -= packet.size ();
		state.signalBudgetFragments -= 1;
		fragment.peerId = state.generation; // The sender identity comes only from the authenticated game connection.
		auto encoded = wire::EncodeSignalFragment (fragment);
		if (target->signals.size () >= 64 || target->signalBytes + encoded.size () > 65536)
			return true;
		target->signalBytes += encoded.size ();
		target->signals.push_back ({std::move (encoded), realtime});
		return true;
	}
	if (length > 128)
		return false;
	if (!packet.empty () && packet[0] == 0xa2)
	{
		wire::Reader  reader (packet);
		uint8_t		  type;
		WorldPosition position;
		uint64_t	  revision;
		ContentHash	  hash;
		if (!reader.U8 (type) || !reader.Position (position) || !reader.U64 (revision) || !reader.Bytes (hash) || reader.Remaining ())
			return false;
		if (state.chunk && (state.offered || state.waitingAck) && state.chunk->key == position.chunk && state.chunk->revision == revision &&
			(state.proof.overrideObject ? state.proof.overrideObject->hash : ContentHash{}) == hash)
		{
			state.sent[position.chunk] = revision;
			state.chunk.reset ();
			state.bytes.clear ();
			state.offered = false;
			state.waitingAck = false;
			state.retryAfter.erase (position.chunk);
		}
		return true;
	}
	wire::Edit edit;
	if (!wire::DecodeEdit (packet, edit) || edit.newRevision || !edit.requestId)
		return false;
	if (edit.epoch != serverWorld->AuthorityEpoch ())
		return true;
	if (edit.requestId <= state.lastRequest)
	{
		if (edit.requestId == state.lastRequest && !state.editReply.empty ())
			state.replyPending = true;
		return true;
	}
	auto reject = [&] (const char *reason)
	{
		if (client->message.cursize + int (strlen (reason)) + 2 < client->message.maxsize)
		{
			MSG_WriteByte (&client->message, svc_print);
			MSG_WriteString (&client->message, reason);
		}
		return true;
	};
	if (state.editPending)
		return reject ("Previous world edit is still committing\n");
	// The callback may complete before this frame reads the next request. Keep
	// the acknowledgment until SendChunks has queued it on the reliable stream.
	if (state.replyPending)
		return reject ("Previous world edit acknowledgment is pending\n");
	state.lastRequest = edit.requestId;
	state.editReply.clear ();
	state.replyPending = false;
	if (!world_editing.value)
		return reject ("World editing is disabled\n");
	if (realtime - state.lastEditTime < 0.1)
		return reject ("World edit rate limit (10 per second)\n");
	state.lastEditTime = realtime;
	if ((edit.cell.contents != Empty && edit.cell.contents != Solid) || (edit.cell.contents == Solid && (edit.cell.material < 1 || edit.cell.material > 4)))
		return reject ("Unsupported world material\n");
	auto chunk = serverWorld->Find (edit.position.chunk);
	if (!chunk || chunk->revision != edit.expectedRevision)
		return reject ("World changed; wait for the new chunk and retry\n");
	if ((!chunk->solids || chunk->solids->empty ()) && serverWorld->Config ().generator != Generator::Planet &&
		serverWorld->Sample (edit.position) == edit.cell)
		return reject ("World voxel is already unchanged\n");
	WorldPosition eye, player;
	OW_GetEntityPosition (client->edict, player);
	if (!Add (player, {client->edict->v.view_ofs[0], client->edict->v.view_ofs[1], client->edict->v.view_ofs[2]}, eye))
		return false;
	WorldPosition center = edit.position;
	for (int a = 0; a < 3; ++a)
		center.local[a] = floorf (center.local[a] / VoxelSize) * VoxelSize + VoxelSize / 2;
	std::array<double, 3> offset;
	if (!EngineDelta (center, eye, offset, 256) || std::hypot (offset[0], offset[1], offset[2]) > 256)
		return reject ("World edit is out of reach\n");
	const bool	 placing = edit.cell.contents == Solid;
	const double distance = std::hypot (offset[0], offset[1], offset[2]);
	if (distance < 1)
		return reject ("World edit overlaps your viewpoint\n");
	if (placing)
		for (double &axis : offset)
			axis *= (distance + 16) / distance;
	const auto	  hit = QueryPhysics (*serverWorld).Trace (eye, offset);
	WorldPosition surface;
	const double  side = placing ? 1 : -1;
	if (hit.startSolid || hit.hitUnknown || hit.fraction == 1 || !Add (hit.end, {hit.normal[0] * side, hit.normal[1] * side, hit.normal[2] * side}, surface))
		return reject ("World edit has no visible supporting surface\n");
	if (!(CanonicalWorldKey (serverWorld->Config (), surface.chunk) == CanonicalWorldKey (serverWorld->Config (), edit.position.chunk)))
		return reject ("World edit is occluded\n");
	for (int a = 0; a < 3; ++a)
		if (int (surface.local[a]) / VoxelSize != int (edit.position.local[a]) / VoxelSize)
			return reject ("World edit is occluded\n");
	if (placing)
	{
		const double margin = serverWorld->Config ().generator != Generator::Empty ? VoxelSize / 2 : 0;
		for (int i = 1; i < qcvm->num_edicts; ++i)
		{
			edict_t *ent = EDICT_NUM (i);
			if (ent->free || ent->v.solid < float (SOLID_BBOX))
				continue;
			WorldPosition position;
			OW_GetEntityPosition (ent, position);
			std::array<double, 3> relative;
			if (!EngineDelta (position, center, relative, 8192))
				continue;
			bool overlaps = true;
			for (int a = 0; a < 3; ++a)
				overlaps &= relative[a] + ent->v.mins[a] < VoxelSize / 2 + margin && relative[a] + ent->v.maxs[a] > -VoxelSize / 2 - margin;
			if (overlaps)
				return reject ("World placement overlaps an entity\n");
		}
	}
	const size_t   index = client - svs.clients;
	const uint64_t generation = state.generation;
	const uint64_t reservation = nextReservation++;
	std::string	   error;
	if (!serverWorld->EditSurfaceAsync (
			edit.position, edit.cell,
			[index, generation, reservation, edit] (bool success, uint64_t revision, const std::string &) mutable
			{
				placements.erase (reservation);
				if (index >= transfers.size () || transfers[index].generation != generation)
					return;
				auto &state = transfers[index];
				state.editPending = false;
				if (!svs.clients[index].active)
					return;
				if (success)
				{
					edit.newRevision = revision;
					state.editReply = wire::EncodeEdit (edit);
					state.replyPending = true;
				}
				else if (svs.clients[index].message.cursize + 64 < svs.clients[index].message.maxsize)
				{
					MSG_WriteByte (&svs.clients[index].message, svc_print);
					MSG_WriteString (&svs.clients[index].message, "World edit failed to commit; terrain was unchanged\n");
				}
			},
			&error))
		return reject ("World edit busy or storage unavailable; retry\n");
	state.editPending = true;
	if (placing)
	{
		WorldPosition lower = edit.position;
		for (int a = 0; a < 3; ++a)
			lower.local[a] = floorf (lower.local[a] / VoxelSize) * VoxelSize;
		placements.emplace (reservation, lower);
	}
	return true;
}

void OW_SendChunks (client_t *client)
{
	if (!serverWorld || !client->spawned || !client->netconnection)
		return;
	// The listen-host renderer already shares the authoritative World object.
	// Loopback needs entity/frame traffic, but no terrain transfer or peer token.
	if (NET_QSocketIsLoopback (client->netconnection))
		return;
	auto	  &transfer = transfers[client - svs.clients];
	const auto root = serverWorld->CurrentRoot ();
	const auto epoch = serverWorld->AuthorityEpoch ();
	if (transfer.epoch != epoch || transfer.root != root)
	{
		auto bytes = wire::EncodeRoot ({root, epoch});
		if (client->message.cursize + int (bytes.size ()) + 4 > client->message.maxsize)
			return;
		WritePacket (&client->message, 5, bytes);
		if (transfer.epoch != epoch)
		{
			transfer.chunk.reset ();
			transfer.bytes.clear ();
			transfer.offered = false;
			transfer.sent.clear ();
			transfer.waitingAck = false;
			transfer.retryAfter.clear ();
			transfer.signals.clear ();
			transfer.signalBytes = 0;
			transfer.introducedPeers.clear ();
			transfer.peerListTime = -1000;
		}
		transfer.root = root;
		transfer.epoch = epoch;
	}
	if (sessionPeerId == peer::Token{})
		peer::RandomToken (sessionPeerId);
	if (realtime - sessionPeerTokenTime > 60)
	{
		peer::Token fresh{};
		if (peer::RandomToken (fresh))
		{
			sessionPeerToken = fresh;
			for (auto &other : transfers)
				other.peerListTime = -1000;
		}
		else
			sessionPeerToken = {};
		sessionPeerTokenTime = realtime;
	}
	if (realtime - transfer.peerListTime > 60 && sessionPeerToken != peer::Token{} && sessionPeerId != peer::Token{})
	{
		wire::PeerDirectory directory;
		directory.epoch = epoch;
		directory.recipientId = transfer.generation;
		directory.sessionId = sessionPeerId;
		directory.token = sessionPeerToken;
		directory.stunServers = world_peer_stun.string;
		if (directory.stunServers.size () > 255)
			directory.stunServers.clear ();
		for (int i = 0; i < svs.maxclients && directory.peers.size () < wire::MaxSessionPeers; ++i)
		{
			const auto &other = svs.clients[i];
			const auto &peer = transfers[i];
			if (&other == client || !other.active || !other.spawned || !other.netconnection || (!peer.peerPort && !peer.peerNAT))
				continue;
			std::string address = NET_QSocketGetTrueAddressString (other.netconnection);
			auto		colon = address.rfind (':');
			if (colon != std::string::npos)
				address.resize (colon);
			uint16_t port = peer.peerPort;
			if (!port || address.empty () || address.size () > 15 || address.find_first_not_of ("0123456789.") != std::string::npos)
			{
				address.clear ();
				port = 0;
			}
			if (!port && !peer.peerNAT)
				continue;
			directory.peers.push_back ({peer.generation, peer.peerNAT, port, address});
		}
		auto bytes = wire::EncodePeerDirectory (directory);
		if (!bytes.empty () && client->message.cursize + int (bytes.size ()) + 4 <= client->message.maxsize)
		{
			WritePacket (&client->message, 7, bytes);
			transfer.peerListTime = realtime;
			transfer.introducedPeers.clear ();
			for (const auto &peer : directory.peers)
				transfer.introducedPeers.insert (peer.peerId);
		}
	}
	if (transfer.replyPending && client->message.cursize + int (transfer.editReply.size ()) + 4 < client->message.maxsize)
	{
		WritePacket (&client->message, 4, transfer.editReply);
		transfer.replyPending = false;
	}
	if (client->message.cursize > 1024)
		return; // Gameplay reliable traffic has priority.
	while (!transfer.signals.empty () && (realtime < transfer.signals.front ().queuedAt || realtime - transfer.signals.front ().queuedAt > 5))
	{
		transfer.signalBytes -= transfer.signals.front ().bytes.size ();
		transfer.signals.pop_front ();
	}
	if (!transfer.signals.empty ())
	{
		const auto &bytes = transfer.signals.front ().bytes;
		if (client->message.cursize + int (bytes.size ()) + 4 <= client->message.maxsize)
		{
			WritePacket (&client->message, 8, bytes);
			transfer.signalBytes -= bytes.size ();
			transfer.signals.pop_front ();
		}
	}
	if (transfer.waitingAck)
	{
		if (realtime < transfer.offerDeadline)
			return;
		// Retry from a fresh authoritative proof after allowing other nearby chunks
		// through. A client that lost a partial transfer cannot stall all interest.
		transfer.retryAfter[transfer.chunk->key] = realtime + 3;
		transfer.chunk.reset ();
		transfer.bytes.clear ();
		transfer.waitingAck = false;
		transfer.offered = false;
	}
	WorldPosition player;
	OW_GetEntityPosition (client->edict, player);
	if (!transfer.chunk)
	{
		// Only keep revision history for resident interest, so travel is bounded.
		for (auto it = transfer.sent.begin (); it != transfer.sent.end ();)
		{
			std::array<double, 3> delta;
			if (!EngineDelta (WorldPosition{it->first, {}}, player, delta, 4096))
			{
				transfer.retryAfter.erase (it->first);
				it = transfer.sent.erase (it);
			}
			else
				++it;
		}
		for (auto it = transfer.retryAfter.begin (); it != transfer.retryAfter.end ();)
		{
			std::array<double, 3> delta;
			if (!EngineDelta ({it->first, {}}, player, delta, 4096))
				it = transfer.retryAfter.erase (it);
			else
				++it;
		}
		auto candidates = serverWorld->ResidentKeys ();
		std::sort (
			candidates.begin (), candidates.end (),
			[&] (const ChunkKey &a, const ChunkKey &b)
			{
				std::array<double, 3> da, db;
				const bool			  va = EngineDelta ({a, {}}, player, da, 3072), vb = EngineDelta ({b, {}}, player, db, 3072);
				if (va != vb)
					return va;
				if (!va)
					return KeyLess{}(a, b);
				return std::hypot (da[0], da[1], da[2]) < std::hypot (db[0], db[1], db[2]);
			});
		for (const auto &key : candidates)
		{
			if (auto retry = transfer.retryAfter.find (key); retry != transfer.retryAfter.end () && realtime < retry->second)
				continue;
			std::array<double, 3> delta;
			if (!EngineDelta (WorldPosition{key, {}}, player, delta, 3072))
				continue;
			auto chunk = serverWorld->Find (key);
			if (!chunk)
				continue;
			auto sent = transfer.sent.find (key);
			if (sent != transfer.sent.end () && sent->second == chunk->revision)
				continue;
			transfer.chunk = chunk;
			if (!serverWorld->GetTerrainProof (key, transfer.proof) || transfer.proof.root != transfer.root || transfer.proof.revision != chunk->revision)
			{
				transfer.chunk.reset ();
				return;
			}
			TerrainProof offer = transfer.proof;
			const bool	 havePeers = std::any_of (
				  transfers.begin (), transfers.end (),
				  [&] (const Transfer &other)
				  {
					  const size_t index = &other - transfers.data ();
					  return &other != &transfer && (other.peerPort || other.peerNAT) && svs.clients[index].active && svs.clients[index].spawned;
				  });
			transfer.offered = sessionPeerToken != peer::Token{} && sessionPeerId != peer::Token{} && havePeers && offer.overrideObject &&
							   offer.overrideObject->bytes.size () <= peer::MaxObjectBytes - 64;
			if (transfer.offered)
				offer.overrideObject->bytes.clear ();
			transfer.bytes = wire::EncodeTerrainProof (offer);
			if (transfer.bytes.empty ())
			{
				transfer.chunk.reset ();
				return;
			}
			transfer.offerDeadline = 0;
			transfer.offset = 0;
			++transfer.serial;
			break;
		}
	}
	if (!transfer.chunk)
		return;
	if (transfer.offered && transfer.offerDeadline)
	{
		if (realtime < transfer.offerDeadline)
			return;
		transfer.bytes = wire::EncodeTerrainProof (transfer.proof);
		transfer.offset = 0;
		transfer.offered = false;
		++transfer.serial;
	}
	wire::ChunkFragment fragment;
	fragment.key = transfer.chunk->key;
	fragment.revision = transfer.chunk->revision;
	fragment.transferId = transfer.serial;
	fragment.totalBytes = uint32_t (transfer.bytes.size ());
	fragment.offset = uint32_t (transfer.offset);
	const size_t size = std::min (wire::FragmentBytes, transfer.bytes.size () - transfer.offset);
	fragment.payload.assign (transfer.bytes.begin () + transfer.offset, transfer.bytes.begin () + transfer.offset + size);
	auto bytes = wire::EncodeFragment (fragment);
	if (bytes.empty () || client->message.cursize + int (bytes.size ()) + 4 > client->message.maxsize)
		return;
	WritePacket (&client->message, 1, bytes);
	transfer.offset += size;
	if (transfer.offset == transfer.bytes.size ())
	{
		if (transfer.offered)
		{
			const bool natPeer =
				transfer.peerNAT && std::any_of (
										transfers.begin (), transfers.end (),
										[&] (const auto &other)
										{
											const size_t index = &other - transfers.data ();
											return &other != &transfer && other.peerNAT && svs.clients[index].active && svs.clients[index].spawned;
										});
			transfer.offerDeadline = realtime + (natPeer ? 8.0 : 2.0);
			return;
		}
		transfer.waitingAck = true;
		transfer.offerDeadline = realtime + 3.0;
	}
}
