/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "editor_sector.h"
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SECTOR_MAX_VERTICES 1048576u
#define SECTOR_MAX_SECTORS 65536u
#define SECTOR_MAX_FACES 4096u
#define SECTOR_MAX_FACE_VERTICES 256u
#define SECTOR_SHELL_DEPTH 32.0

static bool fail(char *error, size_t size, const char *fmt, ...)
{
    if (error && size) { va_list ap; va_start(ap, fmt); vsnprintf(error, size, fmt, ap); va_end(ap); }
    return false;
}
static bool valid_material(const char *s)
{
    if (!s || !*s) return false;
    size_t length = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
        if (++length >= EDITOR_SECTOR_MATERIAL_MAX || isspace(*p) || *p == '"' || *p == '{' || *p == '}' || *p == '(' || *p == ')' || *p < 33 || *p > 126)
            return false;
    }
    return true;
}
void EditorSector_Free(editor_sector_map_t *map)
{
    if (!map) return;
    for (size_t s=0; s<map->num_sectors; ++s) {
        for (size_t f=0; f<map->sectors[s].num_faces; ++f) free(map->sectors[s].faces[f].vertices);
        free(map->sectors[s].faces);
    }
    free(map->sectors); free(map->vertices); EditorMap_Free(&map->entities); memset(map, 0, sizeof(*map));
}
bool EditorSector_Init(editor_sector_map_t *map)
{
    if (!map) return false;
    editor_sector_map_t next = {0};
    if (!EditorMap_Init(&next.entities)) return false;
    EditorSector_Free(map); *map = next; return true;
}
bool EditorSector_Clone(editor_sector_map_t *dest, const editor_sector_map_t *src)
{
    if (!dest || !src || dest == src) return false;
    editor_sector_map_t next = {0};
    if (src->num_vertices > SECTOR_MAX_VERTICES || src->num_sectors > SECTOR_MAX_SECTORS ||
        (src->num_vertices && !src->vertices) || (src->num_sectors && !src->sectors)) return false;
    if (!EditorMap_Clone(&next.entities, &src->entities)) goto bad;
    if (src->num_vertices) {
        next.vertices = malloc(src->num_vertices * sizeof(*next.vertices)); if (!next.vertices) goto bad;
        memcpy(next.vertices, src->vertices, src->num_vertices * sizeof(*next.vertices)); next.num_vertices = src->num_vertices;
    }
    if (src->num_sectors) {
        next.sectors = calloc(src->num_sectors, sizeof(*next.sectors)); if (!next.sectors) goto bad;
        next.num_sectors = src->num_sectors;
        for (size_t s=0; s<src->num_sectors; ++s) {
            const editor_sector_t *a = &src->sectors[s]; editor_sector_t *b = &next.sectors[s];
            b->prop_type = a->prop_type;
            b->prop_no_collision = a->prop_no_collision;
            memcpy(b->prop_velocity,a->prop_velocity,sizeof(b->prop_velocity));
            if (a->num_faces > SECTOR_MAX_FACES || (a->num_faces && !a->faces)) goto bad;
            if (!a->num_faces) continue;
            b->faces = calloc(a->num_faces, sizeof(*b->faces)); if (!b->faces) goto bad; b->num_faces = a->num_faces;
            for (size_t f=0; f<a->num_faces; ++f) {
                b->faces[f] = a->faces[f]; b->faces[f].vertices = NULL;
                if (a->faces[f].num_vertices > SECTOR_MAX_FACE_VERTICES || (a->faces[f].num_vertices && !a->faces[f].vertices)) goto bad;
                if (!a->faces[f].num_vertices) continue;
                b->faces[f].vertices = malloc(a->faces[f].num_vertices * sizeof(size_t)); if (!b->faces[f].vertices) goto bad;
                memcpy(b->faces[f].vertices, a->faces[f].vertices, a->faces[f].num_vertices * sizeof(size_t));
            }
        }
    }
    EditorSector_Free(dest); *dest = next; return true;
bad: EditorSector_Free(&next); return false;
}
static double dot3(const double a[3], const double b[3]) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
static void sub3(const double a[3], const double b[3], double d[3]) { for (int i=0;i<3;++i) d[i]=a[i]-b[i]; }
static void cross3(const double a[3], const double b[3], double o[3]) { o[0]=a[1]*b[2]-a[2]*b[1]; o[1]=a[2]*b[0]-a[0]*b[2]; o[2]=a[0]*b[1]-a[1]*b[0]; }
static bool same_point(const double a[3], const double b[3])
{
    for (int k=0;k<3;++k) if (a[k] != b[k]) return false;
    return true;
}
/* Newell's method supports concave planar loops. */
static bool face_normal(const editor_sector_map_t *m, const editor_sector_face_t *f, double n[3])
{
    n[0]=n[1]=n[2]=0;
    if (!m || !m->vertices || f->num_vertices<3 || f->num_vertices>SECTOR_MAX_FACE_VERTICES || !f->vertices) return false;
    for (size_t i=0;i<f->num_vertices;++i) {
        size_t a=f->vertices[i], b=f->vertices[(i+1)%f->num_vertices]; if(a>=m->num_vertices || b>=m->num_vertices) return false;
        const double *p=m->vertices[a].position, *q=m->vertices[b].position;
        n[0]+=(p[1]-q[1])*(p[2]+q[2]); n[1]+=(p[2]-q[2])*(p[0]+q[0]); n[2]+=(p[0]-q[0])*(p[1]+q[1]);
    }
    double len=sqrt(dot3(n,n)); if(!isfinite(len)||len<1e-8) return false;
    for(int k=0;k<3;++k)n[k]/=len;
    const double *origin=m->vertices[f->vertices[0]].position;
    for(size_t i=1;i<f->num_vertices;++i) { double d[3]; sub3(m->vertices[f->vertices[i]].position, origin, d); if(fabs(dot3(d,n))>1e-6*(1+sqrt(dot3(d,d)))) return false; }
    return true;
}
static double orient2(const double a[2],const double b[2],const double c[2]) { return (b[0]-a[0])*(c[1]-a[1])-(b[1]-a[1])*(c[0]-a[0]); }
static bool between(double a,double b,double x) { return x>=fmin(a,b)-1e-8 && x<=fmax(a,b)+1e-8; }
static bool segments_cross(const double a[2],const double b[2],const double c[2],const double d[2])
{
    double ab1=orient2(a,b,c),ab2=orient2(a,b,d),cd1=orient2(c,d,a),cd2=orient2(c,d,b);
    if(((ab1>1e-8&&ab2< -1e-8)||(ab1< -1e-8&&ab2>1e-8)) && ((cd1>1e-8&&cd2< -1e-8)||(cd1< -1e-8&&cd2>1e-8))) return true;
    if(fabs(ab1)<=1e-8&&between(a[0],b[0],c[0])&&between(a[1],b[1],c[1])) return true;
    if(fabs(ab2)<=1e-8&&between(a[0],b[0],d[0])&&between(a[1],b[1],d[1])) return true;
    if(fabs(cd1)<=1e-8&&between(c[0],d[0],a[0])&&between(c[1],d[1],a[1])) return true;
    if(fabs(cd2)<=1e-8&&between(c[0],d[0],b[0])&&between(c[1],d[1],b[1])) return true;
    return false;
}
/* Emit face-local triangle indices, keeping face winding. */
static size_t triangulate(const editor_sector_map_t *m,const editor_sector_face_t *f,size_t *triangles)
{
    double normal[3]; if(!face_normal(m,f,normal)) return 0;
    int drop=0; if(fabs(normal[1])>fabs(normal[drop]))drop=1; if(fabs(normal[2])>fabs(normal[drop]))drop=2;
    int u=(drop+1)%3,v=(drop+2)%3; double xy[SECTOR_MAX_FACE_VERTICES][2]; size_t ring[SECTOR_MAX_FACE_VERTICES];
    for(size_t i=0;i<f->num_vertices;++i) { const double *p=m->vertices[f->vertices[i]].position; xy[i][0]=p[u]; xy[i][1]=p[v]; ring[i]=i; }
    size_t count=f->num_vertices;
    for(size_t i=0;i<count;++i) for(size_t j=i+1;j<count;++j) {
        if(j==i+1 || (i==0&&j==count-1)) continue;
        if(segments_cross(xy[i],xy[(i+1)%count],xy[j],xy[(j+1)%count])) return 0;
    }
    double area=0; for(size_t i=0;i<count;++i) { const double *a=xy[ring[i]],*b=xy[ring[(i+1)%count]]; area+=a[0]*b[1]-a[1]*b[0]; }
    if(fabs(area)<1e-8) return 0;
    double sign=area>0?1:-1; size_t out=0;
    while(count>3) { bool found=false;
        for(size_t i=0;i<count;++i) {
            size_t a=ring[(i+count-1)%count],b=ring[i],c=ring[(i+1)%count];
            if(sign*orient2(xy[a],xy[b],xy[c])<=1e-8) continue;
            bool occupied=false; for(size_t j=0;j<count;++j) {
                size_t p=ring[j]; if(p==a||p==b||p==c)continue;
                if(sign*orient2(xy[a],xy[b],xy[p])>=-1e-8 && sign*orient2(xy[b],xy[c],xy[p])>=-1e-8 && sign*orient2(xy[c],xy[a],xy[p])>=-1e-8) { occupied=true; break; }
            }
            if(occupied)continue;
            triangles[out++]=a;triangles[out++]=b;triangles[out++]=c;
            memmove(&ring[i],&ring[i+1],(count-i-1)*sizeof(*ring));--count;found=true;break;
        }
        if(!found)return 0;
    }
    if(sign*orient2(xy[ring[0]],xy[ring[1]],xy[ring[2]])<=1e-8)return 0;
    triangles[out++]=ring[0];triangles[out++]=ring[1];triangles[out++]=ring[2];return out/3;
}
static bool matching_portals(const editor_sector_map_t *m,const editor_sector_face_t *a,const editor_sector_face_t *b)
{
    if(a->num_vertices!=b->num_vertices || a->num_vertices<3) return false;
    for(size_t j=0;j<b->num_vertices;++j) if(same_point(m->vertices[a->vertices[0]].position,m->vertices[b->vertices[j]].position)) {
        bool equal=true; for(size_t i=0;i<a->num_vertices;++i)
            if(!same_point(m->vertices[a->vertices[i]].position,m->vertices[b->vertices[(j+b->num_vertices-i)%b->num_vertices]].position)) {equal=false;break;}
        if(equal)return true;
    }
    return false;
}

typedef struct { size_t vertex[3], face; double normal[3]; } shell_triangle_t;
typedef struct { shell_triangle_t *triangles; size_t count; double mins[3], maxs[3]; } shell_mesh_t;
static void shell_mesh_free(shell_mesh_t *mesh) { free(mesh->triangles); memset(mesh,0,sizeof(*mesh)); }
static bool shell_mesh_create(const editor_sector_map_t *map,size_t index,shell_mesh_t *mesh)
{
    memset(mesh,0,sizeof(*mesh));
    for(int k=0;k<3;++k){mesh->mins[k]=INFINITY;mesh->maxs[k]=-INFINITY;}
    const editor_sector_t *sector=&map->sectors[index];size_t total=0;
    for(size_t f=0;f<sector->num_faces;++f){size_t n=sector->faces[f].num_vertices-2;if(n>SIZE_MAX-total)return false;total+=n;}
    if(total>1000000)return false;
    mesh->triangles=calloc(total,sizeof(*mesh->triangles));if(total&&!mesh->triangles)return false;
    for(size_t f=0;f<sector->num_faces;++f){
        const editor_sector_face_t *face=&sector->faces[f];size_t tris[3*(SECTOR_MAX_FACE_VERTICES-2)];
        size_t n=triangulate(map,face,tris);double normal[3];if(!n||!face_normal(map,face,normal)){shell_mesh_free(mesh);return false;}
        for(size_t t=0;t<n;++t){shell_triangle_t *triangle=&mesh->triangles[mesh->count++];triangle->face=f;memcpy(triangle->normal,normal,sizeof(triangle->normal));
            for(int j=0;j<3;++j){triangle->vertex[j]=face->vertices[tris[t*3+j]];const double *p=map->vertices[triangle->vertex[j]].position;
                for(int k=0;k<3;++k){mesh->mins[k]=fmin(mesh->mins[k],p[k]);mesh->maxs[k]=fmax(mesh->maxs[k],p[k]);}}
        }
    }
    return true;
}
static bool point_on_triangle(const editor_sector_map_t *map,const shell_triangle_t *tri,const double p[3],bool strict)
{
    const double *a=map->vertices[tri->vertex[0]].position,*b=map->vertices[tri->vertex[1]].position,*c=map->vertices[tri->vertex[2]].position;
    double ab[3],ac[3],ap[3];sub3(b,a,ab);sub3(c,a,ac);sub3(p,a,ap);
    double scale=1+sqrt(dot3(ab,ab))+sqrt(dot3(ac,ac));
    if(fabs(dot3(ap,tri->normal))>1e-8*scale)return false;
    double d00=dot3(ab,ab),d01=dot3(ab,ac),d11=dot3(ac,ac),d20=dot3(ap,ab),d21=dot3(ap,ac),den=d00*d11-d01*d01;
    if(fabs(den)<1e-20)return false;
    double v=(d11*d20-d01*d21)/den,w=(d00*d21-d01*d20)/den,u=1-v-w;
    return strict ? u>1e-8&&v>1e-8&&w>1e-8 : u>=-1e-8&&v>=-1e-8&&w>=-1e-8;
}
/* Generalized winding number: supports nonconvex closed oriented shells. */
static int point_in_shell(const editor_sector_map_t *map,const shell_mesh_t *mesh,const double p[3])
{
    double sum=0;
    for(size_t i=0;i<mesh->count;++i){const shell_triangle_t *tri=&mesh->triangles[i];if(point_on_triangle(map,tri,p,false))return 0;
        double a[3],b[3],c[3],cross[3];sub3(map->vertices[tri->vertex[0]].position,p,a);sub3(map->vertices[tri->vertex[1]].position,p,b);sub3(map->vertices[tri->vertex[2]].position,p,c);
        double la=sqrt(dot3(a,a)),lb=sqrt(dot3(b,b)),lc=sqrt(dot3(c,c));if(la<1e-12||lb<1e-12||lc<1e-12)return 0;
        cross3(b,c,cross);double numerator=dot3(a,cross),denominator=la*lb*lc+dot3(a,b)*lc+dot3(b,c)*la+dot3(c,a)*lb;
        sum+=2*atan2(numerator,denominator);
    }
    return fabs(sum)>6.283185307179586 ? 1 : -1;
}
static bool boxes_overlap(const shell_mesh_t *a,const shell_mesh_t *b)
{
    for(int k=0;k<3;++k)if(fmin(a->maxs[k],b->maxs[k])-fmax(a->mins[k],b->mins[k])<=1e-8)return false;
    return true;
}
static bool proper_segment_triangle(const editor_sector_map_t *map,const double a[3],const double b[3],const shell_triangle_t *tri,double hit[3])
{
    const double *p=map->vertices[tri->vertex[0]].position;double da[3],db[3];sub3(a,p,da);sub3(b,p,db);
    double x=dot3(da,tri->normal),y=dot3(db,tri->normal);
    if((x>=-1e-8&&y>=-1e-8)||(x<=1e-8&&y<=1e-8))return false;
    double t=x/(x-y);if(t<=1e-8||t>=1-1e-8)return false;
    for(int k=0;k<3;++k)hit[k]=a[k]+t*(b[k]-a[k]);
    return point_on_triangle(map,tri,hit,false);
}
static bool overlap_2d_strict(const double a[3][2],const double b[3][2])
{
    double signa=orient2(a[0],a[1],a[2])>0?1:-1,signb=orient2(b[0],b[1],b[2])>0?1:-1;
    for(int set=0;set<2;++set){const double (*src)[2]=set?a:b,(*dst)[2]=set?b:a;double sign=set?signb:signa;
        for(int i=0;i<4;++i){double p[2];if(i<3){p[0]=src[i][0];p[1]=src[i][1];}else{p[0]=(src[0][0]+src[1][0]+src[2][0])/3;p[1]=(src[0][1]+src[1][1]+src[2][1])/3;}
            if(sign*orient2(dst[0],dst[1],p)>1e-8&&sign*orient2(dst[1],dst[2],p)>1e-8&&sign*orient2(dst[2],dst[0],p)>1e-8)return true;}
    }
    for(int i=0;i<3;++i)for(int j=0;j<3;++j){double x=orient2(a[i],a[(i+1)%3],b[j]),y=orient2(a[i],a[(i+1)%3],b[(j+1)%3]);
        double z=orient2(b[j],b[(j+1)%3],a[i]),w=orient2(b[j],b[(j+1)%3],a[(i+1)%3]);
        if(((x>1e-8&&y< -1e-8)||(x< -1e-8&&y>1e-8))&&((z>1e-8&&w< -1e-8)||(z< -1e-8&&w>1e-8)))return true;}
    return false;
}
static bool coplanar_overlap(const editor_sector_map_t *map,const shell_triangle_t *a,const shell_triangle_t *b)
{
    double alignment=dot3(a->normal,b->normal);if(fabs(alignment)<1-1e-8)return false;
    double delta[3];sub3(map->vertices[b->vertex[0]].position,map->vertices[a->vertex[0]].position,delta);
    if(fabs(dot3(delta,a->normal))>1e-7)return false;
    int drop=0;if(fabs(a->normal[1])>fabs(a->normal[drop]))drop=1;if(fabs(a->normal[2])>fabs(a->normal[drop]))drop=2;
    int u=(drop+1)%3,v=(drop+2)%3;double pa[3][2],pb[3][2];
    for(int i=0;i<3;++i){const double *x=map->vertices[a->vertex[i]].position,*y=map->vertices[b->vertex[i]].position;pa[i][0]=x[u];pa[i][1]=x[v];pb[i][0]=y[u];pb[i][1]=y[v];}
    return overlap_2d_strict(pa,pb);
}
static bool shell_self_intersects(const editor_sector_map_t *map,const shell_mesh_t *mesh)
{
    for(size_t i=0;i<mesh->count;++i)for(size_t j=i+1;j<mesh->count;++j){const shell_triangle_t *a=&mesh->triangles[i],*b=&mesh->triangles[j];
        if(a->face==b->face)continue;
        if(coplanar_overlap(map,a,b))return true;
        for(int k=0;k<3;++k){double hit[3];const double *p=map->vertices[a->vertex[k]].position,*q=map->vertices[a->vertex[(k+1)%3]].position;
            if(proper_segment_triangle(map,p,q,b,hit)&&point_on_triangle(map,b,hit,true))return true;
            p=map->vertices[b->vertex[k]].position;q=map->vertices[b->vertex[(k+1)%3]].position;
            if(proper_segment_triangle(map,p,q,a,hit)&&point_on_triangle(map,a,hit,true))return true;}
    }
    return false;
}
static bool shells_overlap(const editor_sector_map_t *map,const shell_mesh_t *a,const shell_mesh_t *b)
{
    if(!boxes_overlap(a,b))return false;
    for(int side=0;side<2;++side){const shell_mesh_t *one=side?b:a,*other=side?a:b;
        for(size_t i=0;i<one->count;++i){const shell_triangle_t *tri=&one->triangles[i];
            for(int v=0;v<3;++v)if(point_in_shell(map,other,map->vertices[tri->vertex[v]].position)>0)return true;
            double p[3]={0,0,0};for(int v=0;v<3;++v)for(int k=0;k<3;++k)p[k]+=map->vertices[tri->vertex[v]].position[k]/3.0;
            for(int k=0;k<3;++k)p[k]-=tri->normal[k]*1e-5;
            if(point_in_shell(map,one,p)>0&&point_in_shell(map,other,p)>0)return true;
        }
    }
    for(size_t i=0;i<a->count;++i)for(size_t j=0;j<b->count;++j){const shell_triangle_t *ta=&a->triangles[i],*tb=&b->triangles[j];
        if(dot3(ta->normal,tb->normal)>0.99999999&&coplanar_overlap(map,ta,tb))return true;
        for(int side=0;side<2;++side){const shell_triangle_t *one=side?tb:ta,*other=side?ta:tb;
            for(int edge=0;edge<3;++edge){double hit[3];const double *p=map->vertices[one->vertex[edge]].position,*q=map->vertices[one->vertex[(edge+1)%3]].position;
                if(!proper_segment_triangle(map,p,q,other,hit))continue;
                double normal[3];for(int k=0;k<3;++k)normal[k]=one->normal[k]+other->normal[k];
                double len=sqrt(dot3(normal,normal));if(len<1e-8)continue;
                double test[3];for(int k=0;k<3;++k)test[k]=hit[k]-normal[k]*1e-5/len;
                if(point_in_shell(map,a,test)>0&&point_in_shell(map,b,test)>0)return true;
            }
        }
    }
    return false;
}
static bool validate_shells(const editor_sector_map_t *map,char *error,size_t error_size)
{
    shell_mesh_t *meshes=calloc(map->num_sectors,sizeof(*meshes));if(map->num_sectors&&!meshes)return fail(error,error_size,"Out of memory validating sector geometry");
    bool ok=true;for(size_t s=0;s<map->num_sectors&&ok;++s){if(!shell_mesh_create(map,s,&meshes[s])){ok=fail(error,error_size,"Could not triangulate sector %zu",s);break;}
        if(shell_self_intersects(map,&meshes[s]))ok=fail(error,error_size,"Sector %zu has self-intersecting surfaces",s);}
    for(size_t a=0;a<map->num_sectors&&ok;++a)for(size_t b=a+1;b<map->num_sectors&&ok;++b)
        if(map->sectors[a].prop_type==EDITOR_SECTOR_WORLD&&map->sectors[b].prop_type==EDITOR_SECTOR_WORLD&&
           shells_overlap(map,&meshes[a],&meshes[b]))ok=fail(error,error_size,"Sectors %zu and %zu overlap",a,b);
    for(size_t s=0;s<map->num_sectors;++s)shell_mesh_free(&meshes[s]);
    free(meshes);return ok;
}
static bool validate_prop_vertex_ownership(const editor_sector_map_t *map,char *error,size_t error_size)
{
    size_t *owner=malloc(map->num_vertices*sizeof(*owner));
    if(map->num_vertices&&!owner)return fail(error,error_size,"Out of memory validating prop vertices");
    for(size_t i=0;i<map->num_vertices;++i)owner[i]=SIZE_MAX;
    for(size_t s=0;s<map->num_sectors;++s)for(size_t f=0;f<map->sectors[s].num_faces;++f){
        const editor_sector_face_t *face=&map->sectors[s].faces[f];
        for(size_t i=0;i<face->num_vertices;++i){size_t v=face->vertices[i],kind=map->sectors[s].prop_type==EDITOR_SECTOR_WORLD?0:s+1;
            if(owner[v]!=SIZE_MAX&&owner[v]!=kind){free(owner);return fail(error,error_size,"Prop vertices must be independent of other sectors");}
            owner[v]=kind;}}
    free(owner);return true;
}

bool EditorSector_Validate(const editor_sector_map_t *m,char *error,size_t error_size)
{
    if(error&&error_size)*error=0;
    if(!m || m->num_vertices>SECTOR_MAX_VERTICES || m->num_sectors>SECTOR_MAX_SECTORS ||
       (m->num_vertices&&!m->vertices) || (m->num_sectors&&!m->sectors)) return fail(error,error_size,"Invalid sector document");
    if(!m->entities.num_entities || !m->entities.entities || !EditorMap_GetProperty(&m->entities.entities[0],"classname") ||
       strcmp(EditorMap_GetProperty(&m->entities.entities[0],"classname"),"worldspawn")) return fail(error,error_size,"Missing worldspawn entity");
    for(size_t i=0;i<m->num_vertices;++i) for(int k=0;k<3;++k) {
        if(!isfinite(m->vertices[i].position[k])) return fail(error,error_size,"Vertex %zu is not finite",i);
        if(fabs(m->vertices[i].position[k])>EDITOR_SECTOR_COORD_LIMIT)
            return fail(error,error_size,"Vertex %zu exceeds the editor's safe coordinate range",i);
    }
    /* Check every array before manifold and portal traversals inspect peers. */
    for(size_t s=0;s<m->num_sectors;++s) {
        const editor_sector_t *sec=&m->sectors[s];
        if(sec->prop_type<EDITOR_SECTOR_WORLD||sec->prop_type>EDITOR_SECTOR_PROP_MOVABLE)
            return fail(error,error_size,"Sector %zu has an invalid prop type",s);
        if(sec->prop_no_collision&&sec->prop_type!=EDITOR_SECTOR_PROP_STATIC)
            return fail(error,error_size,"Sector %zu collision setting requires a static prop",s);
        for(int k=0;k<3;++k)if(!isfinite(sec->prop_velocity[k])||fabs(sec->prop_velocity[k])>1000000.0)
            return fail(error,error_size,"Sector %zu has an invalid prop velocity",s);
        if(sec->num_faces<4||sec->num_faces>SECTOR_MAX_FACES||!sec->faces) return fail(error,error_size,"Sector %zu needs a closed shell",s);
        for(size_t f=0;f<sec->num_faces;++f) {
            const editor_sector_face_t *face=&sec->faces[f];
            if(face->num_vertices<3||face->num_vertices>SECTOR_MAX_FACE_VERTICES||!face->vertices||!memchr(face->material,0,sizeof(face->material)))
                return fail(error,error_size,"Sector %zu face %zu has invalid data",s,f);
            for(size_t v=0;v<face->num_vertices;++v)if(face->vertices[v]>=m->num_vertices)return fail(error,error_size,"Sector %zu face %zu has invalid vertex",s,f);
        }
    }
    if(!validate_prop_vertex_ownership(m,error,error_size))return false;
    for(size_t s=0;s<m->num_sectors;++s) {
        const editor_sector_t *sec=&m->sectors[s];
        if(sec->num_faces<4 || sec->num_faces>SECTOR_MAX_FACES || !sec->faces) return fail(error,error_size,"Sector %zu needs a closed shell",s);
        double volume6=0;
        for(size_t f=0;f<sec->num_faces;++f) {
            const editor_sector_face_t *face=&sec->faces[f]; double n[3]; size_t triangles[3*(SECTOR_MAX_FACE_VERTICES-2)];
            if(!valid_material(face->material) || !face_normal(m,face,n) || !triangulate(m,face,triangles)) return fail(error,error_size,"Sector %zu face %zu is not a simple planar polygon",s,f);
            for(size_t i=0;i<face->num_vertices;++i) {
                size_t a=face->vertices[i],b=face->vertices[(i+1)%face->num_vertices];
                if(a==b || same_point(m->vertices[a].position,m->vertices[b].position)) return fail(error,error_size,"Sector %zu face %zu has a zero-length edge",s,f);
                size_t reverse=0,forward=0;
                for(size_t j=0;j<sec->num_faces;++j) for(size_t k=0;k<sec->faces[j].num_vertices;++k) {
                    const editor_sector_face_t *other=&sec->faces[j]; if(!other->vertices) return fail(error,error_size,"Missing face vertices");
                    size_t c=other->vertices[k],d=other->vertices[(k+1)%other->num_vertices];
                    if(c==b&&d==a)++reverse;
                    if(c==a&&d==b)++forward;
                }
                if(reverse!=1||forward!=1) return fail(error,error_size,"Sector %zu has an open or non-manifold edge",s);
            }
            const double *a=m->vertices[face->vertices[0]].position;
            for(size_t i=1;i+1<face->num_vertices;++i) { const double *b=m->vertices[face->vertices[i]].position,*c=m->vertices[face->vertices[i+1]].position; double cross[3];cross3(b,c,cross);volume6+=dot3(a,cross); }
            if((face->portal_sector==EDITOR_SECTOR_NO_PORTAL)!=(face->portal_face==EDITOR_SECTOR_NO_PORTAL)) return fail(error,error_size,"Sector %zu face %zu has incomplete portal",s,f);
            if(face->portal_sector!=EDITOR_SECTOR_NO_PORTAL) {
                size_t os=face->portal_sector,of=face->portal_face;
                if(os>=m->num_sectors||os==s||of>=m->sectors[os].num_faces||!m->sectors[os].faces) return fail(error,error_size,"Sector %zu face %zu has invalid portal",s,f);
                if(sec->prop_type!=EDITOR_SECTOR_WORLD||m->sectors[os].prop_type!=EDITOR_SECTOR_WORLD)
                    return fail(error,error_size,"Props cannot have sector portals");
                const editor_sector_face_t *other=&m->sectors[os].faces[of];
                if(other->portal_sector!=s||other->portal_face!=f||!other->vertices||other->num_vertices>SECTOR_MAX_FACE_VERTICES) return fail(error,error_size,"Sector %zu face %zu has an unpaired portal",s,f);
                for(size_t v=0;v<other->num_vertices;++v)if(other->vertices[v]>=m->num_vertices)return fail(error,error_size,"Portal has invalid vertex");
                if(!matching_portals(m,face,other))return fail(error,error_size,"Sector %zu face %zu portal polygons do not match",s,f);
            }
        }
        if(!isfinite(volume6)||volume6<=1e-6)return fail(error,error_size,"Sector %zu shell must have outward winding and nonzero volume",s);
    }
    return validate_shells(m,error,error_size);
}

bool EditorSector_QueryPointsInOpenWorld(const editor_sector_map_t *map,const double (*points)[3],
    size_t count,bool *is_open,bool *has_world,char *error,size_t error_size)
{
    if(has_world)*has_world=false;
    if(!map||!has_world||count>1024||(count&&(!points||!is_open)))return fail(error,error_size,"Invalid point classification request");
    for(size_t i=0;i<count;++i){is_open[i]=false;
        for(int k=0;k<3;++k)if(!isfinite(points[i][k])||fabs(points[i][k])>EDITOR_SECTOR_COORD_LIMIT)
            return fail(error,error_size,"Point exceeds the editor coordinate range");}
    if(!EditorSector_Validate(map,error,error_size))return false;
    for(size_t s=0;s<map->num_sectors;++s)if(map->sectors[s].prop_type==EDITOR_SECTOR_WORLD){*has_world=true;break;}
    if(!*has_world||!count)return true;
    shell_mesh_t *meshes=calloc(map->num_sectors,sizeof(*meshes));
    if(!meshes)return fail(error,error_size,"Out of memory inspecting sector geometry");
    for(size_t s=0;s<map->num_sectors;++s)if(!shell_mesh_create(map,s,&meshes[s])){
        for(size_t i=0;i<map->num_sectors;++i)shell_mesh_free(&meshes[i]);
        free(meshes);
        return fail(error,error_size,"Could not inspect sector %zu",s);
    }
    for(size_t i=0;i<count;++i){const double *point=points[i];bool inside=false;
        for(size_t s=0;s<map->num_sectors&&!inside;++s){
            if(map->sectors[s].prop_type!=EDITOR_SECTOR_WORLD)continue;
            bool in_bounds=true;
            for(int k=0;k<3;++k)if(point[k]<=meshes[s].mins[k]||point[k]>=meshes[s].maxs[k])in_bounds=false;
            if(in_bounds&&point_in_shell(map,&meshes[s],point)>0)inside=true;
        }
        if(!inside)continue;
        for(size_t s=0;s<map->num_sectors&&inside;++s){
            if(map->sectors[s].prop_type==EDITOR_SECTOR_WORLD||map->sectors[s].prop_no_collision)continue;
            bool in_bounds=true;
            for(int k=0;k<3;++k)if(point[k]<meshes[s].mins[k]||point[k]>meshes[s].maxs[k])in_bounds=false;
            if(in_bounds&&point_in_shell(map,&meshes[s],point)>=0)inside=false;
        }
        is_open[i]=inside;
    }
    for(size_t s=0;s<map->num_sectors;++s)shell_mesh_free(&meshes[s]);
    free(meshes);return true;
}
bool EditorSector_QueryPointInOpenWorld(const editor_sector_map_t *map,const double point[3],
    bool *is_open,bool *has_world,char *error,size_t error_size)
{
    return EditorSector_QueryPointsInOpenWorld(map,(const double (*)[3])point,1,is_open,has_world,error,error_size);
}

size_t EditorSector_TriangulateFace(const editor_sector_map_t *map,size_t sector,size_t face,size_t (*triangles)[3],size_t capacity)
{
    if(!map||sector>=map->num_sectors||!map->sectors||face>=map->sectors[sector].num_faces||!map->sectors[sector].faces||
       (!triangles&&capacity)) return 0;
    size_t local[3*(SECTOR_MAX_FACE_VERTICES-2)];
    const editor_sector_face_t *f=&map->sectors[sector].faces[face];
    size_t count=triangulate(map,f,local);
    if(!triangles)return count;
    if(count>capacity)return 0;
    for(size_t i=0;i<count;++i)for(int j=0;j<3;++j)triangles[i][j]=f->vertices[local[i*3+j]];
    return count;
}
static bool add_vertex(editor_sector_map_t *map,const double p[3],size_t *index)
{
    for(size_t i=0;i<map->num_vertices;++i)if(same_point(map->vertices[i].position,p)){
        bool prop=false;
        for(size_t s=0;s<map->num_sectors&&!prop;++s)if(map->sectors[s].prop_type!=EDITOR_SECTOR_WORLD)
            for(size_t f=0;f<map->sectors[s].num_faces&&!prop;++f)
                for(size_t v=0;v<map->sectors[s].faces[f].num_vertices;++v)
                    if(map->sectors[s].faces[f].vertices[v]==i){prop=true;break;}
        if(!prop){*index=i;return true;}
    }
    if(map->num_vertices>=SECTOR_MAX_VERTICES)return false;
    void *next=realloc(map->vertices,(map->num_vertices+1)*sizeof(*map->vertices)); if(!next)return false;
    map->vertices=next;memcpy(map->vertices[map->num_vertices].position,p,3*sizeof(double));*index=map->num_vertices++;return true;
}
static bool add_vertex_unique(editor_sector_map_t *map,const double p[3],size_t *index)
{
    if(map->num_vertices>=SECTOR_MAX_VERTICES)return false;
    void *next=realloc(map->vertices,(map->num_vertices+1)*sizeof(*map->vertices));if(!next)return false;
    map->vertices=next;memcpy(map->vertices[map->num_vertices].position,p,3*sizeof(double));*index=map->num_vertices++;return true;
}

typedef struct { size_t sector, a, b; } knife_edge_t;

/* Resolve a point to the face boundary and use the actual edge coordinates,
 * so projection/float roundoff in the viewport cannot open a shell. */
static bool knife_boundary_point(const editor_sector_map_t *map,const editor_sector_face_t *face,
    const double input[3],double point[3],size_t *edge,size_t *existing)
{
    double best=INFINITY,best_t=0;
    *existing=SIZE_MAX;*edge=SIZE_MAX;
    for(int k=0;k<3;++k)if(!isfinite(input[k]))return false;
    for(size_t i=0;i<face->num_vertices;++i){
        const double *a=map->vertices[face->vertices[i]].position,*b=map->vertices[face->vertices[(i+1)%face->num_vertices]].position;
        double ab[3],ap[3];sub3(b,a,ab);sub3(input,a,ap);double len=dot3(ab,ab);
        if(len<1e-20)continue;
        double t=fmax(0,fmin(1,dot3(ap,ab)/len)),d=0;
        for(int k=0;k<3;++k){double x=input[k]-(a[k]+t*ab[k]);d+=x*x;}
        if(d<best){best=d;best_t=t;*edge=i;}
    }
    /* The editor's coordinates arrive as floats, including large maps. */
    double scale=1;for(int k=0;k<3;++k)scale=fmax(scale,fabs(input[k]));
    double tolerance=fmax(1e-4,scale*2e-7);
    if(*edge==SIZE_MAX||!isfinite(best)||best>tolerance*tolerance)return false;
    size_t a=face->vertices[*edge],b=face->vertices[(*edge+1)%face->num_vertices];
    if(best_t<1e-8){*existing=a;memcpy(point,map->vertices[a].position,3*sizeof(double));}
    else if(best_t>1-1e-8){*existing=b;memcpy(point,map->vertices[b].position,3*sizeof(double));}
    else for(int k=0;k<3;++k)point[k]=map->vertices[a].position[k]+best_t*(map->vertices[b].position[k]-map->vertices[a].position[k]);
    return true;
}

static bool knife_enqueue(knife_edge_t **queue,size_t *count,size_t sector,size_t a,size_t b)
{
    for(size_t i=0;i<*count;++i)if((*queue)[i].sector==sector&&
        (((*queue)[i].a==a&&(*queue)[i].b==b)||((*queue)[i].a==b&&(*queue)[i].b==a)))return true;
    void *p=realloc(*queue,(*count+1)*sizeof(**queue));if(!p)return false;
    *queue=p;(*queue)[(*count)++]=(knife_edge_t){sector,a,b};return true;
}

/* Insert an edge point into both incident faces and propagate across any
 * portals on that edge. Portal peers may use different global vertex IDs. */
static bool knife_insert_edge(editor_sector_map_t *map,size_t sector,size_t a,size_t b,size_t vertex)
{
    knife_edge_t *queue=NULL;size_t count=0;
    if(!knife_enqueue(&queue,&count,sector,a,b))return false;
    for(size_t q=0;q<count;++q){knife_edge_t e=queue[q];editor_sector_t *sec=&map->sectors[e.sector];
        for(size_t f=0;f<sec->num_faces;++f){editor_sector_face_t *face=&sec->faces[f];
            for(size_t i=0;i<face->num_vertices;++i){size_t x=face->vertices[i],y=face->vertices[(i+1)%face->num_vertices];
                if(!((x==e.a&&y==e.b)||(x==e.b&&y==e.a)))continue;
                if(face->portal_sector!=EDITOR_SECTOR_NO_PORTAL){
                    editor_sector_face_t *peer=&map->sectors[face->portal_sector].faces[face->portal_face];bool found=false;
                    for(size_t j=0;j<peer->num_vertices;++j){size_t u=peer->vertices[j],v=peer->vertices[(j+1)%peer->num_vertices];
                        if(same_point(map->vertices[u].position,map->vertices[y].position)&&same_point(map->vertices[v].position,map->vertices[x].position)){
                            if(!knife_enqueue(&queue,&count,face->portal_sector,u,v)){free(queue);return false;}found=true;break;}
                        size_t w=peer->vertices[(j+2)%peer->num_vertices];
                        if(same_point(map->vertices[u].position,map->vertices[y].position)&&same_point(map->vertices[v].position,map->vertices[vertex].position)&&
                           same_point(map->vertices[w].position,map->vertices[x].position)){found=true;break;}}
                    if(!found){free(queue);return false;}
                }
                if(face->num_vertices>=SECTOR_MAX_FACE_VERTICES){free(queue);return false;}
                size_t *next=realloc(face->vertices,(face->num_vertices+1)*sizeof(*next));if(!next){free(queue);return false;}
                face->vertices=next;memmove(&next[i+2],&next[i+1],(face->num_vertices-i-1)*sizeof(*next));
                next[i+1]=vertex;++face->num_vertices;break;
            }
        }
    }
    free(queue);return true;
}

/* Split a face whose boundary already contains the two points. Appending
 * rather than inserting a face keeps every other portal index stable. */
static bool knife_split_loop(editor_sector_map_t *map,size_t sector,size_t face_index,
    const double start[3],const double end[3],size_t *new_index)
{
    editor_sector_t *sec=&map->sectors[sector];editor_sector_face_t *face=&sec->faces[face_index];
    size_t a=SIZE_MAX,b=SIZE_MAX,n=face->num_vertices;
    for(size_t i=0;i<n;++i){const double *p=map->vertices[face->vertices[i]].position;
        if(same_point(p,start))a=i;
        if(same_point(p,end))b=i;}
    if(a==SIZE_MAX||b==SIZE_MAX||a==b)return false;
    size_t na=(b+n-a)%n+1,nb=(a+n-b)%n+1;
    if(na<3||nb<3||sec->num_faces>=SECTOR_MAX_FACES)return false;
    double normal[3];if(!face_normal(map,face,normal))return false;
    int drop=0;if(fabs(normal[1])>fabs(normal[drop]))drop=1;if(fabs(normal[2])>fabs(normal[drop]))drop=2;
    int u=(drop+1)%3,v=(drop+2)%3;double p[2]={start[u],start[v]},q[2]={end[u],end[v]};
    for(size_t i=0;i<n;++i){size_t j=(i+1)%n;if(i==a||i==b||j==a||j==b)continue;
        const double *x=map->vertices[face->vertices[i]].position,*y=map->vertices[face->vertices[j]].position;
        double c[2]={x[u],x[v]},d[2]={y[u],y[v]};if(segments_cross(p,q,c,d))return false;}
    /* Check the midpoint is inside, which rules out an exterior chord of a
     * concave polygon even when it meets no other boundary edge. */
    double mid[2]={(p[0]+q[0])*0.5,(p[1]+q[1])*0.5};bool inside=false;
    for(size_t i=0,j=n-1;i<n;j=i++){
        const double *x=map->vertices[face->vertices[i]].position,*y=map->vertices[face->vertices[j]].position;
        if((x[v]>mid[1])!=(y[v]>mid[1])&&mid[0]<(y[u]-x[u])*(mid[1]-x[v])/(y[v]-x[v])+x[u])inside=!inside;
    }
    if(!inside)return false;
    size_t *first=malloc(na*sizeof(*first)),*second=malloc(nb*sizeof(*second));
    if(!first||!second){free(first);free(second);return false;}
    for(size_t i=0;i<na;++i)first[i]=face->vertices[(a+i)%n];
    for(size_t i=0;i<nb;++i)second[i]=face->vertices[(b+i)%n];
    editor_sector_face_t copy=*face;void *grown=realloc(sec->faces,(sec->num_faces+1)*sizeof(*sec->faces));
    if(!grown){free(first);free(second);return false;}sec->faces=grown;face=&sec->faces[face_index];
    free(face->vertices);face->vertices=first;face->num_vertices=na;face->source_group=0;
    *new_index=sec->num_faces++;sec->faces[*new_index]=copy;sec->faces[*new_index].vertices=second;
    sec->faces[*new_index].num_vertices=nb;sec->faces[*new_index].source_group=0;
    return true;
}

bool EditorSector_CutFace(editor_sector_map_t *map,size_t sector,size_t face_index,const double start[3],const double end[3],char *error,size_t error_size)
{
    if(!map||!start||!end||!EditorSector_Validate(map,error,error_size))return false;
    if(sector>=map->num_sectors||face_index>=map->sectors[sector].num_faces)return fail(error,error_size,"Invalid knife face");
    editor_sector_map_t next={0};if(!EditorSector_Clone(&next,map))return fail(error,error_size,"Out of memory cutting face");
    double points[2][3];const double *input[2]={start,end};bool ok=false;
    for(int i=0;i<2;++i){editor_sector_face_t *face=&next.sectors[sector].faces[face_index];size_t edge,vertex;
        if(!knife_boundary_point(&next,face,input[i],points[i],&edge,&vertex)){fail(error,error_size,"Knife endpoints must be on the face boundary");goto done;}
        if(vertex==SIZE_MAX){size_t a=face->vertices[edge],b=face->vertices[(edge+1)%face->num_vertices];
            if(!(next.sectors[sector].prop_type==EDITOR_SECTOR_WORLD ? add_vertex(&next,points[i],&vertex) : add_vertex_unique(&next,points[i],&vertex))||
               !knife_insert_edge(&next,sector,a,b,vertex)){
                fail(error,error_size,"Could not insert knife point into shell edges");goto done;}}
    }
    editor_sector_face_t *source=&next.sectors[sector].faces[face_index];
    size_t peer_sector=source->portal_sector,peer_face=source->portal_face,new_face,new_peer=SIZE_MAX;
    if(!knife_split_loop(&next,sector,face_index,points[0],points[1],&new_face)){
        fail(error,error_size,"Knife cut must cross the face interior between distinct boundary edges");goto done;}
    if(peer_sector!=EDITOR_SECTOR_NO_PORTAL){
        if(!knife_split_loop(&next,peer_sector,peer_face,points[0],points[1],&new_peer)){
            fail(error,error_size,"Knife cut could not split the matching portal");goto done;}
        editor_sector_face_t *a=&next.sectors[sector].faces[face_index],*b=&next.sectors[sector].faces[new_face];
        editor_sector_face_t *c=&next.sectors[peer_sector].faces[peer_face],*d=&next.sectors[peer_sector].faces[new_peer];
        if(matching_portals(&next,a,c)&&matching_portals(&next,b,d)){
            a->portal_face=peer_face;b->portal_face=new_peer;c->portal_face=face_index;d->portal_face=new_face;
        }else if(matching_portals(&next,a,d)&&matching_portals(&next,b,c)){
            a->portal_face=new_peer;b->portal_face=peer_face;d->portal_face=face_index;c->portal_face=new_face;
        }else{fail(error,error_size,"Knife cut produced mismatched portal faces");goto done;}
    }
    if(!EditorSector_Validate(&next,error,error_size))goto done;
    EditorSector_Free(map);*map=next;memset(&next,0,sizeof(next));ok=true;
done:EditorSector_Free(&next);return ok;
}
bool EditorSector_AddBoxOfType(editor_sector_map_t *map,const double mins[3],const double maxs[3],const char *material,editor_sector_prop_type_t type,size_t *out_sector,char *error,size_t error_size)
{
    if(!map||!mins||!maxs||!valid_material(material))return fail(error,error_size,"Invalid box or material");
    if(type<EDITOR_SECTOR_WORLD||type>EDITOR_SECTOR_PROP_MOVABLE)return fail(error,error_size,"Invalid sector type");
    for(int i=0;i<3;++i)if(!isfinite(mins[i])||!isfinite(maxs[i])||maxs[i]-mins[i]<1e-6)return fail(error,error_size,"Box must have finite positive dimensions");
    if(map->num_sectors>=SECTOR_MAX_SECTORS)return fail(error,error_size,"Too many sectors");
    editor_sector_map_t next={0};if(!EditorSector_Clone(&next,map))return fail(error,error_size,"Unable to copy sector map");
    static const unsigned corners[6][4]={{0,4,6,2},{1,3,7,5},{0,1,5,4},{2,6,7,3},{0,2,3,1},{4,5,7,6}};
    size_t vertex[8];for(int i=0;i<8;++i){double p[3];for(int a=0;a<3;++a)p[a]=(i&(1<<a))?maxs[a]:mins[a];
        if(!(type==EDITOR_SECTOR_WORLD?add_vertex(&next,p,&vertex[i]):add_vertex_unique(&next,p,&vertex[i])))goto bad;}
    void *grown=realloc(next.sectors,(next.num_sectors+1)*sizeof(*next.sectors));if(!grown)goto bad;
    next.sectors=grown; editor_sector_t *s=&next.sectors[next.num_sectors];memset(s,0,sizeof(*s));s->prop_type=type;++next.num_sectors;
    s->faces=calloc(6,sizeof(*s->faces));if(!s->faces)goto bad;s->num_faces=6;
    for(int f=0;f<6;++f){editor_sector_face_t *face=&s->faces[f];face->vertices=malloc(4*sizeof(size_t));if(!face->vertices)goto bad;
        face->num_vertices=4;face->portal_sector=face->portal_face=EDITOR_SECTOR_NO_PORTAL;
        memcpy(face->material,material,strlen(material)+1);for(int v=0;v<4;++v)face->vertices[v]=vertex[corners[f][v]];}
    if(!EditorSector_Validate(&next,error,error_size)){EditorSector_Free(&next);return false;}
    if(out_sector)*out_sector=next.num_sectors-1;
    EditorSector_Free(map);*map=next;return true;
bad: EditorSector_Free(&next);return fail(error,error_size,"Out of memory adding sector");
}
bool EditorSector_AddBox(editor_sector_map_t *map,const double mins[3],const double maxs[3],const char *material,size_t *out_sector,char *error,size_t error_size)
{ return EditorSector_AddBoxOfType(map,mins,maxs,material,EDITOR_SECTOR_WORLD,out_sector,error,error_size); }
bool EditorSector_SetPropType(editor_sector_map_t *map,size_t sector,editor_sector_prop_type_t type,char *error,size_t error_size)
{
    if(!map||!EditorSector_Validate(map,error,error_size))return false;
    if(sector>=map->num_sectors||type<EDITOR_SECTOR_WORLD||type>EDITOR_SECTOR_PROP_MOVABLE)return fail(error,error_size,"Invalid sector type");
    if(map->sectors[sector].prop_type==type)return true;
    if(type!=EDITOR_SECTOR_WORLD)for(size_t f=0;f<map->sectors[sector].num_faces;++f)
        if(map->sectors[sector].faces[f].portal_sector!=EDITOR_SECTOR_NO_PORTAL)return fail(error,error_size,"Unlink sector portals before making a prop");
    editor_sector_map_t next={0};if(!EditorSector_Clone(&next,map))return fail(error,error_size,"Out of memory changing sector type");
    size_t old_vertices=next.num_vertices;
    bool *shared=calloc(old_vertices,sizeof(*shared));size_t *remap=malloc(old_vertices*sizeof(*remap));
    if(old_vertices&&(!shared||!remap)){free(shared);free(remap);EditorSector_Free(&next);return fail(error,error_size,"Out of memory changing sector type");}
    for(size_t i=0;i<old_vertices;++i)remap[i]=SIZE_MAX;
    if(type!=EDITOR_SECTOR_WORLD){
        for(size_t s=0;s<next.num_sectors;++s)if(s!=sector)for(size_t f=0;f<next.sectors[s].num_faces;++f)
            for(size_t i=0;i<next.sectors[s].faces[f].num_vertices;++i)shared[next.sectors[s].faces[f].vertices[i]]=true;
        editor_sector_t *selected=&next.sectors[sector];
        for(size_t f=0;f<selected->num_faces;++f)for(size_t i=0;i<selected->faces[f].num_vertices;++i){
            size_t id=selected->faces[f].vertices[i];if(!shared[id])continue;
            if(remap[id]==SIZE_MAX){double p[3];memcpy(p,next.vertices[id].position,sizeof(p));
                if(!add_vertex_unique(&next,p,&remap[id])){free(shared);free(remap);EditorSector_Free(&next);return fail(error,error_size,"Out of memory separating prop vertices");}}
            selected->faces[f].vertices[i]=remap[id];
        }
    }
    free(shared);free(remap);next.sectors[sector].prop_type=type;
    if(type!=EDITOR_SECTOR_PROP_STATIC)next.sectors[sector].prop_no_collision=false;
    if(!EditorSector_Validate(&next,error,error_size)){EditorSector_Free(&next);return false;}
    EditorSector_Free(map);*map=next;return true;
}
bool EditorSector_SetPropCollisionEnabled(editor_sector_map_t *map,size_t sector,bool enabled,char *error,size_t error_size)
{
    if(!map||!EditorSector_Validate(map,error,error_size))return false;
    if(sector>=map->num_sectors||map->sectors[sector].prop_type!=EDITOR_SECTOR_PROP_STATIC)
        return fail(error,error_size,"Collision setting requires a static prop");
    map->sectors[sector].prop_no_collision=!enabled;return true;
}
bool EditorSector_SetPropVelocity(editor_sector_map_t *map,size_t sector,const double velocity[3],char *error,size_t error_size)
{
    if(!velocity||!map)return fail(error,error_size,"Invalid prop velocity");
    if(!EditorSector_Validate(map,error,error_size))return false;
    if(sector>=map->num_sectors)return fail(error,error_size,"Invalid prop velocity");
    if(map->sectors[sector].prop_type!=EDITOR_SECTOR_PROP_DYNAMIC&&map->sectors[sector].prop_type!=EDITOR_SECTOR_PROP_MOVABLE)
        return fail(error,error_size,"Velocity requires a dynamic or movable prop");
    for(int i=0;i<3;++i)if(!isfinite(velocity[i])||fabs(velocity[i])>1000000.0)
        return fail(error,error_size,"Prop velocity must be finite and within +/-1000000");
    memcpy(map->sectors[sector].prop_velocity,velocity,3*sizeof(*velocity));return true;
}
bool EditorSector_DuplicateProp(editor_sector_map_t *map,size_t sector,const double offset[3],size_t *out_sector,char *error,size_t error_size)
{
    if(!map||!offset||!EditorSector_Validate(map,error,error_size))return false;
    if(sector>=map->num_sectors||map->sectors[sector].prop_type==EDITOR_SECTOR_WORLD)
        return fail(error,error_size,"Select a sector prop to duplicate");
    if(map->num_sectors>=SECTOR_MAX_SECTORS)return fail(error,error_size,"Too many sectors");
    for(int k=0;k<3;++k)if(!isfinite(offset[k]))return fail(error,error_size,"Duplicate offset must be finite");
    const editor_sector_t *source=&map->sectors[sector];editor_sector_map_t next={0};
    size_t *remap=malloc(map->num_vertices*sizeof(*remap));
    if(map->num_vertices&&!remap)return fail(error,error_size,"Out of memory duplicating prop");
    for(size_t i=0;i<map->num_vertices;++i)remap[i]=SIZE_MAX;
    if(!EditorSector_Clone(&next,map)){free(remap);return fail(error,error_size,"Out of memory copying prop");}
    void *grown=realloc(next.sectors,(next.num_sectors+1)*sizeof(*next.sectors));
    if(!grown)goto memory;
    next.sectors=grown;size_t added=next.num_sectors++;
    editor_sector_t *dest=&next.sectors[added];memset(dest,0,sizeof(*dest));
    dest->prop_type=source->prop_type;dest->prop_no_collision=source->prop_no_collision;memcpy(dest->prop_velocity,source->prop_velocity,sizeof(dest->prop_velocity));
    dest->faces=calloc(source->num_faces,sizeof(*dest->faces));if(!dest->faces)goto memory;
    dest->num_faces=source->num_faces;
    for(size_t f=0;f<source->num_faces;++f){const editor_sector_face_t *old=&source->faces[f];editor_sector_face_t *face=&dest->faces[f];
        if(old->portal_sector!=EDITOR_SECTOR_NO_PORTAL){fail(error,error_size,"Unlink prop portals before duplicating");goto bad;}
        *face=*old;face->vertices=NULL;face->vertices=malloc(old->num_vertices*sizeof(*face->vertices));if(!face->vertices)goto memory;
        for(size_t i=0;i<old->num_vertices;++i){size_t id=old->vertices[i];
            if(remap[id]==SIZE_MAX){double p[3];for(int k=0;k<3;++k){p[k]=map->vertices[id].position[k]+offset[k];
                    if(!isfinite(p[k])||fabs(p[k])>EDITOR_SECTOR_COORD_LIMIT){fail(error,error_size,"Duplicated prop exceeds the editor coordinate range");goto bad;}}
                if(!add_vertex_unique(&next,p,&remap[id]))goto memory;}
            face->vertices[i]=remap[id];
        }
    }
    free(remap);remap=NULL;
    if(!EditorSector_Validate(&next,error,error_size))goto bad;
    EditorSector_Free(map);*map=next;if(out_sector)*out_sector=added;return true;
memory:fail(error,error_size,"Out of memory duplicating prop");
bad:free(remap);EditorSector_Free(&next);return false;
}
bool EditorSector_Remove(editor_sector_map_t *map,size_t sector,char *error,size_t error_size)
{
    if(!map||sector>=map->num_sectors)return fail(error,error_size,"Invalid sector");
    for(size_t s=0;s<map->num_sectors;++s)for(size_t f=0;f<map->sectors[s].num_faces;++f){editor_sector_face_t *face=&map->sectors[s].faces[f];
        if(face->portal_sector==sector)face->portal_sector=face->portal_face=EDITOR_SECTOR_NO_PORTAL;
        else if(face->portal_sector!=EDITOR_SECTOR_NO_PORTAL&&face->portal_sector>sector)--face->portal_sector;}
    for(size_t f=0;f<map->sectors[sector].num_faces;++f)free(map->sectors[sector].faces[f].vertices);
    free(map->sectors[sector].faces);memmove(&map->sectors[sector],&map->sectors[sector+1],(map->num_sectors-sector-1)*sizeof(*map->sectors));--map->num_sectors;return true;
}
bool EditorSector_LinkPortal(editor_sector_map_t *map,size_t sector,size_t face,size_t other_sector,size_t other_face,char *error,size_t error_size)
{
    if(!EditorSector_Validate(map,error,error_size))return false;
    if(sector>=map->num_sectors||other_sector>=map->num_sectors||sector==other_sector||face>=map->sectors[sector].num_faces||other_face>=map->sectors[other_sector].num_faces)
        return fail(error,error_size,"Invalid portal faces");
    if(map->sectors[sector].prop_type!=EDITOR_SECTOR_WORLD||map->sectors[other_sector].prop_type!=EDITOR_SECTOR_WORLD)
        return fail(error,error_size,"Props cannot have sector portals");
    editor_sector_face_t *a=&map->sectors[sector].faces[face],*b=&map->sectors[other_sector].faces[other_face];
    if(a->portal_sector!=EDITOR_SECTOR_NO_PORTAL||b->portal_sector!=EDITOR_SECTOR_NO_PORTAL)return fail(error,error_size,"Face already has a portal");
    if(!matching_portals(map,a,b))return fail(error,error_size,"Portal faces must match with opposite winding");
    a->portal_sector=other_sector;a->portal_face=other_face;b->portal_sector=sector;b->portal_face=face;return true;
}
bool EditorSector_UnlinkPortal(editor_sector_map_t *map,size_t sector,size_t face,char *error,size_t error_size)
{
    if(!EditorSector_Validate(map,error,error_size))return false;
    if(sector>=map->num_sectors||face>=map->sectors[sector].num_faces)return fail(error,error_size,"Invalid portal face");
    editor_sector_face_t *a=&map->sectors[sector].faces[face];if(a->portal_sector==EDITOR_SECTOR_NO_PORTAL)return fail(error,error_size,"Face has no portal");
    editor_sector_face_t *b=&map->sectors[a->portal_sector].faces[a->portal_face];
    a->portal_sector=a->portal_face=b->portal_sector=b->portal_face=EDITOR_SECTOR_NO_PORTAL;return true;
}
/* Mark all vertices paired through portals, even when parsed source uses
 * separate indices for the same point on opposite portal faces. */
static void extend_portal_vertices(const editor_sector_map_t *map,bool *marked)
{
    bool changed;do{changed=false;for(size_t s=0;s<map->num_sectors;++s)for(size_t f=0;f<map->sectors[s].num_faces;++f){
        const editor_sector_face_t *a=&map->sectors[s].faces[f];if(a->portal_sector==EDITOR_SECTOR_NO_PORTAL)continue;
        const editor_sector_face_t *b=&map->sectors[a->portal_sector].faces[a->portal_face];
        for(size_t i=0;i<a->num_vertices;++i)if(marked[a->vertices[i]])for(size_t j=0;j<b->num_vertices;++j)
            if(!marked[b->vertices[j]]&&same_point(map->vertices[a->vertices[i]].position,map->vertices[b->vertices[j]].position)){marked[b->vertices[j]]=true;changed=true;}
    }}while(changed);
}
/* Split affected nonportal faces into triangles before moving one corner. This
 * keeps a closed manifold even when a formerly planar quad becomes sloped. */
static bool triangulate_marked_faces(editor_sector_map_t *map,const bool *marked,char *error,size_t error_size)
{
    size_t **remap=calloc(map->num_sectors,sizeof(*remap)),*counts=calloc(map->num_sectors,sizeof(*counts));
    editor_sector_t *replacement=calloc(map->num_sectors,sizeof(*replacement));
    if(map->num_sectors&&(!remap||!counts||!replacement))goto memory;
    for(size_t s=0;s<map->num_sectors;++s){
        const editor_sector_t *sector=&map->sectors[s];remap[s]=calloc(sector->num_faces,sizeof(**remap));if(sector->num_faces&&!remap[s])goto memory;
        for(size_t f=0;f<sector->num_faces;++f){
            const editor_sector_face_t *face=&sector->faces[f];bool affected=false;
            for(size_t v=0;v<face->num_vertices;++v)affected|=marked[face->vertices[v]];
            size_t pieces=1;
            if(affected&&face->num_vertices>3){
                size_t tris[3*(SECTOR_MAX_FACE_VERTICES-2)];pieces=triangulate(map,face,tris);
                if(!pieces){fail(error,error_size,"Unable to triangulate the affected face");goto failed;}
            }
            remap[s][f]=counts[s];if(pieces>SECTOR_MAX_FACES-counts[s]){fail(error,error_size,"Too many sector faces");goto failed;}counts[s]+=pieces;
        }
        replacement[s].faces=calloc(counts[s],sizeof(*replacement[s].faces));if(counts[s]&&!replacement[s].faces)goto memory;
        replacement[s].num_faces=counts[s];
        replacement[s].prop_type=sector->prop_type;
        replacement[s].prop_no_collision=sector->prop_no_collision;
        memcpy(replacement[s].prop_velocity,sector->prop_velocity,sizeof(replacement[s].prop_velocity));
        size_t next_group=0;
        for(size_t f=0;f<sector->num_faces;++f)if(sector->faces[f].source_group>next_group)next_group=sector->faces[f].source_group;
        for(size_t f=0;f<sector->num_faces;++f){
            const editor_sector_face_t *face=&sector->faces[f];size_t first=remap[s][f],pieces=(f+1<sector->num_faces?remap[s][f+1]:counts[s])-first;
            size_t group=face->source_group;
            if(pieces>1&&!group){if(next_group==SIZE_MAX){fail(error,error_size,"Too many face groups");goto failed;}group=++next_group;}
            const editor_sector_face_t *basis=face;
            bool reverse=false;
            if(pieces>1&&face->portal_sector!=EDITOR_SECTOR_NO_PORTAL&&face->portal_sector<s){
                basis=&map->sectors[face->portal_sector].faces[face->portal_face];reverse=true;
            }
            size_t tris[3*(SECTOR_MAX_FACE_VERTICES-2)];if(pieces>1&&triangulate(map,basis,tris)!=pieces)goto failed;
            for(size_t part=0;part<pieces;++part){editor_sector_face_t *dst=&replacement[s].faces[first+part];
                *dst=*face;dst->vertices=NULL;dst->num_vertices=pieces>1?3:face->num_vertices;
                dst->source_group=group;
                dst->vertices=malloc(dst->num_vertices*sizeof(*dst->vertices));if(!dst->vertices)goto memory;
                if(pieces==1)memcpy(dst->vertices,face->vertices,dst->num_vertices*sizeof(*dst->vertices));
                else for(size_t v=0;v<3;++v){
                    size_t basis_index=basis->vertices[tris[part*3+(reverse&&v?3-v:v)]];
                    if(basis==face)dst->vertices[v]=basis_index;
                    else {
                        size_t match=0;while(match<face->num_vertices&&!same_point(map->vertices[face->vertices[match]].position,map->vertices[basis_index].position))++match;
                        if(match==face->num_vertices){fail(error,error_size,"Portal triangulation does not match");goto failed;}
                        dst->vertices[v]=face->vertices[match];
                    }
                }
            }
        }
    }
    for(size_t s=0;s<map->num_sectors;++s)for(size_t f=0;f<map->sectors[s].num_faces;++f){
        const editor_sector_face_t *old=&map->sectors[s].faces[f];if(old->portal_sector==EDITOR_SECTOR_NO_PORTAL)continue;
        size_t first=remap[s][f],parts=(f+1<map->sectors[s].num_faces?remap[s][f+1]:counts[s])-first;
        size_t os=old->portal_sector,of=old->portal_face,otherfirst=remap[os][of];
        size_t otherparts=(of+1<map->sectors[os].num_faces?remap[os][of+1]:counts[os])-otherfirst;
        if(parts!=otherparts){fail(error,error_size,"Portal triangulation does not match");goto failed;}
        for(size_t part=0;part<parts;++part)replacement[s].faces[first+part].portal_face=otherfirst+part;
    }
    for(size_t s=0;s<map->num_sectors;++s){for(size_t f=0;f<map->sectors[s].num_faces;++f)free(map->sectors[s].faces[f].vertices);
        free(map->sectors[s].faces);map->sectors[s]=replacement[s];replacement[s].faces=NULL;replacement[s].num_faces=0;}
    for(size_t s=0;s<map->num_sectors;++s)free(remap[s]);
    free(remap);free(counts);free(replacement);return true;
memory:fail(error,error_size,"Out of memory triangulating faces");
failed:
    if(replacement)for(size_t s=0;s<map->num_sectors;++s){for(size_t f=0;f<replacement[s].num_faces;++f)free(replacement[s].faces[f].vertices);free(replacement[s].faces);}
    if(remap)for(size_t s=0;s<map->num_sectors;++s)free(remap[s]);
    free(remap);free(counts);free(replacement);return false;
}
static bool move_marked(editor_sector_map_t *map,bool *marked,const double delta[3],char *error,size_t error_size)
{
    for(int k=0;k<3;++k)if(!isfinite(delta[k]))return fail(error,error_size,"Movement must be finite");
    editor_sector_vertex_t *old=malloc(map->num_vertices*sizeof(*old));if(map->num_vertices&&!old)return fail(error,error_size,"Out of memory moving vertices");
    memcpy(old,map->vertices,map->num_vertices*sizeof(*old));extend_portal_vertices(map,marked);
    for(size_t i=0;i<map->num_vertices;++i)if(marked[i])for(int k=0;k<3;++k)map->vertices[i].position[k]+=delta[k];
    if(!EditorSector_Validate(map,error,error_size)){memcpy(map->vertices,old,map->num_vertices*sizeof(*old));free(old);return false;}
    free(old);return true;
}
/* Distinct sectors may meet at a point without sharing vertex indices. Treat
 * their coincident, in-use vertices as one handle during explicit vertex
 * edits. Exact coordinates avoid pulling merely nearby geometry along, and
 * unused vertices left by older edits are not modified. */
static size_t prop_vertex_owner(const editor_sector_map_t *map,size_t vertex)
{
    for(size_t s=0;s<map->num_sectors;++s)if(map->sectors[s].prop_type!=EDITOR_SECTOR_WORLD)
        for(size_t f=0;f<map->sectors[s].num_faces;++f)for(size_t i=0;i<map->sectors[s].faces[f].num_vertices;++i)
            if(map->sectors[s].faces[f].vertices[i]==vertex)return s;
    return SIZE_MAX;
}
bool EditorSector_VerticesEditTogether(const editor_sector_map_t *map,size_t sector_a,size_t vertex_a,size_t sector_b,size_t vertex_b)
{
    if(!map||sector_a>=map->num_sectors||sector_b>=map->num_sectors||vertex_a>=map->num_vertices||vertex_b>=map->num_vertices)return false;
    if(sector_a!=sector_b&&(map->sectors[sector_a].prop_type!=EDITOR_SECTOR_WORLD||map->sectors[sector_b].prop_type!=EDITOR_SECTOR_WORLD))return false;
    return same_point(map->vertices[vertex_a].position,map->vertices[vertex_b].position);
}
static void mark_coincident_vertices(const editor_sector_map_t *map,size_t vertex,bool *marked)
{
    const double *position=map->vertices[vertex].position;
    size_t prop=prop_vertex_owner(map,vertex);
    marked[vertex]=true;
    for(size_t s=0;s<map->num_sectors;++s){
        if(prop!=SIZE_MAX?s!=prop:map->sectors[s].prop_type!=EDITOR_SECTOR_WORLD)continue;
        for(size_t f=0;f<map->sectors[s].num_faces;++f){
        const editor_sector_face_t *face=&map->sectors[s].faces[f];
        for(size_t i=0;i<face->num_vertices;++i){size_t id=face->vertices[i];
            if(!marked[id]&&same_point(map->vertices[id].position,position))marked[id]=true;}
    }}
}
bool EditorSector_SetVertex(editor_sector_map_t *map,size_t vertex,const double p[3],char *error,size_t error_size)
{
    if(!p||!EditorSector_Validate(map,error,error_size))return false;
    if(vertex>=map->num_vertices)return fail(error,error_size,"Invalid vertex");
    if(same_point(map->vertices[vertex].position,p))return true;
    bool *marked=calloc(map->num_vertices,sizeof(*marked));if(!marked)return fail(error,error_size,"Out of memory moving vertex");
    mark_coincident_vertices(map,vertex,marked);extend_portal_vertices(map,marked);
    double delta[3];for(int k=0;k<3;++k)delta[k]=p[k]-map->vertices[vertex].position[k];
    editor_sector_map_t next={0};
    if(!EditorSector_Clone(&next,map)){free(marked);return fail(error,error_size,"Out of memory copying sector map");}
    bool ok=triangulate_marked_faces(&next,marked,error,error_size)&&move_marked(&next,marked,delta,error,error_size);
    free(marked);
    if(!ok){EditorSector_Free(&next);return false;}
    EditorSector_Free(map);*map=next;return true;
}
/* The edge validator alone permits a pinched vertex with two disconnected
 * surface fans. A weld must not create that non-manifold topology. */
static bool merge_vertex_is_manifold(const editor_sector_map_t *map,size_t vertex,char *error,size_t error_size)
{
    for(size_t s=0;s<map->num_sectors;++s){const editor_sector_t *sec=&map->sectors[s];
        bool *incident=calloc(sec->num_faces,sizeof(*incident)),*seen=calloc(sec->num_faces,sizeof(*seen));
        if(sec->num_faces&&(!incident||!seen)){free(incident);free(seen);return fail(error,error_size,"Out of memory validating merge");}
        size_t first=SIZE_MAX,total=0;
        for(size_t f=0;f<sec->num_faces;++f)for(size_t i=0;i<sec->faces[f].num_vertices;++i)
            if(sec->faces[f].vertices[i]==vertex){incident[f]=true;first=f;++total;break;}
        if(total){seen[first]=true;bool changed=true;
            while(changed){changed=false;for(size_t a=0;a<sec->num_faces;++a)if(seen[a]){
                const editor_sector_face_t *fa=&sec->faces[a];
                for(size_t i=0;i<fa->num_vertices;++i)if(fa->vertices[i]==vertex){
                    size_t neighbors[2]={fa->vertices[(i+fa->num_vertices-1)%fa->num_vertices],fa->vertices[(i+1)%fa->num_vertices]};
                    for(size_t b=0;b<sec->num_faces;++b)if(incident[b]&&!seen[b]){
                        const editor_sector_face_t *fb=&sec->faces[b];
                        for(size_t j=0;j<fb->num_vertices;++j)if(fb->vertices[j]==vertex&&
                            (fb->vertices[(j+fb->num_vertices-1)%fb->num_vertices]==neighbors[1]||
                             fb->vertices[(j+1)%fb->num_vertices]==neighbors[0])){seen[b]=true;changed=true;break;}
                    }
                }
            }}
            for(size_t f=0;f<sec->num_faces;++f)if(incident[f]&&!seen[f]){free(incident);free(seen);return fail(error,error_size,"Merge would create a non-manifold vertex");}
        }
        free(incident);free(seen);
    }
    return true;
}
/* Weld explicitly rather than on ordinary grid snaps. All edits are made to a
 * clone so an invalid shell or mismatched portal never changes the document. */
static bool merge_vertices(editor_sector_map_t *map,size_t source,size_t target,bool all_coincident,char *error,size_t error_size)
{
    if(!map||!EditorSector_Validate(map,error,error_size))return false;
    if(source>=map->num_vertices||target>=map->num_vertices)return fail(error,error_size,"Invalid merge vertex");
    size_t source_prop=prop_vertex_owner(map,source),target_prop=prop_vertex_owner(map,target);
    if(source_prop!=target_prop)return fail(error,error_size,"Cannot weld a prop to another sector");
    if(source==target)return true;
    bool *marked=calloc(map->num_vertices,sizeof(*marked));
    editor_sector_map_t next={0};size_t **remap=NULL;size_t remap_count=0;bool ok=false;
    if(!marked)return fail(error,error_size,"Out of memory merging vertices");
    if(all_coincident)mark_coincident_vertices(map,source,marked);else marked[source]=true;
    extend_portal_vertices(map,marked);
    bool moved=!same_point(map->vertices[source].position,map->vertices[target].position);
    if(!EditorSector_Clone(&next,map)){fail(error,error_size,"Out of memory merging vertices");goto done;}
    if(moved&&!triangulate_marked_faces(&next,marked,error,error_size))goto done;
    /* Replacing indices moves the marked portal-connected vertices exactly to
     * the destination and gives subsequent edits a single shared identity. */
    remap_count=next.num_sectors;remap=calloc(remap_count,sizeof(*remap));if(remap_count&&!remap){fail(error,error_size,"Out of memory merging vertices");goto done;}
    for(size_t s=0;s<next.num_sectors;++s){
        editor_sector_t *sec=&next.sectors[s];remap[s]=malloc(sec->num_faces*sizeof(**remap));
        if(sec->num_faces&&!remap[s]){fail(error,error_size,"Out of memory merging vertices");goto done;}
        size_t kept=0;
        for(size_t f=0;f<sec->num_faces;++f){
            editor_sector_face_t *face=&sec->faces[f];size_t n=0;
            for(size_t v=0;v<face->num_vertices;++v){size_t id=face->vertices[v];if(marked[id])id=target;
                if(!n||face->vertices[n-1]!=id)face->vertices[n++]=id;}
            if(n>1&&face->vertices[0]==face->vertices[n-1])--n;
            face->num_vertices=n;remap[s][f]=n<3?SIZE_MAX:kept++;
        }
    }
    for(size_t s=0;s<next.num_sectors;++s){editor_sector_t *sec=&next.sectors[s];
        for(size_t f=0;f<sec->num_faces;++f){editor_sector_face_t *face=&sec->faces[f];
            if(face->portal_sector==EDITOR_SECTOR_NO_PORTAL)continue;
            size_t other=remap[face->portal_sector][face->portal_face];
            if((remap[s][f]==SIZE_MAX)!=(other==SIZE_MAX)){fail(error,error_size,"Merge would break a portal");goto done;}
            if(other!=SIZE_MAX)face->portal_face=other;
        }
    }
    for(size_t s=0;s<next.num_sectors;++s){editor_sector_t *sec=&next.sectors[s];size_t kept=0;
        for(size_t f=0;f<sec->num_faces;++f){if(remap[s][f]==SIZE_MAX)free(sec->faces[f].vertices);
            else {if(kept!=f)sec->faces[kept]=sec->faces[f];++kept;}}
        sec->num_faces=kept;
    }
    if(!EditorSector_Validate(&next,error,error_size)||!merge_vertex_is_manifold(&next,target,error,error_size))goto done;
    EditorSector_Free(map);*map=next;memset(&next,0,sizeof(next));ok=true;
done:
    if(remap){for(size_t s=0;s<remap_count;++s)free(remap[s]);free(remap);}
    EditorSector_Free(&next);free(marked);return ok;
}
bool EditorSector_MergeVertices(editor_sector_map_t *map,size_t source,size_t target,char *error,size_t error_size)
{ return merge_vertices(map,source,target,true,error,error_size); }
/* Portal cleanup deliberately remains scoped to linked vertices; unrelated
 * coincident rooms should not have their topology changed by cleanup. */
bool EditorSector_MergePortalVertices(editor_sector_map_t *map,size_t source,size_t target,char *error,size_t error_size)
{ return merge_vertices(map,source,target,false,error,error_size); }
bool EditorSector_MoveFace(editor_sector_map_t *map,size_t sector,size_t face,const double delta[3],char *error,size_t error_size)
{
    if(!delta||!EditorSector_Validate(map,error,error_size))return false;
    if(sector>=map->num_sectors||face>=map->sectors[sector].num_faces)return fail(error,error_size,"Invalid face");
    bool *marked=calloc(map->num_vertices,sizeof(*marked));if(!marked)return fail(error,error_size,"Out of memory moving face");
    editor_sector_face_t *f=&map->sectors[sector].faces[face];for(size_t i=0;i<f->num_vertices;++i)marked[f->vertices[i]]=true;
    bool ok=move_marked(map,marked,delta,error,error_size);free(marked);return ok;
}
bool EditorSector_Translate(editor_sector_map_t *map,size_t sector,const double delta[3],char *error,size_t error_size)
{
    if(!delta||!EditorSector_Validate(map,error,error_size))return false;
    if(sector>=map->num_sectors)return fail(error,error_size,"Invalid sector");
    bool *marked=calloc(map->num_vertices,sizeof(*marked));if(!marked)return fail(error,error_size,"Out of memory moving sector");
    editor_sector_t *s=&map->sectors[sector];for(size_t f=0;f<s->num_faces;++f)for(size_t i=0;i<s->faces[f].num_vertices;++i)marked[s->faces[f].vertices[i]]=true;
    bool ok=move_marked(map,marked,delta,error,error_size);free(marked);return ok;
}

typedef struct { char *data; size_t length, capacity; } sector_buffer_t;
static bool append_bytes(sector_buffer_t *b,const char *data,size_t length)
{
    if(length>SIZE_MAX-b->length-1)return false;
    size_t need=b->length+length+1;
    if(need>b->capacity){size_t cap=b->capacity?b->capacity:256;while(cap<need){if(cap>SIZE_MAX/2){cap=need;break;}cap*=2;}void *p=realloc(b->data,cap);if(!p)return false;b->data=p;b->capacity=cap;}
    memcpy(b->data+b->length,data,length);b->length+=length;b->data[b->length]=0;return true;
}
static bool appendf(sector_buffer_t *b,const char *fmt,...)
{
    va_list ap,copy;va_start(ap,fmt);va_copy(copy,ap);int n=vsnprintf(NULL,0,fmt,copy);va_end(copy);
    if(n<0){va_end(ap);return false;}char *line=malloc((size_t)n+1);if(!line){va_end(ap);return false;}
    vsnprintf(line,(size_t)n+1,fmt,ap);va_end(ap);bool ok=append_bytes(b,line,(size_t)n);free(line);return ok;
}
bool EditorSector_IsFormat(const char *text,size_t length) { return text&&length>=10&&!memcmp(text,"QSECTOR 1\n",10); }
bool EditorSector_Serialize(const editor_sector_map_t *map,char **out,size_t *length)
{
    if(!map||!out||!length||!EditorSector_Validate(map,NULL,0))return false;
    sector_buffer_t b={0};char *entities=NULL;size_t entity_len=0;
    if(!EditorMap_Serialize(&map->entities,&entities,&entity_len)||!appendf(&b,"QSECTOR 1\nVERTICES %zu\n",map->num_vertices))goto bad;
    for(size_t i=0;i<map->num_vertices;++i){const double *p=map->vertices[i].position;if(!appendf(&b,"%.17g %.17g %.17g\n",p[0],p[1],p[2]))goto bad;}
    if(!appendf(&b,"SECTORS %zu\n",map->num_sectors))goto bad;
    for(size_t s=0;s<map->num_sectors;++s){const editor_sector_t *sector=&map->sectors[s];if(!appendf(&b,"SECTOR %zu\n",sector->num_faces))goto bad;
        for(size_t f=0;f<sector->num_faces;++f){const editor_sector_face_t *face=&sector->faces[f];
            if(!appendf(&b,"FACE %zu ",face->num_vertices))goto bad;
            if(face->portal_sector==EDITOR_SECTOR_NO_PORTAL){if(!appendf(&b,"-1 -1 "))goto bad;}
            else if(!appendf(&b,"%zu %zu ",face->portal_sector,face->portal_face))goto bad;
            if(!appendf(&b,"%s",face->material))goto bad;
            for(size_t i=0;i<face->num_vertices;++i)if(!appendf(&b," %zu",face->vertices[i]))goto bad;
            if(!appendf(&b,"\n"))goto bad;
        }
    }
    if(!appendf(&b,"FACE_GROUPS\n"))goto bad;
    for(size_t s=0;s<map->num_sectors;++s){for(size_t f=0;f<map->sectors[s].num_faces;++f){
        if(!appendf(&b,"%zu ",map->sectors[s].faces[f].source_group))goto bad;}
        if(!appendf(&b,"\n"))goto bad;}
    bool has_props=false;
    for(size_t s=0;s<map->num_sectors;++s)if(map->sectors[s].prop_type!=EDITOR_SECTOR_WORLD||
        map->sectors[s].prop_velocity[0]!=0||map->sectors[s].prop_velocity[1]!=0||map->sectors[s].prop_velocity[2]!=0){has_props=true;break;}
    if(has_props){if(!appendf(&b,"SECTOR_PROPS\n"))goto bad;
        for(size_t s=0;s<map->num_sectors;++s){const editor_sector_t *sec=&map->sectors[s];
            if(!appendf(&b,"%d %.17g %.17g %.17g\n",(int)sec->prop_type,sec->prop_velocity[0],sec->prop_velocity[1],sec->prop_velocity[2]))goto bad;}}
    bool has_no_collision=false;
    for(size_t s=0;s<map->num_sectors;++s)if(map->sectors[s].prop_no_collision){has_no_collision=true;break;}
    if(has_no_collision){if(!appendf(&b,"SECTOR_COLLISION\n"))goto bad;
        for(size_t s=0;s<map->num_sectors;++s)if(!appendf(&b,"%d\n",map->sectors[s].prop_no_collision?0:1))goto bad;}
    if(!appendf(&b,"ENTITIES %zu\n",entity_len)||!append_bytes(&b,entities,entity_len))goto bad;
    free(entities);*out=b.data;*length=b.length;return true;
bad:free(entities);free(b.data);return false;
}
typedef struct { const char *next,*end; } sector_reader_t;
static bool token(sector_reader_t *r,char *out,size_t capacity)
{
    while(r->next<r->end&&isspace((unsigned char)*r->next))++r->next;
    const char *start=r->next;while(r->next<r->end&&!isspace((unsigned char)*r->next)){if(!*r->next)return false;++r->next;}
    size_t n=(size_t)(r->next-start);if(!n||n>=capacity)return false;memcpy(out,start,n);out[n]=0;return true;
}
static bool tok_eq(sector_reader_t *r,const char *expected){char t[64];return token(r,t,sizeof(t))&&!strcmp(t,expected);}
static bool tok_size(sector_reader_t *r,size_t *out,bool sentinel)
{
    char t[64],*end;if(!token(r,t,sizeof(t)))return false;
    if(sentinel&&!strcmp(t,"-1")){*out=EDITOR_SECTOR_NO_PORTAL;return true;}
    if(!isdigit((unsigned char)*t))return false;
    errno=0;unsigned long long n=strtoull(t,&end,10);
    if(errno||*end||n>SIZE_MAX)return false;
    *out=(size_t)n;return true;
}
static bool tok_double(sector_reader_t *r,double *out)
{
    char t[128],*end;if(!token(r,t,sizeof(t)))return false;errno=0;double x=strtod(t,&end);if(errno||*end||!isfinite(x))return false;*out=x;return true;
}
/* Legacy sources did not retain the authored face after triangulation. Only
 * recover a mutually unique triangle pair forming a strictly convex quad;
 * ambiguous meshes remain separate rather than opening unrelated surfaces. */
static bool legacy_quad_pair(const editor_sector_map_t *map,const editor_sector_face_t *a,const editor_sector_face_t *b)
{
    if(a->num_vertices!=3||b->num_vertices!=3||a->portal_sector!=EDITOR_SECTOR_NO_PORTAL||
       b->portal_sector!=EDITOR_SECTOR_NO_PORTAL||strcmp(a->material,b->material))return false;
    double an[3],bn[3];if(!face_normal(map,a,an)||!face_normal(map,b,bn)||dot3(an,bn)<0.999999)return false;
    for(size_t i=0;i<3;++i){double d[3];sub3(map->vertices[b->vertices[i]].position,map->vertices[a->vertices[0]].position,d);
        if(fabs(dot3(d,an))>1e-6*(1.0+sqrt(dot3(d,d))))return false;}
    for(size_t i=0;i<3;++i)for(size_t j=0;j<3;++j){
        size_t ai=a->vertices[i],aj=a->vertices[(i+1)%3],bi=b->vertices[j],bj=b->vertices[(j+1)%3];
        if(ai!=bj||aj!=bi)continue;
        size_t v[4]={aj,a->vertices[(i+2)%3],ai,b->vertices[(j+2)%3]};
        for(size_t k=0;k<4;++k){double x[3],y[3],n[3];
            sub3(map->vertices[v[(k+1)%4]].position,map->vertices[v[k]].position,x);
            sub3(map->vertices[v[(k+2)%4]].position,map->vertices[v[(k+1)%4]].position,y);cross3(x,y,n);
            if(dot3(n,an)<=1e-8)return false;
        }
        return true;
    }
    return false;
}
static bool recover_legacy_groups(editor_sector_map_t *map)
{
    for(size_t s=0;s<map->num_sectors;++s){editor_sector_t *sec=&map->sectors[s];
        size_t *partner=malloc(sec->num_faces*sizeof(*partner)),*matches=calloc(sec->num_faces,sizeof(*matches));
        if(sec->num_faces&&(!partner||!matches)){free(partner);free(matches);return false;}
        for(size_t i=0;i<sec->num_faces;++i)for(size_t j=i+1;j<sec->num_faces;++j)
            if(legacy_quad_pair(map,&sec->faces[i],&sec->faces[j])){partner[i]=j;partner[j]=i;++matches[i];++matches[j];}
        size_t group=0;
        for(size_t i=0;i<sec->num_faces;++i)if(matches[i]==1){size_t j=partner[i];
            if(j>i&&matches[j]==1&&partner[j]==i)sec->faces[i].source_group=sec->faces[j].source_group=++group;}
        free(partner);free(matches);
    }
    return true;
}
bool EditorSector_Parse(editor_sector_map_t *map,const char *text,size_t length,char *error,size_t error_size)
{
    if(!map||!EditorSector_IsFormat(text,length))return fail(error,error_size,"Not a QSECTOR version 1 document");
    editor_sector_map_t next={0};sector_reader_t r={text+10,text+length};size_t count=0;
    if(!tok_eq(&r,"VERTICES")||!tok_size(&r,&count,false)||count>SECTOR_MAX_VERTICES)goto malformed;
    if(count){next.vertices=calloc(count,sizeof(*next.vertices));if(!next.vertices)goto memory;}next.num_vertices=count;
    for(size_t i=0;i<count;++i)for(int k=0;k<3;++k)if(!tok_double(&r,&next.vertices[i].position[k]))goto malformed;
    if(!tok_eq(&r,"SECTORS")||!tok_size(&r,&count,false)||count>SECTOR_MAX_SECTORS)goto malformed;
    if(count){next.sectors=calloc(count,sizeof(*next.sectors));if(!next.sectors)goto memory;}next.num_sectors=count;
    for(size_t s=0;s<count;++s){size_t faces;if(!tok_eq(&r,"SECTOR")||!tok_size(&r,&faces,false)||faces>SECTOR_MAX_FACES)goto malformed;
        editor_sector_t *sector=&next.sectors[s];if(faces){sector->faces=calloc(faces,sizeof(*sector->faces));if(!sector->faces)goto memory;}sector->num_faces=faces;
        for(size_t f=0;f<faces;++f){editor_sector_face_t *face=&sector->faces[f];size_t n;
            if(!tok_eq(&r,"FACE")||!tok_size(&r,&n,false)||n>SECTOR_MAX_FACE_VERTICES||!tok_size(&r,&face->portal_sector,true)||
                !tok_size(&r,&face->portal_face,true)||!token(&r,face->material,sizeof(face->material)))goto malformed;
            if(n){face->vertices=calloc(n,sizeof(*face->vertices));if(!face->vertices)goto memory;}face->num_vertices=n;
            for(size_t i=0;i<n;++i)if(!tok_size(&r,&face->vertices[i],false))goto malformed;
        }
    }
    char section[64];if(!token(&r,section,sizeof(section)))goto malformed;
    bool has_groups=!strcmp(section,"FACE_GROUPS");
    if(has_groups){
        for(size_t s=0;s<next.num_sectors;++s)for(size_t f=0;f<next.sectors[s].num_faces;++f)
            if(!tok_size(&r,&next.sectors[s].faces[f].source_group,false))goto malformed;
        if(!token(&r,section,sizeof(section)))goto malformed;
    }
    if(!strcmp(section,"SECTOR_PROPS")){
        for(size_t s=0;s<next.num_sectors;++s){size_t type;
            if(!tok_size(&r,&type,false)||type>EDITOR_SECTOR_PROP_MOVABLE)goto malformed;
            next.sectors[s].prop_type=(editor_sector_prop_type_t)type;
            for(int k=0;k<3;++k)if(!tok_double(&r,&next.sectors[s].prop_velocity[k]))goto malformed;
        }
        if(!token(&r,section,sizeof(section)))goto malformed;
    }
    if(!strcmp(section,"SECTOR_COLLISION")){
        for(size_t s=0;s<next.num_sectors;++s){size_t enabled;
            if(!tok_size(&r,&enabled,false)||enabled>1)goto malformed;
            next.sectors[s].prop_no_collision=!enabled;
        }
        if(!token(&r,section,sizeof(section)))goto malformed;
    }
    size_t n;if(strcmp(section,"ENTITIES")||!tok_size(&r,&n,false))goto malformed;
    if(r.next<r.end&&*r.next=='\r')++r.next;
    if(r.next>=r.end||*r.next++!='\n'||n!=(size_t)(r.end-r.next))goto malformed;
    if(!EditorMap_Parse(&next.entities,r.next,n,error,error_size)){EditorSector_Free(&next);return false;}
    if(!EditorSector_Validate(&next,error,error_size)){EditorSector_Free(&next);return false;}
    if(!has_groups&&!recover_legacy_groups(&next))goto memory;
    EditorSector_Free(map);*map=next;return true;
malformed:EditorSector_Free(&next);return fail(error,error_size,"Malformed QSECTOR version 1 document");
memory:EditorSector_Free(&next);return fail(error,error_size,"Out of memory reading sector document");
}

static void free_brush(editor_map_brush_t *b)
{
    for(size_t i=0;i<b->num_faces;++i)free(b->faces[i].mapping);
    free(b->faces);memset(b,0,sizeof(*b));
}
static bool add_prism(editor_map_entity_t *world,const double a[3],const double b[3],const double c[3],const double normal[3],const char *material)
{
    double p[6][3];
    for(int k=0;k<3;++k){p[0][k]=a[k];p[1][k]=b[k];p[2][k]=c[k];for(int i=0;i<3;++i)p[i+3][k]=p[i][k]+normal[k]*SECTOR_SHELL_DEPTH;}
    static const int indices[5][3]={{0,1,2},{3,5,4},{0,3,1},{1,4,2},{2,5,0}};
    editor_map_brush_t brush={0};brush.faces=calloc(5,sizeof(*brush.faces));if(!brush.faces)return false;brush.num_faces=5;
    size_t len=strlen(material), n=len+sizeof(" 0 0 0 1 1");
    for(size_t f=0;f<5;++f){editor_map_face_t *face=&brush.faces[f];face->mapping=malloc(n);if(!face->mapping){free_brush(&brush);return false;}
        snprintf(face->mapping,n,"%s 0 0 0 1 1",material);for(int i=0;i<3;++i)memcpy(face->points[i],p[indices[f][i]],sizeof(p[0]));}
    double mins[3],maxs[3];if(!EditorMap_BrushBounds(&brush,mins,maxs)){free_brush(&brush);return false;}
    if(world->num_brushes==SIZE_MAX/sizeof(*world->brushes)){free_brush(&brush);return false;}
    void *grown=realloc(world->brushes,(world->num_brushes+1)*sizeof(*world->brushes));if(!grown){free_brush(&brush);return false;}
    world->brushes=grown;world->brushes[world->num_brushes++]=brush;return true;
}
#include "editor_sector_clip.h"
#define SECTOR_SEPARATOR_DEPTH 0.25

/* Partition the wall prism on the planes of intersecting sector boundaries.
 * Every resulting convex cell is wholly inside or outside each closed sector,
 * including concave sectors. Discard the interior cells. */
static bool add_clipped_prism(const editor_sector_map_t *map,const shell_mesh_t *meshes,editor_map_entity_t *world,
    const double a[3],const double b[3],const double c[3],const double normal[3],const char *material)
{
    double lo[3],hi[3];for(int k=0;k<3;++k){lo[k]=fmin(fmin(a[k],b[k]),c[k]);hi[k]=fmax(fmax(a[k],b[k]),c[k]);
        lo[k]=fmin(lo[k],lo[k]+normal[k]*SECTOR_SHELL_DEPTH);hi[k]=fmax(hi[k],hi[k]+normal[k]*SECTOR_SHELL_DEPTH);}
    bool *candidates=calloc(map->num_sectors,sizeof(*candidates)),*separator=calloc(map->num_sectors,sizeof(*separator));sector_clip_plane_t *planes=NULL;size_t nplanes=0;
    sector_clip_list_t list={0};size_t work=0;bool ok=false,split_any=false;
    if(map->num_sectors&&(!candidates||!separator))goto done;
    for(size_t s=0;s<map->num_sectors;++s){bool overlaps=true;
        if(map->sectors[s].prop_type!=EDITOR_SECTOR_WORLD)continue;
        for(int k=0;k<3;++k)if(hi[k]<=meshes[s].mins[k]+SECTOR_CLIP_EPS||lo[k]>=meshes[s].maxs[k]-SECTOR_CLIP_EPS)overlaps=false;
        if(!overlaps)continue;
        candidates[s]=true;
        for(size_t f=0;f<map->sectors[s].num_faces;++f){const editor_sector_face_t *face=&map->sectors[s].faces[f];sector_clip_plane_t plane;
            if(!face_normal(map,face,plane.n))goto done;
            plane.d=dot3(plane.n,map->vertices[face->vertices[0]].position);
            if(face->portal_sector==EDITOR_SECTOR_NO_PORTAL&&sector_clip_opposite_normals(plane.n,normal)&&fabs(plane.d+dot3(normal,a))<1e-7)separator[s]=true;
            if(!sector_clip_plane_add_if_crossing(&planes,&nplanes,&plane,lo,hi))goto done;
        }
    }
    for(size_t s=0;s<map->num_sectors;++s)if(separator[s]){
        sector_clip_plane_t plane;memcpy(plane.n,normal,sizeof(plane.n));plane.d=dot3(normal,a)+SECTOR_SEPARATOR_DEPTH;
        if(!sector_clip_plane_add_if_crossing(&planes,&nplanes,&plane,lo,hi))goto done;
        /* Polygon edges partition the coincident plane into portal and wall
         * regions, so a separator can never spread across an opening. */
        for(size_t f=0;f<map->sectors[s].num_faces;++f){const editor_sector_face_t *face=&map->sectors[s].faces[f];double fn[3];
            if(face->portal_sector!=EDITOR_SECTOR_NO_PORTAL||!face_normal(map,face,fn)||!sector_clip_opposite_normals(fn,normal)||
               fabs(dot3(fn,map->vertices[face->vertices[0]].position)+dot3(normal,a))>1e-7)continue;
            for(size_t v=0;v<face->num_vertices;++v){const double *p=map->vertices[face->vertices[v]].position,*q=map->vertices[face->vertices[(v+1)%face->num_vertices]].position;double edge[3];
                sub3(q,p,edge);cross3(normal,edge,plane.n);double length=sqrt(dot3(plane.n,plane.n));if(length<1e-10)goto done;
                for(int k=0;k<3;++k)plane.n[k]/=length;
                plane.d=dot3(plane.n,p);
                if(!sector_clip_plane_add_if_crossing(&planes,&nplanes,&plane,lo,hi))goto done;}
        }
    }
    sector_clip_poly_t prism={0};if(!sector_clip_prism(&prism,a,b,c,normal)||!sector_clip_append(&list,&prism)){sector_clip_free(&prism);goto done;}
    for(size_t p=0;p<nplanes;++p){size_t count=list.n;
        for(size_t i=0;i<count;++i){if(++work>SECTOR_CLIP_WORK_LIMIT)goto done;sector_clip_poly_t *cell=&list.p[i];double min=INFINITY,max=-INFINITY;
            for(size_t f=0;f<cell->n;++f)for(size_t v=0;v<cell->faces[f].n;++v){double d=dot3(planes[p].n,cell->faces[f].p[v])-planes[p].d;min=fmin(min,d);max=fmax(max,d);}
            if(min>=-SECTOR_CLIP_EPS||max<=SECTOR_CLIP_EPS)continue;
            sector_clip_poly_t plus={0},minus={0};if(!sector_clip_split(cell,&planes[p],&plus,&minus)){sector_clip_free(&plus);sector_clip_free(&minus);goto done;}
            sector_clip_free(cell);list.p[i]=plus;if(!sector_clip_append(&list,&minus)){sector_clip_free(&minus);goto done;}split_any=true;
        }
    }
    for(size_t i=0;i<list.n;++i){sector_clip_poly_t *cell=&list.p[i];double center[3]={0};size_t points=0;
        for(size_t f=0;f<cell->n;++f)for(size_t v=0;v<cell->faces[f].n;++v){for(int k=0;k<3;++k)center[k]+=cell->faces[f].p[v][k];++points;}
        if(!points)goto done;
        for(int k=0;k<3;++k)center[k]/=points;
        bool inside=false;for(size_t s=0;s<map->num_sectors;++s)if(candidates[s]){int where=point_in_shell(map,&meshes[s],center);if(!where)goto done;if(where>0){
            bool keep=false;double distance=dot3(normal,center)-dot3(normal,a);
            if(separator[s]&&distance>=-SECTOR_CLIP_EPS&&distance<SECTOR_SEPARATOR_DEPTH){double projected[3];for(int k=0;k<3;++k)projected[k]=center[k]-normal[k]*distance;
                for(size_t t=0;t<meshes[s].count;++t){const shell_triangle_t *triangle=&meshes[s].triangles[t];const editor_sector_face_t *face=&map->sectors[s].faces[triangle->face];
                    if(face->portal_sector==EDITOR_SECTOR_NO_PORTAL&&sector_clip_opposite_normals(normal,triangle->normal)&&point_on_triangle(map,triangle,projected,false)){keep=true;break;}}}
            if(!keep){inside=true;break;}
        }}
        if(!inside){if(!split_any){if(!add_prism(world,a,b,c,normal,material))goto done;}else if(!sector_clip_export(world,cell,material))goto done;}
    }
    ok=true;
done:free(candidates);free(separator);free(planes);sector_clip_list_free(&list);return ok;
}
#include "editor_sector_prop.h"
bool EditorSector_Compile(const editor_sector_map_t *map,editor_map_t *out,char *error,size_t error_size)
{
    if(!out||!map||out==&map->entities)return fail(error,error_size,"Invalid sector compile destination");
    if(!EditorSector_Validate(map,error,error_size))return false;
    /* Embedded QBSP constructs finite-size base windings. Leave room for
     * generated wall shells, collision hulls and headnode expansion. */
    for(size_t i=0;i<map->num_vertices;++i)for(int k=0;k<3;++k)
        if(fabs(map->vertices[i].position[k])>EDITOR_SECTOR_QBSP_COORD_LIMIT)
            return fail(error,error_size,"Vertex %zu exceeds the embedded QBSP winding range",i);
    editor_map_t next={0};if(!EditorMap_Clone(&next,&map->entities))return fail(error,error_size,"Out of memory copying entities");
    shell_mesh_t *meshes=calloc(map->num_sectors,sizeof(*meshes));if(map->num_sectors&&!meshes){EditorMap_Free(&next);return fail(error,error_size,"Out of memory preparing sector shells");}
    for(size_t s=0;s<map->num_sectors;++s)if(!shell_mesh_create(map,s,&meshes[s])){for(size_t i=0;i<map->num_sectors;++i)shell_mesh_free(&meshes[i]);free(meshes);EditorMap_Free(&next);return fail(error,error_size,"Could not prepare sector shell %zu",s);}
    for(size_t s=0;s<map->num_sectors;++s)if(map->sectors[s].prop_type==EDITOR_SECTOR_WORLD)for(size_t f=0;f<map->sectors[s].num_faces;++f){
        const editor_sector_face_t *face=&map->sectors[s].faces[f];
        if(face->portal_sector!=EDITOR_SECTOR_NO_PORTAL)continue;
        double normal[3];size_t tris[3*(SECTOR_MAX_FACE_VERTICES-2)];size_t count=triangulate(map,face,tris);
        if(!count||!face_normal(map,face,normal)){for(size_t i=0;i<map->num_sectors;++i)shell_mesh_free(&meshes[i]);free(meshes);EditorMap_Free(&next);return fail(error,error_size,"Unable to triangulate sector %zu face %zu",s,f);}
        for(size_t t=0;t<count;++t){const double *a=map->vertices[face->vertices[tris[3*t]]].position,*b=map->vertices[face->vertices[tris[3*t+1]]].position,*c=map->vertices[face->vertices[tris[3*t+2]]].position;
            if(!add_clipped_prism(map,meshes,&next.entities[0],a,b,c,normal,face->material)){for(size_t i=0;i<map->num_sectors;++i)shell_mesh_free(&meshes[i]);free(meshes);EditorMap_Free(&next);return fail(error,error_size,"Could not clip wall for sector %zu face %zu; geometry is too complex or numerically ambiguous",s,f);}}
    }
    for(size_t s=0;s<map->num_sectors;++s)if(map->sectors[s].prop_type!=EDITOR_SECTOR_WORLD)
        if(!sector_prop_export(map,s,&meshes[s],&next,error,error_size)){for(size_t i=0;i<map->num_sectors;++i)shell_mesh_free(&meshes[i]);free(meshes);EditorMap_Free(&next);return false;}
    for(size_t i=0;i<map->num_sectors;++i)shell_mesh_free(&meshes[i]);
    free(meshes);
    EditorMap_Free(out);*out=next;return true;
}
