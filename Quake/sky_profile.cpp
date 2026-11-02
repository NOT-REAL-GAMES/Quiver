/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "quakedef.h"
#include "sky_profile.h"
#include "sky_atmosphere.h"
#include <algorithm>
#include <array>
#include <cstdint>

extern cvar_t r_clouds_filter;

namespace
{
cvar_t			   r_sky_profile = {"r_sky_profile", "0", CVAR_NONE};
constexpr unsigned FrameSlots = 2, Queries = SKY_PROFILE_PASS_COUNT * 2, History = 32;
struct Frame
{
	VkQueryPool		pool = VK_NULL_HANDLE;
	uint32_t		written = 0;
	VkBuffer		readback = VK_NULL_HANDLE;
	vulkan_memory_t memory{};
	size_t			capacity = 0;
	int				width = 0, height = 0, divisor = 0, view_width = 0, view_height = 0;
	int				far_width = 0, far_height = 0, far_divisor = 0;
	bool			copied = false;
};
std::array<Frame, FrameSlots>									frames;
std::array<std::array<double, SKY_PROFILE_PASS_COUNT>, History> timings{};
unsigned														current_slot = 0, history_count = 0, history_next = 0;
uint32_t														valid_bits = 0;
bool															active = false, was_active = false, edges_available = false;
uint64_t														fallback_pixels = 0, total_pixels = 0;
double															next_capture = 0;

void ConsumeEdges (Frame &frame)
{
	if (!frame.copied)
		return;
	frame.copied = false;
	void *data = nullptr;
	if (vkMapMemory (vulkan_globals.device, frame.memory.handle, 0, VK_WHOLE_SIZE, 0, &data) != VK_SUCCESS)
		return;
	VkMappedMemoryRange range{};
	range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
	range.memory = frame.memory.handle;
	range.size = VK_WHOLE_SIZE;
	if (vkInvalidateMappedMemoryRanges (vulkan_globals.device, 1, &range) == VK_SUCCESS)
	{
		const auto *pixels = static_cast<const uint16_t *> (data);
		fallback_pixels = 0;
		total_pixels = uint64_t (frame.view_width) * frame.view_height;
		if (frame.far_divisor)
		{
			// Either cascade's mixed block selects the one complete exact ray.
			// Count their union in full-resolution pixels, including partial blocks.
			const auto *far_pixels = pixels + size_t (frame.width) * frame.height * 4;
			for (int y = 0; y < frame.view_height; ++y)
				for (int x = 0; x < frame.view_width; ++x)
				{
					const size_t near_index = (size_t (y / frame.divisor) * frame.width + x / frame.divisor) * 4 + 3;
					const size_t far_index = (size_t (y / frame.far_divisor) * frame.far_width + x / frame.far_divisor) * 4 + 3;
					fallback_pixels += pixels[near_index] == 0xbc00 || far_pixels[far_index] == 0xbc00;
				}
		}
		else
			for (int y = 0; y < frame.height; ++y)
				for (int x = 0; x < frame.width; ++x)
					if (pixels[(size_t (y) * frame.width + x) * 4 + 3] == 0xbc00) // binary16 -1
						fallback_pixels += uint64_t (std::min (frame.divisor, frame.view_width - x * frame.divisor)) *
										   std::min (frame.divisor, frame.view_height - y * frame.divisor);
		edges_available = true;
	}
	vkUnmapMemory (vulkan_globals.device, frame.memory.handle);
}

void PrintStats ()
{
	// Console commands execute on the main thread. Finish the prior renderer task
	// before reading its CPU bookkeeping; this introduces no GPU/profiling wait.
	GL_SynchronizeEndRenderingTask ();
	const auto clouds = Sky_GetCloudStats ();
	Con_Printf (
		"Sky clouds: path %s resolution %d %d divisor %d filter %s allocated_bytes %zu\n", clouds.reduced ? "reduced" : "full", clouds.width, clouds.height,
		clouds.divisor, r_clouds_filter.value ? "linear" : "nearest", clouds.allocated_bytes);
	const auto atmosphere = SkyAtmosphere_GetStats ();
	Con_Printf (
		"Sky atmosphere: path %s available %d rebuilds %llu allocated_bytes %zu\n", atmosphere.enabled ? "lut" : "direct", atmosphere.available,
		(unsigned long long)atmosphere.rebuilds, atmosphere.allocated_bytes);
	std::array<double, SKY_PROFILE_PASS_COUNT> mean{};
	for (unsigned i = 0; i < history_count; ++i)
		for (unsigned j = 0; j < SKY_PROFILE_PASS_COUNT; ++j)
			mean[j] += timings[i][j] / history_count;
	Con_Printf (
		"Sky GPU: available %d profiling %d samples %u atmosphere_ms %.6f lut_ms %.6f cloud_trace_ms %.6f cloud_composite_ms %.6f\n", valid_bits != 0,
		r_sky_profile.value != 0, history_count, mean[SKY_PROFILE_ATMOSPHERE], mean[SKY_PROFILE_LUT], mean[SKY_PROFILE_CLOUD_TRACE],
		mean[SKY_PROFILE_CLOUD_COMPOSITE]);
	Con_Printf (
		"Sky bands: cascades %d diagnostic %d split %.1f near_divisor %d far_divisor %d near_resolution %d %d far_resolution %d %d\n", clouds.cascades,
		clouds.band, clouds.split, clouds.near_divisor, clouds.far_divisor, clouds.near_width, clouds.near_height, clouds.far_width, clouds.far_height);
	Con_Printf ("Sky GPU bands: near_ms %.6f far_ms %.6f\n", mean[SKY_PROFILE_CLOUD_NEAR], mean[SKY_PROFILE_CLOUD_FAR]);
	const bool current_edges = edges_available && clouds.reduced;
	Con_Printf (
		"Sky edges: available %d fallback_pixels %llu total_pixels %llu percent %.6f\n", current_edges,
		current_edges ? (unsigned long long)fallback_pixels : 0ULL, current_edges ? (unsigned long long)total_pixels : 0ULL,
		current_edges && total_pixels ? 100.0 * double (fallback_pixels) / total_pixels : 0.0);
}
} // namespace

void SkyProfile_Init ()
{
	Cvar_RegisterVariable (&r_sky_profile);
	Cmd_AddCommand ("sky_stats", PrintStats);
}

void SkyProfile_Create (uint32_t timestamp_valid_bits)
{
	valid_bits = std::min (timestamp_valid_bits, 64u);
	if (!valid_bits)
		return;
	VkQueryPoolCreateInfo info{};
	info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
	info.queryType = VK_QUERY_TYPE_TIMESTAMP;
	info.queryCount = Queries;
	for (auto &frame : frames)
		if (vkCreateQueryPool (vulkan_globals.device, &info, nullptr, &frame.pool) != VK_SUCCESS)
		{
			SkyProfile_Destroy ();
			Con_Printf ("Sky GPU timestamps unavailable\n");
			return;
		}
}

void SkyProfile_Destroy ()
{
	for (auto &frame : frames)
	{
		if (frame.pool)
			vkDestroyQueryPool (vulkan_globals.device, frame.pool, nullptr);
		if (frame.readback)
			R_FreeBuffer (frame.readback, &frame.memory, nullptr);
		frame = {};
	}
	valid_bits = 0;
	active = was_active = edges_available = false;
	history_count = history_next = 0;
	fallback_pixels = total_pixels = 0;
	next_capture = 0;
}

void SkyProfile_BeginFrame (VkCommandBuffer cb, unsigned slot)
{
	current_slot = slot % FrameSlots;
	auto &frame = frames[current_slot];
	ConsumeEdges (frame);
	if (frame.written)
	{
		std::array<double, SKY_PROFILE_PASS_COUNT> sample{};
		bool									   complete = true;
		for (unsigned pass = 0; pass < SKY_PROFILE_PASS_COUNT; ++pass)
		{
			if ((frame.written & (3u << (pass * 2))) != (3u << (pass * 2)))
				continue;
			uint64_t result[2][2]{};
			VkResult status = vkGetQueryPoolResults (
				vulkan_globals.device, frame.pool, pass * 2, 2, sizeof (result), result, sizeof (result[0]),
				VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
			if (status != VK_SUCCESS || !result[0][1] || !result[1][1])
			{
				complete = false;
				break;
			}
			uint64_t mask = valid_bits == 64 ? UINT64_MAX : (uint64_t (1) << valid_bits) - 1;
			sample[pass] = double ((result[1][0] - result[0][0]) & mask) * vulkan_globals.device_properties.limits.timestampPeriod / 1e6;
		}
		if (complete)
		{
			timings[history_next] = sample;
			history_next = (history_next + 1) % History;
			history_count = std::min (history_count + 1, History);
		}
	}
	frame.written = 0;
	active = r_sky_profile.value != 0;
	if (active != was_active)
	{
		history_count = history_next = 0;
		edges_available = false;
		next_capture = 0;
	}
	was_active = active;
	if (active && frame.pool)
		vkCmdResetQueryPool (cb, frame.pool, 0, Queries);
}

void SkyProfile_BeginPass (VkCommandBuffer cb, sky_profile_pass_t pass)
{
	// BeginRendering publishes the selected slot before the sky recording task.
	// Only that task writes the atmosphere pair; all remaining pairs are written
	// by EndRendering after draw_done joins the sky task. The next BeginRendering
	// also depends on EndRendering, so no mask updates run concurrently.
	auto &frame = frames[current_slot];
	if (active && frame.pool)
	{
		frame.written |= 1u << (unsigned (pass) * 2);
		vkCmdWriteTimestamp (cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, frame.pool, unsigned (pass) * 2);
	}
}

void SkyProfile_EndPass (VkCommandBuffer cb, sky_profile_pass_t pass)
{
	auto &frame = frames[current_slot];
	if (active && frame.pool)
	{
		frame.written |= 2u << (unsigned (pass) * 2);
		vkCmdWriteTimestamp (cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, frame.pool, unsigned (pass) * 2 + 1);
	}
}

void SkyProfile_CaptureClouds (
	VkCommandBuffer cb, VkImage image, int width, int height, int divisor, int view_width, int view_height, VkImage far_image, int far_width, int far_height,
	int far_divisor)
{
	if (!active || !image || width <= 0 || height <= 0 || divisor <= 0 || Sys_DoubleTime () < next_capture)
		return;
	if (far_image && (far_width <= 0 || far_height <= 0 || far_divisor <= 0))
		return;
	const size_t near_bytes = size_t (width) * height * 8;
	const size_t bytes = near_bytes + (far_image ? size_t (far_width) * far_height * 8 : 0);
	if (bytes > 64 * 1024 * 1024)
	{
		edges_available = false;
		return;
	}
	next_capture = Sys_DoubleTime () + 1.0;
	auto &frame = frames[current_slot];
	if (frame.capacity < bytes)
	{
		if (frame.readback)
			R_FreeBuffer (frame.readback, &frame.memory, nullptr);
		R_CreateBuffer (
			&frame.readback, &frame.memory, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
			nullptr, nullptr, "Sky profile readback");
		frame.capacity = bytes;
	}
	auto copy_image = [&] (VkImage source, int source_width, int source_height, VkDeviceSize offset)
	{
		VkImageMemoryBarrier barrier{};
		barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
		barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image = source;
		barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		barrier.subresourceRange.levelCount = barrier.subresourceRange.layerCount = 1;
		vkCmdPipelineBarrier (
			cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
			nullptr, 1, &barrier);
		VkBufferImageCopy region{};
		region.bufferOffset = offset;
		region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		region.imageSubresource.layerCount = 1;
		region.imageExtent = {uint32_t (source_width), uint32_t (source_height), 1};
		vkCmdCopyImageToBuffer (cb, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, frame.readback, 1, &region);
		barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		vkCmdPipelineBarrier (cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
	};
	copy_image (image, width, height, 0);
	if (far_image)
		copy_image (far_image, far_width, far_height, near_bytes);
	VkBufferMemoryBarrier host_barrier{};
	host_barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
	host_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	host_barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
	host_barrier.srcQueueFamilyIndex = host_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	host_barrier.buffer = frame.readback;
	host_barrier.size = VK_WHOLE_SIZE;
	vkCmdPipelineBarrier (cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &host_barrier, 0, nullptr);
	frame.width = width;
	frame.height = height;
	frame.divisor = divisor;
	frame.view_width = view_width;
	frame.view_height = view_height;
	frame.far_width = far_width;
	frame.far_height = far_height;
	frame.far_divisor = far_image ? far_divisor : 0;
	frame.copied = true;
}
