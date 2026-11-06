/* Integration fixture: a room containing three independent sector props. */
#include "editor_sector.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define REQUIRE(expr) do { if (!(expr)) { fprintf(stderr,"%s: %s\n",#expr,error); goto bad; } } while (0)

static int add_concave_prop(editor_sector_map_t *m)
{
    const double xy[6][2]={{-128,32},{-32,32},{-32,64},{-96,64},{-96,128},{-128,128}};
    size_t base=m->num_vertices,sector=m->num_sectors;
    void *p=realloc(m->vertices,(base+12)*sizeof(*m->vertices));if(!p)return 0;m->vertices=p;
    for(size_t z=0;z<2;++z)for(size_t i=0;i<6;++i){double *q=m->vertices[base+6*z+i].position;
        q[0]=xy[i][0];q[1]=xy[i][1];q[2]=16+64*z;}
    m->num_vertices=base+12;
    p=realloc(m->sectors,(sector+1)*sizeof(*m->sectors));if(!p)return 0;m->sectors=p;
    editor_sector_t *s=&m->sectors[sector];memset(s,0,sizeof(*s));++m->num_sectors;
    s->prop_type=EDITOR_SECTOR_PROP_MOVABLE;s->num_faces=8;s->faces=calloc(8,sizeof(*s->faces));if(!s->faces)return 0;
    for(size_t f=0;f<8;++f){editor_sector_face_t *face=&s->faces[f];face->num_vertices=f<2?6:4;
        face->vertices=calloc(face->num_vertices,sizeof(*face->vertices));if(!face->vertices)return 0;
        strcpy(face->material,f==1?"stone2":"stone1");face->portal_sector=face->portal_face=EDITOR_SECTOR_NO_PORTAL;}
    for(size_t i=0;i<6;++i){s->faces[0].vertices[i]=base+5-i;s->faces[1].vertices[i]=base+6+i;
        size_t *side=s->faces[i+2].vertices;side[0]=base+i;side[1]=base+(i+1)%6;side[2]=base+6+(i+1)%6;side[3]=base+6+i;}
    return 1;
}
int main(int argc,char **argv)
{
    char error[512]={0},*text=NULL;size_t length=0,id;editor_sector_map_t m={0};editor_map_t out={0};FILE *file=NULL;
    const double room_lo[]={-256,-256,0},room_hi[]={256,256,256};
    const double static_lo[]={-160,-128,16},static_hi[]={-96,-64,80};
    const double dynamic_lo[]={32,-128,16},dynamic_hi[]={96,-64,80};
    const double sloped_lo[]={96,32,16},sloped_hi[]={160,96,80};
    const double speed[]={12,-8,4};
    REQUIRE(argc==3||(argc==4&&(!strcmp(argv[3],"compare-dynamic")||!strcmp(argv[3],"vis-static")||!strcmp(argv[3],"vis-dynamic")||!strcmp(argv[3],"vis-nocollision"))));REQUIRE(EditorSector_Init(&m));
    int vis=argc==4&&!strncmp(argv[3],"vis-",4);
    editor_sector_prop_type_t static_type=argc==4&&(!strcmp(argv[3],"compare-dynamic")||!strcmp(argv[3],"vis-dynamic"))?EDITOR_SECTOR_PROP_DYNAMIC:EDITOR_SECTOR_PROP_STATIC;
    REQUIRE(EditorSector_AddBox(&m,room_lo,room_hi,"stone1",NULL,error,sizeof(error)));
    if(vis){
        const double lo[]={-8,-256,0},hi[]={8,160,256};
        REQUIRE(EditorSector_AddBoxOfType(&m,lo,hi,"stone1",static_type,NULL,error,sizeof(error)));
        if(!strcmp(argv[3],"vis-nocollision"))REQUIRE(EditorSector_SetPropCollisionEnabled(&m,1,false,error,sizeof(error)));
        REQUIRE(EditorMap_SetProperty(&m.entities.entities[0],"wad",argv[2]));
        REQUIRE(EditorMap_AddEntity(&m.entities,"info_player_start",&id));
        REQUIRE(EditorMap_SetProperty(&m.entities.entities[id],"origin","-128 -128 32"));
        REQUIRE(EditorMap_AddEntity(&m.entities,"light",&id));
        REQUIRE(EditorMap_SetProperty(&m.entities.entities[id],"origin","128 -128 128"));
        REQUIRE(EditorSector_Compile(&m,&out,error,sizeof(error)));
        REQUIRE(EditorMap_Serialize(&out,&text,&length));
        file=fopen(argv[1],"wb");REQUIRE(file);REQUIRE(fwrite(text,1,length,file)==length);
        {int close_result=fclose(file);file=NULL;REQUIRE(close_result==0);}
        free(text);EditorSector_Free(&m);EditorMap_Free(&out);return 0;
    }
    REQUIRE(EditorSector_AddBoxOfType(&m,static_lo,static_hi,"stone1",static_type,NULL,error,sizeof(error)));
    REQUIRE(EditorSector_AddBoxOfType(&m,dynamic_lo,dynamic_hi,"stone1",EDITOR_SECTOR_PROP_DYNAMIC,NULL,error,sizeof(error)));
    REQUIRE(EditorSector_SetPropVelocity(&m,2,speed,error,sizeof(error)));
    REQUIRE(EditorSector_AddBoxOfType(&m,sloped_lo,sloped_hi,"stone1",static_type,NULL,error,sizeof(error)));
    {size_t vertex=m.sectors[3].faces[0].vertices[0];double point[3];memcpy(point,m.vertices[vertex].position,sizeof(point));point[0]+=8;
        REQUIRE(EditorSector_SetVertex(&m,vertex,point,error,sizeof(error)));}
    REQUIRE(add_concave_prop(&m));REQUIRE(EditorSector_Validate(&m,error,sizeof(error)));
    REQUIRE(EditorMap_SetProperty(&m.entities.entities[0],"wad",argv[2]));
    REQUIRE(EditorMap_AddEntity(&m.entities,"info_player_start",&id));
    REQUIRE(EditorMap_SetProperty(&m.entities.entities[id],"origin","0 0 32"));
    REQUIRE(EditorMap_AddEntity(&m.entities,"light",&id));
    REQUIRE(EditorMap_SetProperty(&m.entities.entities[id],"origin","0 0 192"));
    REQUIRE(EditorSector_Compile(&m,&out,error,sizeof(error)));
    REQUIRE(EditorMap_Serialize(&out,&text,&length));
    file=fopen(argv[1],"wb");REQUIRE(file);REQUIRE(fwrite(text,1,length,file)==length);
    {int close_result=fclose(file);file=NULL;REQUIRE(close_result==0);}
    free(text);EditorSector_Free(&m);EditorMap_Free(&out);return 0;
bad:
    if(file)fclose(file);
    free(text);EditorSector_Free(&m);EditorMap_Free(&out);return 1;
}
