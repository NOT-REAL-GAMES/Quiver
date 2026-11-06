#include "radiance_receiver_select.h"
#include <string.h>
static struct { uint64_t serial; VkCommandBuffer buffers[RC_RECEIVER_ALTERNATE_LIMIT]; uint32_t count; bool ready; VkRenderPass pass; VkFramebuffer framebuffer; uint32_t subpass; } staged;
void RCReceiverSelect_Reset(void) { memset(&staged,0,sizeof(staged)); }
bool RCReceiverSelect_Stage(uint64_t serial,const VkCommandBuffer *secondary,uint32_t count,bool complete_world,
	VkRenderPass pass,VkFramebuffer framebuffer,uint32_t subpass)
{
	RCReceiverSelect_Reset();
	if (!complete_world || !secondary || !count || count>RC_RECEIVER_ALTERNATE_LIMIT || !pass || !framebuffer) return false;
	for (uint32_t i=0;i<count;++i) if (!secondary[i]) return false;
	memcpy(staged.buffers,secondary,count*sizeof(*secondary)); staged.serial=serial; staged.count=count; staged.pass=pass; staged.framebuffer=framebuffer; staged.subpass=subpass; staged.ready=true; return true;
}
bool RCReceiverSelect_Submit(VkCommandBuffer primary,uint64_t serial,bool receiver_active,
	VkRenderPass pass,VkFramebuffer framebuffer,uint32_t subpass)
{
	bool valid=primary && receiver_active && staged.ready && staged.serial==serial && staged.pass==pass && staged.framebuffer==framebuffer && staged.subpass==subpass;
	if (valid) vkCmdExecuteCommands(primary,staged.count,staged.buffers);
	RCReceiverSelect_Reset();
	return valid;
}
