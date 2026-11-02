/* Cached atmospheric scattering. GPL-2.0-or-later. */
#ifndef QUIVER_SKY_ATMOSPHERE_H
#define QUIVER_SKY_ATMOSPHERE_H

struct sky_atmosphere_stats_t
{
	qboolean available;
	qboolean enabled;
	uint64_t rebuilds;
	size_t	 allocated_bytes;
};

void				   Sky_AtmosphereInit (void);
void				   Sky_AtmosphereCreate (VkPhysicalDevice physical_device);
void				   Sky_AtmosphereDestroy (void);
// Record outside render passes, before executing the sky secondary commands.
void				   Sky_AtmospherePrepare (VkCommandBuffer command_buffer);
void				   Sky_AtmosphereBind (cb_context_t *cbx);
qboolean			   Sky_AtmosphereUseLUT (void);
sky_atmosphere_stats_t SkyAtmosphere_GetStats (void);

#endif
