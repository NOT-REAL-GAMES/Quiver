#include "quakedef.h"
#include "editor_map.h"
#include "static_models_engine.h"
#include "radiance_alias.h"
#include "radiance_scene.h"
#include "radiance_runtime.h"
#include <float.h>
#include <limits.h>
#include <stdarg.h>

static bool QSM_EngineError (char *error, size_t size, const char *format, ...)
{
	if (error && size) {
		va_list args; va_start(args,format); vsnprintf(error,size,format,args); va_end(args);
	}
	return false;
}

bool EditorStaticModels_Prepare (editor_map_t *build, uint8_t **lump, size_t *length,
	char *error, size_t error_size)
{
	if (!build || !lump || !length) return QSM_EngineError(error,error_size,"Invalid static-model compiler input");
	uint32_t count=0;
	for (size_t i=1;i<build->num_entities;++i) if (EditorMap_IsModel(build,i)) {
		if (count==QSM_MAX_INSTANCES) return QSM_EngineError(error,error_size,"Too many static models");
		++count;
	}
	if (!count) {*lump=NULL;*length=0;return true;}
	qsm_data_t data={0};rc_scene_t extracted;RCScene_Init(&extracted);
	data.instance_count=count;data.instances=calloc(count,sizeof(*data.instances));
	if (!data.instances) return QSM_EngineError(error,error_size,"Could not allocate static-model placements");
	bool success=false;uint8_t *encoded=NULL;size_t encoded_bytes=0;uint32_t at=0;
	for (size_t i=1;i<build->num_entities;++i) if (EditorMap_IsModel(build,i)) {
		editor_map_model_t source;
		if (!EditorMap_GetModel(build,i,&source) || strlen(source.model)>=MAX_QPATH) {
			QSM_EngineError(error,error_size,"Invalid or overlong model path in entity %zu",i);goto done;
		}
		qmodel_t *model=Mod_ForName(source.model,false);
		if (!model || model->type!=mod_alias) {
			QSM_EngineError(error,error_size,"Could not load static model %s",source.model);goto done;
		}
		aliashdr_t *header=Mod_Extradata_Exact(model,source.skin);
		uint32_t triangles=0;
		for (aliashdr_t *surface=header;surface;surface=surface->nextsurface) {
			if (!surface->radiance_mesh || surface->numframes!=1 || surface->numposes!=1 ||
				surface->numindexes<=0 || surface->numindexes%3 || source.skin>=surface->numskins ||
				(uint32_t)(surface->numindexes/3)>QSM_MAX_TRIANGLES-triangles) {
				QSM_EngineError(error,error_size,"Static model %s must have one retained frame and a valid skin on every surface",source.model);goto done;
			}
			triangles+=(uint32_t)(surface->numindexes/3);
		}
		if (!triangles || triangles>QSM_MAX_TRIANGLES-extracted.triangle_count) {
			QSM_EngineError(error,error_size,"Static-model collision geometry exceeds the map limit");goto done;
		}
		qsm_instance_t *instance=&data.instances[at];
		q_strlcpy(instance->model,source.model,sizeof(instance->model));
		for(unsigned a=0;a<3;++a) {instance->origin[a]=(float)source.origin[a];instance->angles[a]=(float)source.angles[a];}
		instance->modelscale=(float)source.modelscale;instance->skin=(uint32_t)source.skin;
		instance->flags=source.collision?QSM_COLLISION:0;instance->first_triangle=extracted.triangle_count;
		float matrix[16];IdentityMatrix(matrix);
		R_RotateForEntityScale(matrix,instance->origin,instance->angles,instance->modelscale);
		uint32_t surface_index=0;
		for(aliashdr_t *surface=header;surface;surface=surface->nextsurface,++surface_index)
			if(!RCAlias_Append(&extracted,surface->radiance_mesh,0,0,0,matrix,0,0,surface_index,at)) {
				QSM_EngineError(error,error_size,"Could not compile geometry for %s",source.model);goto done;
			}
		instance->triangle_count=extracted.triangle_count-instance->first_triangle;++at;
	}
	data.triangle_count=extracted.triangle_count;
	data.triangles=malloc((size_t)data.triangle_count*sizeof(*data.triangles));
	if(!data.triangles) {QSM_EngineError(error,error_size,"Could not allocate compiled collision triangles");goto done;}
	for(uint32_t i=0;i<data.triangle_count;++i) for(unsigned v=0;v<3;++v)
		memcpy(data.triangles[i].position[v],extracted.triangles[i].position[v],3*sizeof(float));
	if(!QSM_Encode(&data,&encoded,&encoded_bytes,error,error_size)) goto done;
	/* These are compile-only entities. Preserve the source document and remove
	 * them only from its successful compiler snapshot, so no QuakeC mod is
	 * needed to spawn the compiled placements. */
	for(size_t i=build->num_entities;i-->1;) if(EditorMap_IsModel(build,i)) EditorMap_RemoveEntity(build,i);
	*lump=encoded;*length=encoded_bytes;encoded=NULL;success=true;
done:
	free(encoded);QSM_Free(&data);RCScene_Clear(&extracted);return success;
}

static void StaticModels_Stats_f (void)
{
	const qmodel_t *world=cl.worldmodel ? cl.worldmodel : sv.qcvm.worldmodel;
	const qsm_data_t *data=world ? world->static_models : NULL;
	uint32_t enabled=0,compiled=0;
	if(data) for(uint32_t i=0;i<data->instance_count;++i) enabled+=(data->instances[i].flags&QSM_COLLISION)!=0;
	for(int i=0;i<cl.num_statics;++i) compiled+=cl.static_entities[i]->compiled_instance!=NULL;
	Con_Printf("StaticModels: instances=%u triangles=%u collision=%u client_compiled=%u\n",
		data?data->instance_count:0,data?data->triangle_count:0,enabled,compiled);
	uint32_t geometry[4];
	bool active=R_RadianceGeometryCounts(geometry);
	Con_Printf("StaticModels: radiance_active=%u scene_triangles=%u static_triangles=%u base_triangles=%u compiled_triangles=%u\n",
		(unsigned)active,geometry[0],geometry[1],geometry[2],geometry[3]);
	if(!data) return;
	for(uint32_t i=0;i<data->instance_count;++i) {
		const qsm_instance_t *instance=&data->instances[i];
		vec3_t mins={FLT_MAX,FLT_MAX,FLT_MAX},maxs={-FLT_MAX,-FLT_MAX,-FLT_MAX};
		for(uint32_t t=0;t<instance->triangle_count;++t) for(unsigned v=0;v<3;++v) for(unsigned a=0;a<3;++a) {
			float p=data->triangles[instance->first_triangle+t].position[v][a];
			if(p<mins[a]) mins[a]=p;
			if(p>maxs[a]) maxs[a]=p;
		}
		const entity_t *visual=NULL;
		if(i<(uint32_t)cl.num_statics && cl.static_entities[i]->compiled_instance==instance) visual=cl.static_entities[i];
		else for(int e=0;e<cl.num_statics;++e) if(cl.static_entities[e]->compiled_instance==instance) {visual=cl.static_entities[e];break;}
		bool bounds_match=visual && visual->compiled_bounds_valid && !memcmp(mins,visual->compiled_mins,sizeof(mins)) &&
			!memcmp(maxs,visual->compiled_maxs,sizeof(maxs));
		Con_Printf("StaticModels: index=%u model=%s scale=%.9g skin=%u collision=%u client_bounds_match=%u origin=(%.9g %.9g %.9g) angles=(%.9g %.9g %.9g) mins=(%.9g %.9g %.9g) maxs=(%.9g %.9g %.9g)\n",
			i,instance->model,instance->modelscale,instance->skin,(instance->flags&QSM_COLLISION)!=0,
			(unsigned)bounds_match,
			instance->origin[0],instance->origin[1],instance->origin[2],instance->angles[0],instance->angles[1],instance->angles[2],
			mins[0],mins[1],mins[2],maxs[0],maxs[1],maxs[2]);
	}
}

void StaticModels_Init (void)
{
	Cmd_AddCommand("static_models_stats",StaticModels_Stats_f);
}

bool EditorStaticModels_Attach (const char *path, const uint8_t *lump, size_t length,
	char *error, size_t error_size)
{
	if(!length) return true;
	if(!path || !*path || !lump) return QSM_EngineError(error,error_size,"Invalid compiled static-model output");
	FILE *file=Sys_fopen(path,"rb");
	if(!file) return QSM_EngineError(error,error_size,"Could not open compiled BSP");
	bool success=false;uint8_t *bsp=NULL,*output=NULL;size_t output_bytes=0;
	if(fseek(file,0,SEEK_END)) goto read_fail;
	long end=ftell(file);
	if(end<0 || (unsigned long)end>INT_MAX || fseek(file,0,SEEK_SET)) goto read_fail;
	bsp=malloc((size_t)end);
	if(!bsp || fread(bsp,1,(size_t)end,file)!=(size_t)end) goto read_fail;
	if(fclose(file)) {file=NULL;goto read_fail;}file=NULL;
	if(!QSM_AttachBSPX(bsp,(size_t)end,lump,length,&output,&output_bytes,error,error_size)) goto done;
	char temp[MAX_OSPATH];
	for(unsigned attempt=0;attempt<100;++attempt) {
		int n=q_snprintf(temp,sizeof(temp),"%s.qsm-tmp-%u",path,attempt);
		if(n<0 || n>=(int)sizeof(temp)) break;
		if(Sys_FileType(temp)!=FS_ENT_NONE) continue;
		file=Sys_fopen(temp,"wbx");
		if(!file) break;
		bool written=fwrite(output,1,output_bytes,file)==output_bytes && fflush(file)==0;
		if(fclose(file)) written=false;
		file=NULL;
		if(written && Sys_rename(temp,path)==0) success=true;else Sys_remove(temp);
		break;
	}
	if(!success) QSM_EngineError(error,error_size,"Could not publish compiled static models; original BSP preserved");
	goto done;
read_fail:
	QSM_EngineError(error,error_size,"Could not read compiled BSP");
done:
	if(file) fclose(file);
	free(bsp);free(output);return success;
}
