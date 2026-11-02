#ifndef QUIVER_OCTREE_QC_H
#define QUIVER_OCTREE_QC_H

#include <cstdint>
#include <span>
#include <string>
#include <vector>

// Engine declarations; include quakedef.h first.
void OW_QCReset ();
void OW_QCForgetEntity (edict_t *entity);
void OW_QCCheckField (edict_t *entity, int field, int width, int statement);

// Caller selects the server QCVM. Validate is read-only and may run before
// restoring edicts; Import runs after ordinary QC globals/edicts are restored,
// preserving their indices. All three leave their outputs/state intact on failure.
// Multiplayer checkpoints omit fields on players 1..excludedPlayerCount and all
// references to their owned handles. Those registered locations restore as null.
bool OW_QCExportPositions (std::vector<uint8_t> &bytes, std::string &error, uint32_t excludedPlayerCount = 0);
bool OW_QCValidatePositions (std::span<const uint8_t> bytes, uint32_t entityCount, std::string &error, std::span<const uint8_t> liveEntities = {});
bool OW_QCImportPositions (std::span<const uint8_t> bytes, std::string &error);

// Read-only paging eligibility/export. Shared handles and unregistered live
// references pin the entity. Export normalizes self to index zero; Import remaps
// it to the target edict and appends fresh handles without changing other owners.
// The caller durably commits the page before freeing the old entity/handles.
bool OW_QCCanPageEntity (edict_t *entity, std::string &reason);
bool OW_QCExportEntityPositions (edict_t *entity, std::vector<uint8_t> &bytes, std::string &error);
bool OW_QCValidateEntityPositions (std::span<const uint8_t> bytes, std::string &error);
bool OW_QCImportEntityPositions (edict_t *entity, std::span<const uint8_t> bytes, std::string &error);

void PF_wp_active ();
void PF_wp_alloc ();
void PF_wp_free ();
void PF_wp_valid ();
void PF_wp_copy ();
void PF_wp_getentity ();
void PF_wp_setentity ();
void PF_wp_add ();
void PF_wp_delta ();
void PF_wp_distance ();
void PF_wp_direction ();
void PF_wp_contents ();
void PF_wp_tracebox_terrain ();
void PF_wp_findradius ();
void PF_wp_register_field ();
void PF_wp_register_global ();
void PF_wp_streamable ();

#endif
