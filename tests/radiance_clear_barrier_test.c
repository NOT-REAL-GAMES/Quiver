/* Record the real cascade commands against a mocked Vulkan command buffer.
 * A clear must publish untouched validity records to receiver shaders as well
 * as to the compute passes, both for a full reset and sparse-slot reuse. */
#include "radiance_vulkan.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define HANDLE(type, id) ((type)(uintptr_t)(id))

static unsigned fills, clear_barriers;
static VkBuffer filled[2];
static VkDeviceSize offsets[2], sizes[2];

VKAPI_ATTR void VKAPI_CALL vkCmdFillBuffer (VkCommandBuffer cb, VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size, uint32_t data)
{
	assert (cb && fills < 2 && data == 0);
	filled[fills] = buffer; offsets[fills] = offset; sizes[fills] = size; ++fills;
}

VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier (VkCommandBuffer cb, VkPipelineStageFlags src, VkPipelineStageFlags dst,
	VkDependencyFlags flags, uint32_t count, const VkMemoryBarrier *barriers, uint32_t buffer_count,
	const VkBufferMemoryBarrier *buffers, uint32_t image_count, const VkImageMemoryBarrier *images)
{
	(void)flags; (void)buffer_count; (void)buffers; (void)image_count; (void)images;
	assert (cb && count == 1 && barriers);
	if (src == VK_PIPELINE_STAGE_TRANSFER_BIT && (barriers[0].srcAccessMask & VK_ACCESS_TRANSFER_WRITE_BIT))
	{
		const VkPipelineStageFlags consumers = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
			VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
		assert (fills == 2 && (dst & consumers) == consumers);
		assert ((barriers[0].dstAccessMask & VK_ACCESS_SHADER_READ_BIT) != 0);
		++clear_barriers;
	}
}

VKAPI_ATTR void VKAPI_CALL vkCmdBindDescriptorSets (VkCommandBuffer cb, VkPipelineBindPoint bind, VkPipelineLayout layout,
	uint32_t first, uint32_t count, const VkDescriptorSet *sets, uint32_t dynamic_count, const uint32_t *offset)
{ (void)cb; (void)bind; (void)layout; (void)first; (void)count; (void)sets; (void)dynamic_count; (void)offset; }
VKAPI_ATTR void VKAPI_CALL vkCmdBindPipeline (VkCommandBuffer cb, VkPipelineBindPoint bind, VkPipeline pipeline)
{ (void)cb; (void)bind; (void)pipeline; }
VKAPI_ATTR void VKAPI_CALL vkCmdPushConstants (VkCommandBuffer cb, VkPipelineLayout layout, VkShaderStageFlags stage,
	uint32_t offset, uint32_t size, const void *values)
{ (void)cb; (void)layout; (void)stage; (void)offset; (void)size; (void)values; }
VKAPI_ATTR void VKAPI_CALL vkCmdDispatch (VkCommandBuffer cb, uint32_t x, uint32_t y, uint32_t z)
{ (void)cb; (void)x; (void)y; (void)z; }

static void test_clear (bool whole)
{
	rc_vulkan_t vk = {0};
	rc_sparse_range_t range = {3, 7};
	vk.frames = 1; vk.snapshot_ready[0] = true; vk.max_workgroups = 16;
	vk.descriptor_sets[0] = HANDLE (VkDescriptorSet, 1);
	vk.trace_software = HANDLE (VkPipeline, 2); vk.merge_software = HANDLE (VkPipeline, 3);
	vk.software_layout = HANDLE (VkPipelineLayout, 4);
	vk.records[4].buffer = HANDLE (VkBuffer, 5); vk.records[4].size = 128;
	vk.records[5].buffer = HANDLE (VkBuffer, 6); vk.records[5].size = 128;
	vk.work[0][0] = (rc_vk_work_t){.active_count = 1, .records_per_brick = 1};
	if (whole) vk.needs_clear = true;
	else { vk.clears[0] = &range; vk.clear_count[0] = 1; }
	fills = clear_barriers = 0;
	assert (RCVk_Record (&vk, HANDLE (VkCommandBuffer, 7), 0, HANDLE (VkDescriptorSet, 8),
		HANDLE (VkDescriptorSet, 9), false, 0, 0, 1));
	assert (fills == 2 && clear_barriers == 1);
	assert (filled[0] == vk.records[4].buffer && filled[1] == vk.records[5].buffer);
	for (unsigned i = 0; i < 2; ++i)
	{
		assert (offsets[i] == (whole ? 0 : range.first * sizeof (uint32_t)));
		assert (sizes[i] == (whole ? 128 : range.count * sizeof (uint32_t)));
	}
}

int main (void)
{
	test_clear (true);
	test_clear (false);
	puts ("Radiance full and reused-slot clears are visible to graphics and compute");
	return 0;
}
