/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef QUIVER_OCTREE_PAGING_H
#define QUIVER_OCTREE_PAGING_H

#include "octree_world.h"

// Server-thread API; caller selects the server QCVM except Init/Reset.
void	 OW_PagingInit ();
void	 OW_PagingReset ();
void	 OW_PagingFrame ();
void	 OW_PagingForget (edict_t *entity);
void	 OW_PagingStats ();
bool	 OW_SetStreamable (edict_t *entity, bool enabled);
uint64_t OW_EntityPageId (edict_t *entity);
uint64_t OW_EntityPageRevision (edict_t *entity);
bool	 OW_RestoreEntityPageId (edict_t *entity, uint64_t id, uint64_t revision);
bool	 OW_PagingFlush (std::string &error);

#endif
