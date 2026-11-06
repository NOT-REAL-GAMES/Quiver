/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef QUIVER_EDITOR_SECTOR_PORTAL_H
#define QUIVER_EDITOR_SECTOR_PORTAL_H
#include "editor_sector.h"

/* Cut the common area of two oppositely oriented coplanar sector faces into
 * paired portal polygons. The non-overlapping portions stay solid. Adjacent
 * sector faces are split at new boundary vertices to preserve manifold edges.
 * The document is unchanged on failure. */
bool EditorSector_LinkPartialPortal (editor_sector_map_t *map, size_t sector, size_t face,
    size_t other_sector, size_t other_face, char *error, size_t error_size);

/* Link the complete shared area of the selected connected coplanar face
 * regions, even when either region has been split into multiple polygons. */
bool EditorSector_LinkFaceRegions (editor_sector_map_t *map, size_t sector, size_t face,
    size_t other_sector, size_t other_face, char *error, size_t error_size);

/* Clean previously linked portal fragments and weld their exact matching
 * vertices. Work is bounded; changes reports the number of safe operations. */
bool EditorSector_CleanupLinks (editor_sector_map_t *map, size_t *changes, char *error, size_t error_size);

/* Close a connected, coplanar opening as one operation, even if it consists
 * of multiple portal polygons created during subdivision. */
bool EditorSector_UnlinkPortalGroup (editor_sector_map_t *map, size_t sector, size_t face,
    char *error, size_t error_size);
#endif
