/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

enum sky_profile_pass_t
{
	SKY_PROFILE_ATMOSPHERE,
	SKY_PROFILE_LUT,
	SKY_PROFILE_CLOUD_TRACE,
	SKY_PROFILE_CLOUD_COMPOSITE,
	SKY_PROFILE_CLOUD_NEAR,
	SKY_PROFILE_CLOUD_FAR,
	SKY_PROFILE_PASS_COUNT
};

void SkyProfile_Init ();
void SkyProfile_Create (uint32_t timestamp_valid_bits);
void SkyProfile_Destroy ();
// Called after this frame slot's existing fence has completed, outside a render pass.
void SkyProfile_BeginFrame (VkCommandBuffer cb, unsigned slot);
// Atmosphere markers belong in the sky secondary; the remaining markers are
// recorded by EndRendering outside render passes. Task dependencies serialize
// the mask updates, and the normal frame-slot fence protects query reuse.
void SkyProfile_BeginPass (VkCommandBuffer cb, sky_profile_pass_t pass);
void SkyProfile_EndPass (VkCommandBuffer cb, sky_profile_pass_t pass);
// Optional once-per-second readback of the reduced target's alpha=-1 fallback markers.
// Image enters and leaves this call in SHADER_READ_ONLY_OPTIMAL layout.
void SkyProfile_CaptureClouds (
	VkCommandBuffer cb, VkImage image, int width, int height, int divisor, int view_width, int view_height, VkImage far_image = VK_NULL_HANDLE,
	int far_width = 0, int far_height = 0, int far_divisor = 0);
