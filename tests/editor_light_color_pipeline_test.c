/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "editor_map.h"
#include "editor_sector.h"
#include "radiance_lights.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void check_radiance(const editor_map_t *map, const double rgb[3])
{
    char *text=NULL;size_t length=0;
    editor_map_t entities={0};
    assert(EditorMap_Clone(&entities,map));
    /* BSP entity lumps contain properties, not the source MAP brushes. */
    for(size_t e=0;e<entities.num_entities;++e){
        editor_map_entity_t *entity=&entities.entities[e];
        for(size_t b=0;b<entity->num_brushes;++b){
            for(size_t f=0;f<entity->brushes[b].num_faces;++f)free(entity->brushes[b].faces[f].mapping);
            free(entity->brushes[b].faces);
        }
        free(entity->brushes);entity->brushes=NULL;entity->num_brushes=0;
    }
    radiance_light_t lights[2];radiance_light_parse_stats_t stats;
    assert(EditorMap_Serialize(&entities,&text,&length));
    assert(Radiance_ParseMapLights(text,length,lights,2,&stats));
    assert(stats.found==1&&stats.written==1&&stats.malformed==0&&stats.dropped==0);
    for(int i=0;i<3;++i){
        assert(fabs(lights[0].color_intensity[i]-rgb[i])<1e-7);
        assert((lights[0].position_radius[i]==(float[]){32,48,64}[i]));
    }
    assert(lights[0].color_intensity[3]==2); /* preserved authored intensity 600 */
    free(text);
    EditorMap_Free(&entities);
}

static void check_case(const double rgb[3],int alias_first,int embedded)
{
    editor_map_t source={0},loaded={0},compiled={0};editor_sector_map_t sector={0},again={0};
    const double origin[3]={32,48,64},lo[3]={0,0,0},hi[3]={128,128,128};
    char error[256]={0};size_t light;char *text=NULL;size_t length=0;
    assert(EditorMap_Init(&source));assert(EditorMap_AddLight(&source,origin,&light));
    editor_map_entity_t *entity=&source.entities[light];
    /* Both key orders previously allowed a legacy alias to undo an edit. */
    assert(EditorMap_SetProperty(entity,alias_first?"color":"_color","255 0 0"));
    assert(EditorMap_SetProperty(entity,alias_first?"_color":"color","0 255 0"));
    assert(EditorMap_SetProperty(entity,"light",embedded?"255 0 0 600":"600"));
    assert(EditorMap_SetLightColor(&source,light,rgb));
    assert(!EditorMap_GetProperty(&source.entities[light],"color"));
    assert(!strcmp(EditorMap_GetProperty(&source.entities[light],"light"),embedded?"255 0 0 600":"600"));
    assert(EditorMap_Serialize(&source,&text,&length));
    assert(EditorMap_Parse(&loaded,text,length,error,sizeof error));free(text);text=NULL;
    check_radiance(&loaded,rgb);
    assert(EditorSector_Init(&sector));
    assert(EditorSector_AddBox(&sector,lo,hi,"stone1",NULL,error,sizeof error));
    assert(EditorMap_Clone(&sector.entities,&loaded));
    assert(EditorSector_Serialize(&sector,&text,&length));
    assert(EditorSector_Parse(&again,text,length,error,sizeof error));free(text);text=NULL;
    assert(EditorSector_Compile(&again,&compiled,error,sizeof error));
    check_radiance(&compiled,rgb);
    EditorMap_Free(&compiled);EditorMap_Free(&loaded);EditorMap_Free(&source);
    EditorSector_Free(&again);EditorSector_Free(&sector);
}

int main(void)
{
    const double colors[][3]={{0,0,0},{1.0/255,0,1.0/255},{32.0/255,96.0/255,224.0/255},{1,1,1}};
    for(size_t i=0;i<sizeof colors/sizeof colors[0];++i)
        for(int order=0;order<2;++order)for(int embedded=0;embedded<2;++embedded)
            check_case(colors[i],order,embedded);
    puts("MAP/QSECTOR edited color -> Radiance: black, near-black, aliases and embedded RGB PASS");
    return 0;
}
