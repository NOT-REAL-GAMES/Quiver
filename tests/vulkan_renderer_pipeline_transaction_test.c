/* Exercise actual renderer pipeline groups with synthetic Vulkan objects. */
#include <stdlib.h>
static void *test_realloc (void *pointer, size_t size);
#define realloc test_realloc
#include "../Quake/gl_rmisc.c"
#undef realloc
#include <assert.h>
#include <stdarg.h>
#include <stdint.h>

cvar_t r_ssao;
viddef_t vid;
vulkan_pipeline_layout_t ssao_layout, ssao_compute_layout;
vulkan_pipeline_t ssao_pipelines[MAIN_RENDER_PASS_VARIANT_COUNT], ssao_prepare_pipeline, ssao_filter_pipeline;
vulkan_pipeline_t ssao_evaluate_pipelines[3], ssao_mip_pipeline;
static unsigned pipeline_calls, fail_at, serial, live_pipelines, live_modules, subsystem_resets;
static unsigned realloc_calls, fail_realloc_at;
static unsigned node_allocations, fail_node_at, live_nodes;
static bool handles[8192], modules[256], optional_calls[8192];

static void *test_realloc (void *pointer, size_t size)
{
    if (++realloc_calls >= fail_realloc_at && fail_realloc_at) return NULL;
    return realloc (pointer, size);
}

void Sys_Printf (const char *format, ...) { (void)format; }
void Con_Warning (const char *format, ...) { (void)format; }
void R_RadianceReset (const char *reason) { (void)reason; ++subsystem_resets; }
void R_RadianceSetReceiverUse (float divisor, bool checkerboard, bool texture_filter) { (void)divisor; (void)checkerboard; (void)texture_filter; }
void R_RadianceSetReceiverSoftUse (bool active) { (void)active; }
void R_ECSGPUShutdown (void) { ++subsystem_resets; }
void Cvar_SetValueQuick (cvar_t *var, float value) { var->value = value; }
void *Mem_Alloc (const size_t size)
{
    if (++node_allocations == fail_node_at) return NULL;
    void *pointer = calloc (1, size);
    if (pointer) ++live_nodes;
    return pointer;
}
void Mem_Free (const void *pointer)
{
    if (pointer) { assert (live_nodes); --live_nodes; }
    free ((void *)pointer);
}
char *va (const char *format, ...) { static char result[256]; va_list args; va_start (args, format); vsnprintf (result, sizeof (result), format, args); va_end (args); return result; }
int q_snprintf (char *buffer, size_t size, const char *format, ...) { va_list args; va_start (args, format); int result = vsnprintf (buffer, size, format, args); va_end (args); return result; }
const char *COM_GetWriteRoot (void) { return ""; }
FILE *Sys_fopen (const char *path, const char *mode) { (void)path; (void)mode; return NULL; }
void GL_SetObjectName (uint64_t object, VkObjectType type, const char *name)
{
    (void)object;
    if (type == VK_OBJECT_TYPE_PIPELINE)
        optional_calls[pipeline_calls] = !strncmp (name, "world_mesh", 10) || !strncmp (name, "world_task", 10) ||
            !strncmp (name, "alias_instanced", 15) || !strncmp (name, "ssao_", 5) ||
            !strcmp (name, "meshlet_cull") || !strcmp (name, "meshlet_hiz") || !strcmp (name, "alias_ecs_cull");
}
const render_pass_binding_t *R_RenderPassBinding (subpass_type_t stage, main_render_pass_variant_t variant)
{
    (void)variant;
    static render_pass_binding_t binding = {.render_pass = {(VkRenderPass)(uintptr_t)1, (VkRenderPass)(uintptr_t)1}};
    binding.subpass = stage; return &binding;
}
const render_pass_binding_t *R_RenderPassAlternative (VkRenderPass pass, uint32_t subpass, uint32_t index)
{
    (void)pass;
    static render_pass_binding_t binding = {.render_pass = {(VkRenderPass)(uintptr_t)2, (VkRenderPass)(uintptr_t)2}};
    binding.subpass = subpass; return index == 1 ? &binding : NULL;
}
static VkResult make_pipeline (VkPipeline *out)
{
    if (++pipeline_calls == fail_at) { *out = VK_NULL_HANDLE; return VK_ERROR_OUT_OF_DEVICE_MEMORY; }
    unsigned id = ++serial; assert (id < 8192 && !handles[id]); handles[id] = true; ++live_pipelines;
    *out = (VkPipeline)(uintptr_t)id; return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateGraphicsPipelines (VkDevice device, VkPipelineCache cache, uint32_t count,
    const VkGraphicsPipelineCreateInfo *info, const VkAllocationCallbacks *allocator, VkPipeline *out)
{ (void)device; (void)cache; (void)allocator; assert (count == 1 && info->stageCount >= 1); return make_pipeline (out); }
VKAPI_ATTR VkResult VKAPI_CALL vkCreateComputePipelines (VkDevice device, VkPipelineCache cache, uint32_t count,
    const VkComputePipelineCreateInfo *info, const VkAllocationCallbacks *allocator, VkPipeline *out)
{ (void)device; (void)cache; (void)allocator; assert (count == 1 && info->layout); return make_pipeline (out); }
VKAPI_ATTR void VKAPI_CALL vkDestroyPipeline (VkDevice device, VkPipeline pipeline, const VkAllocationCallbacks *allocator)
{
    (void)device; (void)allocator; if (!pipeline) return;
    unsigned id = (unsigned)(uintptr_t)pipeline; assert (id < 8192 && handles[id]); handles[id] = false; --live_pipelines;
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateShaderModule (VkDevice device, const VkShaderModuleCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkShaderModule *out)
{
    (void)device; (void)allocator; assert (info->codeSize == 4); unsigned id = ++live_modules; assert (id < 256 && !modules[id]); modules[id] = true;
    *out = (VkShaderModule)(uintptr_t)id; return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyShaderModule (VkDevice device, VkShaderModule module, const VkAllocationCallbacks *allocator)
{
    (void)device; (void)allocator; unsigned id = (unsigned)(uintptr_t)module; assert (id < 256 && modules[id]); modules[id] = false; --live_modules;
}

static void initialize (void)
{
    VkPipelineLayout layout = (VkPipelineLayout)(uintptr_t)1;
    vid.width = 640; vid.height = 360; vid.render_width = 320; vid.render_height = 180;
    vulkan_globals.sample_count = VK_SAMPLE_COUNT_4_BIT;
    vulkan_globals.non_solid_fill = true;
    vulkan_globals.ray_query = true;
    vulkan_globals.basic_pipeline_layout.handle = layout;
    vulkan_globals.gui_pipeline_layout.handle = layout;
    vulkan_globals.world_pipeline_layout.handle = layout;
    vulkan_globals.fte_particle_pipeline_layout.handle = layout;
    vulkan_globals.sky_pipeline_layout[0].handle = vulkan_globals.sky_pipeline_layout[1].handle = layout;
    vulkan_globals.alias_pipelines[MAIN_RENDER_PASS_STANDARD][0].layout.handle = layout;
    vulkan_globals.md5_pipelines[MAIN_RENDER_PASS_STANDARD][0].layout.handle = layout;
    vulkan_globals.md5_8_pipelines[MAIN_RENDER_PASS_STANDARD][0].layout.handle = layout;
    vulkan_globals.postprocess_pipeline.layout.handle = layout;
    vulkan_globals.wboit_resolve_pipeline.layout.handle = layout;
    vulkan_globals.mboit_resolve_pipeline.layout.handle = layout;
    vulkan_globals.cs_tex_warp_pipeline.layout.handle = layout;
    vulkan_globals.screen_effects_pipeline.layout.handle = layout;
    vulkan_globals.update_lightmap_pipeline.layout.handle = layout;
    vulkan_globals.update_lightmap_rt_pipeline.layout.handle = layout;
    vulkan_globals.indirect_draw_pipeline.layout.handle = layout;
    vulkan_globals.indirect_clear_pipeline.layout.handle = layout;
    vulkan_globals.meshlet_cull_pipeline.layout.handle = layout;
    vulkan_globals.meshlet_hiz_pipeline.layout.handle = layout;
    vulkan_globals.ecs_cull_pipeline.layout.handle = layout;
    vulkan_globals.ray_debug_pipeline.layout.handle = layout;
    vulkan_globals.mesh_interpolate_pipeline.layout.handle = layout;
    vulkan_globals.skinning_pipeline.layout.handle = layout;
    vulkan_globals.skinning_8_pipeline.layout.handle = layout;
    ssao_layout.handle = layout; ssao_prepare_pipeline.layout.handle = ssao_mip_pipeline.layout.handle = layout;
    ssao_filter_pipeline.layout.handle = layout;
    for (int i = 0; i < 3; ++i) ssao_evaluate_pipelines[i].layout.handle = layout;
#if defined(VK_EXT_mesh_shader) && defined(HAVE_WORLD_MESH_SHADER)
    vulkan_globals.mesh_shader = true; vulkan_globals.world_mesh_pipeline_layout.handle = layout;
#if defined(HAVE_WORLD_TASK_SHADER)
    vulkan_globals.task_shader = true; vulkan_globals.world_task_pipeline_layout.handle = layout;
#endif
#endif
    pipeline_cache_attempted = true;
}
int main (void)
{
    initialize ();
    for (int ssao = 0; ssao < 2; ++ssao)
    for (int hdr = 0; hdr < 2; ++hdr)
    {
    vulkan_globals.scene_hdr_active = hdr;
    /* Native-resolution HDR still needs the scene-to-UI composition pass. */
    vid.render_width = hdr ? vid.width : 320;
    vid.render_height = hdr ? vid.height : 180;
    r_ssao.value = ssao;
    vulkan_globals.ssao_broken = false;
    pipeline_calls = serial = fail_at = realloc_calls = fail_realloc_at = node_allocations = fail_node_at = 0;
    pipeline_cache_attempted = true;
    memset (optional_calls, 0, sizeof (optional_calls));
    assert (R_TryCreatePipelines ());
    for (int variant = 0; variant < MAIN_RENDER_PASS_VARIANT_COUNT; ++variant)
        assert (graphics_pipelines[PIPELINE_SCENE_UPSCALE][SUBPASS_UI][variant].handle);
    const unsigned total = pipeline_calls;
    const unsigned transaction_allocations = realloc_calls;
    const unsigned alternative_allocations = node_allocations;
    bool optional[8192]; memcpy (optional, optional_calls, sizeof (optional));
    assert (total > 100 && total < 8192 && !live_modules && live_pipelines == total);
    assert (!R_TryCreatePipelines () && live_pipelines == total);
    R_DestroyPipelines (); assert (!live_pipelines && !live_nodes && subsystem_resets == 2);
    for (unsigned failure = 1; failure <= total; ++failure)
    {
        r_ssao.value = ssao; vulkan_globals.ssao_broken = false;
        pipeline_calls = serial = 0; fail_at = failure; subsystem_resets = 0; pipeline_cache_attempted = true;
        bool ok = R_TryCreatePipelines ();
        assert (ok == optional[failure]);
        assert (!live_modules);
        if (ok) R_DestroyPipelines ();
        else assert (!live_pipelines && !live_nodes && !pipeline_instances && subsystem_resets == 0 && r_ssao.value == ssao && !vulkan_globals.ssao_broken);
        assert (!live_pipelines && !live_nodes);
        r_ssao.value = ssao; vulkan_globals.ssao_broken = false;
        pipeline_calls = serial = fail_at = 0; pipeline_cache_attempted = true;
        assert (R_TryCreatePipelines () && !live_modules);
        R_DestroyPipelines (); assert (!live_pipelines && !live_nodes && !pipeline_instances);
    }
    for (unsigned failure = 1; failure <= transaction_allocations; ++failure)
    {
        r_ssao.value = ssao; vulkan_globals.ssao_broken = false;
        pipeline_calls = serial = fail_at = subsystem_resets = realloc_calls = 0;
        fail_realloc_at = failure; pipeline_cache_attempted = true;
        assert (!R_TryCreatePipelines () && !live_modules && !live_pipelines && !live_nodes && !pipeline_instances && !subsystem_resets);
        assert (r_ssao.value == ssao && !vulkan_globals.ssao_broken);
        pipeline_calls = serial = realloc_calls = fail_realloc_at = 0; pipeline_cache_attempted = true;
        assert (R_TryCreatePipelines ());
        R_DestroyPipelines (); assert (!live_pipelines && !live_nodes && !pipeline_instances);
    }
    for (unsigned failure = 1; failure <= alternative_allocations; ++failure)
    {
        r_ssao.value = ssao; vulkan_globals.ssao_broken = false;
        pipeline_calls = serial = fail_at = subsystem_resets = node_allocations = 0;
        fail_node_at = failure; pipeline_cache_attempted = true;
        bool ok = R_TryCreatePipelines ();
        assert (!live_modules);
        if (ok) R_DestroyPipelines ();
        else assert (!live_pipelines && !live_nodes && !pipeline_instances && !subsystem_resets &&
            r_ssao.value == ssao && !vulkan_globals.ssao_broken);
        assert (!live_pipelines && !live_nodes && !pipeline_instances);
        r_ssao.value = ssao; vulkan_globals.ssao_broken = false;
        pipeline_calls = serial = node_allocations = fail_node_at = 0; pipeline_cache_attempted = true;
        assert (R_TryCreatePipelines () && !live_modules);
        R_DestroyPipelines (); assert (!live_pipelines && !live_nodes && !pipeline_instances);
    }
    printf ("PASS: %u pipeline and %u alternative-node failure positions roll back and retry (SSAO %d, HDR %d)\n",
        total, alternative_allocations, ssao, hdr);
    subsystem_resets = 0;
    }
    return 0;
}
