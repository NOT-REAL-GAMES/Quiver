#include "radiance_receiver_alternates.h"
#include <math.h>
#include <string.h>
void RCReceiverAlternates_Destroy(rc_receiver_alternates_t *a)
{
	if (!a || !a->device) return;
	if (a->pool && a->count) vkFreeCommandBuffers(a->device,a->pool,a->count,a->buffers);
	memset(a,0,sizeof(*a));
}
bool RCReceiverAlternates_Create(rc_receiver_alternates_t *out,VkDevice device,VkCommandPool pool,uint64_t serial,
	VkRenderPass pass,VkFramebuffer framebuffer,uint32_t subpass,VkPipelineLayout layout,const VkPipeline pipelines[4],
	const VkViewport *viewport,const VkRect2D *scissor,const rc_receiver_alternate_part_t *parts,uint32_t count,
	VkQueryPool world_timestamps,uint32_t world_first_query)
{
	if (!out || !device || !pool || !pass || !framebuffer || !layout || !pipelines || !viewport || !scissor || !parts ||
		!count || count>RC_RECEIVER_ALTERNATE_LIMIT || !isfinite(viewport->x) || !isfinite(viewport->y) ||
		!isfinite(viewport->width) || !isfinite(viewport->height) || viewport->width<=0 || viewport->height<=0 ||
		!isfinite(viewport->minDepth) || !isfinite(viewport->maxDepth) || viewport->minDepth<0 || viewport->maxDepth>1 ||
		viewport->minDepth>viewport->maxDepth || !scissor->extent.width || !scissor->extent.height) return false;
	uint32_t last_token=0; uint64_t fingerprint=RC_RECEIVER_FORWARD_HASH_SEED;
	for(uint32_t i=0;i<count;++i) {
		const rc_receiver_world_snapshot_t *s=parts[i].snapshot;
		if (!s || s->frame_serial!=serial || !s->draws || !s->count || !parts[i].materials) return false;
		for(uint32_t j=0;j<s->count;++j) { uint32_t token=s->draws[j].token; if(!token || token<=last_token)return false; last_token=token; }
		fingerprint=RCReceiverForward_Fingerprint(fingerprint,s,parts[i].materials);
	}
	rc_receiver_alternates_t pending={.device=device,.pool=pool,.serial=serial,.fingerprint=fingerprint,.pass=pass,.framebuffer=framebuffer,.subpass=subpass,.count=count};
	VkCommandBufferAllocateInfo alloc={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=pool,
		.level=VK_COMMAND_BUFFER_LEVEL_SECONDARY,.commandBufferCount=count};
	if(vkAllocateCommandBuffers(device,&alloc,pending.buffers)!=VK_SUCCESS) { pending.count=0; goto fail; }
	for(uint32_t i=0;i<count;++i) {
		VkCommandBuffer command=pending.buffers[i];
		if(!command || !RCReceiverForward_CanRecord(command,layout,pipelines,parts[i].snapshot,parts[i].materials,serial))goto fail;
		VkCommandBufferInheritanceInfo inheritance={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO,.renderPass=pass,.subpass=subpass,.framebuffer=framebuffer};
		VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
			.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT|VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT,.pInheritanceInfo=&inheritance};
		if(vkBeginCommandBuffer(command,&begin)!=VK_SUCCESS)goto fail;
		if(world_timestamps && i==0)
			vkCmdWriteTimestamp(command,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,world_timestamps,world_first_query);
		vkCmdSetViewport(command,0,1,viewport); vkCmdSetScissor(command,0,1,scissor);
		RCReceiverForward_Record(command,layout,pipelines,parts[i].snapshot,parts[i].materials);
		if(world_timestamps && i==count-1)
			vkCmdWriteTimestamp(command,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,world_timestamps,world_first_query+1);
		if(vkEndCommandBuffer(command)!=VK_SUCCESS)goto fail;
	}
	RCReceiverAlternates_Destroy(out); *out=pending; return true;
fail:
	RCReceiverAlternates_Destroy(&pending); return false;
}
