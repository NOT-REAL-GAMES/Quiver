/* Convex clipping for generated sector wall prisms. Included by editor_sector.c. */
#define SECTOR_CLIP_LIMIT 512u
#define SECTOR_CLIP_WORK_LIMIT 100000u
#define SECTOR_CLIP_EPS 1e-7

typedef struct { double (*p)[3]; size_t n; } sector_clip_face_t;
typedef struct { sector_clip_face_t *faces; size_t n; } sector_clip_poly_t;
typedef struct { double n[3], d; } sector_clip_plane_t;
typedef struct { sector_clip_poly_t *p; size_t n; } sector_clip_list_t;
static bool sector_clip_opposite_normals(const double a[3],const double b[3]) {
    for(int k=0;k<3;++k)if(fabs(a[k]+b[k])>=1e-12)return false;
    return true;
}
static bool sector_clip_plane_add(sector_clip_plane_t **planes,size_t *count,const sector_clip_plane_t *plane) {
    for(size_t p=0;p<*count;++p){double same=0,opposite=0;for(int k=0;k<3;++k){same=fmax(same,fabs(plane->n[k]-(*planes)[p].n[k]));opposite=fmax(opposite,fabs(plane->n[k]+(*planes)[p].n[k]));}
        if((same<1e-12&&fabs(plane->d-(*planes)[p].d)<1e-7)||(opposite<1e-12&&fabs(plane->d+(*planes)[p].d)<1e-7))return true;}
    if(*count>=SECTOR_CLIP_LIMIT)return false;
    void *grown=realloc(*planes,(*count+1)*sizeof(**planes));if(!grown)return false;
    *planes=grown;(*planes)[(*count)++]=*plane;return true;
}
static bool sector_clip_plane_add_if_crossing(sector_clip_plane_t **planes,size_t *count,const sector_clip_plane_t *plane,const double lo[3],const double hi[3]) {
    double min=-plane->d,max=-plane->d;
    for(int k=0;k<3;++k){min+=plane->n[k]*(plane->n[k]>=0?lo[k]:hi[k]);max+=plane->n[k]*(plane->n[k]>=0?hi[k]:lo[k]);}
    if(min>=-SECTOR_CLIP_EPS||max<=SECTOR_CLIP_EPS)return true;
    return sector_clip_plane_add(planes,count,plane);
}
static void sector_clip_free(sector_clip_poly_t *p) {
    for(size_t i=0;i<p->n;++i)free(p->faces[i].p);
    free(p->faces);memset(p,0,sizeof(*p));
}
static void sector_clip_list_free(sector_clip_list_t *l) {
    for(size_t i=0;i<l->n;++i)sector_clip_free(&l->p[i]);
    free(l->p);memset(l,0,sizeof(*l));
}
static bool sector_clip_addface(sector_clip_poly_t *p,const double (*v)[3],size_t n) {
    if(n<3)return true;
    if(p->n>=256)return false;
    void *f=realloc(p->faces,(p->n+1)*sizeof(*p->faces));if(!f)return false;
    p->faces=f;p->faces[p->n].p=malloc(n*sizeof(*v));if(!p->faces[p->n].p)return false;
    memcpy(p->faces[p->n].p,v,n*sizeof(*v));p->faces[p->n++].n=n;return true;
}
static bool sector_clip_append(sector_clip_list_t *l,sector_clip_poly_t *p) {
    if(l->n>=SECTOR_CLIP_LIMIT)return false;
    void *q=realloc(l->p,(l->n+1)*sizeof(*l->p));if(!q)return false;
    l->p=q;l->p[l->n++]=*p;memset(p,0,sizeof(*p));return true;
}
static bool sector_clip_prism(sector_clip_poly_t *poly,const double a[3],const double b[3],const double c[3],const double normal[3]) {
    double p[6][3];for(int k=0;k<3;++k){p[0][k]=a[k];p[1][k]=b[k];p[2][k]=c[k];for(int i=0;i<3;++i)p[i+3][k]=p[i][k]+normal[k]*SECTOR_SHELL_DEPTH;}
    static const int idx[5][4]={{0,1,2,-1},{3,5,4,-1},{0,3,4,1},{1,4,5,2},{2,5,3,0}};
    for(int f=0;f<5;++f){double v[4][3];size_t n=idx[f][3]<0?3:4;for(size_t i=0;i<n;++i)memcpy(v[i],p[idx[f][i]],sizeof(v[i]));
        if(!sector_clip_addface(poly,(const double (*)[3])v,n))return false;}
    return true;
}
static bool sector_clip_near(const double a[3],const double b[3]) {
    double d[3];sub3(a,b,d);return dot3(d,d)<1e-14;
}
/* Clip a convex polyhedron to side * (dot(normal,p)-dist) >= 0. */
static bool sector_clip_side(const sector_clip_poly_t *src,const sector_clip_plane_t *plane,int side,sector_clip_poly_t *dst) {
    double (*cap)[3]=NULL;size_t caps=0;
    for(size_t f=0;f<src->n;++f){const sector_clip_face_t *face=&src->faces[f];size_t n=0;
        double (*v)[3]=malloc((face->n*2+2)*sizeof(*v));if(!v)goto bad;
        for(size_t i=0;i<face->n;++i){const double *a=face->p[i],*b=face->p[(i+1)%face->n];
            double da=side*(dot3(plane->n,a)-plane->d),db=side*(dot3(plane->n,b)-plane->d);
            if(da>=-SECTOR_CLIP_EPS){if(!n||!sector_clip_near(v[n-1],a))memcpy(v[n++],a,sizeof(*v));}
            if((da>SECTOR_CLIP_EPS&&db< -SECTOR_CLIP_EPS)||(da< -SECTOR_CLIP_EPS&&db>SECTOR_CLIP_EPS)){
                double t=da/(da-db),q[3];for(int k=0;k<3;++k)q[k]=a[k]+t*(b[k]-a[k]);
                if(!n||!sector_clip_near(v[n-1],q))memcpy(v[n++],q,sizeof(q));
                bool have=false;for(size_t j=0;j<caps;++j)if(sector_clip_near(cap[j],q)){have=true;break;}
                if(!have){void *grown=realloc(cap,(caps+1)*sizeof(*cap));if(!grown){free(v);goto bad;}cap=grown;memcpy(cap[caps++],q,sizeof(q));}
            } else if(fabs(da)<=SECTOR_CLIP_EPS) {
                bool have=false;for(size_t j=0;j<caps;++j)if(sector_clip_near(cap[j],a)){have=true;break;}
                if(!have){void *grown=realloc(cap,(caps+1)*sizeof(*cap));if(!grown){free(v);goto bad;}cap=grown;memcpy(cap[caps++],a,sizeof(*cap));}
            }
        }
        if(n>1&&sector_clip_near(v[0],v[n-1]))--n;
        if(!sector_clip_addface(dst,(const double (*)[3])v,n)){free(v);goto bad;}free(v);
    }
    if(caps<3)goto bad;
    double center[3]={0},u[3],v[3],axis[3]={0};for(size_t i=0;i<caps;++i)for(int k=0;k<3;++k)center[k]+=cap[i][k]/caps;
    int smallest=0;for(int k=1;k<3;++k)if(fabs(plane->n[k])<fabs(plane->n[smallest]))smallest=k;axis[smallest]=1;
    cross3(axis,plane->n,u);double len=sqrt(dot3(u,u));for(int k=0;k<3;++k)u[k]/=len;
    cross3(plane->n,u,v);for(int k=0;k<3;++k)v[k]*=side; /* inward cap normal is side * normal */
    for(size_t i=1;i<caps;++i){double item[3];memcpy(item,cap[i],sizeof(item));double q[3];sub3(item,center,q);double angle=atan2(dot3(q,v),dot3(q,u));size_t j=i;
        while(j){sub3(cap[j-1],center,q);if(atan2(dot3(q,v),dot3(q,u))<=angle)break;memcpy(cap[j],cap[j-1],sizeof(item));--j;}memcpy(cap[j],item,sizeof(item));}
    if(!sector_clip_addface(dst,(const double (*)[3])cap,caps))goto bad;
    free(cap);return true;
bad:free(cap);sector_clip_free(dst);return false;
}
static bool sector_clip_split(const sector_clip_poly_t *p,const sector_clip_plane_t *plane,sector_clip_poly_t *plus,sector_clip_poly_t *minus) {
    return sector_clip_side(p,plane,1,plus)&&sector_clip_side(p,plane,-1,minus);
}
static bool sector_clip_export(editor_map_entity_t *world,const sector_clip_poly_t *poly,const char *material) {
    editor_map_brush_t brush={0};brush.faces=calloc(poly->n,sizeof(*brush.faces));if(!brush.faces)return false;brush.num_faces=poly->n;
    size_t n=strlen(material)+sizeof(" 0 0 0 1 1");
    for(size_t f=0;f<poly->n;++f){const sector_clip_face_t *src=&poly->faces[f];editor_map_face_t *dst=&brush.faces[f];
        size_t i=1,j=2;double a[3],b[3],cross[3];while(j<src->n){sub3(src->p[i],src->p[0],a);sub3(src->p[j],src->p[0],b);cross3(a,b,cross);if(dot3(cross,cross)>1e-16)break;++j;}
        if(j==src->n){free_brush(&brush);return false;}
        memcpy(dst->points[0],src->p[0],sizeof(dst->points[0]));memcpy(dst->points[1],src->p[i],sizeof(dst->points[1]));memcpy(dst->points[2],src->p[j],sizeof(dst->points[2]));
        dst->mapping=malloc(n);if(!dst->mapping){free_brush(&brush);return false;}snprintf(dst->mapping,n,"%s 0 0 0 1 1",material);
    }
    double mins[3],maxs[3];if(!EditorMap_BrushBounds(&brush,mins,maxs)){free_brush(&brush);return false;}
    void *grown=realloc(world->brushes,(world->num_brushes+1)*sizeof(*world->brushes));if(!grown){free_brush(&brush);return false;}
    world->brushes=grown;world->brushes[world->num_brushes++]=brush;return true;
}
