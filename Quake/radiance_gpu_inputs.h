/* Per-frame Vulkan snapshots for the radiance lighting descriptor set (set 2).
 * The caller must wait for a frame slot's GPU fence before replacing it and
 * must synchronize GPU/device work before destroying the whole object. */
#ifndef QUIVER_RADIANCE_GPU_INPUTS_H
#define QUIVER_RADIANCE_GPU_INPUTS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan.h>
#include "radiance_lights.h"

#define RADIANCE_INPUT_MAX_FRAMES 2u

typedef struct
{
	const radiance_light_t *lights;
	size_t light_count;
	const radiance_material_t *materials;
	size_t material_count;
	radiance_lighting_parameters_t parameters;
	const uint32_t *grid_offsets, *grid_indices;
	size_t grid_offset_count, grid_index_count;
	/* Nonzero uniquely identifies immutable grid arrays for the lifetime of
	 * the input object; zero requires copying them on every update. */
	uint64_t grid_revision;
	/* Actual sRGB RGBA texels, tightly packed as array layers. The caller
	 * produces these from the map/model textures and fullbright masks. */
	const uint8_t *atlas_rgba;
	uint32_t atlas_width, atlas_height, atlas_layers;
	/* Nonzero identifies immutable texels for this input object's lifetime;
	 * callers increment it whenever pixels change. Zero requests bytewise
	 * comparison for callers that cannot guarantee this contract. */
	uint64_t atlas_revision;
} radiance_input_snapshot_t;

typedef struct
{
	VkBuffer buffer;
	VkDeviceMemory memory;
	VkDeviceSize size, allocation_size;
	void *mapped;
} radiance_input_buffer_t;

typedef struct
{
	VkDescriptorSet descriptor;
	radiance_input_buffer_t buffers[5]; /* lights, params, materials, offsets, indices */
	radiance_input_buffer_t staging;
	VkImage atlas;
	VkDeviceMemory atlas_memory;
	VkDeviceSize atlas_allocation_size;
	VkImageView atlas_view;
	uint32_t width, height, layers;
	uint64_t atlas_revision; /* identity of texels currently in staging */
	uint64_t grid_revision; /* identity of the coherent per-slot grid buffers */
	size_t grid_offset_count, grid_index_count;
	bool valid, atlas_uploaded, atlas_dirty, atlas_recorded;
} radiance_input_frame_t;

typedef struct
{
	VkPhysicalDevice physical_device;
	VkDevice device;
	VkPhysicalDeviceMemoryProperties memory_properties;
	VkPhysicalDeviceLimits limits;
	VkDescriptorSetLayout layout;
	VkDescriptorPool pool;
	VkSampler sampler;
	uint32_t frames;
	radiance_input_frame_t frame[RADIANCE_INPUT_MAX_FRAMES];
} radiance_gpu_inputs_t;

bool RadianceInputs_Create (radiance_gpu_inputs_t *inputs, VkPhysicalDevice physical_device, VkDevice device,
	const VkPhysicalDeviceMemoryProperties *memory_properties, const VkPhysicalDeviceLimits *limits, uint32_t frames);
void RadianceInputs_Destroy (radiance_gpu_inputs_t *inputs);

/* Material and light-grid buffers are copied into coherent per-frame memory.
 * Atlas pixels are refreshed when their revision, bytes or dimensions change. Failure
 * invalidates this slot and allows the caller to use the classic renderer. */
bool RadianceInputs_Update (radiance_gpu_inputs_t *inputs, uint32_t frame, const radiance_input_snapshot_t *snapshot);
/* Record on the graphics/compute queue before the first shader read. After a
 * successful queue submission call MarkSubmitted; if the command buffer is
 * discarded or submission fails call DiscardRecorded. The slot cannot be
 * updated or recorded again until one of these resolves a pending upload. */
bool RadianceInputs_RecordUpload (radiance_gpu_inputs_t *inputs, uint32_t frame, VkCommandBuffer commands);
void RadianceInputs_MarkSubmitted (radiance_gpu_inputs_t *inputs, uint32_t frame);
void RadianceInputs_DiscardRecorded (radiance_gpu_inputs_t *inputs, uint32_t frame);
VkDescriptorSet RadianceInputs_Descriptor (const radiance_gpu_inputs_t *inputs, uint32_t frame);
/* Host-visible + device-local allocations owned by this object. */
uint64_t RadianceInputs_AllocationBytes (const radiance_gpu_inputs_t *inputs);

#endif
