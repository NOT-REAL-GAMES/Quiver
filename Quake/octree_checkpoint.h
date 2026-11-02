/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef QUIVER_OCTREE_CHECKPOINT_H
#define QUIVER_OCTREE_CHECKPOINT_H

#include "octree_world.h"
#include <span>

// Save/load only at a server command/frame boundary, outside QuakeC execution.
// Capture drains terrain/page writes synchronously; callers commit the bytes before allowing another server tick.
bool OW_CheckpointCapture (quiver::World &world, std::vector<uint8_t> &bytes, std::string &error);
// Validate compatibility and all typed values without changing the VM or world.
bool OW_CheckpointValidate (quiver::World &world, const std::vector<uint8_t> &bytes, std::string &error);
bool OW_CheckpointSave (quiver::World &world, std::string &error);
bool OW_CheckpointLoad (quiver::World &world, bool &restored, std::string &error);
bool OW_CheckpointRestoreSnapshot (quiver::World &world, const std::string &name, std::string &error);
bool OW_EntityPageEncode (edict_t *entity, double snapshotTime, std::vector<uint8_t> &bytes, std::string &error);
bool OW_EntityPageDecode (std::span<const uint8_t> bytes, edict_t *&entity, std::string &error);
bool OW_EntityPagePosition (std::span<const uint8_t> bytes, quiver::WorldPosition &position, std::string &error);
// Called with the affected QCVM selected, before its string storage is cleared.
void OW_CheckpointResetStrings ();

#endif
