/* All-frame inventory regression: a large static alias source must not be
 * decoded/repacked/uploaded when tiny brush animation selections change. */
#include "quakedef.h"
#include "radiance_materials_quake.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

cvar_t gl_fullbrights = {.name="gl_fullbrights", .string="1"};
cvar_t gl_nocolors = {.name="gl_nocolors", .string="0"};
client_state_t cl;
viddef_t vid;
gltexture_t *playertextures[MAX_SCOREBOARD];
unsigned int d_8to24table[256];
static unsigned phase;
typedef struct { const char *name; unsigned size, red, calls; bool fail; } image_t;
static image_t images[] = {{"large",1024,43,0,false}, {"p0",2,11,0,false},
	{"p1",2,22,0,false}, {"a0",2,33,0,false}, {"a1",2,44,0,false},
	{"projectile",2,77,0,false}};

texture_t *R_TextureAnimation (texture_t *base, int frame)
{
	if (frame && base->alternate_anims) base = base->alternate_anims;
	return phase && base->anim_next ? base->anim_next : base;
}
qfilesize_t COM_OpenFile (const char *name, int *handle, unsigned int *path_id)
{ (void)name; (void)path_id; *handle=-1; return -1; }
qfileofs_t Sys_FilePos (int handle) { (void)handle; return -1; }
int Sys_FileSeek (int handle, qfileofs_t offset) { (void)handle; (void)offset; return -1; }
int Sys_FileRead (int handle, void *data, int size) { (void)handle; (void)data; (void)size; return 0; }
void Sys_FileClose (int handle) { (void)handle; }
void Mem_Free (const void *data) { free ((void *)data); }
void Con_Printf (const char *format, ...) { (void)format; }
byte *Image_LoadImage (const char *name, int *width, int *height, enum srcformat *format, unsigned int path_id)
{
	(void)path_id;
	for (unsigned i=0;i<sizeof(images)/sizeof(*images);++i) if (!strcmp(name,images[i].name))
	{
		image_t *image=&images[i]; ++image->calls;
		if (image->fail) return NULL;
		*width=*height=(int)image->size; *format=SRC_RGBA;
		byte *out=malloc(image->size*image->size*4); assert(out);
		for (unsigned p=0;p<image->size*image->size;++p)
		{ out[p*4]=(byte)image->red; out[p*4+1]=7; out[p*4+2]=9; out[p*4+3]=123; }
		return out;
	}
	return NULL;
}
static gltexture_t image_texture (unsigned index)
{
	gltexture_t t={0}; image_t *image=&images[index];
	strcpy(t.source_file,image->name); t.source_width=t.source_height=image->size;
	t.source_format=SRC_RGBA; t.shirt=t.pants=-1;
	return t;
}
static const byte *texel (const radiance_material_atlas_t *atlas, size_t material, bool emission)
{
	const radiance_material_t *m=&atlas->materials[material];
	const uint32_t *r=emission?m->emission_rect:m->albedo_rect;
	assert(r[2] && r[3]);
	return atlas->rgba+(((size_t)m->layers_flags[emission?1:0]*atlas->height+r[1])*atlas->width+r[0])*4;
}
static bool update (radiance_quake_materials_t *materials, radiance_material_atlas_t *atlas, bool *pixels)
{ return RadianceQuakeMaterials_UpdateAtlas(materials,atlas,1024,16,16u<<20,pixels); }

int main (void)
{
	gl_fullbrights.value=1;
	gltexture_t texture[6]; for(unsigned i=0;i<6;++i) texture[i]=image_texture(i);
	texture_t p0={0},p1={0},a0={0},a1={0};
	p0.gltexture=&texture[1];p1.gltexture=&texture[2];a0.gltexture=&texture[3];a1.gltexture=&texture[4];
	p0.anim_next=&p1;p1.anim_next=&p0;a0.anim_next=&a1;a1.anim_next=&a0;
	p0.alternate_anims=&a0;p1.alternate_anims=&a0;
	radiance_quake_materials_t materials; RadianceQuakeMaterials_Init(&materials);
	radiance_material_atlas_t atlas={0}; entity_t entity={0}; bool pixels=false;
	assert(RadianceQuakeMaterials_Alias(&entity,0,&texture[0],NULL,&materials)==0);
	assert(RadianceQuakeMaterials_Brush(&p0,&materials)==1);
	RadianceQuakeMaterials_SetBrushFrame(&materials,1);
	assert(RadianceQuakeMaterials_Brush(&p0,&materials)==2);
	assert(update(&materials,&atlas,&pixels) && pixels && atlas.material_count==3);
	assert(texel(&atlas,0,false)[0]==43 && texel(&atlas,0,false)[3]==123);
	assert(texel(&atlas,1,false)[0]==11 && texel(&atlas,2,false)[0]==33);
	for(unsigned i=0;i<5;++i) assert(images[i].calls==1); /* both sequences preloaded */
	byte *packed=atlas.rgba; size_t bytes=atlas.bytes;
	uint64_t old_revision=RadianceQuakeMaterials_Revision(&materials);
	for(unsigned i=0;i<100;++i)
	{
		phase=(i+1)&1;
		assert(update(&materials,&atlas,&pixels) && !pixels && atlas.rgba==packed && atlas.bytes==bytes);
		assert(texel(&atlas,1,false)[0]==(phase?22:11));
		assert(texel(&atlas,2,false)[0]==(phase?44:33));
		for(unsigned j=0;j<5;++j) assert(images[j].calls==1);
	}
	phase=1; assert(RadianceQuakeMaterials_Revision(&materials)!=old_revision); phase=0;
	/* A new projectile repacks the inventory but must not decode the large
	 * source or unchanged animation frames again. Its original file may no
	 * longer be readable without invalidating the already loaded texture. */
	images[0].fail=true;
	assert(RadianceQuakeMaterials_Alias(&entity,0,&texture[5],NULL,&materials)==3);
	assert(update(&materials,&atlas,&pixels) && pixels && atlas.material_count==4);
	for(unsigned i=0;i<5;++i) assert(images[i].calls==1);
	assert(images[5].calls==1 && texel(&atlas,3,false)[0]==77 && texel(&atlas,3,false)[3]==123);
	assert(texel(&atlas,0,false)[0]==43 && texel(&atlas,0,false)[3]==123);
	assert(texel(&atlas,1,false)[0]==11 && texel(&atlas,2,false)[0]==33);
	/* Both borrowed full-width pages and copied small rectangles survive a
	 * failed new-source reload or a stricter packing budget unchanged. */
	packed=atlas.rgba; radiance_material_t retained=atlas.materials[0];
	++texture[5].source_crc; images[5].fail=true;
	assert(!update(&materials,&atlas,&pixels) && !pixels && atlas.rgba==packed);
	assert(!memcmp(&retained,&atlas.materials[0],sizeof(retained)));
	images[5].fail=false;
	assert(update(&materials,&atlas,&pixels) && pixels);
	for(unsigned i=0;i<5;++i) assert(images[i].calls==1);
	packed=atlas.rgba;
	assert(!RadianceQuakeMaterials_UpdateAtlas(&materials,&atlas,1024,1,16u<<20,&pixels) && !pixels && atlas.rgba==packed);
	assert(!RadianceQuakeMaterials_UpdateAtlas(&materials,&atlas,1024,16,4u<<20,&pixels) && !pixels && atlas.rgba==packed);
	assert(update(&materials,&atlas,&pixels) && !pixels && atlas.rgba==packed);
	images[0].fail=false;
	/* Reload an inactive alternate frame; the current selection is unchanged,
	 * but its cached pixels must be invalidated before it becomes visible. */
	++texture[4].source_crc; images[4].red=55;
	assert(update(&materials,&atlas,&pixels) && pixels);
	assert(texel(&atlas,2,false)[0]==33);
	phase=1; assert(update(&materials,&atlas,&pixels) && !pixels && texel(&atlas,2,false)[0]==55);
	phase=0;
	/* Failed inactive-frame reload is transactional for both records and pixels. */
	packed=atlas.rgba; radiance_material_t before=atlas.materials[1];
	++texture[2].source_crc; images[2].fail=true;
	assert(!update(&materials,&atlas,&pixels) && !pixels && atlas.rgba==packed);
	assert(!memcmp(&before,&atlas.materials[1],sizeof(before)));
	images[2].fail=false;
	assert(update(&materials,&atlas,&pixels) && pixels);
	/* Real static-alias reload also invalidates the inventory. */
	++texture[0].source_crc; images[0].red=63;
	assert(update(&materials,&atlas,&pixels) && pixels && texel(&atlas,0,false)[0]==63);
	/* Compaction may discard texture handles before cache comparison. The old
	 * cache is never dereferenced: only current registry sources are inspected. */
	materials.entries[0]=materials.entries[2]; materials.count=1;
	memset(&texture[0],0xff,sizeof(texture[0])); unsigned large_calls=images[0].calls;
	packed=atlas.rgba;
	assert(update(&materials,&atlas,&pixels) && !pixels && atlas.rgba==packed && atlas.material_count==1);
	assert(images[0].calls==large_calls && texel(&atlas,0,false)[0]==33);
	/* A malformed non-root cycle must not loop forever or publish a partial atlas. */
	p1.anim_next=&p1;
	assert(!update(&materials,&atlas,&pixels) && !pixels && atlas.rgba==packed);
	p1.anim_next=&p0;
	RadianceMaterialAtlas_Clear(&atlas);RadianceQuakeMaterials_Clear(&materials);
	/* The same shared source decoder preserves indexed liquid fullbrights and
	 * alpha under the inventory API, including a fullbright setting change. */
	for(unsigned i=0;i<256;++i) { byte *rgb=(byte *)&d_8to24table[i];rgb[0]=(byte)i;rgb[1]=7;rgb[2]=9;rgb[3]=255; }
	byte lava_pixels[4]={224,1,255,2}; gltexture_t lava_image={0};texture_t lava={0};
	lava_image.source_width=lava_image.source_height=2;lava_image.source_format=SRC_INDEXED;
	lava_image.source_offset=(src_offset_t)(uintptr_t)lava_pixels;lava_image.flags=TEXPREF_NOBRIGHT|TEXPREF_ALPHA;
	lava_image.shirt=lava_image.pants=-1;lava.type=TEXTYPE_LAVA;lava.gltexture=&lava_image;
	RadianceQuakeMaterials_Init(&materials);assert(RadianceQuakeMaterials_Brush(&lava,&materials)==0);
	assert(update(&materials,&atlas,&pixels) && pixels);
	assert(texel(&atlas,0,false)[0]==0 && texel(&atlas,0,true)[0]==224);
	gl_fullbrights.value=0;
	assert(update(&materials,&atlas,&pixels) && pixels && texel(&atlas,0,false)[0]==224);
	assert(!(atlas.materials[0].layers_flags[2]&RADIANCE_MATERIAL_EMISSION_TEXTURE));
	RadianceMaterialAtlas_Clear(&atlas);RadianceQuakeMaterials_Clear(&materials);
	puts("PASS: all-frame inventory, alternate selection, immutable large pixels, reloads, compaction, liquids and rollback");
	return 0;
}
