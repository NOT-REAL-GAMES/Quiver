#include "radiance_receiver_snapshot.h"
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
static bool reject;
static unsigned stages;
static rc_receiver_frame_plan_t staged;
bool RCReceiverWorld_CanRecord(VkCommandBuffer command,VkPipelineLayout layout,const VkPipeline pipelines[2],const rc_receiver_world_draw_t *draws,uint32_t count)
{ (void)command;(void)layout;(void)pipelines; return !reject && count==1 && draws[0].token!=0; }
void RCReceiverFrame_Stage(const rc_receiver_frame_plan_t *p)
{ ++stages; if (p) staged=*p; else staged=(rc_receiver_frame_plan_t){0}; }
int main(void)
{
	rc_receiver_world_draw_t in[3]={{0}}, out[3]={{0}};
	rc_receiver_world_snapshot_t s={0}; VkPipeline pipelines[2]={0};
	assert(RCReceiverSnapshot_Build(&s,41,out,3,in,3,19,0,0,pipelines));
	assert(s.frame_serial==41 && s.draws==out && s.count==3 && out[0].token==19 && out[1].token==20 && out[2].token==21);
	reject=true; assert(!RCReceiverSnapshot_Build(&s,42,out,3,in,3,100,0,0,pipelines));
	assert(s.frame_serial==41 && out[0].token==19 && out[2].token==21); reject=false;
	assert(!RCReceiverSnapshot_Build(&s,42,out,2,in,3,100,0,0,pipelines));
	assert(!RCReceiverSnapshot_Build(&s,42,out,3,in,3,UINT32_MAX-1,0,0,pipelines));
	assert(!RCReceiverSnapshot_Build(&s,42,out+1,2,out,2,100,0,0,pipelines));
	assert(s.frame_serial==41 && out[0].token==19 && out[2].token==21);
	rc_receiver_frame_plan_t p={.frame_serial=41,.enabled=true}; assert(RCReceiverSnapshot_Stage(&s,&p));
	assert(stages==1 && staged.world_draws==out && staged.world_draw_count==3 && staged.enabled);
	p.frame_serial=42; assert(!RCReceiverSnapshot_Stage(&s,&p)); assert(stages==2 && !staged.enabled);
	puts("PASS: direct-world snapshots validate before copying and stage only on their frame");
	return 0;
}
