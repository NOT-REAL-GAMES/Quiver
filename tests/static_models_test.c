#include "static_models.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* This target isolates the binary format; collision has its own tests. */
void QSM_FreeCollision (qsm_collision_t *collision) { assert(!collision); }
static void put32 (uint8_t *p,uint32_t n)
{ for(unsigned i=0;i<4;++i) p[i]=(uint8_t)(n>>(i*8)); }
static uint32_t get32 (const uint8_t *p)
{ return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }

int main(void)
{
	const char *valid[]={"progs/sponza.md5mesh","progs/props/crate.MD3","progs/a b.mdl"};
	const char *invalid[]={"","/x.mdl","../x.mdl","progs/../x.mdl","progs//x.mdl","./x.mdl",
		"C:/x.mdl","progs\\x.mdl","progs/x.txt","progs/x.mdl/","progs/x\n.mdl","progs/\"x.mdl"};
	for(unsigned i=0;i<sizeof(valid)/sizeof(*valid);++i) assert(QSM_ValidModelPath(valid[i]));
	for(unsigned i=0;i<sizeof(invalid)/sizeof(*invalid);++i) assert(!QSM_ValidModelPath(invalid[i]));
	qsm_instance_t instances[2]={0};qsm_triangle_t triangles[2]={{{{0,0,0},{1,0,0},{0,1,0}}},{{{2,0,0},{2,1,0},{2,0,1}}}};
	strcpy(instances[0].model,valid[0]);instances[0].modelscale=1;instances[0].flags=QSM_COLLISION;instances[0].triangle_count=1;
	strcpy(instances[1].model,valid[1]);instances[1].modelscale=2.5f;instances[1].origin[0]=-123.25f;instances[1].angles[2]=45;
	instances[1].skin=3;instances[1].first_triangle=1;instances[1].triangle_count=1;
	qsm_data_t source={.instances=instances,.triangles=triangles,.instance_count=2,.triangle_count=2},decoded={0};
	uint8_t *encoded=NULL;size_t bytes=0;char error[256];
	assert(QSM_Encode(&source,&encoded,&bytes,error,sizeof(error)) && bytes==16+600+72);
	assert(get32(encoded)==QSM_VERSION && get32(encoded+12)==300);
	assert(QSM_Decode(&decoded,encoded,bytes,error,sizeof(error)));
	assert(decoded.instance_count==2 && decoded.triangle_count==2);
	assert(!memcmp(decoded.instances,instances,sizeof(instances)) && !memcmp(decoded.triangles,triangles,sizeof(triangles)));
	qsm_instance_t *kept=decoded.instances;uint8_t *bad=malloc(bytes+1);assert(bad);
	for(size_t n=0;n<bytes;++n) assert(!QSM_Decode(&decoded,encoded,n,error,sizeof(error)) && decoded.instances==kept);
	for(unsigned mutation=0;mutation<8;++mutation) {
		memcpy(bad,encoded,bytes);
		switch(mutation) {
		case 0:put32(bad,2);break;
		case 1:put32(bad+4,UINT32_MAX);break;
		case 2:put32(bad+12,304);break;
		case 3:memcpy(bad+16,"../",3);break;
		case 4:put32(bad+16+280,0x7fc00000);break;
		case 5:put32(bad+16+288,2);break;
		case 6:put32(bad+16+292,1);break;
		case 7:put32(bad+16+600,0x7f800000);break;
		}
		assert(!QSM_Decode(&decoded,bad,bytes,error,sizeof(error)) && decoded.instances==kept);
	}
	memcpy(bad,encoded,bytes);bad[bytes]=0;
	assert(!QSM_Decode(&decoded,bad,bytes+1,error,sizeof(error)) && decoded.instances==kept);
	/* Every transform and geometry coordinate is validated by the writer too. */
	instances[1].modelscale=0;assert(!QSM_Encode(&source,&bad,&bytes,error,sizeof(error)));instances[1].modelscale=2.5f;
	triangles[0].position[0][0]=NAN;assert(!QSM_Encode(&source,&bad,&bytes,error,sizeof(error)));triangles[0].position[0][0]=0;
	bytes=688;
	/* Base BSP bytes/version and unrelated extensions survive attachment. */
	uint8_t bsp[172]={0};put32(bsp,29);put32(bsp+4,124);put32(bsp+8,3);memcpy(bsp+124,"{}",3);
	memcpy(bsp+128,"BSPX",4);put32(bsp+132,1);memcpy(bsp+136,"RGBLIGHTING",12);
	put32(bsp+160,168);put32(bsp+164,3);bsp[168]=7;bsp[169]=9;bsp[170]=11;
	uint8_t *extended=NULL;size_t extended_bytes=0;const void *found=NULL;size_t found_bytes=0;
	assert(QSM_AttachBSPX(bsp,sizeof(bsp),encoded,bytes,&extended,&extended_bytes,error,sizeof(error)));
	assert(!memcmp(extended,bsp,128) && get32(extended)==29 && get32(extended+132)==2);
	assert(!memcmp(extended+get32(extended+160),bsp+168,3));
	assert(QSM_FindBSPXLump(extended,extended_bytes,&found,&found_bytes,error,sizeof(error)));
	assert(found_bytes==bytes && !memcmp(found,encoded,bytes));
	uint8_t *replaced=NULL;size_t replaced_bytes=0;
	assert(QSM_AttachBSPX(extended,extended_bytes,encoded,bytes,&replaced,&replaced_bytes,error,sizeof(error)));
	assert(replaced_bytes==extended_bytes && !memcmp(replaced,extended,extended_bytes));
	uint8_t *removed=NULL;size_t removed_bytes=0;
	assert(QSM_AttachBSPX(extended,extended_bytes,NULL,0,&removed,&removed_bytes,error,sizeof(error)));
	assert(get32(removed+132)==1 && !memcmp(removed+get32(removed+160),bsp+168,3));
	assert(QSM_FindBSPXLump(removed,removed_bytes,&found,&found_bytes,error,sizeof(error)) && !found && !found_bytes);
	free(removed);removed=NULL;
	put32(bsp+132,UINT32_MAX);
	assert(!QSM_FindBSPXLump(bsp,sizeof(bsp),&found,&found_bytes,error,sizeof(error)));
	put32(bsp+132,1);put32(bsp+160,UINT32_MAX);
	assert(!QSM_FindBSPXLump(bsp,sizeof(bsp),&found,&found_bytes,error,sizeof(error)));
	uint8_t plain[128]={0};put32(plain,0x32505342);
	assert(QSM_AttachBSPX(plain,124,encoded,bytes,&removed,&removed_bytes,error,sizeof(error)));
	assert(get32(removed)==0x32505342);free(removed);
	QSM_Free(&decoded);free(encoded);free(bad);free(extended);free(replaced);
	puts("PASS: static-model codec, bounded transforms, malformed input, transactional decode and BSPX preservation");
	return 0;
}
