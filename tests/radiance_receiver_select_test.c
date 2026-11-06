#include "radiance_receiver_select.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#define H(type,n) ((type)(uintptr_t)(n))
static unsigned executions;
VKAPI_ATTR void VKAPI_CALL vkCmdExecuteCommands(VkCommandBuffer primary,uint32_t count,const VkCommandBuffer *secondary)
{ assert(primary==H(VkCommandBuffer,1) && count==2 && secondary[0]==H(VkCommandBuffer,2) && secondary[1]==H(VkCommandBuffer,3)); ++executions; }
int main(void)
{
	VkCommandBuffer main=H(VkCommandBuffer,1), b[2]={H(VkCommandBuffer,2),H(VkCommandBuffer,3)};
	VkRenderPass pass=H(VkRenderPass,1); VkFramebuffer fb=H(VkFramebuffer,1);
	assert(!RCReceiverSelect_Stage(4,b,2,false,pass,fb,0)); assert(!RCReceiverSelect_Submit(main,4,true,pass,fb,0));
	assert(RCReceiverSelect_Stage(4,b,2,true,pass,fb,0)); assert(!RCReceiverSelect_Submit(main,4,false,pass,fb,0));
	assert(RCReceiverSelect_Stage(4,b,2,true,pass,fb,0)); assert(!RCReceiverSelect_Submit(main,5,true,pass,fb,0));
	assert(RCReceiverSelect_Stage(4,b,2,true,pass,fb,0)); assert(!RCReceiverSelect_Submit(0,4,true,pass,fb,0));
	assert(RCReceiverSelect_Stage(4,b,2,true,pass,fb,0)); assert(!RCReceiverSelect_Submit(main,4,true,pass,H(VkFramebuffer,9),0));
	assert(RCReceiverSelect_Stage(4,b,2,true,pass,fb,0)); assert(!RCReceiverSelect_Submit(main,4,true,pass,fb,1));
	assert(!executions); assert(RCReceiverSelect_Stage(4,b,2,true,pass,fb,0));
	assert(RCReceiverSelect_Submit(main,4,true,pass,fb,0) && executions==1);
	assert(!RCReceiverSelect_Submit(main,4,true,pass,fb,0) && executions==1);
	b[1]=0; assert(!RCReceiverSelect_Stage(4,b,2,true,pass,fb,0)); assert(!RCReceiverSelect_Submit(main,4,true,pass,fb,0));
	puts("PASS: world alternate secondaries submit only after matching receiver pass"); return 0;
}
