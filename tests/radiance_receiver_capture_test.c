#include "radiance_receiver_capture.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
int main(void)
{
	rc_receiver_world_draw_t draws[2]={0}, d={.index_count=6};
	rc_receiver_forward_material_t mats[2]={0}, m={.alpha=1};
	rc_receiver_world_capture_t c={.frame_serial=17,.draws=draws,.materials=mats,.capacity=2,.first_token=9};
	RCReceiverCapture_Reset(); assert(RCReceiverCapture_Install(1,&c));
	assert(!RCReceiverCapture_Acquire(1,16) && RCReceiverCapture_Acquire(1,17)==&c);
	rc_receiver_world_draw_t d2[2]={0}; rc_receiver_forward_material_t m2[2]={0};
	rc_receiver_world_capture_t c2={.frame_serial=17,.draws=d2,.materials=m2,.capacity=2,.first_token=10};
	assert(!RCReceiverCapture_Install(2,&c2)); c2.first_token=11; assert(RCReceiverCapture_Install(2,&c2));
	assert(RCReceiverCapture_Add(&c,&d,&m) && RCReceiverCapture_Add(&c,&d,&m));
	assert(c.count==2 && draws[0].token==9 && draws[1].token==10 && mats[1].alpha==1);
	assert(RCReceiverCapture_Add(&c2,&d,&m));
	rc_receiver_world_capture_t *parts[]={&c,&c2}; rc_receiver_world_draw_t merged[3]={0};
	rc_receiver_forward_material_t merged_m[3]={0}; rc_receiver_world_snapshot_t snapshot={0};
	assert(!RCReceiverCapture_Collect(&snapshot,17,merged,merged_m,2,parts,2));
	assert(!snapshot.draws && !merged[0].token);
	assert(RCReceiverCapture_Collect(&snapshot,17,merged,merged_m,3,parts,2));
	assert(snapshot.count==3 && merged[0].token==9 && merged[1].token==10 && merged[2].token==11);
	assert(!RCReceiverCapture_Collect(&snapshot,18,merged,merged_m,3,parts,2));
	assert(!RCReceiverCapture_Collect(&snapshot,17,draws,merged_m,3,parts,2));
	assert(snapshot.count==3 && merged[0].token==9);
	assert(!RCReceiverCapture_Add(&c,&d,&m) && c.failed && c.count==2);
	assert(!RCReceiverCapture_Add(&c,&d,&m));
	c.first_token=UINT32_MAX; assert(!RCReceiverCapture_Install(1,&c)); assert(!RCReceiverCapture_Acquire(1,17));
	c.first_token=100; assert(RCReceiverCapture_Install(1,&c) && !c.failed && c.count==0);
	RCReceiverCapture_Reset(); assert(!RCReceiverCapture_Acquire(1,17));
	puts("PASS: direct-world per-worker capture rejects stale, overflow and failed frames"); return 0;
}
