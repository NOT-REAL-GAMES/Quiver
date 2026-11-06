/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef QUIVER_MESHLET_HIZ_H
#define QUIVER_MESHLET_HIZ_H

// All levels are packed at successive offsets in set 1, binding 0. The final
// five uint words are optional GPU culling counters (see meshlet cull shader).
#define ML_HIZ_DIM		64
#define ML_HIZ_WORDS	5461
#define ML_HIZ_COUNTERS 5

VkDescriptorSetLayout MLHiZ_Layout (void);
void				  MLHiZ_Bake (const qmodel_t *world);
void				  MLHiZ_Reset (void);	 // brush buffers are idle
void				  MLHiZ_Shutdown (void); // device idle
bool				  MLHiZ_Record (cb_context_t *cbx, bool build_depth, bool count_stats);
VkDescriptorSet		  MLHiZ_Set (void);
VkBuffer                MLHiZ_BufferForSlot (uint32_t slot);
void				  MLHiZ_FinishStats (cb_context_t *cbx);
bool				  MLHiZ_GetStats (uint32_t counts[ML_HIZ_COUNTERS]);
uint32_t			  MLHiZ_OccluderCount (void);
uint32_t			  MLHiZ_ActiveOccluders (void);
#endif
