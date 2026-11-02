#ifndef QUIVER_OCTREE_ENGINE_H
#define QUIVER_OCTREE_ENGINE_H

#include "octree_world.h"

bool OW_RestoreEntityPositions (const std::vector<quiver::WorldPosition> &saved, const std::vector<uint8_t> &live);
bool OW_SetStreamable (edict_t *entity, bool enabled);

// Engine entry points. Include quakedef.h before this header.
void				  OW_Init ();
void				  OW_Clear ();
void				  OW_ServerLoad (const char *map);
void				  OW_PrepareRestart ();
void				  OW_ClientConnected (client_t *client);
void				  OW_ServerFrame ();
void				  OW_ClientFrame ();
void				  OW_ClientDisconnect ();
bool				  OW_Active ();
bool				  OW_ServerActive ();
bool				  OW_ClientProtocol ();
quiver::World		 *OW_GetWorld ();
quiver::WorldPosition OW_GetRenderPosition ();
quiver::WorldPosition OW_GetRenderOrigin ();
quiver::WorldPosition OW_GetSimulationOrigin ();
bool				  OW_BeginEntity (edict_t *ent);
void				  OW_SyncEntities ();
void				  OW_ForgetEntity (edict_t *ent);
bool				  OW_EntityVisible (edict_t *ent);
bool				  OW_GetEntityPosition (edict_t *ent, quiver::WorldPosition &position);
bool				  OW_SetEntityPosition (edict_t *ent, const quiver::WorldPosition &position);
bool				  OW_MovePlayer (edict_t *ent, float deltaTime, bool allowStairs, bool stickToFloor, std::vector<int> &touched);
bool				  OW_ClipMove (const float *start, const float *mins, const float *maxs, const float *end, trace_t *trace);
// Precise-coordinate, nonblocking terrain query for QC and engine callers.
quiver::TraceResult
OW_TraceTerrain (const quiver::WorldPosition &start, const std::array<double, 3> &offset, const std::array<float, 3> &mins, const std::array<float, 3> &maxs);
int	 OW_PointContents (const float *point);
void OW_WriteServerInfo (sizebuf_t *message);
void OW_ReadServerInfo ();
void OW_WriteFrame (client_t *client, sizebuf_t *message);
void OW_ReadMessage ();
bool OW_ReadEdit (client_t *client);
void OW_SendChunks (client_t *client);
bool OW_StartSound (edict_t *entity, const float *origin, int channel, unsigned int sound, int volume, float attenuation);
bool OW_StartParticle (const float *origin, const float *direction, int color, int count);
bool OW_IsEmptyMap ();

// These opcodes are only legal after the separately versioned Quiver marker.
constexpr int PROTOCOL_QUIVER_WORLD = 0x51565231;
constexpr int svc_quiver_world = 87;
constexpr int clc_quiver_world = 87;

#endif
