#include "radiance_receiver_shaders.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#define BLOB(n,id) const unsigned char n##_spv[4]={id,0,0,0}; const int n##_spv_size=4
BLOB(world_radiance_vert,1); BLOB(radiance_receiver_world_frag,2); BLOB(radiance_receiver_half_comp,3);
BLOB(radiance_receiver_direct_sw_comp,4); BLOB(radiance_receiver_direct_rt_comp,5);
BLOB(world_receiver_forward_sw_frag,6); BLOB(world_receiver_forward_rt_frag,7);
BLOB(radiance_receiver_reconstruct_comp,8);
BLOB(radiance_receiver_direct_fast_sw_comp,9); BLOB(radiance_receiver_direct_fast_rt_comp,10);
BLOB(world_receiver_forward_fast_sw_frag,11); BLOB(world_receiver_forward_fast_rt_frag,12);
BLOB(radiance_receiver_filter_comp,13);
BLOB(radiance_receiver_half_fast_comp,14);
BLOB(radiance_receiver_soft_sw_comp,15); BLOB(radiance_receiver_soft_rt_comp,16);
#define H(type,n) ((type)(uintptr_t)(n))
static unsigned calls,fail,destroyed; static bool hardware,texture_filter;
static bool soft_creation,fail_compute,partial_compute;
static unsigned pipeline_calls,pipeline_destroyed;
VKAPI_ATTR VkResult VKAPI_CALL vkCreateShaderModule(VkDevice d,const VkShaderModuleCreateInfo *i,const VkAllocationCallbacks *a,VkShaderModule *out)
{ (void)d;(void)a; ++calls; if(calls==fail)return VK_ERROR_OUT_OF_HOST_MEMORY; assert(i->codeSize==4 && ((uintptr_t)i->pCode%4)==0); unsigned id=*(const unsigned char*)i->pCode; const unsigned expected[]={1,2,texture_filter?14u:3u,texture_filter?(hardware?10u:9u):(hardware?5u:4u),texture_filter?13u:8u,texture_filter?(hardware?12u:11u):(hardware?7u:6u)}; assert(id==(soft_creation?(hardware?16u:15u):expected[(calls-1)%6])); *out=H(VkShaderModule,calls);return VK_SUCCESS; }
VKAPI_ATTR void VKAPI_CALL vkDestroyShaderModule(VkDevice d,VkShaderModule s,const VkAllocationCallbacks *a)
{ (void)d;(void)s;(void)a;++destroyed; }
VKAPI_ATTR VkResult VKAPI_CALL vkCreateComputePipelines(VkDevice d,VkPipelineCache c,uint32_t count,const VkComputePipelineCreateInfo *i,const VkAllocationCallbacks *a,VkPipeline *out)
{ (void)d;(void)c;(void)a;assert(soft_creation && count==1 && i->layout==H(VkPipelineLayout,9) && i->stage.module==H(VkShaderModule,1));++pipeline_calls;
 if(!fail_compute || partial_compute)*out=H(VkPipeline,77);
 return fail_compute?VK_ERROR_OUT_OF_HOST_MEMORY:VK_SUCCESS; }
VKAPI_ATTR void VKAPI_CALL vkDestroyPipeline(VkDevice d,VkPipeline p,const VkAllocationCallbacks *a)
{ (void)d;(void)a;assert(p==H(VkPipeline,77));++pipeline_destroyed; }
int main(void)
{
	rc_receiver_shaders_t s={0}; VkDevice device=H(VkDevice,1);
	for(unsigned mode=0;mode<4;++mode) {
		hardware=(mode&1)!=0; texture_filter=(mode&2)!=0;
		for(unsigned f=1;f<=6;++f) {
			calls=destroyed=0;fail=f;
			assert(!RCReceiverShaders_Create(&s,device,hardware,texture_filter));
			assert(!s.device && destroyed==f-1);
		}
		calls=fail=0; assert(RCReceiverShaders_Create(&s,device,hardware,texture_filter)); assert(s.forward);
		VkShaderModule prior=s.forward;
		calls=0;fail=4;assert(!RCReceiverShaders_Create(&s,device,hardware,texture_filter));assert(s.forward==prior);
		RCReceiverShaders_Destroy(&s); assert(!s.device);
	}
	soft_creation=true;
	for(unsigned backend=0;backend<2;++backend) {
		hardware=backend!=0;VkPipeline pipeline=VK_NULL_HANDLE;
		calls=destroyed=pipeline_calls=pipeline_destroyed=0;fail=1;
		assert(!RCReceiverShaders_CreateSoftPipeline(&pipeline,device,0,H(VkPipelineLayout,9),hardware) && !pipeline && !pipeline_calls && !destroyed);
		calls=fail=0;fail_compute=true;partial_compute=false;
		assert(!RCReceiverShaders_CreateSoftPipeline(&pipeline,device,0,H(VkPipelineLayout,9),hardware) && !pipeline && destroyed==1 && pipeline_calls==1);
		calls=0;partial_compute=true;
		assert(!RCReceiverShaders_CreateSoftPipeline(&pipeline,device,0,H(VkPipelineLayout,9),hardware) && !pipeline && destroyed==2 && pipeline_destroyed==1);
		calls=0;fail_compute=partial_compute=false;
		assert(RCReceiverShaders_CreateSoftPipeline(&pipeline,device,0,H(VkPipelineLayout,9),hardware) && pipeline && destroyed==3);
		vkDestroyPipeline(device,pipeline,NULL);assert(pipeline_destroyed==2);
	}
	puts("PASS: receiver shader modules choose backend/filter and preserve prior modules on failure");return 0;
}
