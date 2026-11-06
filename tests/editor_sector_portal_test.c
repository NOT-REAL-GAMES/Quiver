/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "editor_sector_portal.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static double face_area(const editor_sector_map_t *m,const editor_sector_face_t *f)
{
    double sum[3]={0,0,0};
    for(size_t i=0;i<f->num_vertices;++i) {
        const double *a=m->vertices[f->vertices[i]].position,*b=m->vertices[f->vertices[(i+1)%f->num_vertices]].position;
        sum[0]+=a[1]*b[2]-a[2]*b[1];sum[1]+=a[2]*b[0]-a[0]*b[2];sum[2]+=a[0]*b[1]-a[1]*b[0];
    }
    return .5*sqrt(sum[0]*sum[0]+sum[1]*sum[1]+sum[2]*sum[2]);
}
static double portal_area(const editor_sector_map_t *m,size_t sector,size_t *count)
{
    double area=0;*count=0;
    for(size_t f=0;f<m->sectors[sector].num_faces;++f) {
        const editor_sector_face_t *face=&m->sectors[sector].faces[f];
        if(face->portal_sector==EDITOR_SECTOR_NO_PORTAL)continue;
        const editor_sector_face_t *other=&m->sectors[face->portal_sector].faces[face->portal_face];
        assert(other->portal_sector==sector&&other->portal_face==f);
        area+=face_area(m,face);++*count;
    }
    return area;
}
static void setup(editor_sector_map_t *map,const double a[3],const double b[3],const double c[3],const double d[3])
{
    char error[512]={0};assert(EditorSector_Init(map));
    assert(EditorSector_AddBox(map,a,b,"stone1",NULL,error,sizeof error));
    assert(EditorSector_AddBox(map,c,d,"stone1",NULL,error,sizeof error));
    assert(EditorSector_Validate(map,error,sizeof error));
}
static void link_case(const double a[3],const double b[3],const double c[3],const double d[3],
    size_t face_a,size_t face_b,double expected_area,int transform)
{
    editor_sector_map_t map={0},roundtrip={0};char error[512]={0};
    setup(&map,a,b,c,d);
    if(transform)for(size_t i=0;i<map.num_vertices;++i) {
        double x=map.vertices[i].position[0],y=map.vertices[i].position[1],z=map.vertices[i].position[2];
        map.vertices[i].position[0]=x+.3*z;map.vertices[i].position[1]=y+.5*x;map.vertices[i].position[2]=z+.2*y;
    }
    assert(EditorSector_Validate(&map,error,sizeof error));
    if(!EditorSector_LinkPartialPortal(&map,0,face_a,1,face_b,error,sizeof error)) {
        fprintf(stderr,"partial portal failed: %s\n",error);abort();
    }
    assert(EditorSector_Validate(&map,error,sizeof error));
    size_t n_a,n_b;double area_a=portal_area(&map,0,&n_a),area_b=portal_area(&map,1,&n_b);
    assert(n_a>=1&&n_a==n_b&&area_a>0&&fabs(area_a-area_b)<1e-6);
    if(expected_area>0)assert(fabs(area_a-expected_area)<1e-6);
    char *encoded=NULL;size_t size=0;assert(EditorSector_Serialize(&map,&encoded,&size));
    if(!EditorSector_Parse(&roundtrip,encoded,size,error,sizeof error)) {
        fprintf(stderr,"portal roundtrip failed: %s\n",error);abort();
    }
    size_t roundtrip_count;assert(fabs(portal_area(&roundtrip,0,&roundtrip_count)-area_a)<1e-6&&roundtrip_count==n_a);
    editor_map_t compiled={0};
    if(!EditorSector_Compile(&roundtrip,&compiled,error,sizeof error)) {
        fprintf(stderr,"portal compilation failed: %s\n",error);abort();
    }
    assert(compiled.num_entities>0&&compiled.entities[0].num_brushes>0);
    size_t first=0;
    while(first<roundtrip.sectors[0].num_faces&&roundtrip.sectors[0].faces[first].portal_sector==EDITOR_SECTOR_NO_PORTAL)++first;
    assert(first<roundtrip.sectors[0].num_faces);
    if(!EditorSector_UnlinkPortalGroup(&roundtrip,0,first,error,sizeof error)) {
        fprintf(stderr,"portal unlink failed: %s\n",error);abort();
    }
    size_t remaining_a,remaining_b;portal_area(&roundtrip,0,&remaining_a);portal_area(&roundtrip,1,&remaining_b);
    assert(!remaining_a&&!remaining_b&&EditorSector_Validate(&roundtrip,error,sizeof error));
    EditorMap_Free(&compiled);free(encoded);EditorSector_Free(&roundtrip);EditorSector_Free(&map);
}
static void reject_case(const double a[3],const double b[3],const double c[3],const double d[3],size_t fa,size_t fb)
{
    editor_sector_map_t map={0};char error[512]={0};char *before=NULL,*after=NULL;size_t before_n=0,after_n=0;
    setup(&map,a,b,c,d);assert(EditorSector_Serialize(&map,&before,&before_n));
    assert(!EditorSector_LinkPartialPortal(&map,0,fa,1,fb,error,sizeof error));assert(*error);
    assert(EditorSector_Serialize(&map,&after,&after_n));assert(before_n==after_n&&!memcmp(before,after,before_n));
    free(before);free(after);EditorSector_Free(&map);
}
static void existing_portal_case(void)
{
    editor_sector_map_t map={0};char error[512]={0};
    const double a[3]={-128,-128,0},b[3]={0,128,128};
    const double c[3]={0,64,0},d[3]={128,128,96};
    const double e[3]={-128,128,0},f[3]={0,256,128};
    setup(&map,a,b,c,d);
    assert(EditorSector_AddBox(&map,e,f,"stone1",NULL,error,sizeof error));
    assert(EditorSector_LinkPortal(&map,0,3,2,2,error,sizeof error));
    if(!EditorSector_LinkPartialPortal(&map,0,1,1,0,error,sizeof error)) {
        fprintf(stderr,"partial portal bordering another portal failed: %s\n",error);abort();
    }
    assert(EditorSector_Validate(&map,error,sizeof error));
    assert(map.sectors[0].faces[3].portal_sector==2&&map.sectors[2].faces[2].portal_sector==0);
    assert(map.sectors[0].faces[3].num_vertices>4&&map.sectors[0].faces[3].num_vertices==map.sectors[2].faces[2].num_vertices);
    size_t selected=0;while(selected<map.sectors[0].num_faces&&map.sectors[0].faces[selected].portal_sector!=1)++selected;
    assert(selected<map.sectors[0].num_faces&&EditorSector_UnlinkPortalGroup(&map,0,selected,error,sizeof error));
    assert(map.sectors[0].faces[3].portal_sector==2&&map.sectors[2].faces[2].portal_sector==0);
    assert(EditorSector_Validate(&map,error,sizeof error));EditorSector_Free(&map);
}
static void concave_case(void)
{
    editor_sector_map_t map={0};char error[512]={0};assert(EditorSector_Init(&map));
    static const double yz[6][2]={{0,0},{128,0},{128,64},{64,64},{64,128},{0,128}};
    map.vertices=calloc(12,sizeof(*map.vertices));assert(map.vertices);map.num_vertices=12;
    for(size_t i=0;i<6;++i)for(int side=0;side<2;++side) {
        size_t id=i+6*(size_t)side;map.vertices[id].position[0]=side?0:-128;
        map.vertices[id].position[1]=yz[i][0];map.vertices[id].position[2]=yz[i][1];
    }
    map.sectors=calloc(1,sizeof(*map.sectors));assert(map.sectors);map.num_sectors=1;
    editor_sector_t *s=&map.sectors[0];s->faces=calloc(8,sizeof(*s->faces));assert(s->faces);s->num_faces=8;
    for(size_t i=0;i<8;++i) {
        editor_sector_face_t *face=&s->faces[i];face->num_vertices=i<2?6:4;
        face->vertices=calloc(face->num_vertices,sizeof(*face->vertices));assert(face->vertices);
        face->portal_sector=face->portal_face=EDITOR_SECTOR_NO_PORTAL;strcpy(face->material,"stone1");
        if(i==0)for(size_t j=0;j<6;++j)face->vertices[j]=5-j;
        else if(i==1)for(size_t j=0;j<6;++j)face->vertices[j]=6+j;
        else {size_t j=i-2,k=(j+1)%6;face->vertices[0]=j;face->vertices[1]=k;face->vertices[2]=k+6;face->vertices[3]=j+6;}
    }
    if(!EditorSector_Validate(&map,error,sizeof error)){fprintf(stderr,"concave fixture invalid: %s\n",error);abort();}
    const double lo[3]={0,-32,16},hi[3]={128,160,112};
    assert(EditorSector_AddBox(&map,lo,hi,"stone1",NULL,error,sizeof error));
    if(!EditorSector_LinkPartialPortal(&map,0,1,1,0,error,sizeof error)) {
        fprintf(stderr,"concave partial portal failed: %s\n",error);abort();
    }
    size_t na,nb;double aa=portal_area(&map,0,&na),ab=portal_area(&map,1,&nb);
    assert(na==nb&&na>1&&fabs(aa-9216)<1e-6&&fabs(ab-aa)<1e-6);
    assert(EditorSector_Validate(&map,error,sizeof error));
    size_t first=0;while(first<map.sectors[0].num_faces&&map.sectors[0].faces[first].portal_sector==EDITOR_SECTOR_NO_PORTAL)++first;
    assert(first<map.sectors[0].num_faces&&EditorSector_UnlinkPortalGroup(&map,0,first,error,sizeof error));
    portal_area(&map,0,&na);portal_area(&map,1,&nb);assert(!na&&!nb);
    EditorSector_Free(&map);
}
static void disconnected_openings_case(void)
{
    editor_sector_map_t map={0};char error[512]={0};assert(EditorSector_Init(&map));
    static const double yz[8][2]={{0,0},{128,0},{128,128},{96,128},{96,32},{32,32},{32,128},{0,128}};
    map.vertices=calloc(16,sizeof(*map.vertices));assert(map.vertices);map.num_vertices=16;
    for(size_t i=0;i<8;++i)for(int side=0;side<2;++side) {
        size_t id=i+8*(size_t)side;map.vertices[id].position[0]=side?0:-128;
        map.vertices[id].position[1]=yz[i][0];map.vertices[id].position[2]=yz[i][1];
    }
    map.sectors=calloc(1,sizeof(*map.sectors));assert(map.sectors);map.num_sectors=1;
    editor_sector_t *s=&map.sectors[0];s->faces=calloc(10,sizeof(*s->faces));assert(s->faces);s->num_faces=10;
    for(size_t i=0;i<10;++i) {
        editor_sector_face_t *face=&s->faces[i];face->num_vertices=i<2?8:4;
        face->vertices=calloc(face->num_vertices,sizeof(*face->vertices));assert(face->vertices);
        face->portal_sector=face->portal_face=EDITOR_SECTOR_NO_PORTAL;strcpy(face->material,"stone1");
        if(i==0)for(size_t j=0;j<8;++j)face->vertices[j]=7-j;
        else if(i==1)for(size_t j=0;j<8;++j)face->vertices[j]=8+j;
        else {size_t j=i-2,k=(j+1)%8;face->vertices[0]=j;face->vertices[1]=k;face->vertices[2]=k+8;face->vertices[3]=j+8;}
    }
    assert(EditorSector_Validate(&map,error,sizeof error));
    const double lo[3]={0,-16,64},hi[3]={128,144,96};
    assert(EditorSector_AddBox(&map,lo,hi,"stone1",NULL,error,sizeof error));
    if(!EditorSector_LinkPartialPortal(&map,0,1,1,0,error,sizeof error)) {
        fprintf(stderr,"disconnected partial portals failed: %s\n",error);abort();
    }
    size_t na,nb;double area=portal_area(&map,0,&na);portal_area(&map,1,&nb);
    assert(na==nb&&na>1&&fabs(area-2048)<1e-6);
    size_t first=0;while(first<map.sectors[0].num_faces&&map.sectors[0].faces[first].portal_sector==EDITOR_SECTOR_NO_PORTAL)++first;
    assert(first<map.sectors[0].num_faces&&EditorSector_UnlinkPortalGroup(&map,0,first,error,sizeof error));
    area=portal_area(&map,0,&na);double other=portal_area(&map,1,&nb);
    assert(na>0&&na==nb&&fabs(area-1024)<1e-6&&fabs(area-other)<1e-6);
    assert(EditorSector_Validate(&map,error,sizeof error));EditorSector_Free(&map);
}
/* A vertex edit triangulates the affected planar quad. Restoring the corner
 * leaves its two coplanar triangles as separate source faces, as happens in
 * the editor after an undo-independent shape edit. */
static size_t split_wall(editor_sector_map_t *map)
{
    char error[512]={0};size_t vertex=SIZE_MAX;
    for(size_t i=0;i<map->num_vertices;++i){
        const double *p=map->vertices[i].position;
        if(p[0]==0 && p[1]==256 && p[2]==192){vertex=i;break;}
    }
    assert(vertex!=SIZE_MAX);
    const double raised[3]={0,256,208}, restored[3]={0,256,192};
    assert(EditorSector_SetVertex(map,vertex,raised,error,sizeof error));
    assert(EditorSector_SetVertex(map,vertex,restored,error,sizeof error));
    size_t count=0,first=SIZE_MAX;
    for(size_t f=0;f<map->sectors[0].num_faces;++f){
        const editor_sector_face_t *face=&map->sectors[0].faces[f];
        bool wall=face->num_vertices==3;
        for(size_t i=0;i<face->num_vertices;++i)wall &= fabs(map->vertices[face->vertices[i]].position[0])<1e-8;
        if(wall){if(first==SIZE_MAX)first=f;++count;}
    }
    assert(count==2 && first!=SIZE_MAX);
    return first;
}
/* Produce an actual old QSECTOR 1 document without the optional group block. */
static void old_format_clone(editor_sector_map_t *out,const editor_sector_map_t *map)
{
    char error[512]={0},*text=NULL;size_t length=0;
    assert(EditorSector_Serialize(map,&text,&length));
    char *groups=strstr(text,"FACE_GROUPS\n"),*entities=groups?strstr(groups,"ENTITIES "):NULL;
    assert(groups&&entities&&entities>groups);
    size_t removed=(size_t)(entities-groups);memmove(groups,entities,length-(size_t)(entities-text));length-=removed;
    assert(EditorSector_Parse(out,text,length,error,sizeof error));free(text);
}
static void triangulated_wall_case(void)
{
    const double a[3]={-256,-256,0},b[3]={0,256,192},c[3]={0,-48,0},d[3]={256,48,128};
    editor_sector_map_t map={0},saved={0},legacy={0};char error[512]={0};setup(&map,a,b,c,d);
    size_t first=split_wall(&map), tested=0;
    char *encoded=NULL;size_t encoded_length=0;
    assert(EditorSector_Serialize(&map,&encoded,&encoded_length));
    assert(EditorSector_Parse(&saved,encoded,encoded_length,error,sizeof error));free(encoded);
    old_format_clone(&legacy,&map);
    for(size_t face=first;face<map.sectors[0].num_faces;++face){
        const editor_sector_face_t *source=&map.sectors[0].faces[face];bool wall=source->num_vertices==3;
        for(size_t i=0;i<source->num_vertices;++i)wall &= fabs(map.vertices[source->vertices[i]].position[0])<1e-8;
        if(!wall)continue;
        for(int source_map=0;source_map<3;++source_map){
        editor_sector_map_t linked={0},roundtrip={0};char *text=NULL;size_t length=0,na,nb;
        assert(EditorSector_Clone(&linked,source_map==0?&map:source_map==1?&saved:&legacy));
        if(!EditorSector_LinkFaceRegions(&linked,0,face,1,0,error,sizeof error)){
            fprintf(stderr,"triangulated wall link failed: %s\n",error);abort();
        }
        double area=portal_area(&linked,0,&na), other=portal_area(&linked,1,&nb);
        assert(na==nb && na>0 && fabs(area-96*128)<1e-6 && fabs(other-area)<1e-6);
        assert(EditorSector_Validate(&linked,error,sizeof error));
        assert(EditorSector_Serialize(&linked,&text,&length));
        assert(EditorSector_Parse(&roundtrip,text,length,error,sizeof error));
        assert(fabs(portal_area(&roundtrip,0,&na)-96*128)<1e-6);
        free(text);EditorSector_Free(&roundtrip);EditorSector_Free(&linked);++tested;
        }
    }
    assert(tested==6);EditorSector_Free(&legacy);EditorSector_Free(&saved);EditorSector_Free(&map);
}
/* Two deliberately separate, adjacent, coplanar quads are not the two
 * triangles generated by splitting one face.  Linking either must leave the
 * other solid even when both materials and the opposing wall are identical. */
static void adjacent_quads_case(void)
{
    static const double yz[6][2]={{-128,0},{0,0},{128,0},{128,128},{0,128},{-128,128}};
    editor_sector_map_t map={0},legacy={0};char error[512]={0};
    assert(EditorSector_Init(&map));
    map.vertices=calloc(12,sizeof(*map.vertices));assert(map.vertices);map.num_vertices=12;
    for(size_t i=0;i<6;++i)for(size_t side=0;side<2;++side){
        double *p=map.vertices[i+6*side].position;p[0]=side?0:-128;p[1]=yz[i][0];p[2]=yz[i][1];
    }
    map.sectors=calloc(1,sizeof(*map.sectors));assert(map.sectors);map.num_sectors=1;
    editor_sector_t *s=&map.sectors[0];s->faces=calloc(9,sizeof(*s->faces));assert(s->faces);s->num_faces=9;
    static const size_t first[4]={6,7,10,11},second[4]={7,8,9,10};
    for(size_t i=0;i<9;++i){
        editor_sector_face_t *f=&s->faces[i];f->num_vertices=i==0?6:4;
        f->vertices=calloc(f->num_vertices,sizeof(*f->vertices));assert(f->vertices);
        f->portal_sector=f->portal_face=EDITOR_SECTOR_NO_PORTAL;strcpy(f->material,"stone1");
        if(i==0)for(size_t j=0;j<6;++j)f->vertices[j]=5-j;
        else if(i==1||i==2)memcpy(f->vertices,i==1?first:second,sizeof first);
        else {size_t j=i-3,k=(j+1)%6;f->vertices[0]=j;f->vertices[1]=k;f->vertices[2]=k+6;f->vertices[3]=j+6;}
    }
    const double lo[3]={0,-128,0},hi[3]={128,128,128};
    assert(EditorSector_AddBox(&map,lo,hi,"stone1",NULL,error,sizeof error));
    assert(EditorSector_Validate(&map,error,sizeof error));
    old_format_clone(&legacy,&map);
    for(int roundtrip=0;roundtrip<2;++roundtrip)for(size_t face=1;face<=2;++face){
        editor_sector_map_t linked={0};size_t na,nb;
        assert(EditorSector_Clone(&linked,roundtrip?&legacy:&map));
        if(!EditorSector_LinkFaceRegions(&linked,0,face,1,0,error,sizeof error)){
            fprintf(stderr,"adjacent quad link failed: %s\n",error);abort();
        }
        double area=portal_area(&linked,0,&na),other=portal_area(&linked,1,&nb);
        assert(na==nb&&na>0&&fabs(area-128*128)<1e-6&&fabs(other-area)<1e-6);
        assert(EditorSector_Validate(&linked,error,sizeof error));EditorSector_Free(&linked);
    }
    /* Four triangles in a strip admit more than one convex quad pairing.
     * A legacy file lacks provenance, so none may be inferred by guessing. */
    editor_sector_map_t strip={0},old_strip={0};assert(EditorSector_Clone(&strip,&map));
    editor_sector_t *strip_sector=&strip.sectors[0];
    strip_sector->faces=realloc(strip_sector->faces,11*sizeof(*strip_sector->faces));assert(strip_sector->faces);
    memset(&strip_sector->faces[9],0,2*sizeof(*strip_sector->faces));strip_sector->num_faces=11;
    const size_t triangles[4][3]={{6,7,10},{6,10,11},{7,8,9},{7,9,10}};
    const size_t face_ids[4]={1,9,2,10};
    for(size_t i=0;i<4;++i){
        editor_sector_face_t *f=&strip_sector->faces[face_ids[i]];free(f->vertices);
        f->vertices=malloc(sizeof triangles[i]);assert(f->vertices);memcpy(f->vertices,triangles[i],sizeof triangles[i]);
        f->num_vertices=3;f->portal_sector=f->portal_face=EDITOR_SECTOR_NO_PORTAL;f->source_group=0;strcpy(f->material,"stone1");
    }
    assert(EditorSector_Validate(&strip,error,sizeof error));old_format_clone(&old_strip,&strip);
    for(size_t i=0;i<4;++i)assert(old_strip.sectors[0].faces[face_ids[i]].source_group==0);
    size_t na,nb;assert(EditorSector_LinkFaceRegions(&old_strip,0,1,1,0,error,sizeof error));
    assert(fabs(portal_area(&old_strip,0,&na)-8192)<1e-6&&na>0);
    assert(fabs(portal_area(&old_strip,1,&nb)-8192)<1e-6&&nb==na);
    EditorSector_Free(&old_strip);EditorSector_Free(&strip);
    EditorSector_Free(&legacy);EditorSector_Free(&map);
}
static void cleanup_case(void)
{
    editor_sector_map_t map={0},roundtrip={0};char error[512]={0};size_t na,nb;
    const double a[3]={-128,-128,0},b[3]={0,128,128},c[3]={0,-128,0},d[3]={128,128,128};
    const double e[3]={-128,128,128},f[3]={0,256,256};
    setup(&map,a,b,c,d);
    assert(EditorSector_AddBox(&map,e,f,"stone1",NULL,error,sizeof error));
    /* A third, unlinked room touches one corner. It must not get welded. */
    size_t unrelated=SIZE_MAX;
    for(size_t i=0;i<map.num_vertices;++i){const double *p=map.vertices[i].position;
        if(p[0]==0&&p[1]==128&&p[2]==128)unrelated=i;}
    assert(unrelated!=SIZE_MAX);
    assert(EditorSector_LinkFaceRegions(&map,0,1,1,0,error,sizeof error));
    assert(portal_area(&map,0,&na)==256*128 && portal_area(&map,1,&nb)==256*128);
    assert(na==1&&nb==1);
    size_t portal=0;while(map.sectors[0].faces[portal].portal_sector==EDITOR_SECTOR_NO_PORTAL)++portal;
    const editor_sector_face_t *left=&map.sectors[0].faces[portal],*right=&map.sectors[1].faces[left->portal_face];
    assert(left->num_vertices==4&&right->num_vertices==4);
    for(size_t i=0;i<4;++i){bool found=false;for(size_t j=0;j<4;++j)if(left->vertices[i]==right->vertices[j])found=true;assert(found);}
    for(size_t i=0;i<4;++i)assert(left->vertices[i]!=unrelated);
    bool unrelated_used=false;for(size_t face=0;face<map.sectors[2].num_faces;++face)
        for(size_t i=0;i<map.sectors[2].faces[face].num_vertices;++i)
            if(map.sectors[2].faces[face].vertices[i]==unrelated)unrelated_used=true;
    assert(unrelated_used);
    char *encoded=NULL;size_t len=0;assert(EditorSector_Serialize(&map,&encoded,&len));
    assert(EditorSector_Parse(&roundtrip,encoded,len,error,sizeof error));free(encoded);EditorSector_Free(&roundtrip);EditorSector_Free(&map);

    /* Existing exact links may predate automatic welding. Cleanup shares the
     * four portal vertex IDs, and a second pass has no changes. */
    setup(&map,a,b,c,d);
    size_t shared[4];memcpy(shared,map.sectors[1].faces[0].vertices,sizeof(shared));
    map.vertices=realloc(map.vertices,(map.num_vertices+4)*sizeof(*map.vertices));assert(map.vertices);
    for(size_t i=0;i<4;++i){size_t replacement=map.num_vertices++;
        map.vertices[replacement]=map.vertices[shared[i]];
        for(size_t face=0;face<map.sectors[1].num_faces;++face)
            for(size_t v=0;v<map.sectors[1].faces[face].num_vertices;++v)
                if(map.sectors[1].faces[face].vertices[v]==shared[i])map.sectors[1].faces[face].vertices[v]=replacement;
    }
    assert(EditorSector_LinkPortal(&map,0,1,1,0,error,sizeof error));
    size_t changes=0;assert(EditorSector_CleanupLinks(&map,&changes,error,sizeof error)&&changes==4);
    left=&map.sectors[0].faces[1];right=&map.sectors[1].faces[0];
    for(size_t i=0;i<4;++i){bool found=false;for(size_t j=0;j<4;++j)if(left->vertices[i]==right->vertices[j])found=true;assert(found);}
    assert(EditorSector_CleanupLinks(&map,&changes,error,sizeof error)&&changes==0);
    assert(EditorSector_Validate(&map,error,sizeof error));EditorSector_Free(&map);

    /* A deliberately authored straight edge vertex predating a link must not
     * be dissolved by the automatic cleanup, even though it is collinear. */
    setup(&map,a,b,c,d);size_t v0=map.sectors[0].faces[1].vertices[0],v1=map.sectors[0].faces[1].vertices[1];
    size_t middle=map.num_vertices++;map.vertices=realloc(map.vertices,map.num_vertices*sizeof(*map.vertices));assert(map.vertices);
    for(int axis=0;axis<3;++axis)map.vertices[middle].position[axis]=(map.vertices[v0].position[axis]+map.vertices[v1].position[axis])*0.5;
    for(size_t face=0;face<map.sectors[0].num_faces;++face){editor_sector_face_t *p=&map.sectors[0].faces[face];
        for(size_t i=0;i<p->num_vertices;++i){size_t x=p->vertices[i],y=p->vertices[(i+1)%p->num_vertices];
            if(!((x==v0&&y==v1)||(x==v1&&y==v0)))continue;
            p->vertices=realloc(p->vertices,(p->num_vertices+1)*sizeof(*p->vertices));assert(p->vertices);
            memmove(&p->vertices[i+2],&p->vertices[i+1],(p->num_vertices-i-1)*sizeof(*p->vertices));
            p->vertices[i+1]=middle;++p->num_vertices;break;
        }
    }
    assert(EditorSector_Validate(&map,error,sizeof error));
    assert(EditorSector_LinkFaceRegions(&map,0,1,1,0,error,sizeof error));
    bool authored=false;for(size_t face=0;face<map.sectors[0].num_faces;++face)
        if(map.sectors[0].faces[face].portal_sector==1)for(size_t i=0;i<map.sectors[0].faces[face].num_vertices;++i)
            if(map.sectors[0].faces[face].vertices[i]==middle)authored=true;
    assert(authored);assert(EditorSector_Validate(&map,error,sizeof error));EditorSector_Free(&map);
}
static void write_fixture(const char *path,const char *wad,int closed,int stacked,int triangulated)
{
    editor_sector_map_t map={0};editor_map_t compiled={0};char error[512]={0};char *text=NULL;size_t length=0,entity;
    const double a[3]={-256,-256,0},b[3]={0,256,192},c[3]={0,-48,0},d[3]={256,48,128};
    const double e[3]={-128,-128,-128},f[3]={128,128,0},g[3]={-32,-32,0},h[3]={32,32,128};
    if(stacked) setup(&map,e,f,g,h); else setup(&map,a,b,c,d);
    if(triangulated){
        size_t face=split_wall(&map);
        assert(EditorSector_LinkFaceRegions(&map,0,face,1,0,error,sizeof error));
        size_t count;assert(fabs(portal_area(&map,0,&count)-96*128)<1e-6);
    } else assert(EditorSector_LinkPartialPortal(&map,0,stacked?5:1,1,stacked?4:0,error,sizeof error));
    if(closed) {
        size_t portal=0;
        while(portal<map.sectors[0].num_faces&&map.sectors[0].faces[portal].portal_sector==EDITOR_SECTOR_NO_PORTAL)++portal;
        assert(portal<map.sectors[0].num_faces&&EditorSector_UnlinkPortalGroup(&map,0,portal,error,sizeof error));
    }
    assert(EditorMap_SetProperty(&map.entities.entities[0],"wad",wad));
    assert(EditorMap_AddEntity(&map.entities,"info_player_start",&entity));
    assert(EditorMap_SetProperty(&map.entities.entities[entity],"origin",stacked?"0 0 -80":"-128 0 32"));
    assert(EditorMap_AddEntity(&map.entities,"light",&entity));
    assert(EditorMap_SetProperty(&map.entities.entities[entity],"origin",stacked?"0 0 -40":"-128 0 100"));
    assert(EditorSector_Compile(&map,&compiled,error,sizeof error));
    assert(EditorMap_Serialize(&compiled,&text,&length));
    FILE *file=fopen(path,"wb");assert(file);
    assert(fwrite(text,1,length,file)==length&&fclose(file)==0);
    free(text);EditorMap_Free(&compiled);EditorSector_Free(&map);
}
int main(int argc,char **argv)
{
    if(argc==4&&!strncmp(argv[1],"--write-",8)){
        int open=!strcmp(argv[1],"--write-fixture"),closed=!strcmp(argv[1],"--write-closed"),stacked=!strcmp(argv[1],"--write-stacked"),
            triangulated=!strcmp(argv[1],"--write-triangulated");
        assert(open||closed||stacked||triangulated);write_fixture(argv[2],argv[3],closed,stacked,triangulated);return 0;
    }
    assert(argc==1);
    const double a[3]={-128,-128,0},b[3]={0,128,128},c[3]={0,-32,0},d[3]={128,32,96};
    link_case(a,b,c,d,1,0,64*96,0);              /* Smaller vertical doorway. */
    link_case(a,b,c,d,1,0,0,1);                  /* The same portal on an oblique plane. */
    const double e[3]={-128,-128,-128},f[3]={128,128,0},g[3]={-32,-32,0},h[3]={32,32,128};
    link_case(e,f,g,h,5,4,64*64,0);              /* Stacked floors with an opening. */
    const double cfar[3]={1,-32,0},dfar[3]={128,32,96};
    reject_case(a,b,cfar,dfar,1,0);              /* Parallel but separated planes. */
    const double coff[3]={0,256,0},doff[3]={128,320,96};
    reject_case(a,b,coff,doff,1,0);              /* Coplanar but disjoint. */
    reject_case(a,b,c,d,0,0);                    /* Equally oriented faces. */
    existing_portal_case();                      /* Propagate new boundary vertices through existing portal. */
    concave_case();                              /* Portal overlap itself is non-convex. */
    disconnected_openings_case();                /* Unlink leaves a separate opening intact. */
    triangulated_wall_case();                    /* Selecting either triangle links the whole quad. */
    adjacent_quads_case();                       /* Independent quads must remain independent. */
    cleanup_case();                              /* Weld and simplify only the connected opening. */
    puts("partial sector portal tests passed");return 0;
}
