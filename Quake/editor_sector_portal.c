/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "editor_sector_portal.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Bounded to the source model's face/loop limits. Limit intermediate pieces so
 * malformed or extremely complex input cannot exhaust unbounded memory. */
#define PORTAL_FACE_LIMIT 4096u
#define PORTAL_VERTEX_LIMIT 256u
#define PORTAL_PIECE_LIMIT 4096u
#define PORTAL_EPS 1e-7

typedef struct { double x, y; } point2_t;
typedef struct { point2_t *v; size_t n; } polygon_t;
typedef struct { polygon_t *v; size_t n, capacity; } polygons_t;
typedef struct { double origin[3], u[3], v[3], normal[3]; } plane_basis_t;
typedef struct { double t; size_t vertex; } edge_point_t;

static bool portal_error(char *error, size_t size, const char *fmt, ...)
{
    if (error && size) { va_list ap; va_start(ap, fmt); vsnprintf(error, size, fmt, ap); va_end(ap); }
    return false;
}
static double dot3p(const double *a, const double *b) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
static void cross3p(const double *a, const double *b, double *out)
{ out[0]=a[1]*b[2]-a[2]*b[1]; out[1]=a[2]*b[0]-a[0]*b[2]; out[2]=a[0]*b[1]-a[1]*b[0]; }
static double orient(point2_t a, point2_t b, point2_t c)
{ return (b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x); }
static double area2(const point2_t *v, size_t n)
{ double a=0; for(size_t i=0;i<n;++i) a+=v[i].x*v[(i+1)%n].y-v[(i+1)%n].x*v[i].y; return a; }
static bool near2(point2_t a,point2_t b)
{ double x=a.x-b.x,y=a.y-b.y; return x*x+y*y<=PORTAL_EPS*PORTAL_EPS; }
static void clear_polygons(polygons_t *list)
{ for(size_t i=0;i<list->n;++i)free(list->v[i].v); free(list->v); memset(list,0,sizeof(*list)); }
static bool append_polygon(polygons_t *list,const point2_t *points,size_t n)
{
    point2_t clean[PORTAL_VERTEX_LIMIT]; size_t count=0;
    for(size_t i=0;i<n;++i) if(!count||!near2(points[i],clean[count-1])) {
        if(count==PORTAL_VERTEX_LIMIT)return false;
        clean[count++]=points[i];
    }
    if(count>1&&near2(clean[0],clean[count-1]))--count;
    if(count<3||fabs(area2(clean,count))<=PORTAL_EPS*PORTAL_EPS)return true;
    if(list->n>=PORTAL_PIECE_LIMIT)return false;
    if(list->n==list->capacity) { size_t next=list->capacity?list->capacity*2:8;
        if(next>PORTAL_PIECE_LIMIT)next=PORTAL_PIECE_LIMIT;
        polygon_t *p=realloc(list->v,next*sizeof(*p)); if(!p)return false; list->v=p; list->capacity=next; }
    point2_t *copy=malloc(count*sizeof(*copy)); if(!copy)return false;
    memcpy(copy,clean,count*sizeof(*copy)); list->v[list->n++]=(polygon_t){copy,count}; return true;
}
static bool make_basis(const editor_sector_map_t *map,const editor_sector_face_t *face,plane_basis_t *basis)
{
    if(!face->vertices||face->num_vertices<3)return false;
    double normal[3]={0,0,0};
    for(size_t i=0;i<face->num_vertices;++i) {
        const double *a=map->vertices[face->vertices[i]].position,*b=map->vertices[face->vertices[(i+1)%face->num_vertices]].position;
        normal[0]+=(a[1]-b[1])*(a[2]+b[2]); normal[1]+=(a[2]-b[2])*(a[0]+b[0]); normal[2]+=(a[0]-b[0])*(a[1]+b[1]);
    }
    double length=sqrt(dot3p(normal,normal)); if(!isfinite(length)||length<PORTAL_EPS)return false;
    for(int i=0;i<3;++i)basis->normal[i]=normal[i]/length;
    memcpy(basis->origin,map->vertices[face->vertices[0]].position,sizeof(basis->origin));
    /* Pick the least parallel world axis to avoid a singular basis. */
    int axis=0; if(fabs(basis->normal[1])<fabs(basis->normal[axis]))axis=1;
    if(fabs(basis->normal[2])<fabs(basis->normal[axis]))axis=2;
    double a[3]={0,0,0};a[axis]=1; cross3p(a,basis->normal,basis->u);
    length=sqrt(dot3p(basis->u,basis->u)); if(length<PORTAL_EPS)return false;
    for(int i=0;i<3;++i)basis->u[i]/=length;
    cross3p(basis->normal,basis->u,basis->v); return true;
}
static point2_t project(const plane_basis_t *basis,const double *p)
{ double d[3];for(int i=0;i<3;++i)d[i]=p[i]-basis->origin[i];return (point2_t){dot3p(d,basis->u),dot3p(d,basis->v)}; }
static void unproject(const plane_basis_t *basis,point2_t p,double out[3])
{ for(int i=0;i<3;++i)out[i]=basis->origin[i]+basis->u[i]*p.x+basis->v[i]*p.y; }

/* Split a convex polygon against the left side of a directed clip edge. The
 * boundary is shared; each returned polygon retains the input winding. */
static bool split_line(const polygon_t *poly,point2_t a,point2_t b,double sign,polygons_t *inside,polygons_t *outside)
{
    point2_t in[PORTAL_VERTEX_LIMIT],out[PORTAL_VERTEX_LIMIT]; size_t ni=0,no=0;
    for(size_t i=0;i<poly->n;++i) {
        point2_t p=poly->v[i],q=poly->v[(i+1)%poly->n];
        double dp=sign*orient(a,b,p),dq=sign*orient(a,b,q);
        /* A distance tolerance scaled by edge length retains coordinate units. */
        double tolerance=PORTAL_EPS*fmax(1.0,hypot(b.x-a.x,b.y-a.y));
        if(fabs(dp)<=tolerance)dp=0;
        if(fabs(dq)<=tolerance)dq=0;
        if(dp>=0) { if(ni==PORTAL_VERTEX_LIMIT)return false; in[ni++]=p; }
        if(dp<=0) { if(no==PORTAL_VERTEX_LIMIT)return false; out[no++]=p; }
        if((dp>0&&dq<0)||(dp<0&&dq>0)) {
            double t=dp/(dp-dq); point2_t x={p.x+t*(q.x-p.x),p.y+t*(q.y-p.y)};
            if(ni==PORTAL_VERTEX_LIMIT||no==PORTAL_VERTEX_LIMIT)return false;
            in[ni++]=x;out[no++]=x;
        }
    }
    return append_polygon(inside,in,ni)&&append_polygon(outside,out,no);
}
/* Convex subject minus a triangle: each emitted outside piece is disjoint,
 * and the final inside piece is the intersection. */
static bool subtract_triangle(const polygon_t *subject,const polygon_t *clip,polygons_t *out,polygons_t *intersection)
{
    polygons_t active={0}; bool ok=append_polygon(&active,subject->v,subject->n);
    const double sign=area2(clip->v,clip->n)>0?1.0:-1.0;
    for(size_t e=0;ok&&e<clip->n&&active.n;++e) {
        polygons_t next={0};
        for(size_t i=0;ok&&i<active.n;++i)
            ok=split_line(&active.v[i],clip->v[e],clip->v[(e+1)%clip->n],sign,&next,out);
        clear_polygons(&active);active=next;
    }
    if(ok&&intersection)for(size_t i=0;ok&&i<active.n;++i)ok=append_polygon(intersection,active.v[i].v,active.v[i].n);
    clear_polygons(&active);return ok;
}
static bool triangles_for_face(const editor_sector_map_t *map,size_t s,size_t f,const plane_basis_t *basis,polygons_t *out)
{
    const editor_sector_face_t *face=&map->sectors[s].faces[f];
    size_t (*indices)[3]=malloc((face->num_vertices-2)*sizeof(*indices)); if(!indices)return false;
    size_t n=EditorSector_TriangulateFace(map,s,f,indices,face->num_vertices-2); bool ok=n>0;
    for(size_t i=0;ok&&i<n;++i) { point2_t points[3];for(int j=0;j<3;++j)points[j]=project(basis,map->vertices[indices[i][j]].position);ok=append_polygon(out,points,3); }
    free(indices);return ok;
}
static bool partition(const polygons_t *source,const polygons_t *cutters,polygons_t *remainder,polygons_t *intersection)
{
    for(size_t i=0;i<source->n;++i) {
        polygons_t active={0}; if(!append_polygon(&active,source->v[i].v,source->v[i].n))return false;
        for(size_t j=0;j<cutters->n&&active.n;++j) {
            polygons_t next={0}; bool ok=true;
            for(size_t k=0;ok&&k<active.n;++k)ok=subtract_triangle(&active.v[k],&cutters->v[j],&next,intersection);
            clear_polygons(&active);active=next;
            if(!ok) {clear_polygons(&active);return false;}
        }
        for(size_t j=0;j<active.n;++j)if(!append_polygon(remainder,active.v[j].v,active.v[j].n)){clear_polygons(&active);return false;}
        clear_polygons(&active);
    }
    return true;
}

static bool same_pos(const double *a,const double *b)
{ return a[0]==b[0]&&a[1]==b[1]&&a[2]==b[2]; }
static bool close_pos(const double *a,const double *b)
{ double d[3];for(int i=0;i<3;++i)d[i]=a[i]-b[i];return dot3p(d,d)<=PORTAL_EPS*PORTAL_EPS; }
static bool vertex_for_point(editor_sector_map_t *map,const plane_basis_t *basis,point2_t point,
    const bool *eligible,size_t original_vertices,size_t *index)
{
    double p[3];unproject(basis,point,p);
    for(size_t i=0;i<map->num_vertices;++i)if((i>=original_vertices||eligible[i])&&close_pos(map->vertices[i].position,p)){*index=i;return true;}
    if(map->num_vertices>=1048576u)return false;
    editor_sector_vertex_t *vertices=realloc(map->vertices,(map->num_vertices+1)*sizeof(*vertices));
    if(!vertices)return false;
    map->vertices=vertices;*index=map->num_vertices++;
    memcpy(map->vertices[*index].position,p,sizeof(p));return true;
}
static bool make_face(editor_sector_map_t *map,editor_sector_face_t *face,const polygon_t *polygon,
    const plane_basis_t *basis,const char *material,bool reverse,const bool *eligible,size_t original_vertices)
{
    size_t *ids=malloc(polygon->n*sizeof(*ids));if(!ids)return false;
    for(size_t i=0;i<polygon->n;++i) {
        size_t source=reverse?polygon->n-1-i:i;
        if(!vertex_for_point(map,basis,polygon->v[source],eligible,original_vertices,&ids[i])){free(ids);return false;}
    }
    memset(face,0,sizeof(*face)); face->vertices=ids;face->num_vertices=polygon->n;
    memcpy(face->material,material,EDITOR_SECTOR_MATERIAL_MAX);
    face->portal_sector=face->portal_face=EDITOR_SECTOR_NO_PORTAL;return true;
}
static bool replace_face(editor_sector_map_t *map,size_t sector,size_t face,const polygons_t *remainder,
    const polygons_t *overlap,const plane_basis_t *basis,bool reverse,size_t *portal_indices,
    const bool *eligible,size_t original_vertices)
{
    editor_sector_t *sec=&map->sectors[sector];const size_t total=remainder->n+overlap->n, old=sec->num_faces;
    if(!total||total>PORTAL_FACE_LIMIT||old>PORTAL_FACE_LIMIT-total+1)return false;
    char material[EDITOR_SECTOR_MATERIAL_MAX];memcpy(material,sec->faces[face].material,sizeof(material));
    size_t group=sec->faces[face].source_group;
    if(total>1&&!group){
        for(size_t f=0;f<old;++f)if(sec->faces[f].source_group>group)group=sec->faces[f].source_group;
        if(group==SIZE_MAX)return false;
        ++group;
    }
    editor_sector_face_t *faces=realloc(sec->faces,(old+total-1)*sizeof(*faces));if(!faces)return false;
    sec->faces=faces;memset(&faces[old],0,(total-1)*sizeof(*faces));sec->num_faces=old+total-1;
    free(faces[face].vertices);memset(&faces[face],0,sizeof(faces[face]));
    for(size_t i=0;i<total;++i) {
        size_t slot=i?old+i-1:face; bool portal=i>=remainder->n;
        const polygon_t *polygon=portal?&overlap->v[i-remainder->n]:&remainder->v[i];
        if(!make_face(map,&sec->faces[slot],polygon,basis,material,portal&&reverse,eligible,original_vertices))return false;
        sec->faces[slot].source_group=group;
        if(portal)portal_indices[i-remainder->n]=slot;
    }
    return true;
}
static int sort_edge_point(const void *a,const void *b)
{ const edge_point_t *x=a,*y=b;return x->t<y->t?-1:x->t>y->t?1:0; }
/* Insert all coincident cut vertices into every incident shell edge, including
 * edges of faces untouched by the cut. This avoids T-junctions and preserves
 * the model's exact reverse-directed-edge invariant. */
static bool conform_sector_edges(editor_sector_map_t *map,size_t sector,const size_t *candidates,size_t ncandidates)
{
    editor_sector_t *sec=&map->sectors[sector];
    for(size_t f=0;f<sec->num_faces;++f) {
        editor_sector_face_t *face=&sec->faces[f];
        for(size_t i=0;i<face->num_vertices;++i)
            for(size_t c=0;c<ncandidates;++c)
                if(same_pos(map->vertices[face->vertices[i]].position,map->vertices[candidates[c]].position)) {
                    face->vertices[i]=candidates[c];break;
                }
    }
    edge_point_t *points=malloc(ncandidates*sizeof(*points));if(ncandidates&&!points)return false;
    for(size_t f=0;f<sec->num_faces;++f) {
        editor_sector_face_t *face=&sec->faces[f];size_t ids[PORTAL_VERTEX_LIMIT],count=0;
        for(size_t i=0;i<face->num_vertices;++i) {
            size_t a=face->vertices[i],b=face->vertices[(i+1)%face->num_vertices];
            if(count==PORTAL_VERTEX_LIMIT){free(points);return false;} ids[count++]=a;
            const double *pa=map->vertices[a].position,*pb=map->vertices[b].position;
            double edge[3];for(int k=0;k<3;++k)edge[k]=pb[k]-pa[k];double length2=dot3p(edge,edge);
            if(length2<=PORTAL_EPS*PORTAL_EPS){free(points);return false;}
            size_t n=0;
            for(size_t c=0;c<ncandidates;++c) {
                size_t id=candidates[c];if(id==a||id==b)continue;
                const double *p=map->vertices[id].position;double d[3],perp[3];
                for(int k=0;k<3;++k)d[k]=p[k]-pa[k];
                double t=dot3p(d,edge)/length2;
                if(t<=PORTAL_EPS||t>=1.0-PORTAL_EPS)continue;
                for(int k=0;k<3;++k)perp[k]=d[k]-t*edge[k];
                if(dot3p(perp,perp)>PORTAL_EPS*PORTAL_EPS)continue;
                bool duplicate=false;for(size_t j=0;j<n;++j)if(fabs(points[j].t-t)<PORTAL_EPS){duplicate=true;break;}
                if(!duplicate)points[n++]=(edge_point_t){t,id};
            }
            qsort(points,n,sizeof(*points),sort_edge_point);
            for(size_t j=0;j<n;++j){if(count==PORTAL_VERTEX_LIMIT){free(points);return false;}ids[count++]=points[j].vertex;}
        }
        size_t *ids_copy=malloc(count*sizeof(*ids_copy));if(!ids_copy){free(points);return false;}
        memcpy(ids_copy,ids,count*sizeof(*ids_copy));free(face->vertices);face->vertices=ids_copy;face->num_vertices=count;
    }
    free(points);return true;
}
static bool add_candidates(const editor_sector_map_t *map,size_t sector,size_t old_count,size_t replaced,
    size_t **candidates,size_t *count,size_t *capacity)
{
    const editor_sector_t *sec=&map->sectors[sector];
    for(size_t f=0;f<sec->num_faces;++f) {
        if(f!=replaced&&f<old_count)continue;
        const editor_sector_face_t *face=&sec->faces[f];
        for(size_t i=0;i<face->num_vertices;++i) {
            size_t id=face->vertices[i],j=0;while(j<*count&&(*candidates)[j]!=id)++j;if(j<*count)continue;
            if(*count==*capacity){size_t next=*capacity?*capacity*2:32;size_t *p=realloc(*candidates,next*sizeof(*p));if(!p)return false;*candidates=p;*capacity=next;}
            (*candidates)[(*count)++]=id;
        }
    }
    return true;
}

bool EditorSector_LinkPartialPortal(editor_sector_map_t *map,size_t sector,size_t face,size_t other_sector,size_t other_face,char *error,size_t error_size)
{
    if(error&&error_size)*error=0;
    if(!map||!EditorSector_Validate(map,error,error_size))return false;
    if(sector>=map->num_sectors||other_sector>=map->num_sectors||sector==other_sector||
       face>=map->sectors[sector].num_faces||other_face>=map->sectors[other_sector].num_faces)
        return portal_error(error,error_size,"Select faces on two different sectors");
    const editor_sector_face_t *a=&map->sectors[sector].faces[face],*b=&map->sectors[other_sector].faces[other_face];
    if(a->portal_sector!=EDITOR_SECTOR_NO_PORTAL||b->portal_sector!=EDITOR_SECTOR_NO_PORTAL)
        return portal_error(error,error_size,"Unlink existing portals before cutting these faces");
    plane_basis_t basis,other_basis;
    if(!make_basis(map,a,&basis)||!make_basis(map,b,&other_basis)||dot3p(basis.normal,other_basis.normal)>-0.999999)
        return portal_error(error,error_size,"Portal faces must face opposite directions");
    for(size_t i=0;i<b->num_vertices;++i) {
        const double *p=map->vertices[b->vertices[i]].position;double d[3];
        for(int k=0;k<3;++k)d[k]=p[k]-basis.origin[k];
        if(fabs(dot3p(d,basis.normal))>1e-6*(1.0+sqrt(dot3p(d,d))))
            return portal_error(error,error_size,"Portal faces must share one plane");
    }
    polygons_t triangles_a={0},triangles_b={0},rem_a={0},rem_b={0},overlap={0};
    editor_sector_map_t copy={0};size_t *indices_a=NULL,*indices_b=NULL,*candidates=NULL,ncandidates=0,cap=0;
    bool *connected=NULL,*eligible=NULL;
    bool ok=triangles_for_face(map,sector,face,&basis,&triangles_a)&&triangles_for_face(map,other_sector,other_face,&basis,&triangles_b)&&
        partition(&triangles_a,&triangles_b,&rem_a,&overlap)&&partition(&triangles_b,&triangles_a,&rem_b,NULL)&&overlap.n>0;
    if(!ok){portal_error(error,error_size,"Faces have no usable overlap or subdivision is too complex");goto done;}
    indices_a=malloc(overlap.n*sizeof(*indices_a));indices_b=malloc(overlap.n*sizeof(*indices_b));
    connected=calloc(map->num_sectors,sizeof(*connected));eligible=calloc(map->num_vertices,sizeof(*eligible));
    if(!indices_a||!indices_b||!connected||!eligible||!EditorSector_Clone(&copy,map)){portal_error(error,error_size,"Out of memory creating portal");ok=false;goto done;}
    /* Propagate subdivisions through already connected portals, but do not
     * weld a separate room merely because a vertex occupies the same point. */
    connected[sector]=connected[other_sector]=true;
    bool changed=true;while(changed){changed=false;
        for(size_t s=0;s<map->num_sectors;++s)if(connected[s])for(size_t f=0;f<map->sectors[s].num_faces;++f){
            size_t peer=map->sectors[s].faces[f].portal_sector;
            if(peer!=EDITOR_SECTOR_NO_PORTAL&&!connected[peer]){connected[peer]=true;changed=true;}
        }
    }
    for(size_t s=0;s<map->num_sectors;++s)if(connected[s])for(size_t f=0;f<map->sectors[s].num_faces;++f)
        for(size_t i=0;i<map->sectors[s].faces[f].num_vertices;++i)eligible[map->sectors[s].faces[f].vertices[i]]=true;
    for(size_t s=0;s<map->num_sectors;++s)if(!connected[s])for(size_t f=0;f<map->sectors[s].num_faces;++f)
        for(size_t i=0;i<map->sectors[s].faces[f].num_vertices;++i)eligible[map->sectors[s].faces[f].vertices[i]]=false;
    const size_t old_a=copy.sectors[sector].num_faces,old_b=copy.sectors[other_sector].num_faces;
    ok=replace_face(&copy,sector,face,&rem_a,&overlap,&basis,false,indices_a,eligible,map->num_vertices)&&
       replace_face(&copy,other_sector,other_face,&rem_b,&overlap,&basis,true,indices_b,eligible,map->num_vertices)&&
       add_candidates(&copy,sector,old_a,face,&candidates,&ncandidates,&cap)&&
       add_candidates(&copy,other_sector,old_b,other_face,&candidates,&ncandidates,&cap);
    /* Cuts can land on an edge shared with a previously linked third sector.
     * Propagate those vertices across every incident shell, so paired portal
     * loops and their neighboring faces keep exactly matching edge topology. */
    for(size_t s=0;ok&&s<copy.num_sectors;++s)if(connected[s])
        ok=conform_sector_edges(&copy,s,candidates,ncandidates);
    if(!ok){portal_error(error,error_size,"Portal subdivision exceeded limits or ran out of memory");goto done;}
    for(size_t i=0;i<overlap.n;++i) {
        editor_sector_face_t *fa=&copy.sectors[sector].faces[indices_a[i]],*fb=&copy.sectors[other_sector].faces[indices_b[i]];
        fa->portal_sector=other_sector;fa->portal_face=indices_b[i];fb->portal_sector=sector;fb->portal_face=indices_a[i];
    }
    if(!EditorSector_Validate(&copy,error,error_size)){ok=false;goto done;}
    EditorSector_Free(map);*map=copy;memset(&copy,0,sizeof(copy));
done:
    EditorSector_Free(&copy);free(indices_a);free(indices_b);free(candidates);free(connected);free(eligible);
    clear_polygons(&triangles_a);clear_polygons(&triangles_b);clear_polygons(&rem_a);clear_polygons(&rem_b);clear_polygons(&overlap);
    return ok;
}

static bool share_edge(const editor_sector_map_t *map,const editor_sector_face_t *a,const editor_sector_face_t *b)
{
    for(size_t i=0;i<a->num_vertices;++i) {
        const double *a0=map->vertices[a->vertices[i]].position,*a1=map->vertices[a->vertices[(i+1)%a->num_vertices]].position;
        for(size_t j=0;j<b->num_vertices;++j) {
            const double *b0=map->vertices[b->vertices[j]].position,*b1=map->vertices[b->vertices[(j+1)%b->num_vertices]].position;
            if((same_pos(a0,b0)&&same_pos(a1,b1))||(same_pos(a0,b1)&&same_pos(a1,b0)))return true;
        }
    }
    return false;
}
static bool same_plane(const editor_sector_map_t *map,const editor_sector_face_t *a,const editor_sector_face_t *b)
{
    plane_basis_t p,q;
    if(!make_basis(map,a,&p)||!make_basis(map,b,&q)||dot3p(p.normal,q.normal)<0.999999)return false;
    for(size_t i=0;i<b->num_vertices;++i) {
        const double *v=map->vertices[b->vertices[i]].position;double d[3];
        for(int k=0;k<3;++k)d[k]=v[k]-p.origin[k];
        if(fabs(dot3p(d,p.normal))>1e-6*(1.0+sqrt(dot3p(d,d))))return false;
    }
    return true;
}

/* Only join fragments known to originate from the selected authored face. */
static void mark_face_region(const editor_sector_map_t *map,size_t sector,size_t face,bool *marked)
{
    const editor_sector_t *sec=&map->sectors[sector];
    const editor_sector_face_t *anchor=&sec->faces[face];
    marked[face]=true;bool changed=true;
    while(changed) {
        changed=false;
        for(size_t a=0;a<sec->num_faces;++a)if(marked[a])
            for(size_t b=0;b<sec->num_faces;++b)if(!marked[b]) {
                const editor_sector_face_t *candidate=&sec->faces[b];
                if(anchor->source_group && candidate->source_group==anchor->source_group &&
                   candidate->portal_sector==EDITOR_SECTOR_NO_PORTAL && !strcmp(anchor->material,candidate->material) &&
                   same_plane(map,anchor,candidate) && share_edge(map,&sec->faces[a],candidate)) {
                    marked[b]=true;changed=true;
                }
            }
    }
}

/* Returns -1 on a computation/allocation failure, otherwise whether a pair
 * has an actual positive-area intersection. */
static int face_overlap(const editor_sector_map_t *map,size_t s,size_t f,size_t t,size_t g)
{
    const editor_sector_face_t *a=&map->sectors[s].faces[f],*b=&map->sectors[t].faces[g];
    plane_basis_t basis,other;
    if(!make_basis(map,a,&basis)||!make_basis(map,b,&other))return -1;
    if(dot3p(basis.normal,other.normal)>-0.999999)return 0;
    for(size_t i=0;i<b->num_vertices;++i){
        const double *p=map->vertices[b->vertices[i]].position;double d[3];
        for(int k=0;k<3;++k)d[k]=p[k]-basis.origin[k];
        if(fabs(dot3p(d,basis.normal))>1e-6*(1.0+sqrt(dot3p(d,d))))return 0;
    }
    polygons_t triangles_a={0},triangles_b={0},remainder={0},overlap={0};
    bool ok=triangles_for_face(map,s,f,&basis,&triangles_a)&&triangles_for_face(map,t,g,&basis,&triangles_b)&&
        partition(&triangles_a,&triangles_b,&remainder,&overlap);
    int result=ok?(overlap.n>0?1:0):-1;
    clear_polygons(&triangles_a);clear_polygons(&triangles_b);clear_polygons(&remainder);clear_polygons(&overlap);
    return result;
}

/* Join two pieces of the same authored face across one complete, oppositely
 * directed edge. Keeping all other vertices makes neighboring shell edges
 * conforming, including vertices inserted by other portal cuts. */
static bool joined_loop(const editor_sector_face_t *a,const editor_sector_face_t *b,size_t **out,size_t *count)
{
    size_t ai=SIZE_MAX,bi=SIZE_MAX,matches=0;
    for(size_t i=0;i<a->num_vertices;++i)for(size_t j=0;j<b->num_vertices;++j)
        if(a->vertices[i]==b->vertices[(j+1)%b->num_vertices] && a->vertices[(i+1)%a->num_vertices]==b->vertices[j])
            {ai=i;bi=j;++matches;}
    size_t n=a->num_vertices+b->num_vertices-2;
    if(matches!=1 || n>PORTAL_VERTEX_LIMIT)return false;
    size_t *v=malloc(n*sizeof(*v));if(!v)return false;
    size_t k=0;
    for(size_t i=0;i<a->num_vertices;++i)v[k++]=a->vertices[(ai+1+i)%a->num_vertices];
    for(size_t i=2;i<b->num_vertices;++i)v[k++]=b->vertices[(bi+i)%b->num_vertices];
    *out=v;*count=n;return true;
}

/* Remove a face and repair every reciprocal face index. References to the
 * removed face now point to the surviving, joined face. */
static void remove_joined_face(editor_sector_map_t *map,size_t sector,size_t removed,size_t kept)
{
    for(size_t s=0;s<map->num_sectors;++s)for(size_t f=0;f<map->sectors[s].num_faces;++f){
        editor_sector_face_t *face=&map->sectors[s].faces[f];
        if(face->portal_sector!=sector)continue;
        if(face->portal_face==removed)face->portal_face=kept;
        if(face->portal_face>removed)--face->portal_face;
    }
    editor_sector_t *sec=&map->sectors[sector];free(sec->faces[removed].vertices);
    memmove(&sec->faces[removed],&sec->faces[removed+1],(sec->num_faces-removed-1)*sizeof(*sec->faces));
    --sec->num_faces;
}

static bool group_has_link(const editor_sector_t *sec,size_t group,size_t neighbor)
{
    for(size_t f=0;f<sec->num_faces;++f)
        if(sec->faces[f].source_group==group && sec->faces[f].portal_sector==neighbor)return true;
    return false;
}

/* Coalesce only editor-generated pieces from the same source face. An
 * intentional knife cut has group zero and remains a separate editable face.
 * A portal pair is joined on both sides together; validation is performed on
 * a clone, so a non-simple union is simply left subdivided. */
static bool cleanup_pair(editor_sector_map_t *map,size_t sector,size_t first,size_t second)
{
    const editor_sector_face_t *a=&map->sectors[sector].faces[first],*b=&map->sectors[sector].faces[second];
    size_t *loop=NULL,*peer_loop=NULL,n=0,pn=0;
    if(!joined_loop(a,b,&loop,&n))return false;
    size_t peer=a->portal_sector,pa=a->portal_face,pb=b->portal_face;
    if(peer!=EDITOR_SECTOR_NO_PORTAL){
        const editor_sector_face_t *x=&map->sectors[peer].faces[pa],*y=&map->sectors[peer].faces[pb];
        if(pa==pb||!x->source_group||x->source_group!=y->source_group||strcmp(x->material,y->material)||
           !same_plane(map,x,y)||!joined_loop(x,y,&peer_loop,&pn)){free(loop);return false;}
    }
    editor_sector_map_t copy={0};
    if(!EditorSector_Clone(&copy,map)){free(loop);free(peer_loop);return false;}
    editor_sector_face_t *dst=&copy.sectors[sector].faces[first];free(dst->vertices);dst->vertices=loop;dst->num_vertices=n;
    if(peer!=EDITOR_SECTOR_NO_PORTAL){
        dst=&copy.sectors[peer].faces[pa];free(dst->vertices);dst->vertices=peer_loop;dst->num_vertices=pn;
    }
    remove_joined_face(&copy,sector,second,first);
    if(peer!=EDITOR_SECTOR_NO_PORTAL)remove_joined_face(&copy,peer,pb,pa);
    if(!EditorSector_Validate(&copy,NULL,0)){EditorSector_Free(&copy);return false;}
    EditorSector_Free(map);*map=copy;return true;
}

static size_t cleanup_linked_faces(editor_sector_map_t *map,size_t sector,size_t other,size_t *checks,size_t *attempts)
{
    size_t sectors[2]={sector,other},merged=0;bool changed=true;
    /* Each attempted join validates a clone of the document. Bound both the
     * pair scan and the expensive attempts to keep linking responsive. */
    while(changed&&*checks<32768&&*attempts<32){changed=false;
        for(int side=0;side<2&&!changed;++side){size_t s=sectors[side],neighbor=sectors[1-side];
            editor_sector_t *sec=&map->sectors[s];
            for(size_t i=0;!changed&&i<sec->num_faces&&*checks<32768&&*attempts<32;++i){editor_sector_face_t *a=&sec->faces[i];
                if(!a->source_group||!group_has_link(sec,a->source_group,neighbor))continue;
                if(a->portal_sector!=EDITOR_SECTOR_NO_PORTAL&&a->portal_sector!=neighbor)continue;
                for(size_t j=i+1;j<sec->num_faces&&*checks<32768&&*attempts<32;++j){editor_sector_face_t *b=&sec->faces[j];++*checks;
                    if(a->source_group!=b->source_group || a->portal_sector!=b->portal_sector ||
                       strcmp(a->material,b->material) || !same_plane(map,a,b) || !share_edge(map,a,b))continue;
                    ++*attempts;
                    if(cleanup_pair(map,s,i,j)){changed=true;++merged;break;}
                }
            }
        }
    }
    return merged;
}

/* A point inserted where a portal subdivision crossed an existing edge can
 * be removed only when every incident face continues straight through it.
 * A real corner, knife edge or T-junction fails that test and is retained. */
static bool dissolve_straight_vertex(editor_sector_map_t *map,size_t vertex,const bool *connected)
{
    bool used=false;
    for(size_t s=0;s<map->num_sectors;++s)for(size_t f=0;f<map->sectors[s].num_faces;++f){
        const editor_sector_face_t *face=&map->sectors[s].faces[f];
        for(size_t i=0;i<face->num_vertices;++i)if(face->vertices[i]==vertex){
            if(!connected[s]||face->num_vertices<=3)return false;
            const double *a=map->vertices[face->vertices[(i+face->num_vertices-1)%face->num_vertices]].position;
            const double *b=map->vertices[face->vertices[(i+1)%face->num_vertices]].position;
            const double *p=map->vertices[vertex].position;double edge[3],delta[3],perp[3];
            for(int k=0;k<3;++k){edge[k]=b[k]-a[k];delta[k]=p[k]-a[k];}
            double len=dot3p(edge,edge);if(len<=PORTAL_EPS*PORTAL_EPS)return false;
            double t=dot3p(delta,edge)/len;if(t<=PORTAL_EPS||t>=1.0-PORTAL_EPS)return false;
            for(int k=0;k<3;++k)perp[k]=delta[k]-t*edge[k];
            if(dot3p(perp,perp)>PORTAL_EPS*PORTAL_EPS)return false;
            used=true;
        }
    }
    if(!used)return false;
    editor_sector_map_t copy={0};if(!EditorSector_Clone(&copy,map))return false;
    for(size_t s=0;s<copy.num_sectors;++s)for(size_t f=0;f<copy.sectors[s].num_faces;++f){
        editor_sector_face_t *face=&copy.sectors[s].faces[f];
        for(size_t i=0;i<face->num_vertices;++i)if(face->vertices[i]==vertex){
            memmove(&face->vertices[i],&face->vertices[i+1],(face->num_vertices-i-1)*sizeof(*face->vertices));
            --face->num_vertices;break;
        }
    }
    if(!EditorSector_Validate(&copy,NULL,0)){EditorSector_Free(&copy);return false;}
    EditorSector_Free(map);*map=copy;return true;
}

static size_t cleanup_straight_portal_vertices(editor_sector_map_t *map,size_t sector,size_t other,size_t min_vertex,size_t *checks,size_t *attempts)
{
    bool *connected=calloc(map->num_sectors,sizeof(*connected));
    size_t *queue=malloc(map->num_sectors*sizeof(*queue));
    if(!connected||!queue){free(connected);free(queue);return 0;}
    size_t head=0,tail=0;queue[tail++]=sector;connected[sector]=true;
    while(head<tail){size_t s=queue[head++];for(size_t f=0;f<map->sectors[s].num_faces;++f){
        size_t peer=map->sectors[s].faces[f].portal_sector;
        if(peer!=EDITOR_SECTOR_NO_PORTAL&&!connected[peer]){connected[peer]=true;queue[tail++]=peer;}
    }}
    size_t removed=0;bool changed=true;
    while(changed&&*checks<32768&&*attempts<32){changed=false;
        for(size_t f=0;!changed&&f<map->sectors[sector].num_faces;++f){
            const editor_sector_face_t *face=&map->sectors[sector].faces[f];
            if(face->portal_sector!=other||!face->source_group)continue;
            for(size_t v=0;!changed&&v<face->num_vertices&&*checks<32768&&*attempts<32;++v){
                ++*checks;size_t id=face->vertices[v];if(id<min_vertex)continue;++*attempts;
                if(dissolve_straight_vertex(map,id,connected)){++removed;changed=true;}
            }
        }
    }
    free(connected);free(queue);return removed;
}

bool EditorSector_LinkFaceRegions(editor_sector_map_t *map,size_t sector,size_t face,size_t other_sector,size_t other_face,char *error,size_t error_size)
{
    if(error&&error_size)*error=0;
    if(!map||!EditorSector_Validate(map,error,error_size))return false;
    if(sector>=map->num_sectors||other_sector>=map->num_sectors||sector==other_sector||
       face>=map->sectors[sector].num_faces||other_face>=map->sectors[other_sector].num_faces)
        return portal_error(error,error_size,"Select faces on two different sectors");
    if(map->sectors[sector].faces[face].portal_sector!=EDITOR_SECTOR_NO_PORTAL||
       map->sectors[other_sector].faces[other_face].portal_sector!=EDITOR_SECTOR_NO_PORTAL)
        return portal_error(error,error_size,"Unlink existing portals before linking faces");
    int initial=face_overlap(map,sector,face,other_sector,other_face);
    if(initial<0)return portal_error(error,error_size,"Unable to calculate face overlap");
    /* Selected faces can be two different triangles of a matching quad and
     * share only their diagonal. In that case their connected regions can
     * still have a valid overlap, so test all members below. */
    bool *a=calloc(PORTAL_FACE_LIMIT,sizeof(*a)),*b=calloc(PORTAL_FACE_LIMIT,sizeof(*b));
    editor_sector_map_t next={0};
    if(!a||!b||!EditorSector_Clone(&next,map)) {
        free(a);free(b);EditorSector_Free(&next);
        return portal_error(error,error_size,"Out of memory linking face regions");
    }
    mark_face_region(&next,sector,face,a);mark_face_region(&next,other_sector,other_face,b);
    size_t linked=0;bool ok=true,changed=true;
    while(ok&&changed) {
        changed=false;
        for(size_t i=0;i<next.sectors[sector].num_faces&&!changed&&ok;++i) {
            if(!a[i]||next.sectors[sector].faces[i].portal_sector!=EDITOR_SECTOR_NO_PORTAL)continue;
            for(size_t j=0;j<next.sectors[other_sector].num_faces&&!changed&&ok;++j) {
                if(!b[j]||next.sectors[other_sector].faces[j].portal_sector!=EDITOR_SECTOR_NO_PORTAL)continue;
                int overlap=face_overlap(&next,sector,i,other_sector,j);
                if(overlap<0){ok=portal_error(error,error_size,"Unable to calculate face overlap");break;}
                if(!overlap)continue;
                size_t old_a=next.sectors[sector].num_faces,old_b=next.sectors[other_sector].num_faces;
                if(!EditorSector_LinkPartialPortal(&next,sector,i,other_sector,j,error,error_size)){ok=false;break;}
                for(size_t k=old_a;k<next.sectors[sector].num_faces;++k)a[k]=true;
                for(size_t k=old_b;k<next.sectors[other_sector].num_faces;++k)b[k]=true;
                ++linked;changed=true;
            }
        }
    }
    if(ok&&!linked)ok=portal_error(error,error_size,"Selected face regions have no coplanar overlap");
    if(ok){size_t checks=0,attempts=0;cleanup_linked_faces(&next,sector,other_sector,&checks,&attempts);
        cleanup_straight_portal_vertices(&next,sector,other_sector,map->num_vertices,&checks,&attempts);EditorSector_Free(map);*map=next;memset(&next,0,sizeof(next));}
    EditorSector_Free(&next);free(a);free(b);return ok;
}

bool EditorSector_CleanupLinks(editor_sector_map_t *map,size_t *changes,char *error,size_t error_size)
{
    if(changes)*changes=0;
    if(!map||!EditorSector_Validate(map,error,error_size))return false;
    editor_sector_map_t next={0};
    if(!EditorSector_Clone(&next,map))return portal_error(error,error_size,"Out of memory cleaning sector links");
    size_t *component=malloc(map->num_sectors*sizeof(*component)),*queue=malloc(map->num_sectors*sizeof(*queue));
    size_t *owner=malloc(map->num_vertices*sizeof(*owner)),*visited=calloc(map->num_sectors,sizeof(*visited));
    if((map->num_sectors&&(!component||!queue||!visited))||(map->num_vertices&&!owner)){
        free(component);free(queue);free(owner);free(visited);EditorSector_Free(&next);
        return portal_error(error,error_size,"Out of memory cleaning sector links");
    }
    for(size_t s=0;s<map->num_sectors;++s)component[s]=SIZE_MAX;
    for(size_t s=0;s<map->num_sectors;++s)if(component[s]==SIZE_MAX){
        size_t head=0,tail=0;queue[tail++]=s;component[s]=s;
        while(head<tail){size_t here=queue[head++];for(size_t f=0;f<map->sectors[here].num_faces;++f){
            size_t peer=map->sectors[here].faces[f].portal_sector;
            if(peer!=EDITOR_SECTOR_NO_PORTAL&&component[peer]==SIZE_MAX){component[peer]=s;queue[tail++]=peer;}
        }}
    }
    for(size_t i=0;i<map->num_vertices;++i)owner[i]=SIZE_MAX;
    for(size_t s=0;s<map->num_sectors;++s)for(size_t f=0;f<map->sectors[s].num_faces;++f)
        for(size_t i=0;i<map->sectors[s].faces[f].num_vertices;++i){size_t v=map->sectors[s].faces[f].vertices[i];
            if(owner[v]==SIZE_MAX)owner[v]=component[s];else if(owner[v]!=component[s])owner[v]=SIZE_MAX-1;}
    size_t total=0,weld_attempts=0,checks=0,attempts=0;bool changed=true;
    /* Existing portals can contain different vertex IDs at the exact same
     * position. Weld only IDs confined to this portal-connected component. */
    while(changed&&weld_attempts<32){changed=false;
        for(size_t s=0;s<next.num_sectors&&!changed;++s)for(size_t f=0;f<next.sectors[s].num_faces&&!changed;++f){
            const editor_sector_face_t *a=&next.sectors[s].faces[f];size_t peer=a->portal_sector;
            if(peer==EDITOR_SECTOR_NO_PORTAL||peer<=s)continue;
            const editor_sector_face_t *b=&next.sectors[peer].faces[a->portal_face];
            for(size_t i=0;!changed&&i<a->num_vertices&&weld_attempts<32;++i)for(size_t j=0;!changed&&j<b->num_vertices&&weld_attempts<32;++j){
                size_t av=a->vertices[i],bv=b->vertices[j];
                if(av==bv||owner[av]!=component[s]||owner[bv]!=component[s]||
                   !same_pos(next.vertices[av].position,next.vertices[bv].position))continue;
                ++weld_attempts;
                if(EditorSector_MergePortalVertices(&next,bv,av,NULL,0)){++total;changed=true;}
            }
        }
    }
    for(size_t s=0;s<next.num_sectors&&checks<32768&&attempts<32;++s){
        for(size_t f=0;f<next.sectors[s].num_faces&&checks<32768&&attempts<32;++f){
            size_t peer=next.sectors[s].faces[f].portal_sector;
            if(peer==EDITOR_SECTOR_NO_PORTAL||peer<=s||visited[peer]==s+1)continue;
            visited[peer]=s+1;total+=cleanup_linked_faces(&next,s,peer,&checks,&attempts);
            total+=cleanup_straight_portal_vertices(&next,s,peer,0,&checks,&attempts);
            /* Face indices can change during cleanup; rescan this sector. */
            f=SIZE_MAX;
        }
    }
    free(component);free(queue);free(owner);free(visited);
    if(total){EditorSector_Free(map);*map=next;memset(&next,0,sizeof(next));}
    EditorSector_Free(&next);if(changes)*changes=total;return true;
}

bool EditorSector_UnlinkPortalGroup(editor_sector_map_t *map,size_t sector,size_t face,char *error,size_t error_size)
{
    if(error&&error_size)*error=0;
    if(!map||!EditorSector_Validate(map,error,error_size))return false;
    if(sector>=map->num_sectors||face>=map->sectors[sector].num_faces)
        return portal_error(error,error_size,"Invalid portal face");
    const editor_sector_face_t *source=&map->sectors[sector].faces[face];
    if(source->portal_sector==EDITOR_SECTOR_NO_PORTAL)return portal_error(error,error_size,"Face has no portal");
    const size_t neighbor=source->portal_sector,count=map->sectors[sector].num_faces;
    bool *marked=calloc(count,sizeof(*marked));if(!marked)return portal_error(error,error_size,"Out of memory unlinking portal");
    marked[face]=true;bool changed=true;
    while(changed) {
        changed=false;
        for(size_t a=0;a<count;++a)if(marked[a])for(size_t b=0;b<count;++b) {
            if(marked[b])continue;
            const editor_sector_face_t *one=&map->sectors[sector].faces[a],*two=&map->sectors[sector].faces[b];
            if(two->portal_sector==neighbor&&share_edge(map,one,two)&&same_plane(map,one,two)) {marked[b]=true;changed=true;}
        }
    }
    editor_sector_map_t copy={0};
    if(!EditorSector_Clone(&copy,map)){free(marked);return portal_error(error,error_size,"Out of memory unlinking portal");}
    for(size_t i=0;i<count;++i)if(marked[i]) {
        editor_sector_face_t *a=&copy.sectors[sector].faces[i],*b=&copy.sectors[a->portal_sector].faces[a->portal_face];
        a->portal_sector=a->portal_face=b->portal_sector=b->portal_face=EDITOR_SECTOR_NO_PORTAL;
    }
    free(marked);
    if(!EditorSector_Validate(&copy,error,error_size)){EditorSector_Free(&copy);return false;}
    EditorSector_Free(map);*map=copy;return true;
}
