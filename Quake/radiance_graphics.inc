/* The owner publishes these only between frames, before renderer task dispatch. */
static const rc_graphics_t *rc_graphics_frame;
static VkDescriptorSet rc_graphics_descriptor;

/* Included here to reuse the renderer's render-pass and vertex state builders. */
typedef struct rc_graphics_shaders_s
{
	VkShaderModule world, alias, instanced, md5, md5_8;
	VkShaderModule wf[5], af[10], mf[4];
} rc_graphics_shaders_t;

static void RCGraphics_DestroyPipeline (vulkan_pipeline_t *pipeline)
{
	while (pipeline->alternatives)
	{
		pipeline_instance_t *instance = pipeline->alternatives;
		pipeline->alternatives = instance->next;
		pipeline_instance_t **link = &pipeline_instances;
		while (*link && *link != instance) link = &(*link)->allocation_next;
		if (*link) *link = instance->allocation_next;
		vkDestroyPipeline (vulkan_globals.device, instance->handle, NULL);
		Mem_Free (instance);
	}
	if (pipeline->handle) vkDestroyPipeline (vulkan_globals.device, pipeline->handle, NULL);
	memset (pipeline, 0, sizeof (*pipeline));
}

void RCGraphics_Destroy (rc_graphics_t *g)
{
	if (!g) return;
	if (rc_graphics_frame == g) { rc_graphics_frame = NULL; rc_graphics_descriptor = VK_NULL_HANDLE; }
	for (int v = 0; v < MAIN_RENDER_PASS_VARIANT_COUNT; ++v)
	{
		for (int i = 0; i < WORLD_PIPELINE_COUNT; ++i) RCGraphics_DestroyPipeline (&g->world[v][i]);
		RCGraphics_DestroyPipeline (&g->instanced[v]);
		for (int i = 0; i < MODEL_PIPELINE_COUNT; ++i)
		{
			RCGraphics_DestroyPipeline (&g->alias.main[v][i]);
			RCGraphics_DestroyPipeline (&g->md5.main[v][i]);
			RCGraphics_DestroyPipeline (&g->md5_8.main[v][i]);
		}
	}
	for (int i = 0; i < WORLD_PIPELINE_COUNT; ++i)
	{
		RCGraphics_DestroyPipeline (&g->world_wboit[i]); RCGraphics_DestroyPipeline (&g->world_moment[i]); RCGraphics_DestroyPipeline (&g->world_composite[i]);
	}
	rc_graphics_family_t *families[] = {&g->alias, &g->md5, &g->md5_8};
	for (int f = 0; f < 3; ++f) for (int i = 0; i < MODEL_PIPELINE_COUNT; ++i)
	{
		RCGraphics_DestroyPipeline (&families[f]->wboit[i]); RCGraphics_DestroyPipeline (&families[f]->moment[i]); RCGraphics_DestroyPipeline (&families[f]->composite[i]);
	}
	if (g->world_layout.handle) vkDestroyPipelineLayout (vulkan_globals.device, g->world_layout.handle, NULL);
	if (g->alias_layout.handle) vkDestroyPipelineLayout (vulkan_globals.device, g->alias_layout.handle, NULL);
	if (g->md5_layout.handle) vkDestroyPipelineLayout (vulkan_globals.device, g->md5_layout.handle, NULL);
	if (g->empty_layout) vkDestroyDescriptorSetLayout (vulkan_globals.device, g->empty_layout, NULL);
	memset (g, 0, sizeof (*g));
}

static bool RCGraphics_Layouts (rc_graphics_t *g, VkDescriptorSetLayout receiver)
{
	VkDescriptorSetLayoutCreateInfo empty = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
	if (vkCreateDescriptorSetLayout (vulkan_globals.device, &empty, NULL, &g->empty_layout) != VK_SUCCESS) return false;
	VkDescriptorSetLayout layouts[3][6] = {
		{vulkan_globals.single_texture_set_layout.handle, vulkan_globals.single_texture_set_layout.handle, vulkan_globals.single_texture_set_layout.handle,
		 vulkan_globals.mboit_input_attachment_set_layout.handle, vulkan_globals.bmodel_instances_set_layout.handle, receiver},
		{vulkan_globals.single_texture_set_layout.handle, vulkan_globals.single_texture_set_layout.handle, vulkan_globals.ubo_set_layout.handle,
		 vulkan_globals.mboit_input_attachment_set_layout.handle, g->empty_layout, receiver},
		{vulkan_globals.single_texture_set_layout.handle, vulkan_globals.single_texture_set_layout.handle, vulkan_globals.ubo_set_layout.handle,
		 vulkan_globals.joints_buffer_set_layout.handle, vulkan_globals.mboit_input_attachment_set_layout.handle, receiver}};
	vulkan_pipeline_layout_t *dst[] = {&g->world_layout, &g->alias_layout, &g->md5_layout};
	for (int i = 0; i < 3; ++i)
	{
		dst[i]->push_constant_range = (VkPushConstantRange){VK_SHADER_STAGE_ALL_GRAPHICS, 0, 22 * sizeof (float)};
		dst[i]->mboit_input_attachment_set = i == 2 ? 4 : 3;
		VkPipelineLayoutCreateInfo info = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
			.setLayoutCount = 6, .pSetLayouts = layouts[i], .pushConstantRangeCount = 1, .pPushConstantRanges = &dst[i]->push_constant_range};
		if (vkCreatePipelineLayout (vulkan_globals.device, &info, NULL, &dst[i]->handle) != VK_SUCCESS) return false;
	}
	return true;
}

static void RCGraphics_DestroyShaders (rc_graphics_shaders_t *s)
{
	VkShaderModule vertices[] = {s->world, s->alias, s->instanced, s->md5, s->md5_8};
	for (size_t i = 0; i < countof (vertices); ++i)
		if (vertices[i]) vkDestroyShaderModule (vulkan_globals.device, vertices[i], NULL);
	for (size_t i = 0; i < countof (s->wf); ++i)
		if (s->wf[i]) vkDestroyShaderModule (vulkan_globals.device, s->wf[i], NULL);
	for (size_t i = 0; i < countof (s->af); ++i)
		if (s->af[i]) vkDestroyShaderModule (vulkan_globals.device, s->af[i], NULL);
	for (size_t i = 0; i < countof (s->mf); ++i)
		if (s->mf[i]) vkDestroyShaderModule (vulkan_globals.device, s->mf[i], NULL);
	memset (s, 0, sizeof (*s));
}

#define RC_MODULE(dst, name) do { (dst) = R_TryCreateShaderModule (name##_spv, name##_spv_size, #name); if (!(dst)) return false; } while (0)
#define RC_FRAGMENTS(backend) \
	RC_MODULE(s->wf[0], world_radiance_##backend##_frag); \
	RC_MODULE(s->wf[1], world_radiance_##backend##_oit_frag); \
	RC_MODULE(s->wf[2], world_radiance_##backend##_mboit_moment_frag); \
	RC_MODULE(s->wf[3], world_radiance_##backend##_mboit_composite_frag); \
	RC_MODULE(s->wf[4], world_radiance_##backend##_mboit_composite_msaa_frag); \
	RC_MODULE(s->af[0], alias_radiance_##backend##_frag); \
	RC_MODULE(s->af[1], alias_radiance_##backend##_alphatest_frag); \
	RC_MODULE(s->af[2], alias_radiance_##backend##_oit_frag); \
	RC_MODULE(s->af[3], alias_radiance_##backend##_alphatest_oit_frag); \
	RC_MODULE(s->af[4], alias_radiance_##backend##_mboit_moment_frag); \
	RC_MODULE(s->af[5], alias_radiance_##backend##_alphatest_mboit_moment_frag); \
	RC_MODULE(s->af[6], alias_radiance_##backend##_mboit_composite_frag); \
	RC_MODULE(s->af[7], alias_radiance_##backend##_alphatest_mboit_composite_frag); \
	RC_MODULE(s->af[8], alias_radiance_##backend##_mboit_composite_msaa_frag); \
	RC_MODULE(s->af[9], alias_radiance_##backend##_alphatest_mboit_composite_msaa_frag); \
	RC_MODULE(s->mf[0], md5_radiance_##backend##_mboit_composite_frag); \
	RC_MODULE(s->mf[1], md5_radiance_##backend##_alphatest_mboit_composite_frag); \
	RC_MODULE(s->mf[2], md5_radiance_##backend##_mboit_composite_msaa_frag); \
	RC_MODULE(s->mf[3], md5_radiance_##backend##_alphatest_mboit_composite_msaa_frag)

static bool RCGraphics_Shaders (rc_graphics_shaders_t *s, bool hardware)
{
	RC_MODULE(s->world, world_radiance_vert); RC_MODULE(s->alias, alias_radiance_vert);
	RC_MODULE(s->instanced, alias_instanced_radiance_vert); RC_MODULE(s->md5, md5_radiance_vert); RC_MODULE(s->md5_8, md5_8_radiance_vert);
	if (hardware) { RC_FRAGMENTS(rt); } else { RC_FRAGMENTS(sw); }
	return true;
}
#undef RC_FRAGMENTS
#undef RC_MODULE

static bool RCGraphics_World (rc_graphics_t *g, const rc_graphics_shaders_t *s, int variant)
{
	pipeline_create_infos_t base, info;
	R_InitDefaultStates (&base);
	base.depth_stencil_state.depthTestEnable = VK_TRUE; base.depth_stencil_state.depthWriteEnable = VK_TRUE;
	base.rasterization_state.depthBiasEnable = VK_TRUE;
	base.dynamic_states[base.dynamic_state.dynamicStateCount++] = VK_DYNAMIC_STATE_DEPTH_BIAS;
	base.vertex_input_state.vertexAttributeDescriptionCount = 3; base.vertex_input_state.pVertexAttributeDescriptions = world_vertex_input_attribute_descriptions;
	base.vertex_input_state.vertexBindingDescriptionCount = 1; base.vertex_input_state.pVertexBindingDescriptions = &world_vertex_binding_description;
	base.shader_stages[0].module = s->world;
	VkSpecializationMapEntry entries[5]; uint32_t values[5] = {0, 0, 0, 0, vulkan_globals.color_format == VK_FORMAT_A2B10G10R10_UNORM_PACK32};
	for (int i = 0; i < 5; ++i) entries[i] = (VkSpecializationMapEntry){i, i * sizeof (uint32_t), sizeof (uint32_t)};
	VkSpecializationInfo spec = {5, entries, sizeof (values), values};
	base.shader_stages[1].pSpecializationInfo = &spec;
	for (int i = 0; i < WORLD_PIPELINE_COUNT; ++i)
	{
		values[0] = i & 1; values[1] = (i >> 1) & 1; values[2] = (i >> 2) & 1; values[3] = (i >> 3) & 1;
		{
			const int v = variant;
			R_CopyPipelineCreateInfos (&info, &base); R_SetPipelineRenderPassVariant (&info, SUBPASS_MAIN, v);
			info.shader_stages[1].module = s->wf[0]; info.blend_attachment_states[0].blendEnable = values[2];
			info.multisample_state.sampleShadingEnable = !values[2] && vulkan_globals.supersampling;
			info.depth_stencil_state.depthWriteEnable = !values[2];
			if (!R_TryCreateGraphicsPipeline (&g->world[v][i], &info, g->world_layout, "radiance_world")) return false;
		}
		if (!values[2]) continue;
		for (int pass = 0; pass < 3; ++pass)
		{
			if ((pass == 0 && variant != MAIN_RENDER_PASS_OIT) ||
				(pass != 0 && variant != MAIN_RENDER_PASS_MBOIT)) continue;
			R_CopyPipelineCreateInfos (&info, &base);
			R_SetPipelineRenderPassVariant (&info, pass == 0 ? SUBPASS_WBOIT : pass == 1 ? SUBPASS_MBOIT_MOMENTS : SUBPASS_MBOIT_COMPOSITE,
				pass == 0 ? MAIN_RENDER_PASS_OIT : MAIN_RENDER_PASS_MBOIT);
			info.color_blend_state.attachmentCount = pass == 0 ? WBOIT_COLOR_ATTACHMENT_COUNT : pass == 1 ? MBOIT_MOMENT_COLOR_ATTACHMENT_COUNT : MBOIT_COMPOSITE_COLOR_ATTACHMENT_COUNT;
			info.shader_stages[1].module = s->wf[pass == 0 ? 1 : pass == 1 ? 2 : vulkan_globals.sample_count == VK_SAMPLE_COUNT_1_BIT ? 3 : 4];
			info.depth_stencil_state.depthWriteEnable = VK_FALSE;
			if (pass == 0) R_SetWBOITBlend (info.blend_attachment_states);
			else if (pass == 1) R_SetMBOITMomentBlend (info.blend_attachment_states);
			else R_SetMBOITCompositeBlend (info.blend_attachment_states);
			vulkan_pipeline_t *target = pass == 0 ? &g->world_wboit[i] : pass == 1 ? &g->world_moment[i] : &g->world_composite[i];
			if (!R_TryCreateGraphicsPipeline (target, &info, g->world_layout, "radiance_world_transparent")) return false;
		}
	}
	return true;
}

static bool RCGraphics_Model (rc_graphics_family_t *family, vulkan_pipeline_layout_t layout, const rc_graphics_shaders_t *s,
	VkShaderModule vertex, VkVertexInputAttributeDescription *attrs, uint32_t attr_count,
	VkVertexInputBindingDescription *bindings, uint32_t binding_count, bool md5, int variant)
{
	pipeline_create_infos_t base, info;
	R_InitDefaultStates (&base); base.depth_stencil_state.depthTestEnable = VK_TRUE; base.depth_stencil_state.depthWriteEnable = VK_TRUE;
	base.vertex_input_state.vertexAttributeDescriptionCount = attr_count; base.vertex_input_state.pVertexAttributeDescriptions = attrs;
	base.vertex_input_state.vertexBindingDescriptionCount = binding_count; base.vertex_input_state.pVertexBindingDescriptions = bindings;
	base.shader_stages[0].module = vertex;
	for (int i = 0; i < MODEL_PIPELINE_SHOWTRIS; ++i)
	{
		const int test = !!(i & MODEL_PIPELINE_ALPHA_TEST_BIT), blend = !!(i & MODEL_PIPELINE_ALPHA_BLEND_BIT);
		{
			const int v = variant;
			R_CopyPipelineCreateInfos (&info, &base); R_SetPipelineRenderPassVariant (&info, SUBPASS_MAIN, v);
			info.shader_stages[1].module = s->af[test]; info.blend_attachment_states[0].blendEnable = blend;
			info.multisample_state.sampleShadingEnable = !blend && vulkan_globals.supersampling; info.depth_stencil_state.depthWriteEnable = !blend;
			if (!R_TryCreateGraphicsPipeline (&family->main[v][i], &info, layout, "radiance_model")) return false;
		}
		if (!blend) continue;
		for (int pass = 0; pass < 3; ++pass)
		{
			if ((pass == 0 && variant != MAIN_RENDER_PASS_OIT) ||
				(pass != 0 && variant != MAIN_RENDER_PASS_MBOIT)) continue;
			R_CopyPipelineCreateInfos (&info, &base);
			R_SetPipelineRenderPassVariant (&info, pass == 0 ? SUBPASS_WBOIT : pass == 1 ? SUBPASS_MBOIT_MOMENTS : SUBPASS_MBOIT_COMPOSITE,
				pass == 0 ? MAIN_RENDER_PASS_OIT : MAIN_RENDER_PASS_MBOIT);
			info.color_blend_state.attachmentCount = pass == 0 ? WBOIT_COLOR_ATTACHMENT_COUNT : pass == 1 ? MBOIT_MOMENT_COLOR_ATTACHMENT_COUNT : MBOIT_COMPOSITE_COLOR_ATTACHMENT_COUNT;
			const int msaa = vulkan_globals.sample_count != VK_SAMPLE_COUNT_1_BIT;
			info.shader_stages[1].module = pass == 0 ? s->af[2 + test] : pass == 1 ? s->af[4 + test] : md5 ? s->mf[2 * msaa + test] : s->af[6 + 2 * msaa + test];
			info.depth_stencil_state.depthWriteEnable = VK_FALSE;
			if (pass == 0) R_SetWBOITBlend (info.blend_attachment_states);
			else if (pass == 1) R_SetMBOITMomentBlend (info.blend_attachment_states);
			else R_SetMBOITCompositeBlend (info.blend_attachment_states);
			vulkan_pipeline_t *target = pass == 0 ? &family->wboit[i] : pass == 1 ? &family->moment[i] : &family->composite[i];
			if (!R_TryCreateGraphicsPipeline (target, &info, layout, "radiance_model_transparent")) return false;
		}
	}
	return true;
}

static bool RCGraphics_Instances (rc_graphics_t *g, const rc_graphics_shaders_t *s, int variant)
{
	VkVertexInputAttributeDescription attrs[7]; VkVertexInputBindingDescription bindings[4];
	memcpy (attrs, alias_vertex_input_attribute_descriptions, sizeof (alias_vertex_input_attribute_descriptions));
	memcpy (bindings, alias_vertex_binding_descriptions, sizeof (alias_vertex_binding_descriptions));
	attrs[5] = (VkVertexInputAttributeDescription){5, 3, VK_FORMAT_R32G32B32_SFLOAT, 0};
	attrs[6] = (VkVertexInputAttributeDescription){6, 3, VK_FORMAT_R32G32B32_SFLOAT, 3 * sizeof (float)};
	bindings[3] = (VkVertexInputBindingDescription){3, 6 * sizeof (float), VK_VERTEX_INPUT_RATE_INSTANCE};
	{
		const int v = variant;
		pipeline_create_infos_t info; R_InitDefaultStates (&info); R_SetPipelineRenderPassVariant (&info, SUBPASS_MAIN, v);
		info.depth_stencil_state.depthTestEnable = VK_TRUE; info.depth_stencil_state.depthWriteEnable = VK_TRUE;
		info.vertex_input_state.vertexAttributeDescriptionCount = countof (attrs); info.vertex_input_state.pVertexAttributeDescriptions = attrs;
		info.vertex_input_state.vertexBindingDescriptionCount = countof (bindings); info.vertex_input_state.pVertexBindingDescriptions = bindings;
		info.shader_stages[0].module = s->instanced; info.shader_stages[1].module = s->af[0];
		info.multisample_state.sampleShadingEnable = vulkan_globals.supersampling;
		if (!R_TryCreateGraphicsPipeline (&g->instanced[v], &info, g->alias_layout, "radiance_instanced")) return false;
	}
	return true;
}

/* A failed new variant has never been published, so only its newly-created
 * pipelines may be destroyed while older submitted frames are in flight. */
static void RCGraphics_DestroyVariant (rc_graphics_t *g, int variant)
{
	for (int i = 0; i < WORLD_PIPELINE_COUNT; ++i)
	{
		RCGraphics_DestroyPipeline (&g->world[variant][i]);
		if (variant == MAIN_RENDER_PASS_OIT) RCGraphics_DestroyPipeline (&g->world_wboit[i]);
		if (variant == MAIN_RENDER_PASS_MBOIT) { RCGraphics_DestroyPipeline (&g->world_moment[i]); RCGraphics_DestroyPipeline (&g->world_composite[i]); }
	}
	RCGraphics_DestroyPipeline (&g->instanced[variant]);
	rc_graphics_family_t *families[] = {&g->alias, &g->md5, &g->md5_8};
	for (int f = 0; f < 3; ++f) for (int i = 0; i < MODEL_PIPELINE_COUNT; ++i)
	{
		RCGraphics_DestroyPipeline (&families[f]->main[variant][i]);
		if (variant == MAIN_RENDER_PASS_OIT) RCGraphics_DestroyPipeline (&families[f]->wboit[i]);
		if (variant == MAIN_RENDER_PASS_MBOIT) { RCGraphics_DestroyPipeline (&families[f]->moment[i]); RCGraphics_DestroyPipeline (&families[f]->composite[i]); }
	}
}

bool RCGraphics_EnsureCurrentVariant (rc_graphics_t *g)
{
	if (!g || !g->ready) return false;
	const int variant = R_UseMBOIT () ? MAIN_RENDER_PASS_MBOIT : R_UseWBOIT () ? MAIN_RENDER_PASS_OIT : MAIN_RENDER_PASS_STANDARD;
	const uint32_t bit = 1u << variant;
	if (g->variants_ready & bit) return true;
	if (g->variants_attempted & bit) return false;
	g->variants_attempted |= bit;
	rc_graphics_shaders_t shaders = {0};
	bool ok = RCGraphics_Shaders (&shaders, g->hardware) &&
		RCGraphics_World (g, &shaders, variant) &&
		RCGraphics_Model (&g->alias, g->alias_layout, &shaders, shaders.alias,
			alias_vertex_input_attribute_descriptions, countof (alias_vertex_input_attribute_descriptions),
			alias_vertex_binding_descriptions, countof (alias_vertex_binding_descriptions), false, variant) &&
		RCGraphics_Model (&g->md5, g->md5_layout, &shaders, shaders.md5,
			md5_vertex_input_attribute_descriptions, countof (md5_vertex_input_attribute_descriptions), &md5_vertex_binding_description, 1, true, variant) &&
		RCGraphics_Model (&g->md5_8, g->md5_layout, &shaders, shaders.md5_8,
			md5_8_vertex_input_attribute_descriptions, countof (md5_8_vertex_input_attribute_descriptions), &md5_8_vertex_binding_description, 1, true, variant) &&
		RCGraphics_Instances (g, &shaders, variant);
	RCGraphics_DestroyShaders (&shaders);
	if (!ok) { RCGraphics_DestroyVariant (g, variant); return false; }
	g->variants_ready |= bit;
	return true;
}

bool RCGraphics_Create (rc_graphics_t *g, VkDescriptorSetLayout receiver_layout, bool hardware)
{
	if (!g || !receiver_layout || g->ready || g->world_layout.handle) return false;
	if (!RCGraphics_Layouts (g, receiver_layout)) { RCGraphics_Destroy (g); return false; }
	g->hardware = hardware;
	g->ready = true;
	if (!RCGraphics_EnsureCurrentVariant (g)) { RCGraphics_Destroy (g); return false; }
	return true;
}

vulkan_pipeline_t RCGraphics_WorldPipeline (const rc_graphics_t *g, subpass_type_t subpass, int variant, int index)
{
	if (!g || !g->ready || variant < 0 || variant >= MAIN_RENDER_PASS_VARIANT_COUNT || index < 0 || index >= WORLD_PIPELINE_COUNT)
		return (vulkan_pipeline_t){0};
	return R_PipelineForSubpassType (subpass, g->world[variant][index], g->world_wboit[index], g->world_moment[index], g->world_composite[index]);
}

vulkan_pipeline_t RCGraphics_ModelPipeline (const rc_graphics_t *g, subpass_type_t subpass, int variant, int index, int model_kind)
{
	if (!g || !g->ready || variant < 0 || variant >= MAIN_RENDER_PASS_VARIANT_COUNT || index < 0 || index >= MODEL_PIPELINE_SHOWTRIS || model_kind < 0 || model_kind > 2)
		return (vulkan_pipeline_t){0};
	const rc_graphics_family_t *family = model_kind == 0 ? &g->alias : model_kind == 1 ? &g->md5 : &g->md5_8;
	return R_PipelineForSubpassType (subpass, family->main[variant][index], family->wboit[index], family->moment[index], family->composite[index]);
}

bool RCGraphics_BindReceiver (cb_context_t *context, VkDescriptorSet descriptor)
{
	if (!context || !context->current_pipeline.handle || !descriptor) return false;
	vulkan_globals.vk_cmd_bind_descriptor_sets (context->cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
		context->current_pipeline.layout.handle, 5, 1, &descriptor, 0, NULL);
	return true;
}


VkDescriptorSet RCGraphics_FrameDescriptor (void) { return rc_graphics_frame && rc_graphics_descriptor ? rc_graphics_descriptor : VK_NULL_HANDLE; }

bool RCGraphics_SetFrame (const rc_graphics_t *graphics, VkDescriptorSet descriptor)
{
	if (!graphics && !descriptor)
	{
		rc_graphics_frame = NULL;
		rc_graphics_descriptor = VK_NULL_HANDLE;
		return true;
	}
	if (!graphics || !graphics->ready || !descriptor)
	{
		rc_graphics_frame = NULL;
		rc_graphics_descriptor = VK_NULL_HANDLE;
		return false;
	}
	rc_graphics_frame = graphics;
	rc_graphics_descriptor = descriptor;
	return true;
}

bool RCGraphics_Active (void)
{
	return rc_graphics_frame && rc_graphics_descriptor && R_RadianceActive ();
}

const rc_graphics_t *RCGraphics_Frame (void)
{
	return RCGraphics_Active () ? rc_graphics_frame : NULL;
}

bool RCGraphics_BindFrame (cb_context_t *context)
{
	return RCGraphics_Active () && RCGraphics_BindReceiver (context, rc_graphics_descriptor);
}
