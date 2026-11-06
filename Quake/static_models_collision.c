#include "static_models.h"
#include "radiance_scene.h" /* Portable CPU BVH; no lighting or Vulkan dependency. */

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Match the brush hull's contact skin. Geometry tests themselves use double
 * precision without inflating the triangles into bounding boxes. */
#define QSM_SKIN (1.0 / 32.0)
#define QSM_CONTACT 1e-7

struct qsm_collision_s { rc_scene_t scene; };

typedef struct
{
	double enter, leave, normal[3], plane_dist;
	uint32_t instance, triangle;
	bool inside;
} qsm_interval_t;

typedef struct
{
	double start[3], velocity[3], half[3], time_epsilon;
} qsm_sweep_t;

static bool QSM_Error (char *error, size_t size, const char *message)
{
	if (error && size) snprintf (error, size, "%s", message);
	return false;
}

void QSM_FreeCollision (qsm_collision_t *collision)
{
	if (!collision) return;
	RCScene_Clear (&collision->scene);
	free (collision);
}

static void QSM_Cross (const double a[3], const double b[3], double out[3])
{
	out[0] = a[1]*b[2] - a[2]*b[1];
	out[1] = a[2]*b[0] - a[0]*b[2];
	out[2] = a[0]*b[1] - a[1]*b[0];
}

static double QSM_Dot (const double a[3], const double b[3])
{
	return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
}

static bool QSM_TriangleNondegenerate (const qsm_triangle_t *triangle)
{
	double a[3], b[3], normal[3];
	for (int i = 0; i < 3; ++i)
	{
		a[i] = (double)triangle->position[1][i] - triangle->position[0][i];
		b[i] = (double)triangle->position[2][i] - triangle->position[0][i];
	}
	QSM_Cross (a, b, normal);
	return QSM_Dot (normal, normal) > 0;
}

bool QSM_BuildCollision (qsm_data_t *data, char *error, size_t error_size)
{
	if (!data || data->instance_count > QSM_MAX_INSTANCES || data->triangle_count > QSM_MAX_TRIANGLES ||
		(data->instance_count && !data->instances) || (data->triangle_count && !data->triangles))
		return QSM_Error (error, error_size, "invalid static model collision data");
	uint64_t capacity = 0;
	for (uint32_t i = 0; i < data->instance_count; ++i)
	{
		const qsm_instance_t *instance = &data->instances[i];
		if ((uint64_t)instance->first_triangle + instance->triangle_count > data->triangle_count)
			return QSM_Error (error, error_size, "static model collision triangle range is invalid");
		if (instance->flags & QSM_COLLISION) capacity += instance->triangle_count;
	}
	if (capacity > QSM_MAX_TRIANGLES || capacity > SIZE_MAX / sizeof (rc_scene_triangle_t))
		return QSM_Error (error, error_size, "static model collision allocation exceeds limits");
	qsm_collision_t *pending = calloc (1, sizeof (*pending));
	if (!pending) return QSM_Error (error, error_size, "could not allocate static model collision");
	if (capacity)
	{
		pending->scene.triangles = calloc ((size_t)capacity, sizeof (*pending->scene.triangles));
		if (!pending->scene.triangles)
		{
			QSM_FreeCollision (pending);
			return QSM_Error (error, error_size, "could not allocate static model collision triangles");
		}
		pending->scene.triangle_capacity = (uint32_t)capacity;
	}
	for (uint32_t i = 0; i < data->instance_count; ++i)
	{
		const qsm_instance_t *instance = &data->instances[i];
		if (!(instance->flags & QSM_COLLISION)) continue;
		for (uint32_t j = 0; j < instance->triangle_count; ++j)
		{
			uint32_t source = instance->first_triangle + j;
			const qsm_triangle_t *triangle = &data->triangles[source];
			for (int v = 0; v < 3; ++v)
				for (int axis = 0; axis < 3; ++axis)
					if (!isfinite (triangle->position[v][axis]))
					{
						QSM_FreeCollision (pending);
						return QSM_Error (error, error_size, "nonfinite static model collision vertex");
					}
			/* Exported meshes can contain collapsed faces. They have no surface
			 * to collide with and must not prevent the rest of a model loading. */
			if (!QSM_TriangleNondegenerate (triangle)) continue;
			rc_scene_triangle_t *out = &pending->scene.triangles[pending->scene.triangle_count++];
			for (int v = 0; v < 3; ++v)
				for (int axis = 0; axis < 3; ++axis) out->position[v][axis] = triangle->position[v][axis];
			out->metadata[2] = source;
			out->metadata[3] = i;
		}
	}
	if (!RCScene_Build (&pending->scene))
	{
		QSM_FreeCollision (pending);
		return QSM_Error (error, error_size, "could not build static model collision BVH");
	}
	QSM_FreeCollision (data->collision);
	data->collision = pending;
	if (error && error_size) error[0] = 0;
	return true;
}

static bool QSM_NodeHit (const rc_scene_node_t *node, const qsm_sweep_t *sweep, double maximum)
{
	double enter = 0, leave = maximum;
	for (int axis = 0; axis < 3; ++axis)
	{
		double low = (double)node->mins[axis] - sweep->half[axis] - QSM_CONTACT;
		double high = (double)node->maxs[axis] + sweep->half[axis] + QSM_CONTACT;
		double velocity = sweep->velocity[axis], start = sweep->start[axis];
		if (velocity == 0)
		{
			if (start < low || start > high) return false;
			continue;
		}
		double first = (low-start)/velocity, last = (high-start)/velocity;
		if (first > last) { double swap = first; first = last; last = swap; }
		if (first > enter) enter = first;
		if (last < leave) leave = last;
		if (enter > leave) return false;
	}
	return true;
}

typedef struct
{
	qsm_interval_t interval;
	double depth, inside_normal[3], inside_dist;
	bool tangent;
} qsm_sat_t;

static double QSM_Support (const rc_scene_triangle_t *triangle, const double normal[3])
{
	double result = -DBL_MAX;
	for (int v = 0; v < 3; ++v)
	{
		double dot = 0;
		for (int a = 0; a < 3; ++a) dot += normal[a] * triangle->position[v][a];
		if (dot > result) result = dot;
	}
	return result;
}

/* Intersect the time intervals on each separating axis. Box axes, triangle
 * normal, and all edge/box-axis crosses are necessary: triangle AABBs alone
 * would create solid air around sloped and diagonal model geometry. */
static bool QSM_Axis (const rc_scene_triangle_t *triangle, const qsm_sweep_t *sweep,
	const double direction[3], bool surface_axis, qsm_sat_t *sat)
{
	double length2 = QSM_Dot (direction, direction);
	if (length2 == 0) return true;
	double axis[3], inverse = 1/sqrt (length2);
	for (int a = 0; a < 3; ++a) axis[a] = direction[a]*inverse;
	double low = DBL_MAX, high = -DBL_MAX, radius = 0;
	for (int v = 0; v < 3; ++v)
	{
		double projection = 0;
		for (int a = 0; a < 3; ++a)
			projection += axis[a]*((double)triangle->position[v][a]-sweep->start[a]);
		if (projection < low) low = projection;
		if (projection > high) high = projection;
	}
	for (int a = 0; a < 3; ++a) radius += fabs (axis[a])*sweep->half[a];
	low -= radius; high += radius;
	if (low >= -QSM_CONTACT || high <= QSM_CONTACT) sat->interval.inside = false;
	if (low <= 0 && high >= 0)
	{
		double depth = fmin (-low, high);
		if (depth < sat->depth)
		{
			sat->depth = depth;
			double sign = -low < high ? -1 : 1;
			for (int a = 0; a < 3; ++a) sat->inside_normal[a] = sign*axis[a];
			sat->inside_dist = QSM_Support (triangle, sat->inside_normal);
		}
	}
	double velocity = QSM_Dot (axis, sweep->velocity);
	/* Departing an existing contact is free. In particular a point starting
	 * on a two-sided, zero-thickness face may leave either side. */
	if ((low >= -QSM_CONTACT && velocity < 0) || (high <= QSM_CONTACT && velocity > 0)) return false;
	if (fabs (velocity) <= 1e-12)
	{
		if (low > QSM_CONTACT || high < -QSM_CONTACT) return false;
		/* A box merely grazing a separating plane cannot penetrate while
		 * motion stays parallel. Zero-radius point projections are different:
		 * retaining those is what makes exact shared-edge/vertex rays watertight. */
		if ((surface_axis || radius > 0) && (low >= -QSM_CONTACT || high <= QSM_CONTACT)) sat->tangent = true;
		return true;
	}
	double first = velocity > 0 ? low/velocity : high/velocity;
	double last = velocity > 0 ? high/velocity : low/velocity;
	if (first > sat->interval.enter)
	{
		sat->interval.enter = first;
		for (int a = 0; a < 3; ++a) sat->interval.normal[a] = velocity > 0 ? -axis[a] : axis[a];
		sat->interval.plane_dist = QSM_Support (triangle, sat->interval.normal);
	}
	if (last < sat->interval.leave) sat->interval.leave = last;
	return sat->interval.enter <= sat->interval.leave + sweep->time_epsilon;
}

static bool QSM_TriangleHit (const rc_scene_triangle_t *triangle, const qsm_sweep_t *sweep, qsm_interval_t *out)
{
	qsm_sat_t sat = {0};
	sat.interval.enter = -DBL_MAX; sat.interval.leave = DBL_MAX;
	sat.interval.inside = true; sat.depth = DBL_MAX;
	double edges[3][3], normal[3];
	for (int edge = 0; edge < 3; ++edge)
		for (int a = 0; a < 3; ++a)
			edges[edge][a] = (double)triangle->position[(edge+1)%3][a] - triangle->position[edge][a];
	QSM_Cross (edges[0], edges[1], normal);
	if (!QSM_Axis (triangle, sweep, normal, true, &sat) || sat.tangent) return false;
	for (int a = 0; a < 3; ++a)
	{
		double box_axis[3] = {0,0,0}; box_axis[a] = 1;
		if (!QSM_Axis (triangle, sweep, box_axis, false, &sat)) return false;
		for (int edge = 0; edge < 3; ++edge)
		{
			double cross[3]; QSM_Cross (edges[edge], box_axis, cross);
			if (!QSM_Axis (triangle, sweep, cross, false, &sat)) return false;
		}
	}
	if (sat.tangent || sat.interval.leave < 0 || sat.interval.enter > 1 ||
		(sat.interval.leave <= sweep->time_epsilon && sat.interval.enter <= sweep->time_epsilon)) return false;
	if (sat.interval.inside)
	{
		memcpy (sat.interval.normal, sat.inside_normal, sizeof (sat.inside_normal));
		sat.interval.plane_dist = sat.inside_dist;
	}
	if (sat.interval.enter < 0) sat.interval.enter = 0;
	if (sat.interval.leave > 1) sat.interval.leave = 1;
	sat.interval.instance = triangle->metadata[3]; sat.interval.triangle = triangle->metadata[2];
	*out = sat.interval;
	return true;
}

static int QSM_IntervalOrder (const void *left, const void *right)
{
	const qsm_interval_t *a = left, *b = right;
	if (a->enter != b->enter) return a->enter < b->enter ? -1 : 1;
	if (a->instance != b->instance) return a->instance < b->instance ? -1 : 1;
	return a->triangle < b->triangle ? -1 : a->triangle > b->triangle;
}

static bool QSM_InitialOverlap (const rc_scene_t *scene, const qsm_sweep_t *sweep, qsm_interval_t *initial)
{
	/* A point cannot strictly penetrate a zero-thickness surface. Ordinary
	 * traces then need only the nearest impact, not every interval to the end. */
	if (sweep->half[0] == 0 && sweep->half[1] == 0 && sweep->half[2] == 0) return false;
	qsm_sweep_t stationary = *sweep;
	memset (stationary.velocity, 0, sizeof (stationary.velocity));
	for (uint32_t i = 0; i < scene->node_count;)
	{
		const rc_scene_node_t *node = &scene->nodes[i];
		if (!QSM_NodeHit (node, &stationary, 1)) { i = node->metadata[2]; continue; }
		if (node->metadata[1])
			for (uint32_t j = 0; j < node->metadata[1]; ++j)
			{
				qsm_interval_t hit; uint32_t index = scene->indices[node->metadata[0]+j];
				if (QSM_TriangleHit (&scene->triangles[index], &stationary, &hit) && hit.inside)
				{
					*initial = hit; return true;
				}
			}
		++i;
	}
	return false;
}

static void QSM_Impact (qsm_trace_t *trace, const float start[3], const qsm_sweep_t *sweep,
	const qsm_interval_t *impact, double clear_after)
{
	double fraction = 0;
	if (!trace->allsolid)
	{
		double approach = fabs (QSM_Dot (impact->normal, sweep->velocity));
		fraction = impact->enter - (approach > 0 ? QSM_SKIN/approach : 0);
		if (fraction < clear_after) fraction = clear_after;
		if (fraction > 1) fraction = 1;
	}
	trace->fraction = (float)fraction; trace->instance = impact->instance; trace->triangle = impact->triangle;
	trace->plane_dist = (float)impact->plane_dist;
	for (int a = 0; a < 3; ++a)
	{
		trace->normal[a] = (float)impact->normal[a];
		trace->endpos[a] = (float)((double)start[a]+fraction*sweep->velocity[a]);
	}
}

static void QSM_Nearest (const rc_scene_t *scene, const qsm_sweep_t *sweep, const float start[3], qsm_trace_t *trace)
{
	qsm_interval_t closest = {0}; closest.enter = 1; bool found = false;
	for (uint32_t i = 0; i < scene->node_count;)
	{
		const rc_scene_node_t *node = &scene->nodes[i];
		if (!QSM_NodeHit (node, sweep, closest.enter)) { i = node->metadata[2]; continue; }
		if (node->metadata[1])
			for (uint32_t j = 0; j < node->metadata[1]; ++j)
			{
				qsm_interval_t hit; uint32_t index = scene->indices[node->metadata[0]+j];
				if (QSM_TriangleHit (&scene->triangles[index], sweep, &hit) &&
					(!found || QSM_IntervalOrder (&hit, &closest) < 0)) { closest = hit; found = true; }
			}
		++i;
	}
	if (found) QSM_Impact (trace, start, sweep, &closest, 0);
}

static void QSM_HeapPush (qsm_interval_t *heap, size_t count, const qsm_interval_t *hit)
{
	size_t index = count;
	while (index)
	{
		size_t parent = (index-1)/2;
		if (QSM_IntervalOrder (&heap[parent], hit) <= 0) break;
		heap[index] = heap[parent]; index = parent;
	}
	heap[index] = *hit;
}

static qsm_interval_t QSM_HeapPop (qsm_interval_t *heap, size_t count)
{
	qsm_interval_t result = heap[0], last = heap[count-1]; size_t index = 0;
	while (index*2+1 < count-1)
	{
		size_t child = index*2+1;
		if (child+1 < count-1 && QSM_IntervalOrder (&heap[child+1], &heap[child]) < 0) ++child;
		if (QSM_IntervalOrder (&last, &heap[child]) <= 0) break;
		heap[index] = heap[child]; index = child;
	}
	if (count > 1) heap[index] = last;
	return result;
}

bool QSM_Trace (const qsm_data_t *data, const float start[3], const float mins[3],
	const float maxs[3], const float end[3], qsm_trace_t *trace)
{
	if (!trace) return false;
	memset (trace, 0, sizeof (*trace)); trace->fraction = 1;
	trace->instance = trace->triangle = UINT32_MAX;
	if (!data || !start || !mins || !maxs || !end) return false;
	qsm_sweep_t sweep = {0}; double speed = 0;
	for (int a = 0; a < 3; ++a)
	{
		if (!isfinite (start[a]) || !isfinite (end[a]) || !isfinite (mins[a]) ||
			!isfinite (maxs[a]) || mins[a] > maxs[a]) return false;
		trace->endpos[a] = end[a];
		sweep.start[a] = (double)start[a] + ((double)mins[a]+maxs[a])*.5;
		sweep.half[a] = ((double)maxs[a]-mins[a])*.5;
		sweep.velocity[a] = (double)end[a]-start[a];
		if (fabs (sweep.velocity[a]) > speed) speed = fabs (sweep.velocity[a]);
	}
	sweep.time_epsilon = QSM_CONTACT/(1+speed);
	if (!data->collision || !data->collision->scene.triangle_count) return true;
	const rc_scene_t *scene = &data->collision->scene;
	qsm_interval_t initial;
	if (!QSM_InitialOverlap (scene, &sweep, &initial)) { QSM_Nearest (scene, &sweep, start, trace); return true; }
	trace->startsolid = true;
	double clear_after = 0;
	qsm_interval_t local[64], *intervals = local;
	size_t count = 0, capacity = sizeof (local)/sizeof (*local);
	for (uint32_t node_index = 0; node_index < scene->node_count;)
	{
		const rc_scene_node_t *node = &scene->nodes[node_index];
		if (!QSM_NodeHit (node, &sweep, 1)) { node_index = node->metadata[2]; continue; }
		if (node->metadata[1])
			for (uint32_t j = 0; j < node->metadata[1]; ++j)
			{
				uint32_t triangle = scene->indices[node->metadata[0]+j]; qsm_interval_t hit;
				if (!QSM_TriangleHit (&scene->triangles[triangle], &sweep, &hit)) continue;
				if (count == capacity)
				{
					if (capacity > SIZE_MAX/2/sizeof (*intervals)) { if (intervals != local) free (intervals); return false; }
					qsm_interval_t *grown = malloc (capacity*2*sizeof (*grown));
					if (!grown) { if (intervals != local) free (intervals); return false; }
					memcpy (grown, intervals, count*sizeof (*grown));
					if (intervals != local) free (intervals);
					intervals = grown; capacity *= 2;
				}
				QSM_HeapPush (intervals, count++, &hit);
				/* Pending intervals are a min-heap. Each interval is inserted and
				 * removed once; connected overlap grows without quadratic rescans.
				 * A proven all-solid union can stop BVH traversal immediately. */
				while (count && intervals[0].enter <= clear_after+sweep.time_epsilon)
				{
					qsm_interval_t connected = QSM_HeapPop (intervals, count--);
					if (connected.leave > clear_after) clear_after = connected.leave;
				}
				if (clear_after >= 1-sweep.time_epsilon)
				{
					trace->allsolid = true; QSM_Impact (trace, start, &sweep, &initial, 0);
					if (intervals != local) free (intervals);
					return true;
				}
			}
		++node_index;
	}
	if (count) QSM_Impact (trace, start, &sweep, &intervals[0], clear_after);
	else { trace->instance = initial.instance; trace->triangle = initial.triangle; }
	if (intervals != local) free (intervals);
	return true;
}
