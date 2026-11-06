/* Solid sector props: partition the sector's bounding box by its face
 * planes, keeping precisely the convex cells inside the closed shell.
 * Included after editor_sector_clip.h by editor_sector.c. */

static bool sector_prop_box(sector_clip_poly_t *box,const double lo[3],const double hi[3])
{
    for(int axis=0;axis<3;++axis)for(int side=0;side<2;++side){
        int u=(axis+1)%3,v=(axis+2)%3;double q[4][3];
        for(int i=0;i<4;++i){memcpy(q[i],lo,3*sizeof(double));q[i][axis]=side?hi[axis]:lo[axis];}
        q[1][side?v:u]=hi[side?v:u];q[2][u]=hi[u];q[2][v]=hi[v];q[3][side?u:v]=hi[side?u:v];
        if(!sector_clip_addface(box,(const double (*)[3])q,4))return false;
    }
    return true;
}

/* Add vertical planes through coplanar material boundaries. Without these,
 * a convex cell face could cover two different authored materials. */
static bool sector_prop_planes(const editor_sector_map_t *map,size_t sector,const shell_mesh_t *mesh,
    sector_clip_plane_t **planes,size_t *count)
{
    const editor_sector_t *sec=&map->sectors[sector];
    for(size_t f=0;f<sec->num_faces;++f){sector_clip_plane_t plane;
        if(!face_normal(map,&sec->faces[f],plane.n))return false;
        plane.d=dot3(plane.n,map->vertices[sec->faces[f].vertices[0]].position);
        if(!sector_clip_plane_add_if_crossing(planes,count,&plane,mesh->mins,mesh->maxs))return false;
    }
    bool differing=false;
    for(size_t f=1;f<sec->num_faces;++f)if(strcmp(sec->faces[0].material,sec->faces[f].material)){differing=true;break;}
    if(!differing)return true;
    size_t checks=0;
    for(size_t f=0;f<sec->num_faces;++f){
        const editor_sector_face_t *face=&sec->faces[f];double normal[3];if(!face_normal(map,face,normal))return false;
        const double *first=map->vertices[face->vertices[0]].position;double dist=dot3(normal,first);bool seam=false;
        for(size_t g=0;g<sec->num_faces&&!seam;++g){
            if(++checks>SECTOR_CLIP_WORK_LIMIT)return false;
            const editor_sector_face_t *other=&sec->faces[g];if(!strcmp(face->material,other->material))continue;
            double n[3];if(!face_normal(map,other,n))return false;
            double alignment=dot3(normal,n),d=dot3(n,map->vertices[other->vertices[0]].position);
            if((alignment>1-1e-10&&fabs(dist-d)<1e-6)||(alignment< -1+1e-10&&fabs(dist+d)<1e-6))seam=true;
        }
        if(!seam)continue;
        for(size_t i=0;i<face->num_vertices;++i){const double *a=map->vertices[face->vertices[i]].position;
            const double *b=map->vertices[face->vertices[(i+1)%face->num_vertices]].position;
            double edge[3];sub3(b,a,edge);sector_clip_plane_t plane;cross3(normal,edge,plane.n);
            double length=sqrt(dot3(plane.n,plane.n));if(length<1e-12)return false;
            for(int k=0;k<3;++k)plane.n[k]/=length;
            plane.d=dot3(plane.n,a);
            if(!sector_clip_plane_add_if_crossing(planes,count,&plane,mesh->mins,mesh->maxs))return false;
        }
    }
    return true;
}

static const char *sector_prop_material(const editor_sector_map_t *map,size_t sector,const shell_mesh_t *mesh,
    const sector_clip_face_t *face,const double inward[3])
{
    double center[3]={0,0,0};for(size_t v=0;v<face->n;++v)for(int k=0;k<3;++k)center[k]+=face->p[v][k]/face->n;
    for(size_t t=0;t<mesh->count;++t){const shell_triangle_t *triangle=&mesh->triangles[t];
        if(dot3(triangle->normal,inward)<-0.999999&&point_on_triangle(map,triangle,center,false))
            return map->sectors[sector].faces[triangle->face].material;
    }
    /* Internal cell boundaries are removed by QBSP's brush union. */
    return map->sectors[sector].faces[0].material;
}

static bool sector_prop_export_cell(const editor_sector_map_t *map,size_t sector,const shell_mesh_t *mesh,
    editor_map_entity_t *entity,const sector_clip_poly_t *cell,const double origin[3])
{
    if(cell->n<4||cell->n>64)return false;
    editor_map_brush_t brush={0};brush.faces=calloc(cell->n,sizeof(*brush.faces));if(!brush.faces)return false;brush.num_faces=cell->n;
    for(size_t f=0;f<cell->n;++f){const sector_clip_face_t *src=&cell->faces[f];editor_map_face_t *dst=&brush.faces[f];
        size_t j=2;double a[3],b[3],normal[3]={0,0,0};while(j<src->n){sub3(src->p[1],src->p[0],a);sub3(src->p[j],src->p[0],b);cross3(a,b,normal);
            if(dot3(normal,normal)>1e-16)break;
            ++j;}
        if(j==src->n){free_brush(&brush);return false;}
        double length=sqrt(dot3(normal,normal));for(int k=0;k<3;++k)normal[k]/=length;
        const char *material=sector_prop_material(map,sector,mesh,src,normal);size_t bytes=strlen(material)+sizeof(" 0 0 0 1 1");
        dst->mapping=malloc(bytes);if(!dst->mapping){free_brush(&brush);return false;}
        snprintf(dst->mapping,bytes,"%s 0 0 0 1 1",material);
        const size_t ids[3]={0,1,j};for(int i=0;i<3;++i)for(int k=0;k<3;++k)dst->points[i][k]=src->p[ids[i]][k]-origin[k];
    }
    double lo[3],hi[3];if(!EditorMap_BrushBounds(&brush,lo,hi)){free_brush(&brush);return false;}
    if(entity->num_brushes==SIZE_MAX/sizeof(*entity->brushes)){free_brush(&brush);return false;}
    void *grown=realloc(entity->brushes,(entity->num_brushes+1)*sizeof(*entity->brushes));if(!grown){free_brush(&brush);return false;}
    entity->brushes=grown;entity->brushes[entity->num_brushes++]=brush;return true;
}

static bool sector_prop_export(const editor_sector_map_t *map,size_t sector,const shell_mesh_t *mesh,
    editor_map_t *out,char *error,size_t error_size)
{
    const editor_sector_t *sec=&map->sectors[sector];const char *classname=
        sec->prop_type==EDITOR_SECTOR_PROP_STATIC?(sec->prop_no_collision?"func_detail_illusionary":"prop_static"):sec->prop_type==EDITOR_SECTOR_PROP_DYNAMIC?"prop_dynamic":
        sec->prop_type==EDITOR_SECTOR_PROP_MOVABLE?"prop_movable":NULL;
    if(!classname)return fail(error,error_size,"Invalid prop type on sector %zu",sector);
    /* Static props are world brushes: keep their points in world coordinates.
     * Moving props remain brush submodels with a local origin. */
    bool worldcoords=sec->prop_type==EDITOR_SECTOR_PROP_STATIC;
    bool bake=worldcoords&&!sec->prop_no_collision;
    double origin[3];for(int k=0;k<3;++k)origin[k]=worldcoords?0:(mesh->mins[k]+mesh->maxs[k])*0.5;
    editor_map_t tmp={0};size_t entity_index=0;sector_clip_plane_t *planes=NULL;size_t nplanes=0;
    sector_clip_list_t cells={0};sector_clip_poly_t box={0};bool ok=false;size_t work=0;
    char text[160];
    if(!EditorMap_AddEntity(&tmp,classname,&entity_index))goto done;
    if(!worldcoords){snprintf(text,sizeof(text),"%.17g %.17g %.17g",origin[0],origin[1],origin[2]);
        if(!EditorMap_SetProperty(&tmp.entities[entity_index],"origin",text))goto done;}
    if(worldcoords&&sec->prop_no_collision&&!EditorMap_SetProperty(&tmp.entities[entity_index],"_shadow","1"))goto done;
    if(sec->prop_type!=EDITOR_SECTOR_PROP_STATIC&&(sec->prop_velocity[0]||sec->prop_velocity[1]||sec->prop_velocity[2])){
        snprintf(text,sizeof(text),"%.17g %.17g %.17g",sec->prop_velocity[0],sec->prop_velocity[1],sec->prop_velocity[2]);
        if(!EditorMap_SetProperty(&tmp.entities[entity_index],"velocity",text))goto done;
    }
    if(!sector_prop_planes(map,sector,mesh,&planes,&nplanes)||!sector_prop_box(&box,mesh->mins,mesh->maxs)||!sector_clip_append(&cells,&box))goto done;
    for(size_t p=0;p<nplanes;++p){size_t count=cells.n;
        for(size_t i=0;i<count;++i){if(++work>SECTOR_CLIP_WORK_LIMIT)goto done;
            sector_clip_poly_t *cell=&cells.p[i];double lo=INFINITY,hi=-INFINITY;
            for(size_t f=0;f<cell->n;++f)for(size_t v=0;v<cell->faces[f].n;++v){double d=dot3(planes[p].n,cell->faces[f].p[v])-planes[p].d;lo=fmin(lo,d);hi=fmax(hi,d);}
            if(lo>=-SECTOR_CLIP_EPS||hi<=SECTOR_CLIP_EPS)continue;
            sector_clip_poly_t a={0},b={0};if(!sector_clip_split(cell,&planes[p],&a,&b)){sector_clip_free(&a);sector_clip_free(&b);goto done;}
            sector_clip_free(cell);cells.p[i]=a;if(!sector_clip_append(&cells,&b)){sector_clip_free(&b);goto done;}
        }
    }
    for(size_t i=0;i<cells.n;++i){sector_clip_poly_t *cell=&cells.p[i];double center[3]={0,0,0};size_t n=0;
        for(size_t f=0;f<cell->n;++f)for(size_t v=0;v<cell->faces[f].n;++v){for(int k=0;k<3;++k)center[k]+=cell->faces[f].p[v][k];++n;}
        if(!n)goto done;
        for(int k=0;k<3;++k)center[k]/=n;
        int inside=point_in_shell(map,mesh,center);if(!inside)goto done;
        if(inside>0&&!sector_prop_export_cell(map,sector,mesh,&tmp.entities[entity_index],cell,origin))goto done;
    }
    editor_map_entity_t *generated=&tmp.entities[entity_index];
    if(!generated->num_brushes)goto done;
    if(bake){
        if(!out->num_entities||!out->entities||generated->num_brushes>SIZE_MAX/sizeof(*generated->brushes)-out->entities[0].num_brushes)goto done;
        editor_map_entity_t *world=&out->entities[0];size_t total=world->num_brushes+generated->num_brushes;
        void *grown=realloc(world->brushes,total*sizeof(*world->brushes));if(!grown)goto done;
        world->brushes=grown;memcpy(&world->brushes[world->num_brushes],generated->brushes,generated->num_brushes*sizeof(*generated->brushes));
        world->num_brushes=total;free(generated->brushes);generated->brushes=NULL;generated->num_brushes=0;
    }else{
        if(out->num_entities==SIZE_MAX/sizeof(*out->entities))goto done;
        void *grown=realloc(out->entities,(out->num_entities+1)*sizeof(*out->entities));if(!grown)goto done;
        out->entities=grown;out->entities[out->num_entities++]=*generated;memset(generated,0,sizeof(*generated));
    }
    ok=true;
done:
    free(planes);sector_clip_free(&box);sector_clip_list_free(&cells);EditorMap_Free(&tmp);
    return ok?true:fail(error,error_size,"Could not compile sector prop %zu: geometry is too complex or numerically ambiguous",sector);
}
