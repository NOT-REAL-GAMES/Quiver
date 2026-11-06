#include "editor_sector_portal.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
/* A point is inside a valid convex brush when it lies strictly on the same
 * side of all its oriented boundary planes. Accept either winding. */
static int contains(const editor_map_brush_t *brush,const double p[3]) {
 int positive=1,negative=1;
 for(size_t i=0;i<brush->num_faces;i++){
  const editor_map_face_t *f=&brush->faces[i];double u[3],v[3],w[3],n[3];
  for(int j=0;j<3;j++){u[j]=f->points[1][j]-f->points[0][j];v[j]=f->points[2][j]-f->points[0][j];w[j]=p[j]-f->points[0][j];}
  n[0]=u[1]*v[2]-u[2]*v[1];n[1]=u[2]*v[0]-u[0]*v[2];n[2]=u[0]*v[1]-u[1]*v[0];
  double dot=n[0]*w[0]+n[1]*w[1]+n[2]*w[2],len=sqrt(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]);
  if(len<1e-12)return 0;
  positive &= dot>1e-6*len;negative &= dot < -1e-6*len;
 }
 return positive||negative;
}
#define DO(expr) do { if(!(expr)) { fprintf(stderr,"failed %s: %s\n",#expr,error); return 1; } } while(0)
static int concave_room(editor_sector_map_t *m,double offset) {
 const double xy[6][2]={{-256,-256},{256,-256},{256,256},{-128,256},{-128,128},{-256,128}};
 m->vertices=calloc(12,sizeof(*m->vertices));m->sectors=calloc(1,sizeof(*m->sectors));
 if(!m->vertices||!m->sectors)return 0;
 m->num_vertices=12;m->num_sectors=1;
 for(size_t z=0;z<2;++z)for(size_t i=0;i<6;++i){double *p=m->vertices[z*6+i].position;p[0]=xy[i][0]+offset;p[1]=xy[i][1];p[2]=256*z;}
 editor_sector_t *sector=&m->sectors[0];sector->num_faces=8;sector->faces=calloc(8,sizeof(*sector->faces));if(!sector->faces)return 0;
 for(size_t f=0;f<8;++f){editor_sector_face_t *face=&sector->faces[f];face->num_vertices=f<2?6:4;face->vertices=calloc(face->num_vertices,sizeof(*face->vertices));if(!face->vertices)return 0;
  strcpy(face->material,"stone1");face->portal_sector=face->portal_face=EDITOR_SECTOR_NO_PORTAL;}
 for(size_t i=0;i<6;++i){sector->faces[0].vertices[i]=5-i;sector->faces[1].vertices[i]=6+i;size_t *side=sector->faces[i+2].vertices;
  side[0]=i;side[1]=(i+1)%6;side[2]=6+(i+1)%6;side[3]=6+i;}
 return 1;
}
int main(int argc,char **argv) {
 char error[512]={0};editor_sector_map_t m={0};editor_map_t out={0};size_t entity;
 double a[]={-256,-256,0},b[]={256,256,256},c[]={-32,256,0},d[]={32,320,128};
 DO(argc>=3&&argc<=5);int concave=argc>=4&&!strcmp(argv[3],"concave");double offset=argc==5?strtod(argv[4],NULL):0;
 a[0]+=offset;b[0]+=offset;c[0]+=offset;d[0]+=offset;
 DO(EditorSector_Init(&m));if(concave){DO(concave_room(&m,offset));DO(EditorSector_Validate(&m,error,sizeof error));}else DO(EditorSector_AddBox(&m,a,b,"stone1",NULL,error,sizeof error));
 DO(EditorSector_AddBox(&m,c,d,"stone1",NULL,error,sizeof error));
 DO(EditorSector_LinkPartialPortal(&m,0,concave?4:3,1,2,error,sizeof error));
 double start[]={offset-32,256,64},end[]={offset-32,320,0};
 DO(EditorSector_CutFace(&m,1,0,start,end,error,sizeof error));
 size_t v=0;while(v<m.num_vertices && (fabs(m.vertices[v].position[0]-(offset-32))>1e-6||fabs(m.vertices[v].position[1]-256)>1e-6||fabs(m.vertices[v].position[2]-64)>1e-6))v++;
 DO(v<m.num_vertices);double target[]={offset-16,256,64};DO(EditorSector_SetVertex(&m,v,target,error,sizeof error));
 DO(EditorMap_SetProperty(&m.entities.entities[0],"wad",argv[2]));
 char origin[128];DO(EditorMap_AddEntity(&m.entities,"info_player_start",&entity));snprintf(origin,sizeof origin,"%.17g 0 32",offset);DO(EditorMap_SetProperty(&m.entities.entities[entity],"origin",origin));
 DO(EditorMap_AddEntity(&m.entities,"light",&entity));snprintf(origin,sizeof origin,"%.17g 0 192",offset);DO(EditorMap_SetProperty(&m.entities.entities[entity],"origin",origin));
 DO(EditorSector_Compile(&m,&out,error,sizeof error));
 /* Regression point is inside the big room, but the old 32-unit normal
  * extrusion from the sloped adjoining face put a solid prism around it. */
 const double intrusion[]={offset-37.565944975,252.21702751,87.7085137};
 for(size_t i=0;i<out.entities[0].num_brushes;i++)if(contains(&out.entities[0].brushes[i],intrusion)){
  fprintf(stderr,"generated brush %zu intrudes into main sector\n",i);return 1;
 }
 char *text=NULL;size_t len=0;DO(EditorMap_Serialize(&out,&text,&len));
 FILE *f=fopen(argv[1],"wb");DO(f);DO(fwrite(text,1,len,f)==len);fclose(f);free(text);EditorSector_Free(&m);EditorMap_Free(&out);return 0;
}
