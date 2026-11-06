#include "static_models.h"
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void near_value (float actual, float expected, float epsilon)
{
	if (!(fabsf (actual-expected) <= epsilon))
	{
		fprintf (stderr, "expected %.9g, got %.9g\n", expected, actual); abort ();
	}
}

static qsm_data_t make_data (uint32_t count)
{
	qsm_data_t data = {0}; data.instance_count = 1; data.triangle_count = count;
	data.instances = calloc (1, sizeof (*data.instances));
	data.triangles = calloc (count ? count : 1, sizeof (*data.triangles));
	assert (data.instances && data.triangles);
	data.instances[0].flags = QSM_COLLISION; data.instances[0].triangle_count = count;
	return data;
}

static void dispose (qsm_data_t *data)
{
	QSM_FreeCollision (data->collision); free (data->instances); free (data->triangles); memset (data, 0, sizeof (*data));
}

static void triangle (qsm_triangle_t *out, const float a[3], const float b[3], const float c[3])
{
	memcpy (out->position[0], a, 3*sizeof (float)); memcpy (out->position[1], b, 3*sizeof (float));
	memcpy (out->position[2], c, 3*sizeof (float));
}

static void floor_triangle (qsm_triangle_t *out, float height)
{
	float a[3] = {-100,-100,height}, b[3] = {100,-100,height}, c[3] = {0,100,height}; triangle (out,a,b,c);
}

static void build (qsm_data_t *data)
{
	char error[160]; if (!QSM_BuildCollision (data,error,sizeof (error))) { fprintf (stderr,"%s\n",error); abort (); }
}

static qsm_trace_t query (qsm_data_t *data, const float start[3], const float mins[3], const float maxs[3], const float end[3])
{
	qsm_trace_t trace; assert (QSM_Trace (data,start,mins,maxs,end,&trace)); return trace;
}

static void floor_and_point (void)
{
	qsm_data_t data = make_data (1); floor_triangle (&data.triangles[0],0); build (&data);
	float zero[3] = {0}, start[3] = {0,0,10}, end[3] = {0,0,-10};
	qsm_trace_t trace = query (&data,start,zero,zero,end);
	assert (!trace.startsolid && !trace.allsolid && trace.instance == 0 && trace.triangle == 0);
	near_value (trace.fraction,.4984375f,1e-6f); near_value (trace.endpos[2],.03125f,1e-6f);
	near_value (trace.normal[2],1,1e-6f); near_value (trace.plane_dist,0,1e-6f);
	trace = query (&data,end,zero,zero,start); near_value (trace.normal[2],-1,1e-6f);
	near_value (trace.endpos[2],-.03125f,1e-6f);
	float outside_start[3] = {95,95,10}, outside_end[3] = {95,95,-10};
	trace = query (&data,outside_start,zero,zero,outside_end); assert (trace.fraction == 1 && !trace.startsolid);
	float point[3] = {0}; trace = query (&data,point,zero,zero,start); assert (trace.fraction == 1);
	trace = query (&data,point,zero,zero,end); assert (trace.fraction == 1);
	trace = query (&data,point,zero,zero,point); assert (trace.fraction == 1 && !trace.startsolid);
	float edge[3] = {101,-100,0}, along_edge[3] = {101,-99,0}, boxmin[3] = {-1,-1,-1}, boxmax[3] = {1,1,1};
	trace = query (&data,edge,boxmin,boxmax,edge); assert (trace.fraction == 1 && !trace.startsolid);
	trace = query (&data,edge,boxmin,boxmax,along_edge); assert (trace.fraction == 1 && !trace.startsolid);
	dispose (&data);
}

static void player_rest_and_slope (void)
{
	qsm_data_t data = make_data (1); floor_triangle (&data.triangles[0],0); build (&data);
	float mins[3] = {-16,-16,-24}, maxs[3] = {16,16,32};
	float start[3] = {0,0,80}, end[3] = {0,0,-80};
	qsm_trace_t trace = query (&data,start,mins,maxs,end); near_value (trace.endpos[2],24.03125f,1e-5f);
	float rest[3] = {0,0,24}, tangent[3] = {10,0,24}, up[3] = {0,0,30}, down[3] = {0,0,23};
	trace = query (&data,rest,mins,maxs,tangent); assert (trace.fraction == 1 && !trace.startsolid);
	trace = query (&data,rest,mins,maxs,up); assert (trace.fraction == 1);
	trace = query (&data,rest,mins,maxs,down); assert (trace.fraction == 0 && !trace.startsolid && !trace.allsolid);
	float skin[3] = {0,0,24.03125f}, gravity[3] = {0,0,23.9f};
	trace = query (&data,skin,mins,maxs,gravity); near_value (trace.endpos[2],skin[2],1e-5f);
	float a[3] = {-100,-100,-100}, b[3] = {100,-100,100}, c[3] = {0,100,0};
	triangle (&data.triangles[0],a,b,c); build (&data);
	float boxmin[3] = {-1,-1,-1}, boxmax[3] = {1,1,1}, high[3] = {0,0,10}, low[3] = {0,0,-10};
	trace = query (&data,high,boxmin,boxmax,low);
	near_value (trace.normal[0],-.70710678f,1e-5f); near_value (trace.normal[2],.70710678f,1e-5f);
	near_value (trace.endpos[2],2.0441942f,1e-4f);
	float ramp_start[3] = {-10,0,-8}, ramp_end[3] = {10,0,12};
	trace = query (&data,ramp_start,boxmin,boxmax,ramp_end); assert (trace.fraction == 1 && !trace.startsolid);
	dispose (&data);
}

static void edges_and_continuous_motion (void)
{
	qsm_data_t data = make_data (2);
	float a[3] = {-100,-100,0}, b[3] = {100,-100,0}, c[3] = {100,100,0}, d[3] = {-100,100,0};
	triangle (&data.triangles[0],a,b,c); triangle (&data.triangles[1],a,c,d); build (&data);
	float zero[3] = {0}, start[3] = {0,0,1000000}, end[3] = {0,0,-1000000};
	qsm_trace_t trace = query (&data,start,zero,zero,end);
	assert (trace.fraction > .49f && trace.fraction < .51f); near_value (trace.endpos[2],.03125f,1e-5f);
	start[0] = end[0] = -100; start[1] = end[1] = -100;
	trace = query (&data,start,zero,zero,end); assert (trace.fraction < 1); /* exact shared vertex */
	float tilted_a[3] = {0,0,0}, tilted_b[3] = {4,0,4}, tilted_c[3] = {0,4,4};
	triangle (&data.triangles[0],tilted_a,tilted_b,tilted_c); data.instances[0].triangle_count = 1; build (&data);
	float mins[3] = {-1,-1,-1}, maxs[3] = {1,1,1};
	float miss_start[3] = {3.1f,3.1f,8}, miss_end[3] = {3.1f,3.1f,-2};
	trace = query (&data,miss_start,mins,maxs,miss_end);
	assert (trace.fraction == 1 && !trace.startsolid); /* only an edge-cross axis separates */
	float wall_a[3] = {0,-100,-100}, wall_b[3] = {0,100,-100}, wall_c[3] = {0,0,100};
	triangle (&data.triangles[0],wall_a,wall_b,wall_c); build (&data);
	float left[3] = {-10,0,0}, right[3] = {10,0,0};
	trace = query (&data,left,mins,maxs,right); near_value (trace.endpos[0],-1.03125f,1e-6f);
	near_value (trace.normal[0],-1,1e-6f);
	trace = query (&data,right,mins,maxs,left); near_value (trace.endpos[0],1.03125f,1e-6f);
	float contact[3] = {-1,0,0}, along[3] = {-1,10,0};
	trace = query (&data,contact,mins,maxs,along); assert (trace.fraction == 1);
	dispose (&data);
}

static void initial_overlap_union (void)
{
	qsm_data_t data = make_data (3);
	floor_triangle (&data.triangles[0],0); floor_triangle (&data.triangles[1],1.5f); floor_triangle (&data.triangles[2],5);
	data.instances[0].triangle_count = 1; build (&data);
	float mins[3] = {-1,-1,-1}, maxs[3] = {1,1,1}, start[3] = {0,0,0}, end[3] = {0,0,10};
	qsm_trace_t trace = query (&data,start,mins,maxs,start);
	assert (trace.startsolid && trace.allsolid && trace.fraction == 0);
	trace = query (&data,start,mins,maxs,end); assert (trace.startsolid && !trace.allsolid && trace.fraction == 1);
	data.instances[0].triangle_count = 2; build (&data);
	float inside_end[3] = {0,0,2.4f}; trace = query (&data,start,mins,maxs,inside_end);
	assert (trace.startsolid && trace.allsolid && trace.fraction == 0); /* union of two faces */
	float clear_end[3] = {0,0,4}; trace = query (&data,start,mins,maxs,clear_end);
	assert (trace.startsolid && !trace.allsolid && trace.fraction == 1);
	data.instances[0].triangle_count = 3; build (&data);
	trace = query (&data,start,mins,maxs,end); assert (trace.startsolid && !trace.allsolid);
	near_value (trace.endpos[2],3.96875f,1e-6f); near_value (trace.normal[2],-1,1e-6f);
	dispose (&data);
}

static void flags_transaction_and_invalid_inputs (void)
{
	qsm_data_t data = make_data (2); data.instances = realloc (data.instances,2*sizeof (*data.instances)); assert (data.instances);
	memset (data.instances,0,2*sizeof (*data.instances)); data.instance_count = 2;
	floor_triangle (&data.triangles[0],5); floor_triangle (&data.triangles[1],0);
	data.instances[0].first_triangle = 0; data.instances[0].triangle_count = 1;
	data.instances[1].first_triangle = 1; data.instances[1].triangle_count = 1; data.instances[1].flags = QSM_COLLISION;
	build (&data); float zero[3] = {0}, start[3] = {0,0,10}, end[3] = {0,0,-10};
	qsm_trace_t trace = query (&data,start,zero,zero,end); assert (trace.instance == 1 && trace.triangle == 1);
	near_value (trace.endpos[2],.03125f,1e-6f);
	qsm_collision_t *old = data.collision; char error[160];
	data.instances[1].first_triangle = UINT32_MAX; assert (!QSM_BuildCollision (&data,error,sizeof (error)) && error[0]);
	assert (data.collision == old); trace = query (&data,start,zero,zero,end); assert (trace.fraction < 1);
	data.instances[1].first_triangle = 1; data.triangles[1].position[0][0] = NAN;
	assert (!QSM_BuildCollision (&data,error,sizeof (error)) && data.collision == old);
	trace = query (&data,start,zero,zero,end); assert (trace.fraction < 1); /* owned snapshot preserved */
	floor_triangle (&data.triangles[1],0); data.instances[1].flags = 0; build (&data);
	trace = query (&data,start,zero,zero,end); assert (trace.fraction == 1 && trace.instance == UINT32_MAX);
	data.instances[1].flags = QSM_COLLISION; memset (&data.triangles[1],0,sizeof (*data.triangles)); build (&data);
	trace = query (&data,start,zero,zero,end); assert (trace.fraction == 1); /* degenerate face */
	float bad[3] = {NAN,0,0}; assert (!QSM_Trace (&data,bad,zero,zero,end,&trace));
	float invalid_max[3] = {-1,0,0}; assert (!QSM_Trace (&data,start,zero,invalid_max,end,&trace));
	assert (!QSM_Trace (NULL,start,zero,zero,end,&trace)); assert (!QSM_Trace (&data,start,zero,zero,end,NULL));
	dispose (&data); QSM_FreeCollision (NULL);
	qsm_data_t empty = {0}; build (&empty); trace = query (&empty,start,zero,zero,end); assert (trace.fraction == 1);
	dispose (&empty);
}

static void many_overlaps_and_large_coordinates (void)
{
	qsm_data_t data = make_data (160);
	for (uint32_t i = 0; i < data.triangle_count; ++i) floor_triangle (&data.triangles[i],(float)i*.01f-.8f);
	build (&data); float mins[3] = {-1,-1,-1}, maxs[3] = {1,1,1}, center[3] = {0};
	qsm_trace_t trace = query (&data,center,mins,maxs,center); assert (trace.startsolid && trace.allsolid);
	float escape[3] = {0,0,100}; trace = query (&data,center,mins,maxs,escape);
	assert (trace.startsolid && !trace.allsolid && trace.fraction == 1);
	for (uint32_t i = 1; i < data.triangle_count; ++i) floor_triangle (&data.triangles[i],5+(float)i*.01f);
	build (&data); trace = query (&data,center,mins,maxs,escape); /* disconnected interval heap growth */
	assert (trace.startsolid && !trace.allsolid && trace.fraction < .05f);
	dispose (&data);
	data = make_data (1); floor_triangle (&data.triangles[0],30000);
	for (int v = 0; v < 3; ++v) { data.triangles[0].position[v][0] += 10000; data.triangles[0].position[v][1] -= 20000; }
	build (&data); float zero[3] = {0}, start[3] = {10000,-20000,30010}, end[3] = {10000,-20000,29990};
	trace = query (&data,start,zero,zero,end); near_value (trace.endpos[2],30000.03125f,.002f);
	dispose (&data);
}

static uint32_t random_state = 0x3262981u;
static float random_coordinate (void)
{
	random_state = random_state*1664525u+1013904223u;
	return (random_state>>8)*(8.0f/16777216)-4;
}

static void cross3 (const double a[3], const double b[3], double out[3])
{
	out[0] = a[1]*b[2]-a[2]*b[1]; out[1] = a[2]*b[0]-a[0]*b[2]; out[2] = a[0]*b[1]-a[1]*b[0];
}

static double dot3 (const double a[3], const double b[3]) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }

static void randomized_point_reference (void)
{
	/* Independent Moller-Trumbore segment intersection checks the point-box
	 * specialization of the SAT solver, including arbitrary winding/planes. */
	qsm_data_t data = make_data (1); unsigned compared = 0, hits = 0; float zero[3] = {0};
	for (unsigned sample = 0; sample < 10000; ++sample)
	{
		float start[3], end[3]; double edge1[3],edge2[3],velocity[3],offset[3],p[3],q[3],normal[3];
		for (int v = 0; v < 3; ++v) for (int a = 0; a < 3; ++a) data.triangles[0].position[v][a] = random_coordinate ();
		for (int a = 0; a < 3; ++a)
		{
			start[a] = random_coordinate (); end[a] = random_coordinate ();
			edge1[a] = (double)data.triangles[0].position[1][a]-data.triangles[0].position[0][a];
			edge2[a] = (double)data.triangles[0].position[2][a]-data.triangles[0].position[0][a];
			velocity[a] = (double)end[a]-start[a]; offset[a] = (double)start[a]-data.triangles[0].position[0][a];
		}
		cross3 (velocity,edge2,p); double determinant = dot3 (edge1,p); if (fabs (determinant)<1e-9) continue;
		cross3 (offset,edge1,q); double u = dot3 (offset,p)/determinant;
		double v = dot3 (velocity,q)/determinant, fraction = dot3 (edge2,q)/determinant;
		if (fabs (u)<1e-6 || fabs (v)<1e-6 || fabs (u+v-1)<1e-6 || fabs (fraction)<1e-6 || fabs (fraction-1)<1e-6) continue;
		bool expected = u>0 && v>0 && u+v<1 && fraction>0 && fraction<1; build (&data);
		qsm_trace_t trace = query (&data,start,zero,zero,end); assert ((trace.fraction<1) == expected); ++compared;
		if (expected)
		{
			++hits; cross3 (edge1,edge2,normal); double approach = fabs (dot3 (normal,velocity))/sqrt (dot3 (normal,normal));
			double stopped = fmax (0,fraction-(1.0/32.0)/approach); near_value (trace.fraction,(float)stopped,2e-6f);
		}
	}
	assert (compared>9900 && hits>100); dispose (&data);
}

/* Optional raw fixture: little-endian uint32 count followed by count*9 floats.
 * The Python runner exports the actual identity-joint Sponza mesh into this
 * tiny test-only format, keeping the collision module independent of loaders. */
static uint32_t read_u32 (FILE *file)
{
	unsigned char bytes[4]; assert (fread (bytes,1,4,file) == 4);
	return (uint32_t)bytes[0] | (uint32_t)bytes[1]<<8 | (uint32_t)bytes[2]<<16 | (uint32_t)bytes[3]<<24;
}

static void actual_mesh (const char *path)
{
	FILE *file = fopen (path,"rb"); assert (file); uint32_t count = read_u32 (file); assert (count == 262266);
	qsm_data_t data = make_data (count);
	for (uint32_t i = 0; i < count; ++i)
		for (int v = 0; v < 3; ++v) for (int a = 0; a < 3; ++a)
		{
			uint32_t bits = read_u32 (file); memcpy (&data.triangles[i].position[v][a],&bits,4);
		}
	assert (fgetc (file) == EOF); fclose (file);
	clock_t begin = clock (); build (&data); double build_ms = 1000.0*(clock ()-begin)/CLOCKS_PER_SEC;
	float low[3] = {INFINITY,INFINITY,INFINITY}, high[3] = {-INFINITY,-INFINITY,-INFINITY};
	for (uint32_t i = 0; i < count; ++i) for (int v = 0; v < 3; ++v) for (int a = 0; a < 3; ++a)
	{
		float value = data.triangles[i].position[v][a]; if (value < low[a]) low[a] = value; if (value > high[a]) high[a] = value;
	}
	uint32_t state = 0x739a125u, hits = 0, starts[2] = {0,0}, totals[2] = {0,0};
	double group_cpu[2] = {0,0}, free_cpu[2] = {0,0}; const uint32_t queries = 10000;
	begin = clock ();
	for (uint32_t i = 0; i < queries; ++i)
	{
		float start[3], end[3], mins[3], maxs[3];
		for (int a = 0; a < 3; ++a)
		{
			state = state*1664525u + 1013904223u; float unit = (state>>8)*(1.0f/16777216);
			start[a] = low[a]+unit*(high[a]-low[a]);
			state = state*1664525u + 1013904223u;
			end[a] = start[a] + ((int)(state>>24)-128)*.5f;
			mins[a] = i&1 ? (a == 2 ? -24 : -16) : 0;
			maxs[a] = i&1 ? (a == 2 ? 32 : 16) : 0;
		}
		clock_t query_begin = clock (); qsm_trace_t trace = query (&data,start,mins,maxs,end);
		double elapsed = (double)(clock ()-query_begin)/CLOCKS_PER_SEC;
		unsigned group = i&1; ++totals[group]; group_cpu[group] += elapsed;
		starts[group] += trace.startsolid; if (!trace.startsolid) free_cpu[group] += elapsed;
		hits += trace.fraction < 1 || trace.startsolid;
	}
	double query_ms = 1000.0*(clock ()-begin)/CLOCKS_PER_SEC;
	printf ("actual Sponza: %u triangles, BVH %.3f ms, %u point/player queries %.3f ms (%.3f us/query), %u contacts\n",
		count,build_ms,queries,query_ms,query_ms*1000/queries,hits);
	for (unsigned group = 0; group < 2; ++group)
		printf ("  %s: %.3f us/query; %u/%u start-overlapped; free starts %.3f us/query\n",group ? "player box" : "point",
			group_cpu[group]*1e6/totals[group],starts[group],totals[group],
			totals[group] == starts[group] ? 0 : free_cpu[group]*1e6/(totals[group]-starts[group]));
	assert (hits > 10); dispose (&data);
}

int main (int argc, char **argv)
{
	floor_and_point (); player_rest_and_slope (); edges_and_continuous_motion ();
	initial_overlap_union (); flags_transaction_and_invalid_inputs (); many_overlaps_and_large_coordinates ();
	randomized_point_reference ();
	if (argc > 1) actual_mesh (argv[1]);
	puts ("PASS static model continuous triangle collision"); return 0;
}
