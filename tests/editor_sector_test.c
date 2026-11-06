/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "editor_sector.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void expect_box(editor_sector_map_t *m,const double lo[3],const double hi[3])
{
    char error[256]={0};if(!EditorSector_AddBox(m,lo,hi,"stone1",NULL,error,sizeof(error))){fprintf(stderr,"AddBox: %s\n",error);abort();}
}
static void expect_overlap_rejected(editor_sector_map_t *m,const double lo[3],const double hi[3])
{
    char error[256]={0};size_t count=m->num_sectors;
    assert(!EditorSector_AddBox(m,lo,hi,"stone1",NULL,error,sizeof(error)));
    assert(strstr(error,"overlap"));assert(m->num_sectors==count);
}
static size_t vertex_at(const editor_sector_map_t *m,double x,double y,double z)
{
    for(size_t i=0;i<m->num_vertices;++i){const double *p=m->vertices[i].position;if(p[0]==x&&p[1]==y&&p[2]==z)return i;}
    abort();
}
/* Simulate a saved document whose adjoining sectors independently own a
 * geometrically identical corner rather than sharing one global index. */
static size_t duplicate_sector_vertex(editor_sector_map_t *m,size_t sector,size_t vertex)
{
    size_t id=m->num_vertices;void *grown=realloc(m->vertices,(id+1)*sizeof(*m->vertices));assert(grown);
    m->vertices=grown;m->vertices[id]=m->vertices[vertex];++m->num_vertices;size_t replaced=0;
    for(size_t f=0;f<m->sectors[sector].num_faces;++f)for(size_t i=0;i<m->sectors[sector].faces[f].num_vertices;++i)
        if(m->sectors[sector].faces[f].vertices[i]==vertex){m->sectors[sector].faces[f].vertices[i]=id;++replaced;}
    assert(replaced>=3);return id;
}
static void test_sector_props(void)
{
    char error[256]={0};editor_sector_map_t m={0},loaded={0};char *text=NULL;size_t length=0;
    const double lo[]={-128,-128,0},hi[]={128,128,128},plo[]={-128,-128,0},phi[]={-64,-64,64};
    assert(EditorSector_Init(&m));expect_box(&m,lo,hi);
    expect_overlap_rejected(&m,plo,phi);
    for(int type=EDITOR_SECTOR_PROP_STATIC;type<=EDITOR_SECTOR_PROP_MOVABLE;++type)
        assert(EditorSector_AddBoxOfType(&m,plo,phi,"stone1",(editor_sector_prop_type_t)type,NULL,error,sizeof(error)));
    const double velocity[]={1.25,-2.5,3.75},invalid[]={NAN,0,0};
    assert(!EditorSector_SetPropVelocity(&m,1,velocity,error,sizeof(error)));
    assert(!EditorSector_SetPropVelocity(&m,2,invalid,error,sizeof(error)));
    assert(EditorSector_SetPropVelocity(&m,2,velocity,error,sizeof(error)));
    assert(EditorSector_SetPropVelocity(&m,3,velocity,error,sizeof(error)));
    size_t world=m.sectors[0].faces[0].vertices[0],prop=m.sectors[2].faces[0].vertices[0];
    assert(world!=prop&&!EditorSector_VerticesEditTogether(&m,0,world,2,prop));
    double original[3];memcpy(original,m.vertices[world].position,sizeof(original));
    double moved[3]={m.vertices[prop].position[0]+8,m.vertices[prop].position[1],m.vertices[prop].position[2]};
    assert(EditorSector_SetVertex(&m,prop,moved,error,sizeof(error)));
    assert(!memcmp(original,m.vertices[world].position,sizeof(original)));
    assert(m.sectors[2].prop_type==EDITOR_SECTOR_PROP_DYNAMIC&&!memcmp(m.sectors[2].prop_velocity,velocity,sizeof velocity));
    assert(EditorSector_Serialize(&m,&text,&length)&&strstr(text,"SECTOR_PROPS\n"));
    assert(EditorSector_Parse(&loaded,text,length,error,sizeof(error)));free(text);
    assert(loaded.num_sectors==4);
    for(int type=EDITOR_SECTOR_PROP_STATIC;type<=EDITOR_SECTOR_PROP_MOVABLE;++type)assert(loaded.sectors[type].prop_type==(editor_sector_prop_type_t)type);
    assert(!memcmp(loaded.sectors[2].prop_velocity,velocity,sizeof velocity));
    assert(!EditorSector_SetPropType(&loaded,2,EDITOR_SECTOR_WORLD,error,sizeof(error)));
    assert(loaded.sectors[2].prop_type==EDITOR_SECTOR_PROP_DYNAMIC);
    assert(EditorSector_SetPropType(&loaded,2,EDITOR_SECTOR_PROP_STATIC,error,sizeof(error)));
    assert(EditorSector_SetPropType(&loaded,2,EDITOR_SECTOR_PROP_DYNAMIC,error,sizeof(error)));
    assert(!memcmp(loaded.sectors[2].prop_velocity,velocity,sizeof velocity));
    EditorSector_Free(&loaded);EditorSector_Free(&m);

    /* Converting an adjoining room into an independent prop detaches its
     * shared indices. A portal-connected room cannot be converted. */
    const double a[]={0,0,0},b[]={128,128,128},c[]={128,0,0},d[]={256,128,128};
    assert(EditorSector_Init(&m));expect_box(&m,a,b);expect_box(&m,c,d);
    size_t shared=vertex_at(&m,128,0,0);
    assert(EditorSector_SetPropType(&m,1,EDITOR_SECTOR_PROP_DYNAMIC,error,sizeof(error)));
    size_t detached=SIZE_MAX;
    for(size_t f=0;f<m.sectors[1].num_faces;++f)for(size_t i=0;i<m.sectors[1].faces[f].num_vertices;++i){
        size_t v=m.sectors[1].faces[f].vertices[i];if(!memcmp(m.vertices[v].position,m.vertices[shared].position,sizeof m.vertices[v].position))detached=v;}
    assert(detached!=SIZE_MAX&&detached!=shared);
    double newpoint[]={144,0,0};assert(EditorSector_SetVertex(&m,detached,newpoint,error,sizeof(error)));
    assert(m.vertices[shared].position[0]==128&&m.sectors[1].prop_type==EDITOR_SECTOR_PROP_DYNAMIC);
    assert(!EditorSector_MergeVertices(&m,detached,shared,error,sizeof(error)));
    EditorSector_Free(&m);
    assert(EditorSector_Init(&m));expect_box(&m,a,b);expect_box(&m,c,d);
    assert(EditorSector_LinkPortal(&m,0,1,1,0,error,sizeof(error)));
    assert(!EditorSector_SetPropType(&m,1,EDITOR_SECTOR_PROP_STATIC,error,sizeof(error)));
    assert(m.sectors[1].prop_type==EDITOR_SECTOR_WORLD&&EditorSector_Validate(&m,error,sizeof(error)));
    EditorSector_Free(&m);
}
static void test_duplicate_prop(void)
{
    editor_sector_map_t m={0},loaded={0};char error[256]={0},*text=NULL,*after=NULL;size_t length=0,n=0,copy=SIZE_MAX;
    const double lo[]={0,0,0},hi[]={512,512,256},plo[]={32,32,32},phi[]={96,96,96};
    const double velocity[]={2,-3,4},offset[]={128,0,0};
    assert(EditorSector_Init(&m));expect_box(&m,lo,hi);
    assert(EditorSector_AddBoxOfType(&m,plo,phi,"stone1",EDITOR_SECTOR_PROP_DYNAMIC,NULL,error,sizeof error));
    assert(EditorSector_SetPropVelocity(&m,1,velocity,error,sizeof error));
    strcpy(m.sectors[1].faces[0].material,"stone2");
    assert(EditorSector_DuplicateProp(&m,1,offset,&copy,error,sizeof error)&&copy==2);
    assert(m.sectors[copy].prop_type==EDITOR_SECTOR_PROP_DYNAMIC);
    assert(!memcmp(m.sectors[copy].prop_velocity,velocity,sizeof velocity));
    assert(m.sectors[1].num_faces==m.sectors[copy].num_faces);
    for(size_t f=0;f<m.sectors[1].num_faces;++f){const editor_sector_face_t *a=&m.sectors[1].faces[f],*b=&m.sectors[copy].faces[f];
        assert(a->num_vertices==b->num_vertices&&!strcmp(a->material,b->material));
        for(size_t i=0;i<a->num_vertices;++i){size_t av=a->vertices[i],bv=b->vertices[i];assert(av!=bv);
            for(int k=0;k<3;++k)assert(m.vertices[bv].position[k]==m.vertices[av].position[k]+offset[k]);}}
    assert(EditorSector_Serialize(&m,&text,&length));
    const double bad[]={NAN,0,0},far[]={EDITOR_SECTOR_COORD_LIMIT,0,0};
    assert(!EditorSector_DuplicateProp(&m,0,offset,NULL,error,sizeof error));
    assert(!EditorSector_DuplicateProp(&m,1,bad,NULL,error,sizeof error));
    assert(!EditorSector_DuplicateProp(&m,1,far,NULL,error,sizeof error));
    assert(EditorSector_Serialize(&m,&after,&n)&&n==length&&!memcmp(text,after,n));free(after);
    assert(EditorSector_Parse(&loaded,text,length,error,sizeof error));free(text);
    assert(loaded.sectors[copy].prop_type==EDITOR_SECTOR_PROP_DYNAMIC);
    assert(!memcmp(loaded.sectors[copy].prop_velocity,velocity,sizeof velocity));
    assert(!strcmp(loaded.sectors[copy].faces[0].material,"stone2"));
    size_t source=loaded.sectors[1].faces[0].vertices[0],moved=loaded.sectors[copy].faces[0].vertices[0];
    double before[3],target[3];memcpy(before,loaded.vertices[source].position,sizeof before);
    memcpy(target,loaded.vertices[moved].position,sizeof target);target[0]+=8;
    assert(EditorSector_SetVertex(&loaded,moved,target,error,sizeof error));
    assert(!memcmp(before,loaded.vertices[source].position,sizeof before));
    EditorSector_Free(&loaded);EditorSector_Free(&m);
}
static void test_static_collision(void)
{
    editor_sector_map_t m={0},loaded={0},clone={0};char error[256]={0},*text=NULL;size_t n=0,copy;
    const double lo[]={0,0,0},hi[]={512,512,256},plo[]={32,32,32},phi[]={96,96,96};
    const double inside[]={64,64,64},offset[]={128,0,0};bool open,has;
    assert(EditorSector_Init(&m));expect_box(&m,lo,hi);
    assert(EditorSector_AddBoxOfType(&m,plo,phi,"stone1",EDITOR_SECTOR_PROP_STATIC,NULL,error,sizeof error));
    assert(!m.sectors[1].prop_no_collision);
    assert(EditorSector_QueryPointInOpenWorld(&m,inside,&open,&has,error,sizeof error)&&has&&!open);
    assert(EditorSector_Serialize(&m,&text,&n)&&!strstr(text,"SECTOR_COLLISION"));
    assert(EditorSector_Parse(&loaded,text,n,error,sizeof error)&&!loaded.sectors[1].prop_no_collision);free(text);EditorSector_Free(&loaded);
    assert(!EditorSector_SetPropCollisionEnabled(&m,0,false,error,sizeof error));
    assert(EditorSector_SetPropCollisionEnabled(&m,1,false,error,sizeof error));
    assert(EditorSector_QueryPointInOpenWorld(&m,inside,&open,&has,error,sizeof error)&&has&&open);
    assert(EditorSector_Clone(&clone,&m)&&clone.sectors[1].prop_no_collision);EditorSector_Free(&clone);
    assert(EditorSector_DuplicateProp(&m,1,offset,&copy,error,sizeof error)&&m.sectors[copy].prop_no_collision);
    assert(EditorSector_Serialize(&m,&text,&n)&&strstr(text,"SECTOR_COLLISION\n1\n0\n0\n"));
    assert(EditorSector_Parse(&loaded,text,n,error,sizeof error)&&loaded.sectors[1].prop_no_collision&&loaded.sectors[copy].prop_no_collision);free(text);
    assert(EditorSector_SetPropCollisionEnabled(&loaded,1,true,error,sizeof error));
    assert(EditorSector_QueryPointInOpenWorld(&loaded,inside,&open,&has,error,sizeof error)&&!open);
    assert(EditorSector_SetPropType(&loaded,copy,EDITOR_SECTOR_PROP_DYNAMIC,error,sizeof error)&&!loaded.sectors[copy].prop_no_collision);
    EditorSector_Free(&loaded);EditorSector_Free(&m);
}
static void test_coincident_vertex_edit(void)
{
    editor_sector_map_t m={0},loaded={0};char error[256]={0},*before=NULL,*after=NULL;size_t n=0,k=0;
    const double a[]={0,0,0},b[]={128,128,128},c[]={128,0,0},d[]={256,128,128};
    const double e[]={128.0001,-128,0},f[]={256,0,128};
    assert(EditorSector_Init(&m));expect_box(&m,a,b);expect_box(&m,c,d);expect_box(&m,e,f);
    size_t source=vertex_at(&m,128,0,128),duplicate=duplicate_sector_vertex(&m,1,source);
    size_t nearby=vertex_at(&m,128.0001,0,128);assert(EditorSector_Validate(&m,error,sizeof(error)));
    /* No-op edits must not triangulate a face or modify the document. */
    assert(EditorSector_Serialize(&m,&before,&n));double original[]={128,0,128};
    assert(EditorSector_SetVertex(&m,source,original,error,sizeof(error)));
    assert(EditorSector_Serialize(&m,&after,&k)&&n==k&&!memcmp(before,after,n));free(before);free(after);
    double moved[]={128,0,144};
    if(!EditorSector_SetVertex(&m,source,moved,error,sizeof(error))){fprintf(stderr,"coincident move: %s\n",error);abort();}
    assert(m.vertices[source].position[2]==144&&m.vertices[duplicate].position[2]==144);
    assert(m.vertices[nearby].position[0]==128.0001&&m.vertices[nearby].position[2]==128);
    assert(EditorSector_Validate(&m,error,sizeof(error)));
    assert(EditorSector_Serialize(&m,&before,&n)&&EditorSector_Parse(&loaded,before,n,error,sizeof(error)));free(before);
    assert(loaded.vertices[source].position[2]==144&&loaded.vertices[duplicate].position[2]==144);
    /* An invalid move is all-or-nothing, including every coincident alias. */
    assert(EditorSector_Serialize(&loaded,&before,&n));double invalid[]={256,128,128};
    assert(!EditorSector_SetVertex(&loaded,source,invalid,error,sizeof(error)));
    assert(EditorSector_Serialize(&loaded,&after,&k)&&n==k&&!memcmp(before,after,n));free(before);free(after);
    EditorSector_Free(&loaded);EditorSector_Free(&m);

    /* Snapping a shared corner to another vertex moves and merges every
     * coincident alias, even in an unlinked, independently indexed room. */
    const double g[]={0,128,128},h[]={128,256,256},i[]={144,128,128},j[]={272,256,256};
    assert(EditorSector_Init(&m));expect_box(&m,a,b);expect_box(&m,g,h);expect_box(&m,i,j);
    source=vertex_at(&m,128,128,128);duplicate=duplicate_sector_vertex(&m,1,source);
    size_t target=vertex_at(&m,144,128,128);
    assert(EditorSector_Serialize(&m,&before,&n));size_t invalid_target=vertex_at(&m,0,0,0);
    assert(!EditorSector_MergeVertices(&m,source,invalid_target,error,sizeof(error)));
    assert(EditorSector_Serialize(&m,&after,&k)&&n==k&&!memcmp(before,after,n));free(before);free(after);
    if(!EditorSector_MergeVertices(&m,source,target,error,sizeof(error))){fprintf(stderr,"coincident merge: %s\n",error);abort();}
    for(size_t s=0;s<3;++s){size_t references=0;
        for(size_t face=0;face<m.sectors[s].num_faces;++face)for(size_t v=0;v<m.sectors[s].faces[face].num_vertices;++v)
            references+=m.sectors[s].faces[face].vertices[v]==target;
        assert(references>=3);}
    (void)duplicate;assert(EditorSector_Validate(&m,error,sizeof(error)));EditorSector_Free(&m);

    /* Aliases at a junction between two linked portals all follow the edit.
     * Portal-only maintenance must not weld an unlinked coincident room. */
    const double chain_lo[]={128,128,0},chain_hi[]={256,256,128};
    assert(EditorSector_Init(&m));expect_box(&m,a,b);expect_box(&m,c,d);expect_box(&m,chain_lo,chain_hi);
    assert(EditorSector_LinkPortal(&m,0,1,1,0,error,sizeof(error)));
    assert(EditorSector_LinkPortal(&m,1,3,2,2,error,sizeof(error)));
    source=vertex_at(&m,128,128,128);size_t mid=duplicate_sector_vertex(&m,1,source),last=duplicate_sector_vertex(&m,2,source);
    assert(EditorSector_Validate(&m,error,sizeof(error)));double chain_moved[]={128,128,144};
    if(!EditorSector_SetVertex(&m,source,chain_moved,error,sizeof(error))){fprintf(stderr,"portal junction move: %s\n",error);abort();}
    assert(m.vertices[source].position[2]==144&&m.vertices[mid].position[2]==144&&m.vertices[last].position[2]==144);
    assert(EditorSector_Validate(&m,error,sizeof(error)));EditorSector_Free(&m);

    const double unlinked_lo[]={128,128,128},unlinked_hi[]={256,256,256};
    assert(EditorSector_Init(&m));expect_box(&m,a,b);expect_box(&m,c,d);expect_box(&m,unlinked_lo,unlinked_hi);
    assert(EditorSector_LinkPortal(&m,0,1,1,0,error,sizeof(error)));
    source=vertex_at(&m,128,128,128);mid=duplicate_sector_vertex(&m,1,source);last=duplicate_sector_vertex(&m,2,source);
    assert(EditorSector_MergePortalVertices(&m,mid,source,error,sizeof(error)));
    size_t unlinked_references=0;
    for(size_t face=0;face<m.sectors[2].num_faces;++face)for(size_t v=0;v<m.sectors[2].faces[face].num_vertices;++v)
        unlinked_references+=m.sectors[2].faces[face].vertices[v]==last;
    assert(unlinked_references>=3&&EditorSector_Validate(&m,error,sizeof(error)));EditorSector_Free(&m);
}
static void test_merge_vertices(void)
{
    char error[256]={0};editor_sector_map_t m={0};assert(EditorSector_Init(&m));
    const double a[]={0,0,0},b[]={128,128,128},c[]={144,128,128},d[]={272,256,256};
    expect_box(&m,a,b);expect_box(&m,c,d);
    size_t source=vertex_at(&m,128,128,128),target=vertex_at(&m,144,128,128);
    assert(EditorSector_MergeVertices(&m,source,target,error,sizeof(error)));
    size_t references[2]={0,0};for(size_t s=0;s<2;++s)for(size_t f=0;f<m.sectors[s].num_faces;++f)
        for(size_t v=0;v<m.sectors[s].faces[f].num_vertices;++v)references[s]+=m.sectors[s].faces[f].vertices[v]==target;
    assert(references[0]>=3&&references[1]>=3);
    char *before=NULL,*after=NULL;size_t n=0,k=0;assert(EditorSector_Serialize(&m,&before,&n));
    assert(!EditorSector_MergeVertices(&m,target,m.num_vertices,error,sizeof(error)));
    assert(EditorSector_Serialize(&m,&after,&k)&&n==k&&!memcmp(before,after,n));free(before);free(after);
    EditorSector_Free(&m);
    /* Collapsing an edge removes its degenerate triangles only when the
       resulting solid still validates as a closed manifold. */
    assert(EditorSector_Init(&m));expect_box(&m,a,b);
    source=vertex_at(&m,0,0,0);target=vertex_at(&m,128,0,0);
    assert(EditorSector_MergeVertices(&m,source,target,error,sizeof(error)));
    assert(EditorSector_Validate(&m,error,sizeof(error)));EditorSector_Free(&m);
    /* Joining opposite corners pinches the shell into disconnected fans. */
    assert(EditorSector_Init(&m));expect_box(&m,a,b);
    source=vertex_at(&m,0,0,0);target=vertex_at(&m,128,128,128);
    assert(EditorSector_Serialize(&m,&before,&n));
    assert(!EditorSector_MergeVertices(&m,source,target,error,sizeof(error))&&strstr(error,"non-manifold"));
    assert(EditorSector_Serialize(&m,&after,&k)&&n==k&&!memcmp(before,after,n));free(before);free(after);
    EditorSector_Free(&m);
    /* Old documents can contain separate indices on the two sides of a
       portal; welding them preserves the portal and later shared edits. */
    assert(EditorSector_Init(&m));const double e[]={128,0,0},f[]={256,128,128};expect_box(&m,a,b);expect_box(&m,e,f);
    assert(EditorSector_LinkPortal(&m,0,1,1,0,error,sizeof(error)));
    target=vertex_at(&m,128,0,0);source=m.num_vertices;
    void *vertices=realloc(m.vertices,(m.num_vertices+1)*sizeof(*m.vertices));assert(vertices);m.vertices=vertices;
    m.vertices[m.num_vertices++]=m.vertices[target];
    for(size_t j=0;j<m.sectors[1].num_faces;++j)for(size_t v=0;v<m.sectors[1].faces[j].num_vertices;++v)
        if(m.sectors[1].faces[j].vertices[v]==target)m.sectors[1].faces[j].vertices[v]=source;
    assert(EditorSector_Validate(&m,error,sizeof(error)));
    assert(EditorSector_MergeVertices(&m,source,target,error,sizeof(error)));
    assert(EditorSector_Validate(&m,error,sizeof(error)));
    double moved[]={128,0,16};assert(EditorSector_SetVertex(&m,target,moved,error,sizeof(error)));
    assert(EditorSector_Validate(&m,error,sizeof(error)));EditorSector_Free(&m);
}
/* Two stacked L-shaped prisms exercise concave, sloped portal faces and
   non-convex shells without restricting sectors to horizontal polygons. */
static void make_concave_stack(editor_sector_map_t *m)
{
    const double xy[6][2]={{0,0},{128,0},{128,64},{64,64},{64,128},{0,128}};
    char error[256]={0};assert(EditorSector_Init(m));
    m->vertices=calloc(18,sizeof(*m->vertices));assert(m->vertices);m->num_vertices=18;
    for(size_t layer=0;layer<3;++layer)for(size_t i=0;i<6;++i){double *p=m->vertices[layer*6+i].position;
        p[0]=xy[i][0];p[1]=xy[i][1];p[2]=128*layer+p[0]*0.25+p[1]*0.125;}
    m->sectors=calloc(2,sizeof(*m->sectors));assert(m->sectors);m->num_sectors=2;
    for(size_t s=0;s<2;++s){editor_sector_t *sector=&m->sectors[s];sector->faces=calloc(8,sizeof(*sector->faces));assert(sector->faces);sector->num_faces=8;
        for(size_t f=0;f<8;++f){editor_sector_face_t *face=&sector->faces[f];face->num_vertices=f<2?6:4;face->vertices=calloc(face->num_vertices,sizeof(*face->vertices));assert(face->vertices);
            strcpy(face->material,"stone1");face->portal_sector=face->portal_face=EDITOR_SECTOR_NO_PORTAL;}
        for(size_t i=0;i<6;++i){sector->faces[0].vertices[i]=s*6+5-i;sector->faces[1].vertices[i]=(s+1)*6+i;
            size_t *side=sector->faces[i+2].vertices;side[0]=s*6+i;side[1]=s*6+(i+1)%6;side[2]=(s+1)*6+(i+1)%6;side[3]=(s+1)*6+i;}
    }
    if(!EditorSector_Validate(m,error,sizeof(error))){fprintf(stderr,"concave shell: %s\n",error);abort();}
    if(!EditorSector_LinkPortal(m,0,1,1,0,error,sizeof(error))){fprintf(stderr,"concave portal: %s\n",error);abort();}
    assert(EditorSector_TriangulateFace(m,0,1,NULL,0)==4);
    size_t vertex=m->sectors[0].faces[1].vertices[0];double point[3];memcpy(point,m->vertices[vertex].position,sizeof(point));point[2]+=16;
    if(!EditorSector_SetVertex(m,vertex,point,error,sizeof(error))){fprintf(stderr,"concave portal corner: %s\n",error);abort();}
    assert(EditorSector_Validate(m,error,sizeof(error)));
    size_t player;assert(EditorMap_AddEntity(&m->entities,"info_player_start",&player));assert(EditorMap_SetProperty(&m->entities.entities[player],"origin","32 32 64"));
    size_t light;assert(EditorMap_AddEntity(&m->entities,"light",&light));assert(EditorMap_SetProperty(&m->entities.entities[light],"origin","32 32 96"));assert(EditorMap_SetProperty(&m->entities.entities[light],"light","400"));
}
static void test_knife(void)
{
    char error[256]={0};editor_sector_map_t m={0},copy={0};char *before=NULL,*after=NULL;size_t n=0,len=0;
    const double lo[]={0,0,0},hi[]={128,128,128},other_lo[]={128,0,0},other_hi[]={256,128,128};
    assert(EditorSector_Init(&m));expect_box(&m,lo,hi);
    const double a[]={0,32,0},b[]={0,32,128};
    if(!EditorSector_CutFace(&m,0,0,a,b,error,sizeof(error))){fprintf(stderr,"knife box: %s\n",error);abort();}
    assert(m.sectors[0].num_faces==7&&m.num_vertices==10&&EditorSector_Validate(&m,error,sizeof(error)));
    assert(!m.sectors[0].faces[0].source_group&&!m.sectors[0].faces[6].source_group);
    assert(EditorSector_Serialize(&m,&before,&len)&&EditorSector_Parse(&copy,before,len,error,sizeof(error)));
    assert(copy.sectors[0].num_faces==7&&!copy.sectors[0].faces[0].source_group&&!copy.sectors[0].faces[6].source_group);
    const double outside[]={0,32,129};
    assert(!EditorSector_CutFace(&copy,0,0,a,outside,error,sizeof(error)));
    assert(EditorSector_Serialize(&copy,&after,&n)&&n==len&&!memcmp(before,after,len));
    free(before);free(after);EditorSector_Free(&copy);EditorSector_Free(&m);

    assert(EditorSector_Init(&m));expect_box(&m,lo,hi);expect_box(&m,other_lo,other_hi);
    assert(EditorSector_LinkPortal(&m,0,1,1,0,error,sizeof(error)));
    const double pa[]={128,48,0},pb[]={128,48,128};
    if(!EditorSector_CutFace(&m,0,1,pa,pb,error,sizeof(error))){fprintf(stderr,"knife portal: %s\n",error);abort();}
    assert(m.sectors[0].num_faces==7&&m.sectors[1].num_faces==7&&EditorSector_Validate(&m,error,sizeof(error)));
    assert(EditorSector_Serialize(&m,&before,&len)&&EditorSector_Parse(&copy,before,len,error,sizeof(error)));
    assert(EditorSector_Validate(&copy,error,sizeof(error)));free(before);EditorSector_Free(&copy);EditorSector_Free(&m);

    /* Cutting a neighboring nonportal face subdivides the shared portal edge
     * on both shells without splitting or breaking the portal itself. */
    assert(EditorSector_Init(&m));expect_box(&m,lo,hi);expect_box(&m,other_lo,other_hi);
    assert(EditorSector_LinkPortal(&m,0,1,1,0,error,sizeof(error)));
    const double ea[]={0,48,128},eb[]={128,48,128};
    if(!EditorSector_CutFace(&m,0,5,ea,eb,error,sizeof(error))){fprintf(stderr,"knife portal edge: %s\n",error);abort();}
    assert(m.sectors[0].num_faces==7&&m.sectors[1].num_faces==6);
    assert(m.sectors[0].faces[1].num_vertices==5&&m.sectors[1].faces[0].num_vertices==5);
    assert(EditorSector_Validate(&m,error,sizeof(error)));EditorSector_Free(&m);

    /* Deliberate knife pieces remain independent after save and link. */
    assert(EditorSector_Init(&m));expect_box(&m,lo,hi);expect_box(&m,other_lo,other_hi);
    assert(EditorSector_CutFace(&m,0,1,pa,pb,error,sizeof(error)));
    assert(EditorSector_CutFace(&m,1,0,pa,pb,error,sizeof(error)));
    strcpy(m.sectors[0].faces[1].material,"stone2");
    assert(strcmp(m.sectors[0].faces[1].material,m.sectors[0].faces[6].material));
    assert(EditorSector_Serialize(&m,&before,&len)&&EditorSector_Parse(&copy,before,len,error,sizeof(error)));free(before);
    assert(!strcmp(copy.sectors[0].faces[1].material,"stone2")&&!strcmp(copy.sectors[0].faces[6].material,"stone1"));
    if(!EditorSector_LinkPortal(&copy,0,1,1,0,error,sizeof(error)))
        assert(EditorSector_LinkPortal(&copy,0,1,1,6,error,sizeof(error)));
    assert(copy.sectors[0].faces[6].portal_sector==EDITOR_SECTOR_NO_PORTAL);
    assert(EditorSector_Validate(&copy,error,sizeof(error)));EditorSector_Free(&copy);EditorSector_Free(&m);
}
static void test_point_open_world(void)
{
    editor_sector_map_t map={0};char error[256]={0};bool open=true,has=true;
    const double lo[]={0,0,0},hi[]={128,128,128},point[]={16,16,16},edge[]={0,32,32},outside[]={-1,32,32};
    assert(EditorSector_Init(&map));
    assert(EditorSector_QueryPointInOpenWorld(&map,point,&open,&has,error,sizeof error)&&!open&&!has);
    expect_box(&map,lo,hi);
    assert(EditorSector_QueryPointInOpenWorld(&map,point,&open,&has,error,sizeof error)&&open&&has);
    assert(EditorSector_QueryPointInOpenWorld(&map,edge,&open,&has,error,sizeof error)&&!open&&has);
    assert(EditorSector_QueryPointInOpenWorld(&map,outside,&open,&has,error,sizeof error)&&!open&&has);
    const double plo[]={32,32,32},phi[]={64,64,64},solid[]={48,48,48},prop_edge[]={32,48,48};
    assert(EditorSector_AddBoxOfType(&map,plo,phi,"stone1",EDITOR_SECTOR_PROP_MOVABLE,NULL,error,sizeof error));
    assert(EditorSector_QueryPointInOpenWorld(&map,solid,&open,&has,error,sizeof error)&&!open&&has);
    assert(EditorSector_QueryPointInOpenWorld(&map,prop_edge,&open,&has,error,sizeof error)&&!open&&has);
    assert(EditorSector_QueryPointInOpenWorld(&map,point,&open,&has,error,sizeof error)&&open&&has);
    const double points[][3]={{16,16,16},{48,48,48},{32,48,48},{0,32,32},{-1,32,32}};bool results[5];
    assert(EditorSector_QueryPointsInOpenWorld(&map,points,5,results,&has,error,sizeof error)&&has);
    assert(results[0]&&!results[1]&&!results[2]&&!results[3]&&!results[4]);
    assert(!EditorSector_QueryPointsInOpenWorld(&map,points,1025,results,&has,error,sizeof error));
    const double invalid[]={NAN,0,0};
    assert(!EditorSector_QueryPointInOpenWorld(&map,invalid,&open,&has,error,sizeof error)&&*error);
    editor_sector_prop_type_t saved=map.sectors[0].prop_type;map.sectors[0].prop_type=(editor_sector_prop_type_t)99;
    assert(!EditorSector_QueryPointInOpenWorld(&map,point,&open,&has,error,sizeof error)&&*error);
    map.sectors[0].prop_type=saved;EditorSector_Free(&map);
    make_concave_stack(&map);
    const double inside_l[]={32,96,64},notch[]={96,96,64};
    assert(EditorSector_QueryPointInOpenWorld(&map,inside_l,&open,&has,error,sizeof error)&&open&&has);
    assert(EditorSector_QueryPointInOpenWorld(&map,notch,&open,&has,error,sizeof error)&&!open&&has);
    EditorSector_Free(&map);
}
static void test_light_color_points(void)
{
    editor_sector_map_t map={0},undo={0},redo={0},loaded={0};editor_map_t compiled={0};
    char error[256]={0};const double lo[3]={0,0,0},hi[3]={128,128,128},origin[3]={32,48,64},rgb[3]={1.0/255,.25,1};
    double got[3];size_t light;
    assert(EditorSector_Init(&map));expect_box(&map,lo,hi);
    assert(EditorMap_AddLight(&map.entities,origin,&light));
    assert(EditorSector_Clone(&undo,&map));
    assert(EditorMap_SetLightColor(&map.entities,light,rgb));
    assert(EditorSector_Clone(&redo,&map));
    assert(EditorSector_Clone(&map,&undo));
    assert(EditorMap_GetLightColor(&map.entities,light,got)&&got[0]==1&&got[1]==1&&got[2]==1);
    assert(EditorSector_Clone(&map,&redo));
    assert(EditorMap_GetLightColor(&map.entities,light,got)&&!memcmp(got,rgb,sizeof(got)));
    char *text=NULL;size_t length;
    assert(EditorSector_Serialize(&map,&text,&length));
    assert(EditorSector_Parse(&loaded,text,length,error,sizeof(error)));free(text);
    assert(EditorMap_GetLightColor(&loaded.entities,light,got)&&!memcmp(got,rgb,sizeof(got)));
    assert(EditorSector_Compile(&loaded,&compiled,error,sizeof(error)));
    bool found=false;for(size_t i=1;i<compiled.num_entities;++i)
        if(EditorMap_GetLightColor(&compiled,i,got)){assert(!memcmp(got,rgb,sizeof(got)));found=true;}
    assert(found);EditorMap_Free(&compiled);
    EditorSector_Free(&map);EditorSector_Free(&undo);EditorSector_Free(&redo);EditorSector_Free(&loaded);
}
static void test_static_model_points(void)
{
    editor_sector_map_t source={0},loaded={0};editor_map_t compiled={0};char error[256]={0};
    assert(EditorSector_Init(&source));const double mins[3]={0,0,0},maxs[3]={128,128,128},origin[3]={32,48,64};
    expect_box(&source,mins,maxs);size_t index;
    assert(EditorMap_AddModel(&source.entities,"progs/sponza.md5mesh",origin,&index));
    editor_map_model_t model;assert(EditorMap_GetModel(&source.entities,index,&model));
    model.angles[1]=90;model.modelscale=2;model.skin=1;model.collision=false;
    assert(EditorMap_SetModel(&source.entities,index,&model));
    char *text=NULL;size_t length=0;assert(EditorSector_Serialize(&source,&text,&length));
    assert(EditorSector_Parse(&loaded,text,length,error,sizeof(error)));free(text);
    assert(EditorMap_GetModel(&loaded.entities,index,&model)&&model.angles[1]==90&&model.modelscale==2&&model.skin==1&&!model.collision);
    assert(EditorSector_Compile(&loaded,&compiled,error,sizeof(error)));
    bool found=false;for(size_t i=1;i<compiled.num_entities;++i)if(EditorMap_GetModel(&compiled,i,&model))
    {assert(!strcmp(model.model,"progs/sponza.md5mesh")&&model.origin[0]==32&&model.origin[1]==48&&model.origin[2]==64);found=true;}
    assert(found);EditorMap_Free(&compiled);EditorSector_Free(&source);EditorSector_Free(&loaded);
}
int main(int argc,char **argv)
{
    test_static_model_points();
    test_light_color_points();
    test_point_open_world();
    test_duplicate_prop();
    test_sector_props();
    test_static_collision();
    test_coincident_vertex_edit();
    test_merge_vertices();
    test_knife();
    char error[256]={0};editor_sector_map_t m={0},copy={0};editor_map_t compiled={0};
    assert(EditorSector_Init(&m));
    const double lo[]={-128,-128,0}, hi[]={128,128,128}, up_lo[]={-128,-128,128}, up_hi[]={128,128,256};
    expect_box(&m,lo,hi);expect_box(&m,up_lo,up_hi);
    assert(EditorSector_LinkPortal(&m,0,5,1,4,error,sizeof(error)));
    assert(m.sectors[0].faces[5].portal_sector==1&&m.sectors[1].faces[4].portal_sector==0);
    for(size_t i=0;i<m.num_vertices;++i)m.vertices[i].position[2]+=m.vertices[i].position[0]*0.5+m.vertices[i].position[1]*0.25;
    if(!EditorSector_Validate(&m,error,sizeof(error))){fprintf(stderr,"sloped stacked sectors: %s\n",error);return 1;}
    for(size_t s=0;s<m.num_sectors;++s)for(size_t f=0;f<m.sectors[s].num_faces;++f){
        size_t n=EditorSector_TriangulateFace(&m,s,f,NULL,0),triangles[256][3];
        assert(n==2&&EditorSector_TriangulateFace(&m,s,f,triangles,256)==2);
        assert(!EditorSector_TriangulateFace(&m,s,f,triangles,1));
    }
    assert(EditorMap_SetProperty(&m.entities.entities[0],"message","sector source round trip"));
    size_t player;assert(EditorMap_AddEntity(&m.entities,"info_player_start",&player));assert(EditorMap_SetProperty(&m.entities.entities[player],"origin","0 0 32"));
    size_t light;assert(EditorMap_AddEntity(&m.entities,"light",&light));assert(EditorMap_SetProperty(&m.entities.entities[light],"origin","0 0 80"));assert(EditorMap_SetProperty(&m.entities.entities[light],"light","400"));
    if(argc==3)assert(EditorMap_SetProperty(&m.entities.entities[0],"wad",argv[2]));
    char *text=NULL;size_t len=0;assert(EditorSector_Serialize(&m,&text,&len));assert(EditorSector_IsFormat(text,len));
    assert(EditorSector_Parse(&copy,text,len,error,sizeof(error)));assert(!strcmp(EditorMap_GetProperty(&copy.entities.entities[0],"message"),"sector source round trip"));
    assert(copy.num_sectors==2&&copy.sectors[0].faces[5].portal_sector==1);
    assert(!EditorSector_Parse(&copy,text,len-1,error,sizeof(error)));assert(copy.num_sectors==2);free(text);
    /* Warping a shared portal corner triangulates both sides identically and
       preserves a traversable, reciprocal portal rather than flattening it. */
    size_t portal_vertex=copy.sectors[0].faces[5].vertices[0];double raised[3];
    memcpy(raised,copy.vertices[portal_vertex].position,sizeof(raised));raised[2]+=16;
    assert(EditorSector_SetVertex(&copy,portal_vertex,raised,error,sizeof(error)));
    size_t portals=0;for(size_t s=0;s<copy.num_sectors;++s)for(size_t f=0;f<copy.sectors[s].num_faces;++f)
        if(copy.sectors[s].faces[f].portal_sector!=EDITOR_SECTOR_NO_PORTAL){assert(copy.sectors[s].faces[f].num_vertices==3);++portals;}
    assert(portals==4&&EditorSector_Validate(&copy,error,sizeof(error)));
    assert(EditorSector_Serialize(&copy,&text,&len));assert(EditorSector_Parse(&m,text,len,error,sizeof(error)));free(text);
    assert(EditorSector_Compile(&copy,&compiled,error,sizeof(error)));assert(compiled.entities[0].num_brushes>0);
    if(argc==3){assert(EditorMap_Serialize(&compiled,&text,&len));FILE *f=fopen(argv[1],"wb");assert(f);assert(fwrite(text,1,len,f)==len);assert(!fclose(f));free(text);}
    EditorMap_Free(&compiled);EditorSector_Free(&copy);EditorSector_Free(&m);

    assert(EditorSector_Init(&m));expect_box(&m,lo,hi);
    double huge[]={1.0e100,0,0};
    assert(!EditorSector_SetVertex(&m,7,huge,error,sizeof(error))&&*error);
    assert(m.vertices[7].position[2]==128);
    const double nested_lo[]={-64,-64,16},nested_hi[]={64,64,112};expect_overlap_rejected(&m,nested_lo,nested_hi);
    const double overlap_lo[]={120,-64,0},overlap_hi[]={200,64,128};expect_overlap_rejected(&m,overlap_lo,overlap_hi);
    const double cross_lo[]={-256,-8,32},cross_hi[]={256,8,96};expect_overlap_rejected(&m,cross_lo,cross_hi);
    const double touch_lo[]={128,-128,0},touch_hi[]={256,128,128};expect_box(&m,touch_lo,touch_hi);
    assert(EditorSector_LinkPortal(&m,0,1,1,0,error,sizeof(error)));
    assert(EditorSector_UnlinkPortal(&m,0,1,error,sizeof(error)));
    assert(EditorSector_Remove(&m,1,error,sizeof(error)));
    double invalid_corner[]={-200,-200,-300};
    assert(!EditorSector_SetVertex(&m,7,invalid_corner,error,sizeof(error)));
    assert(strstr(error,"self-intersect")&&m.sectors[0].num_faces==6&&m.vertices[7].position[2]==128);
    double corner[]={128,128,160};assert(EditorSector_SetVertex(&m,7,corner,error,sizeof(error)));
    assert(m.sectors[0].num_faces==9);assert(EditorSector_Validate(&m,error,sizeof(error)));
    assert(EditorSector_Compile(&m,&compiled,error,sizeof(error)));EditorMap_Free(&compiled);EditorSector_Free(&m);
    assert(EditorSector_Init(&m));
    const double far_lo[]={100000,0,0},far_hi[]={100128,128,128};expect_box(&m,far_lo,far_hi);
    assert(EditorSector_Compile(&m,&compiled,error,sizeof(error)));EditorMap_Free(&compiled);
    if(argc==4&&!strcmp(argv[3],"far")){
        assert(EditorMap_SetProperty(&m.entities.entities[0],"wad",argv[2]));
        size_t p,l;assert(EditorMap_AddEntity(&m.entities,"info_player_start",&p));assert(EditorMap_SetProperty(&m.entities.entities[p],"origin","100064 64 32"));
        assert(EditorMap_AddEntity(&m.entities,"light",&l));assert(EditorMap_SetProperty(&m.entities.entities[l],"origin","100064 64 80"));assert(EditorMap_SetProperty(&m.entities.entities[l],"light","400"));
        assert(EditorSector_Compile(&m,&compiled,error,sizeof(error)));assert(EditorMap_Serialize(&compiled,&text,&len));
        FILE *f=fopen(argv[1],"wb");assert(f);assert(fwrite(text,1,len,f)==len);assert(!fclose(f));free(text);EditorMap_Free(&compiled);
    }
    EditorSector_Free(&m);
    assert(EditorSector_Init(&m));const double beyond_lo[]={1100000,0,0},beyond_hi[]={1100128,128,128};expect_box(&m,beyond_lo,beyond_hi);
    assert(!EditorSector_Compile(&m,&compiled,error,sizeof(error))&&strstr(error,"winding range"));EditorSector_Free(&m);
    assert(EditorSector_Init(&m));expect_box(&m,lo,hi);
    size_t swap=m.sectors[0].faces[5].vertices[1];m.sectors[0].faces[5].vertices[1]=m.sectors[0].faces[5].vertices[2];m.sectors[0].faces[5].vertices[2]=swap;
    assert(!EditorSector_Validate(&m,error,sizeof(error)));assert(*error);EditorSector_Free(&m);
    make_concave_stack(&m);
    assert(EditorSector_Compile(&m,&compiled,error,sizeof(error)));
    if(argc==4&&!strcmp(argv[3],"concave")){
        assert(EditorMap_SetProperty(&m.entities.entities[0],"wad",argv[2]));EditorMap_Free(&compiled);
        assert(EditorSector_Compile(&m,&compiled,error,sizeof(error)));assert(EditorMap_Serialize(&compiled,&text,&len));
        FILE *f=fopen(argv[1],"wb");assert(f);assert(fwrite(text,1,len,f)==len);assert(!fclose(f));free(text);
    }
    EditorMap_Free(&compiled);EditorSector_Free(&m);
    puts("editor_sector_test: OK");return 0;
}
