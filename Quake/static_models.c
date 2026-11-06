#include "static_models.h"
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define QSM_HEADER_BYTES 16u
#define QSM_RECORD_BYTES 300u
#define QSM_TRIANGLE_BYTES 36u
#define BSP_HEADER_BYTES 124u
#define BSPX_MAX_LUMPS 1024u

static bool QSM_Error (char *error, size_t size, const char *format, ...)
{
	if (error && size) {
		va_list args; va_start (args, format); vsnprintf (error, size, format, args); va_end (args);
	}
	return false;
}
static uint32_t QSM_Read32 (const uint8_t *p)
{ return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24; }
static void QSM_Write32 (uint8_t *p, uint32_t v)
{ for (unsigned i=0; i<4; ++i) p[i]=(uint8_t)(v>>(8*i)); }
static float QSM_ReadFloat (const uint8_t *p)
{ uint32_t bits=QSM_Read32(p); float value; memcpy(&value,&bits,4); return value; }
static void QSM_WriteFloat (uint8_t *p, float value)
{ uint32_t bits; memcpy(&bits,&value,4); QSM_Write32(p,bits); }
static int QSM_Lower (int c) { return c>='A' && c<='Z' ? c+'a'-'A' : c; }

bool QSM_ValidModelPath (const char *path)
{
	if (!path || !*path || strlen(path)>=QSM_MODEL_PATH || *path=='/') return false;
	const char *segment=path;
	for (const unsigned char *p=(const unsigned char *)path;;++p) {
		if (*p && (*p<32 || *p==127 || *p=='\\' || *p==':' || *p=='"')) return false;
		if (!*p || *p=='/') {
			size_t n=(const char *)p-segment;
			if (!n || (n==1 && segment[0]=='.') || (n==2 && !memcmp(segment,"..",2))) return false;
			if (!*p) break;
			segment=(const char *)p+1;
		}
	}
	const char *ext=strrchr(path,'.');
	if (!ext) return false;
	static const char *const supported[]={".mdl",".md3",".md5mesh"};
	for (unsigned i=0;i<3;++i) {
		const char *a=ext,*b=supported[i];
		while (*a && *b && QSM_Lower((unsigned char)*a)==*b) {++a;++b;}
		if (!*a && !*b) return true;
	}
	return false;
}

void QSM_Free (qsm_data_t *data)
{
	if (!data) return;
	QSM_FreeCollision(data->collision);
	free(data->instances); free(data->triangles); memset(data,0,sizeof(*data));
}
static bool QSM_Validate (const qsm_data_t *data, char *error, size_t size)
{
	if (!data || data->instance_count>QSM_MAX_INSTANCES || data->triangle_count>QSM_MAX_TRIANGLES ||
		(data->instance_count && !data->instances) || (data->triangle_count && !data->triangles))
		return QSM_Error(error,size,"Invalid compiled static-model counts");
	uint32_t expected=0;
	for (uint32_t i=0;i<data->instance_count;++i) {
		const qsm_instance_t *m=&data->instances[i];
		if (!memchr(m->model,0,sizeof(m->model)) || !QSM_ValidModelPath(m->model) ||
			!isfinite(m->modelscale) || m->modelscale<=0 || m->modelscale>1024 ||
			m->skin>INT_MAX || (m->flags & ~QSM_COLLISION) || m->first_triangle!=expected ||
			!m->triangle_count || m->triangle_count>data->triangle_count-expected)
			return QSM_Error(error,size,"Invalid compiled static model %u",i);
		for (unsigned a=0;a<3;++a)
			if (!isfinite(m->origin[a]) || fabsf(m->origin[a])>16777216.f || !isfinite(m->angles[a]))
				return QSM_Error(error,size,"Invalid static-model transform %u",i);
		expected+=m->triangle_count;
	}
	if (expected!=data->triangle_count) return QSM_Error(error,size,"Unowned compiled collision triangles");
	for (uint32_t i=0;i<data->triangle_count;++i)
		for (unsigned v=0;v<3;++v) for (unsigned a=0;a<3;++a)
			if (!isfinite(data->triangles[i].position[v][a]) || fabsf(data->triangles[i].position[v][a])>16777216.f)
				return QSM_Error(error,size,"Invalid static-model triangle %u",i);
	return true;
}
bool QSM_Encode (const qsm_data_t *data, uint8_t **bytes, size_t *length, char *error, size_t size)
{
	if (!bytes || !length || !QSM_Validate(data,error,size)) return false;
	uint64_t total=QSM_HEADER_BYTES+(uint64_t)data->instance_count*QSM_RECORD_BYTES+(uint64_t)data->triangle_count*QSM_TRIANGLE_BYTES;
	if (total>SIZE_MAX) return QSM_Error(error,size,"Compiled static-model data is too large");
	uint8_t *out=calloc(1,(size_t)total);
	if (!out) return QSM_Error(error,size,"Could not allocate compiled static models");
	QSM_Write32(out,QSM_VERSION); QSM_Write32(out+4,data->instance_count);
	QSM_Write32(out+8,data->triangle_count); QSM_Write32(out+12,QSM_RECORD_BYTES);
	uint8_t *p=out+QSM_HEADER_BYTES;
	for (uint32_t i=0;i<data->instance_count;++i,p+=QSM_RECORD_BYTES) {
		const qsm_instance_t *m=&data->instances[i];
		memcpy(p,m->model,strlen(m->model)+1);
		for(unsigned a=0;a<3;++a) { QSM_WriteFloat(p+256+4*a,m->origin[a]); QSM_WriteFloat(p+268+4*a,m->angles[a]); }
		QSM_WriteFloat(p+280,m->modelscale); QSM_Write32(p+284,m->skin); QSM_Write32(p+288,m->flags);
		QSM_Write32(p+292,m->first_triangle); QSM_Write32(p+296,m->triangle_count);
	}
	for (uint32_t i=0;i<data->triangle_count;++i,p+=QSM_TRIANGLE_BYTES)
		for(unsigned v=0;v<3;++v) for(unsigned a=0;a<3;++a) QSM_WriteFloat(p+4*(v*3+a),data->triangles[i].position[v][a]);
	*bytes=out; *length=(size_t)total; return true;
}
bool QSM_Decode (qsm_data_t *data, const void *bytes, size_t length, char *error, size_t size)
{
	const uint8_t *in=bytes;
	if (!data || !in || length<QSM_HEADER_BYTES) return QSM_Error(error,size,"Truncated compiled static-model header");
	if (QSM_Read32(in)!=QSM_VERSION || QSM_Read32(in+12)!=QSM_RECORD_BYTES)
		return QSM_Error(error,size,"Unsupported compiled static-model version");
	qsm_data_t pending={0};
	pending.instance_count=QSM_Read32(in+4); pending.triangle_count=QSM_Read32(in+8);
	uint64_t needed=QSM_HEADER_BYTES+(uint64_t)pending.instance_count*QSM_RECORD_BYTES+(uint64_t)pending.triangle_count*QSM_TRIANGLE_BYTES;
	if (pending.instance_count>QSM_MAX_INSTANCES || pending.triangle_count>QSM_MAX_TRIANGLES || needed!=length)
		return QSM_Error(error,size,"Invalid compiled static-model payload length");
	if (pending.instance_count && !(pending.instances=calloc(pending.instance_count,sizeof(*pending.instances)))) goto memory;
	if (pending.triangle_count && !(pending.triangles=malloc((size_t)pending.triangle_count*sizeof(*pending.triangles)))) goto memory;
	const uint8_t *p=in+QSM_HEADER_BYTES;
	for(uint32_t i=0;i<pending.instance_count;++i,p+=QSM_RECORD_BYTES) {
		qsm_instance_t *m=&pending.instances[i]; memcpy(m->model,p,sizeof(m->model));
		for(unsigned a=0;a<3;++a) {m->origin[a]=QSM_ReadFloat(p+256+4*a);m->angles[a]=QSM_ReadFloat(p+268+4*a);}
		m->modelscale=QSM_ReadFloat(p+280);m->skin=QSM_Read32(p+284);m->flags=QSM_Read32(p+288);
		m->first_triangle=QSM_Read32(p+292);m->triangle_count=QSM_Read32(p+296);
	}
	for(uint32_t i=0;i<pending.triangle_count;++i,p+=QSM_TRIANGLE_BYTES)
		for(unsigned v=0;v<3;++v) for(unsigned a=0;a<3;++a) pending.triangles[i].position[v][a]=QSM_ReadFloat(p+4*(v*3+a));
	if (!QSM_Validate(&pending,error,size)) {QSM_Free(&pending);return false;}
	QSM_Free(data); *data=pending; return true;
memory:
	QSM_Free(&pending); return QSM_Error(error,size,"Could not allocate compiled static models");
}

typedef struct { char name[24]; const uint8_t *data; uint32_t bytes; } qsm_xlump_t;
static bool QSM_BSPXLumps (const uint8_t *bsp, size_t bytes, size_t *base, qsm_xlump_t **lumps,
	uint32_t *count, char *error, size_t size)
{
	*lumps=NULL; *count=0;
	if (!bsp || bytes<BSP_HEADER_BYTES) return QSM_Error(error,size,"Truncated BSP header");
	uint32_t version=QSM_Read32(bsp);
	if (version!=29 && version!=30 && version!=UINT32_C(0x32505342) && version!=UINT32_C(0x42535032) && version!=UINT32_C(0x51363420))
		return QSM_Error(error,size,"Unsupported base BSP version");
	size_t end=BSP_HEADER_BYTES;
	for(unsigned i=0;i<15;++i) {
		uint32_t offset=QSM_Read32(bsp+4+8*i),length=QSM_Read32(bsp+8+8*i);
		if (offset>INT_MAX || length>INT_MAX || offset>bytes || length>bytes-offset)
			return QSM_Error(error,size,"Invalid BSP lump %u",i);
		if ((size_t)offset+length>end) end=(size_t)offset+length;
	}
	if (end>SIZE_MAX-3) return QSM_Error(error,size,"BSP alignment overflow");
	*base=(end+3)&~(size_t)3;
	if (*base>bytes || bytes-*base<4 || memcmp(bsp+*base,"BSPX",4)) return true;
	if (bytes-*base<8) return QSM_Error(error,size,"Truncated BSPX header");
	uint32_t n=QSM_Read32(bsp+*base+4);
	if (n>BSPX_MAX_LUMPS || (uint64_t)n*32>bytes-*base-8) return QSM_Error(error,size,"Invalid BSPX directory");
	qsm_xlump_t *out=n?calloc(n,sizeof(*out)):NULL;
	if (n && !out) return QSM_Error(error,size,"Could not allocate BSPX directory");
	bool found=false;
	for(uint32_t i=0;i<n;++i) {
		const uint8_t *p=bsp+*base+8+32*i;
		uint32_t offset=QSM_Read32(p+24),length=QSM_Read32(p+28);
		if (!p[0] || offset>INT_MAX || length>INT_MAX || offset>bytes || length>bytes-offset)
			{free(out);return QSM_Error(error,size,"Invalid BSPX lump %u",i);}
		memcpy(out[i].name,p,24);out[i].data=bsp+offset;out[i].bytes=length;
		if (!strncmp(out[i].name,QSM_LUMP_NAME,24)) {
			if (found) {free(out);return QSM_Error(error,size,"Duplicate static-model BSPX lump");}
			found=true;
		}
	}
	*lumps=out;*count=n;return true;
}
bool QSM_FindBSPXLump (const void *bsp, size_t bsp_bytes, const void **lump, size_t *lump_bytes,
	char *error, size_t size)
{
	if (!lump || !lump_bytes) return QSM_Error(error,size,"Invalid BSPX output");
	size_t base; qsm_xlump_t *all;uint32_t count;
	if (!QSM_BSPXLumps(bsp,bsp_bytes,&base,&all,&count,error,size)) return false;
	*lump=NULL;*lump_bytes=0;
	for(uint32_t i=0;i<count;++i) if (!strncmp(all[i].name,QSM_LUMP_NAME,24)) {*lump=all[i].data;*lump_bytes=all[i].bytes;}
	free(all);return true;
}
bool QSM_AttachBSPX (const void *bsp, size_t bsp_bytes, const void *lump, size_t lump_bytes,
	uint8_t **output, size_t *output_bytes, char *error, size_t size)
{
	if (!output || !output_bytes || (!lump && lump_bytes) || lump_bytes>INT_MAX)
		return QSM_Error(error,size,"Invalid static-model BSPX attachment");
	qsm_data_t check={0};
	if (lump_bytes && !QSM_Decode(&check,lump,lump_bytes,error,size)) return false;
	QSM_Free(&check);
	size_t base; qsm_xlump_t *all;uint32_t count;
	if (!QSM_BSPXLumps(bsp,bsp_bytes,&base,&all,&count,error,size)) return false;
	if (bsp_bytes>base && (bsp_bytes-base<4 || memcmp((const uint8_t *)bsp+base,"BSPX",4)))
		for (size_t i=base;i<bsp_bytes;++i) if (((const uint8_t *)bsp)[i])
			{free(all);return QSM_Error(error,size,"Unknown trailing BSP data; original preserved");}
	uint32_t kept=0;
	for(uint32_t i=0;i<count;++i) if (strncmp(all[i].name,QSM_LUMP_NAME,24)) all[kept++]=all[i];
	uint32_t total_lumps=kept+(lump_bytes?1u:0u);
	if(total_lumps>BSPX_MAX_LUMPS) {free(all);return QSM_Error(error,size,"Too many BSPX lumps");}
	uint64_t total=base+(total_lumps?8+(uint64_t)total_lumps*32:0);
	for(uint32_t i=0;i<kept;++i) total+=((uint64_t)all[i].bytes+3)&~UINT64_C(3);
	if(lump_bytes) total+=((uint64_t)lump_bytes+3)&~UINT64_C(3);
	if(total>INT_MAX || total>SIZE_MAX) {free(all);return QSM_Error(error,size,"BSPX output is too large");}
	uint8_t *out=calloc(1,(size_t)total);
	if(!out) {free(all);return QSM_Error(error,size,"Could not allocate extended BSP");}
	memcpy(out,bsp,base<bsp_bytes?base:bsp_bytes);
	if(total_lumps) {
		memcpy(out+base,"BSPX",4);QSM_Write32(out+base+4,total_lumps);
		size_t offset=base+8+(size_t)total_lumps*32;
		for(uint32_t i=0;i<total_lumps;++i) {
			uint8_t *entry=out+base+8+(size_t)i*32;
			const void *data=i<kept?all[i].data:lump;size_t length=i<kept?all[i].bytes:lump_bytes;
			if(i<kept) memcpy(entry,all[i].name,24);else memcpy(entry,QSM_LUMP_NAME,sizeof(QSM_LUMP_NAME));
			QSM_Write32(entry+24,(uint32_t)offset);QSM_Write32(entry+28,(uint32_t)length);
			if(length) memcpy(out+offset,data,length);
			offset=(offset+length+3)&~(size_t)3;
		}
	}
	free(all);*output=out;*output_bytes=(size_t)total;return true;
}
