#ifndef RADIANCE_RECEIVER_MODEL_H
#define RADIANCE_RECEIVER_MODEL_H
#include <vulkan/vulkan.h>
#include <stdbool.h>
#include <stdint.h>

#define RC_RECEIVER_MODEL_KINDS 3u
#define RC_RECEIVER_MODEL_CAPTURE_SLOTS 32u
/* A packet is one actual opaque alias/MD3/MD5 draw. Every borrowed buffer,
 * dynamic UBO range and descriptor remains fenced until both passes finish. */
typedef struct {
	VkBuffer vertices[3], index_buffer;
	VkDeviceSize vertex_offsets[3], index_offset;
	VkDescriptorSet sets[4], lighting_set;
	VkPipeline fallback_pipeline;
	VkPipelineLayout fallback_layout;
	float constants[20]; /* MVP, fog RGB, fog density; token is appended at80. */
	float alpha;
	uint32_t kind, token, vertex_count, set_count, uniform_offset, shader_flags;
	uint32_t index_count, first_index;
	int32_t vertex_offset;
	bool alpha_test, reuse_eligible;
} rc_receiver_model_draw_t;
typedef struct rc_receiver_model_capture_s {
	uint64_t frame_serial;
	rc_receiver_model_draw_t *draws;
	uint32_t count, capacity, first_token;
	bool failed;
} rc_receiver_model_capture_t;
typedef struct {
	uint64_t frame_serial;
	const rc_receiver_model_draw_t *draws;
	uint32_t count;
} rc_receiver_model_snapshot_t;
/* The ordered stream also retains native blended/debug surfaces. Only these
 * eligible draws participate in prepass lighting or count as accelerated. */
static inline uint32_t RCReceiverModel_EligibleCount(const rc_receiver_model_snapshot_t *snapshot)
{
	uint32_t count=0;
	if(snapshot && snapshot->draws)for(uint32_t i=0;i<snapshot->count;++i)count+=snapshot->draws[i].reuse_eligible;
	return count;
}

typedef struct {
	VkDevice device;
	VkDescriptorSetLayout empty_layout;
	VkPipelineLayout layouts[2]; /* alias, shared MD5/8 */
	VkPipeline prepass[3][2], forward[3][2];
	VkRenderPass receiver_pass, forward_pass;
	uint32_t forward_subpass;
	uint64_t attempted_revision;
} rc_receiver_model_pipelines_t;
typedef struct {
	VkDevice device; VkCommandPool pool;
	uint64_t serial, fingerprint;
	VkRenderPass pass; VkFramebuffer framebuffer; uint32_t subpass;
	VkCommandBuffer buffers[1]; uint32_t count;
} rc_receiver_model_alternates_t;

bool RCReceiverModelCapture_Install(uint32_t slot,rc_receiver_model_capture_t *capture);
rc_receiver_model_capture_t *RCReceiverModelCapture_Acquire(uint32_t slot,uint64_t serial);
void RCReceiverModelCapture_Reset(void);
bool RCReceiverModelCapture_Add(rc_receiver_model_capture_t *capture,const rc_receiver_model_draw_t *draw);
bool RCReceiverModelCapture_Collect(rc_receiver_model_snapshot_t *snapshot,uint64_t serial,
	rc_receiver_model_draw_t *storage,uint32_t capacity,rc_receiver_model_capture_t *const *parts,uint32_t count);
bool RCReceiverModel_DrawValid(const rc_receiver_model_draw_t *draw);
uint64_t RCReceiverModel_Fingerprint(const rc_receiver_model_snapshot_t *snapshot);
bool RCReceiverModel_CanRecord(VkCommandBuffer command,const rc_receiver_model_pipelines_t *pipelines,
	const rc_receiver_model_snapshot_t *snapshot,uint64_t serial,bool forward,VkDescriptorSet reuse);
void RCReceiverModel_Record(VkCommandBuffer command,const rc_receiver_model_pipelines_t *pipelines,
	const rc_receiver_model_snapshot_t *snapshot,bool forward,VkDescriptorSet reuse);
/* model[alias/MD5][0..4] matches the existing native descriptor prefix. Alias
 * set4 may be NULL: an owned empty layout is substituted transactionally. */
bool RCReceiverModelPipelines_Create(rc_receiver_model_pipelines_t *out,VkDevice device,VkPipelineCache cache,
	const VkDescriptorSetLayout model[2][5],VkDescriptorSetLayout lighting,VkDescriptorSetLayout receiver,
	VkRenderPass prepass,VkRenderPass forward,uint32_t subpass,
	const VkGraphicsPipelineCreateInfo prepass_info[3][2],const VkGraphicsPipelineCreateInfo forward_info[3][2]);
void RCReceiverModelPipelines_Destroy(rc_receiver_model_pipelines_t *pipelines);
bool RCReceiverModelAlternates_Create(rc_receiver_model_alternates_t *out,VkDevice device,VkCommandPool pool,
	uint64_t serial,VkRenderPass pass,VkFramebuffer framebuffer,uint32_t subpass,
	const rc_receiver_model_pipelines_t *pipelines,const rc_receiver_model_snapshot_t *snapshot,
	VkDescriptorSet reuse,VkDescriptorSet lighting,const VkViewport *viewport,const VkRect2D *scissor,
	VkQueryPool timestamps,uint32_t end_query);
void RCReceiverModelAlternates_Destroy(rc_receiver_model_alternates_t *alternates);
#endif
