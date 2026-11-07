#include "radiance_receiver_pipelines.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

static unsigned live, call, fail_at, serial;
static VkResult create(VkPipeline *out)
{
	++call;
	/* Exercise cleanup even when a failing create returns a partial handle. */
	*out=(VkPipeline)(uintptr_t)++serial; ++live;
	return call==fail_at ? VK_ERROR_OUT_OF_HOST_MEMORY : VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateGraphicsPipelines(VkDevice d,VkPipelineCache c,uint32_t n,
	const VkGraphicsPipelineCreateInfo *p,const VkAllocationCallbacks *a,VkPipeline *out)
{ (void)d;(void)c;(void)p;(void)a; assert(n==1); return create(out); }
VKAPI_ATTR VkResult VKAPI_CALL vkCreateComputePipelines(VkDevice d,VkPipelineCache c,uint32_t n,
	const VkComputePipelineCreateInfo *p,const VkAllocationCallbacks *a,VkPipeline *out)
{ (void)d;(void)c;(void)p;(void)a; assert(n==1); return create(out); }
VKAPI_ATTR void VKAPI_CALL vkDestroyPipeline(VkDevice d,VkPipeline p,const VkAllocationCallbacks *a)
{ (void)d;(void)p;(void)a; assert(live); --live; }

int main(void)
{
	VkDevice device=(VkDevice)(uintptr_t)1;
	VkPipelineMultisampleStateCreateInfo ms={.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT};
	VkPipelineDepthStencilStateCreateInfo depth={.depthTestEnable=VK_TRUE,.depthWriteEnable=VK_TRUE};
	VkPipelineColorBlendAttachmentState colors[2]={0};
	VkPipelineColorBlendStateCreateInfo blend={.attachmentCount=2,.pAttachments=colors};
	VkGraphicsPipelineCreateInfo raster={.layout=(VkPipelineLayout)(uintptr_t)2,.renderPass=(VkRenderPass)(uintptr_t)3,
		.pMultisampleState=&ms,.pDepthStencilState=&depth,.pColorBlendState=&blend};
	VkComputePipelineCreateInfo compute={.layout=(VkPipelineLayout)(uintptr_t)4};
	rc_receiver_pipelines_t p={0};
	assert(RCReceiverPipelines_Create(&p,device,VK_NULL_HANDLE,&raster,&compute,&compute));
	VkPipeline old=p.raster; assert(live==3);
	for(unsigned i=1;i<=3;++i) {
		call=0; fail_at=i;
		assert(!RCReceiverPipelines_Create(&p,device,VK_NULL_HANDLE,&raster,&compute,&compute));
		assert(p.raster==old && live==3);
	}
	fail_at=0; call=0;
	ms.rasterizationSamples=(VkSampleCountFlagBits)3;
	assert(!RCReceiverPipelines_Create(&p,device,VK_NULL_HANDLE,&raster,&compute,&compute) && !call && live==3);
	ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT; colors[1].blendEnable=VK_TRUE;
	assert(!RCReceiverPipelines_Create(&p,device,VK_NULL_HANDLE,&raster,&compute,&compute) && !call && live==3);
	colors[1].blendEnable=VK_FALSE;
	assert(RCReceiverPipelines_Create(&p,device,VK_NULL_HANDLE,&raster,&compute,&compute));
	assert(p.raster!=old && live==3);
	RCReceiverPipelines_Destroy(&p); assert(live==0);
	puts("PASS: receiver pipeline creation rolls back partial failures and unsupported states");
	return 0;
}
