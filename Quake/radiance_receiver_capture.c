#include "radiance_receiver_capture.h"
#include <string.h>
static rc_receiver_world_capture_t *slots[RC_RECEIVER_CAPTURE_SLOTS];
void RCReceiverCapture_Reset(void) { memset(slots,0,sizeof(slots)); }
bool RCReceiverCapture_Install(uint32_t slot,rc_receiver_world_capture_t *c)
{
	if (slot>=RC_RECEIVER_CAPTURE_SLOTS) return false;
	slots[slot]=NULL;
	if (!c || !c->draws || !c->materials || !c->capacity || !c->first_token ||
		c->first_token>UINT32_MAX-(c->capacity-1)) return false;
	for (uint32_t i=0;i<RC_RECEIVER_CAPTURE_SLOTS;++i) {
		rc_receiver_world_capture_t *other=slots[i];
		if (!other || other->frame_serial!=c->frame_serial) continue;
		uint64_t a=c->first_token, b=other->first_token;
		if (other==c || (a<b+other->capacity && b<a+c->capacity)) return false;
	}
	c->count=0; c->failed=false; slots[slot]=c; return true;
}
rc_receiver_world_capture_t *RCReceiverCapture_Acquire(uint32_t slot,uint64_t serial)
{
	if (slot>=RC_RECEIVER_CAPTURE_SLOTS || !slots[slot] || slots[slot]->frame_serial!=serial) return NULL;
	return slots[slot];
}
bool RCReceiverCapture_Add(rc_receiver_world_capture_t *c,const rc_receiver_world_draw_t *draw,const rc_receiver_forward_material_t *material)
{
	if (!c) return false;
	if (c->failed || !draw || !material || !c->draws || !c->materials || c->count>=c->capacity ||
		!c->first_token || c->first_token>UINT32_MAX-c->count) { c->failed=true; return false; }
	uint32_t n=c->count; c->draws[n]=*draw; c->draws[n].token=c->first_token+n; c->materials[n]=*material; ++c->count; return true;
}

static bool disjoint(const void *a,size_t as,const void *b,size_t bs)
{
	uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;
	return x<=UINTPTR_MAX-as && y<=UINTPTR_MAX-bs && !(x<y+bs && y<x+as);
}
bool RCReceiverCapture_Collect(rc_receiver_world_snapshot_t *snapshot,uint64_t serial,
	rc_receiver_world_draw_t *draw_storage,rc_receiver_forward_material_t *material_storage,uint32_t capacity,
	rc_receiver_world_capture_t *const *captures,uint32_t capture_count)
{
	if (!snapshot || !draw_storage || !material_storage || !capacity || !captures || !capture_count) return false;
	uint64_t count=0, last_token=0;
	for (uint32_t i=0;i<capture_count;++i) {
		const rc_receiver_world_capture_t *c=captures[i];
		if (!c || c->failed || c->frame_serial!=serial || !c->draws || !c->materials || c->count>c->capacity) return false;
		for (uint32_t j=0;j<c->count;++j) {
			uint32_t token=c->draws[j].token;
			if (!token || token<=last_token) return false;
			last_token=token;
		}
		count+=c->count;
		if (count>capacity) return false;
	}
	if (!count || count>SIZE_MAX/sizeof(*draw_storage) || count>SIZE_MAX/sizeof(*material_storage)) return false;
	size_t draw_bytes=(size_t)count*sizeof(*draw_storage), material_bytes=(size_t)count*sizeof(*material_storage);
	if (!disjoint(draw_storage,draw_bytes,material_storage,material_bytes)) return false;
	for (uint32_t i=0;i<capture_count;++i) {
		const rc_receiver_world_capture_t *c=captures[i];
		size_t a=(size_t)c->count*sizeof(*c->draws), b=(size_t)c->count*sizeof(*c->materials);
		if (!disjoint(draw_storage,draw_bytes,c->draws,a) || !disjoint(draw_storage,draw_bytes,c->materials,b) ||
			!disjoint(material_storage,material_bytes,c->draws,a) || !disjoint(material_storage,material_bytes,c->materials,b)) return false;
	}
	uint32_t next=0;
	for (uint32_t i=0;i<capture_count;++i) {
		const rc_receiver_world_capture_t *c=captures[i];
		memcpy(draw_storage+next,c->draws,(size_t)c->count*sizeof(*c->draws));
		memcpy(material_storage+next,c->materials,(size_t)c->count*sizeof(*c->materials));
		next+=c->count;
	}
	*snapshot=(rc_receiver_world_snapshot_t){.frame_serial=serial,.draws=draw_storage,.count=next};
	return true;
}
