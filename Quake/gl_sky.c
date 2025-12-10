/*
Copyright (C) 1996-2001 Id Software, Inc.
Copyright (C) 2002-2009 John Fitzgibbons and others
Copyright (C) 2007-2008 Kristian Duske
Copyright (C) 2010-2014 QuakeSpasm developers
Copyright (C) 2016 Axel Gneiting

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
// gl_sky.c

#include "quakedef.h"
#include "kd_model.h"
#include "noise_gen.h"

float Fog_GetDensity (void);
void  Fog_GetColor (float *c);

extern atomic_uint32_t rs_skypolys; // for r_speeds readout
float				   skyflatcolor[3];
float				   skymins[2][6], skymaxs[2][6];

char skybox_name[1024]; // name of current skybox, or "" if no skybox

gltexture_t *skybox_textures[6];
gltexture_t *skybox_cubemap;
gltexture_t *solidskytexture, *alphaskytexture;
gltexture_t *cloud_noise_texture; // 3D noise texture for volumetric clouds

extern cvar_t gl_farclip;
cvar_t		  r_fastsky = {"r_fastsky", "0", CVAR_NONE};
cvar_t		  r_sky_quality = {"r_sky_quality", "12", CVAR_NONE};
cvar_t		  r_skyalpha = {"r_skyalpha", "1", CVAR_NONE};
cvar_t		  r_skyfog = {"r_skyfog", "0.5", CVAR_NONE};

// Atmospheric sky cvars
cvar_t r_atmosphere = {"r_atmosphere", "1", CVAR_ARCHIVE};
cvar_t r_atmosphere_quality = {"r_atmosphere_quality", "2", CVAR_ARCHIVE}; // 0=off, 1=low, 2=med, 3=high
cvar_t r_sun_angle = {"r_sun_angle", "45", CVAR_NONE};					   // Sun elevation (degrees)
cvar_t r_sun_azimuth = {"r_sun_azimuth", "0", CVAR_NONE};				   // Sun horizontal angle (degrees)
cvar_t r_sun_intensity = {"r_sun_intensity", "1.0", CVAR_NONE};			   // Sun brightness multiplier

// Volumetric cloud cvars
cvar_t r_clouds = {"r_clouds", "1", CVAR_ARCHIVE};
cvar_t r_clouds_density = {"r_clouds_density", "0.5", CVAR_NONE};	 // Cloud opacity
cvar_t r_clouds_coverage = {"r_clouds_coverage", "0.4", CVAR_NONE};	 // Sky coverage amount
cvar_t r_clouds_speed = {"r_clouds_speed", "0.01", CVAR_NONE};		 // Wind animation speed
cvar_t r_clouds_height = {"r_clouds_height", "2000", CVAR_NONE};	 // Cloud layer base height (Quake units)
cvar_t r_clouds_thickness = {"r_clouds_thickness", "1500", CVAR_NONE}; // Cloud layer thickness

/*
================
Sky_ComputeSunDir

Compute sun direction from angle (elevation) and azimuth cvars
================
*/
static void Sky_ComputeSunDir (vec3_t sun_dir)
{
	float angle_rad = r_sun_angle.value * M_PI / 180.0f;
	float azimuth_rad = r_sun_azimuth.value * M_PI / 180.0f;

	// Convert spherical to cartesian (Y is up in Quake)
	float cos_elev = cosf (angle_rad);
	sun_dir[0] = cos_elev * sinf (azimuth_rad);
	sun_dir[1] = cos_elev * cosf (azimuth_rad);
	sun_dir[2] = sinf (angle_rad);
}

static const int skytexorder[6] = {0, 2, 1, 3, 4, 5}; // for skybox

static const vec3_t skyclip[6] = {{1, 1, 0}, {1, -1, 0}, {0, -1, 1}, {0, 1, 1}, {1, 0, 1}, {-1, 0, 1}};

static const int st_to_vec[6][3] = {
	{3, -1, 2}, {-3, 1, 2}, {1, 3, 2}, {-1, -3, 2}, {-2, -1, 3}, // straight up
	{2, -1, -3}													 // straight down
};

static const int vec_to_st[6][3] = {{-2, 3, 1}, {2, 3, -1}, {1, 3, 2}, {-1, 3, -2}, {-2, -1, 3}, {-2, 1, -3}};

float skyfog; // ericw

char skybox_name_worldspawn[1024];

static SDL_mutex *load_skytexture_mutex;
static int		  max_skytexture_index = -1;

qboolean need_bounds;

typedef struct
{
	float position[3];
	float texcoord1[2];
	float texcoord2[2];
	byte  color[4];
} skylayervertex_t;

//==============================================================================
//
//  INIT
//
//==============================================================================

/*
=============
Sky_LoadTexture

A sky texture is 256*128, with the left side being a masked overlay
==============
*/
void Sky_LoadTexture (qmodel_t *mod, texture_t *mt, int tex_index)
{
	char	 texturename[64];
	unsigned x, y, p, r, g, b, count, halfwidth, *rgba;
	byte	*src, *front_data, *back_data;

	if (mt->width != 256 || mt->height != 128)
	{
		Con_Warning ("Sky texture %s is %d x %d, expected 256 x 128\n", mt->name, mt->width, mt->height);
		if (mt->width < 2 || mt->height < 1)
			return;
	}

	halfwidth = mt->width / 2;
	back_data = (byte *)Mem_Alloc (halfwidth * mt->height * 2);
	front_data = back_data + halfwidth * mt->height;
	src = (byte *)(mt + 1);

	// extract back layer and upload
	for (y = 0; y < mt->height; y++)
		memcpy (back_data + y * halfwidth, src + halfwidth + y * mt->width, halfwidth);

	q_snprintf (texturename, sizeof (texturename), "%s:%s_back", mod->name, mt->name);
	solidskytexture = TexMgr_LoadImage (mod, texturename, halfwidth, mt->height, SRC_INDEXED, back_data, "", (src_offset_t)back_data, TEXPREF_NONE);

	// extract front layer and upload
	r = g = b = count = 0;
	for (y = 0; y < mt->height; src += mt->width, front_data += halfwidth, y++)
	{
		for (x = 0; x < halfwidth; x++)
		{
			p = src[x];
			if (p == 0)
				p = 255;
			else
			{
				rgba = &d_8to24table[p];
				r += ((byte *)rgba)[0];
				g += ((byte *)rgba)[1];
				b += ((byte *)rgba)[2];
				count++;
			}
			front_data[x] = p;
		}
	}

	front_data = back_data + halfwidth * mt->height;
	q_snprintf (texturename, sizeof (texturename), "%s:%s_front", mod->name, mt->name);

	// This is horrible but it matches the non-threaded behavior. Does this even make sense?
	SDL_LockMutex (load_skytexture_mutex);
	if (tex_index > max_skytexture_index)
	{
		max_skytexture_index = tex_index;
		alphaskytexture = TexMgr_LoadImage (mod, texturename, halfwidth, mt->height, SRC_INDEXED, front_data, "", (src_offset_t)front_data, TEXPREF_ALPHA);

		// calculate r_fastsky color based on average of all opaque foreground colors
		skyflatcolor[0] = (float)r / (count * 255);
		skyflatcolor[1] = (float)g / (count * 255);
		skyflatcolor[2] = (float)b / (count * 255);
	}
	SDL_UnlockMutex (load_skytexture_mutex);

	Mem_Free (back_data);
}

/*
=============
Sky_LoadTextureQ64

Quake64 sky textures are 32*64
==============
*/
void Sky_LoadTextureQ64 (qmodel_t *mod, texture_t *mt, int tex_index)
{
	char	 texturename[64];
	unsigned i, p, r, g, b, count, halfheight, *rgba;
	byte	*front, *back, *front_rgba;

	if (mt->width != 32 || mt->height != 64)
	{
		Con_DWarning ("Q64 sky texture %s is %d x %d, expected 32 x 64\n", mt->name, mt->width, mt->height);
		if (mt->width < 1 || mt->height < 2)
			return;
	}

	// pointers to both layer textures
	halfheight = mt->height / 2;
	front = (byte *)(mt + 1);
	back = (byte *)(mt + 1) + mt->width * halfheight;
	front_rgba = (byte *)Mem_Alloc (4 * mt->width * halfheight);

	// Normal indexed texture for the back layer
	q_snprintf (texturename, sizeof (texturename), "%s:%s_back", mod->name, mt->name);
	solidskytexture = TexMgr_LoadImage (mod, texturename, mt->width, halfheight, SRC_INDEXED, back, "", (src_offset_t)back, TEXPREF_NONE);

	// front layer, convert to RGBA and upload
	p = r = g = b = count = 0;

	for (i = mt->width * halfheight; i != 0; i--)
	{
		rgba = &d_8to24table[*front++];

		// RGB
		front_rgba[p++] = ((byte *)rgba)[0];
		front_rgba[p++] = ((byte *)rgba)[1];
		front_rgba[p++] = ((byte *)rgba)[2];
		// Alpha
		front_rgba[p++] = 128; // this look ok to me!

		// Fast sky
		r += ((byte *)rgba)[0];
		g += ((byte *)rgba)[1];
		b += ((byte *)rgba)[2];
		count++;
	}

	q_snprintf (texturename, sizeof (texturename), "%s:%s_front", mod->name, mt->name);
	// This is horrible but it matches the non-threaded behavior. Does this even make sense?
	SDL_LockMutex (load_skytexture_mutex);
	if (tex_index > max_skytexture_index)
	{
		max_skytexture_index = tex_index;
		if (alphaskytexture)
			TexMgr_FreeTexture (alphaskytexture);

		alphaskytexture = TexMgr_LoadImage (mod, texturename, mt->width, halfheight, SRC_RGBA, front_rgba, "", (src_offset_t)front_rgba, TEXPREF_ALPHA);
		// calculate r_fastsky color based on average of all opaque foreground colors
		skyflatcolor[0] = (float)r / (count * 255);
		skyflatcolor[1] = (float)g / (count * 255);
		skyflatcolor[2] = (float)b / (count * 255);
	}
	SDL_UnlockMutex (load_skytexture_mutex);

	Mem_Free (front_rgba);
}

/*
==================
Sky_LoadSkyBox
==================
*/
static const char *const suf[6] = {"rt", "bk", "lf", "ft", "up", "dn"};

void Sky_LoadSkyBox (const char *name)
{
	int			   i, width[6], height[6];
	enum srcformat fmt[6];
	char		   filename[6][MAX_OSPATH];
	byte		  *data[6];
	qboolean	   nonefound = true, cubemap = true;

	if (strcmp (skybox_name, name) == 0)
		return; // no change

	// purge old textures
	for (i = 0; i < 6; i++)
	{
		if (skybox_textures[i] && skybox_textures[i] != notexture)
			TexMgr_FreeTexture (skybox_textures[i]);
		skybox_textures[i] = NULL;
	}
	if (skybox_cubemap)
		TexMgr_FreeTexture (skybox_cubemap);
	skybox_cubemap = NULL;

	// turn off skybox if sky is set to ""
	if (name[0] == 0)
	{
		skybox_name[0] = 0;
		return;
	}

	// load textures
	for (i = 0; i < 6; i++)
	{
		q_snprintf (filename[i], sizeof (filename[i]), "gfx/env/%s%s", name, suf[i]);
		if ((data[i] = Image_LoadImage (filename[i], &width[i], &height[i], &fmt[i], (cl.worldmodel ? cl.worldmodel->path_id : 0))))
			nonefound = false;
		if (!data[i] || (width[i] != height[i]) || (width[i] != width[0]) || (fmt[i] != SRC_RGBA))
			cubemap = false;
	}

	if (cubemap)
	{
		q_snprintf (filename[0], sizeof (filename[0]), "gfx/env/%scube", name);
		skybox_cubemap = TexMgr_LoadImage (cl.worldmodel, filename[0], width[0], height[0], SRC_RGBA_CUBEMAP, (byte *)data, filename[0], 0, TEXPREF_NONE);
	}
	else
		for (i = 0; i < 6; i++)
		{
			if (data[i])
				skybox_textures[i] = TexMgr_LoadImage (cl.worldmodel, filename[i], width[i], height[i], fmt[i], data[i], filename[i], 0, TEXPREF_NONE);
			else
			{
				Con_Printf ("Couldn't load %s\n", filename[i]);
				skybox_textures[i] = notexture;
			}
		}

	for (i = 0; i < 6; i++)
		if (data[i])
			Mem_Free (data[i]);

	if (nonefound) // go back to scrolling sky if skybox is totally missing
	{
		for (i = 0; i < 6; i++)
		{
			if (skybox_textures[i] && skybox_textures[i] != notexture)
				TexMgr_FreeTexture (skybox_textures[i]);
			skybox_textures[i] = NULL;
		}
		skybox_name[0] = 0;
		return;
	}

	q_strlcpy (skybox_name, name, sizeof (skybox_name));
}

/*
=================
Sky_GetSkyCommand

To preserve dynamic skies in demos and savegames
=================
*/
const char *Sky_GetSkyCommand (qboolean always)
{
	qboolean need_sky = always || strcmp (skybox_name, skybox_name_worldspawn);
	qboolean need_skyfog = always; // no safe way to record skyfog in demos; r_skyfog is user pref

	if (need_sky || need_skyfog)
	{
		char sky[128];
		char fog[128];
		q_strlcpy (sky, va ("sky \"%s\"", skybox_name), sizeof (sky));
		q_strlcpy (fog, va ("skyfog %g", skyfog), sizeof (fog));
		return va ("\n%s%s%s\n", need_sky ? sky : "", need_sky && need_skyfog ? "\n" : "", need_skyfog ? fog : "");
	}

	return NULL;
}

/*
=================
Sky_ClearAll

Called on map unload/game change to avoid keeping pointers to freed data
=================
*/
void Sky_ClearAll (void)
{
	int i;

	skybox_name[0] = 0;
	for (i = 0; i < 6; i++)
		skybox_textures[i] = NULL;
	skybox_cubemap = NULL;
	solidskytexture = NULL;
	alphaskytexture = NULL;
	max_skytexture_index = -1;
}

/*
=================
Sky_NewMap
=================
*/
void Sky_NewMap (void)
{
	char		key[128], value[4096];
	const char *data;

	skyfog = r_skyfog.value;

	// Generate cloud noise texture if not already created
	if (!cloud_noise_texture)
	{
		const int noise_size = 128;
		byte	 *noise_data = Noise_Generate3DTexture (noise_size);
		if (noise_data)
		{
			cloud_noise_texture =
				TexMgr_LoadImage3D (NULL, "cloud_noise", noise_size, noise_size, noise_size, SRC_R8_3D, noise_data, TEXPREF_PERSIST | TEXPREF_LINEAR);
			Mem_Free (noise_data);
			Con_DPrintf ("Generated %dx%dx%d cloud noise texture\n", noise_size, noise_size, noise_size);
		}
	}

	//
	// read worldspawn (this is so ugly, and shouldn't it be done on the server?)
	//
	data = cl.worldmodel->entities;
	if (!data)
		return; // FIXME: how could this possibly ever happen? -- if there's no
	// worldspawn then the sever wouldn't send the loadmap message to the client

	data = COM_Parse (data);
	if (!data)				 // should never happen
		return;				 // error
	if (com_token[0] != '{') // should never happen
		return;				 // error
	while (1)
	{
		data = COM_Parse (data);
		if (!data)
			return; // error
		if (com_token[0] == '}')
			break; // end of worldspawn
		if (com_token[0] == '_')
			q_strlcpy (key, com_token + 1, sizeof (key));
		else
			q_strlcpy (key, com_token, sizeof (key));
		while (key[0] && key[strlen (key) - 1] == ' ') // remove trailing spaces
			key[strlen (key) - 1] = 0;
		data = COM_ParseEx (data, CPE_ALLOWTRUNC);
		if (!data)
			return; // error
		q_strlcpy (value, com_token, sizeof (value));

		if (!strcmp ("sky", key))
			Sky_LoadSkyBox (value);

		if (!strcmp ("skyfog", key))
			skyfog = atof (value);

#if 1									   // also accept non-standard keys
		else if (!strcmp ("skyname", key)) // half-life
			Sky_LoadSkyBox (value);
		else if (!strcmp ("qlsky", key)) // quake lives
			Sky_LoadSkyBox (value);
#endif
	}

	q_strlcpy (skybox_name_worldspawn, skybox_name, sizeof (skybox_name_worldspawn));
}

/*
=================
Sky_SkyCommand_f
=================
*/
void Sky_SkyCommand_f (void)
{
	switch (Cmd_Argc ())
	{
	case 1:
		Con_Printf ("\"sky\" is \"%s\"\n", skybox_name);
		break;
	case 2:
		Sky_LoadSkyBox (Cmd_Argv (1));
		break;
	default:
		Con_Printf ("usage: sky <skyname>\n");
	}
}

/*
====================
R_SetSkyfog_f -- ericw
====================
*/
static void R_SetSkyfog_f (cvar_t *var)
{
	// clear any skyfog setting from worldspawn
	skyfog = var->value;
}

/*
====================
Sky_SetSkyfog
====================
*/
void Sky_SetSkyfog (float value)
{
	skyfog = value;
}

/*
=============
Sky_Init
=============
*/
void Sky_Init (void)
{
	int i;

	Cvar_RegisterVariable (&r_fastsky);
	Cvar_RegisterVariable (&r_sky_quality);
	Cvar_RegisterVariable (&r_skyalpha);
	Cvar_RegisterVariable (&r_skyfog);
	Cvar_SetCallback (&r_skyfog, R_SetSkyfog_f);

	// Atmospheric sky cvars
	Cvar_RegisterVariable (&r_atmosphere);
	Cvar_RegisterVariable (&r_atmosphere_quality);
	Cvar_RegisterVariable (&r_sun_angle);
	Cvar_RegisterVariable (&r_sun_azimuth);
	Cvar_RegisterVariable (&r_sun_intensity);

	// Volumetric cloud cvars
	Cvar_RegisterVariable (&r_clouds);
	Cvar_RegisterVariable (&r_clouds_density);
	Cvar_RegisterVariable (&r_clouds_coverage);
	Cvar_RegisterVariable (&r_clouds_speed);
	Cvar_RegisterVariable (&r_clouds_height);
	Cvar_RegisterVariable (&r_clouds_thickness);

	Cmd_AddCommand ("sky", Sky_SkyCommand_f);

	skybox_name[0] = 0;
	for (i = 0; i < 6; i++)
		skybox_textures[i] = NULL;
	skybox_cubemap = NULL;
	cloud_noise_texture = NULL;

	load_skytexture_mutex = SDL_CreateMutex ();
}

/*
=============
Sky_NewGame

Called when a new game starts - regenerate cloud noise texture
=============
*/
void Sky_NewGame (void)
{
	// Generate 3D noise texture for volumetric clouds (128^3 = 2MB)
	if (cloud_noise_texture)
	{
		TexMgr_FreeTexture (cloud_noise_texture);
		cloud_noise_texture = NULL;
	}

	const int noise_size = 128;
	byte	 *noise_data = Noise_Generate3DTexture (noise_size);
	if (noise_data)
	{
		cloud_noise_texture =
			TexMgr_LoadImage3D (NULL, "cloud_noise", noise_size, noise_size, noise_size, SRC_R8_3D, noise_data, TEXPREF_PERSIST | TEXPREF_LINEAR);
		Mem_Free (noise_data);
		Con_DPrintf ("Generated %dx%dx%d cloud noise texture\n", noise_size, noise_size, noise_size);
	}
}

//==============================================================================
//
//  PROCESS SKY SURFS
//
//==============================================================================

/*
=================
Sky_ProjectPoly

update sky bounds
=================
*/
void Sky_ProjectPoly (int nump, vec3_t vecs)
{
	int	   i, j;
	vec3_t v, av;
	float  s, t, dv;
	int	   axis;
	float *vp;

	// decide which face it maps to
	VectorCopy (vec3_origin, v);
	for (i = 0, vp = vecs; i < nump; i++, vp += 3)
	{
		VectorAdd (vp, v, v);
	}
	av[0] = fabs (v[0]);
	av[1] = fabs (v[1]);
	av[2] = fabs (v[2]);
	if (av[0] > av[1] && av[0] > av[2])
	{
		if (v[0] < 0)
			axis = 1;
		else
			axis = 0;
	}
	else if (av[1] > av[2] && av[1] > av[0])
	{
		if (v[1] < 0)
			axis = 3;
		else
			axis = 2;
	}
	else
	{
		if (v[2] < 0)
			axis = 5;
		else
			axis = 4;
	}

	// project new texture coords
	for (i = 0; i < nump; i++, vecs += 3)
	{
		j = vec_to_st[axis][2];
		if (j > 0)
			dv = vecs[j - 1];
		else
			dv = -vecs[-j - 1];

		j = vec_to_st[axis][0];
		if (j < 0)
			s = -vecs[-j - 1] / dv;
		else
			s = vecs[j - 1] / dv;
		j = vec_to_st[axis][1];
		if (j < 0)
			t = -vecs[-j - 1] / dv;
		else
			t = vecs[j - 1] / dv;

		if (s < skymins[0][axis])
			skymins[0][axis] = s;
		if (t < skymins[1][axis])
			skymins[1][axis] = t;
		if (s > skymaxs[0][axis])
			skymaxs[0][axis] = s;
		if (t > skymaxs[1][axis])
			skymaxs[1][axis] = t;
	}
}

/*
=================
Sky_ClipPoly
=================
*/
static void Sky_ClipPoly (int nump, vec3_t vecs, int stage)
{
	float	*v;
	qboolean front, back;
	float	 d, e;
	int		 newc[2];

	int i, j = 0;

	const size_t MAX_CLIP_VERTS = nump + 2;

	if (stage == 6) // fully clipped
	{
		Sky_ProjectPoly (nump, vecs);
		return;
	}

	front = back = false;

	TEMP_ALLOC (int, sides, MAX_CLIP_VERTS);
	TEMP_ALLOC (float, dists, MAX_CLIP_VERTS);

	for (i = 0, v = vecs; i < nump; i++, v += 3)
	{
		d = DotProduct (v, skyclip[stage]);

		if (d > ON_EPSILON)
		{
			front = true;
			sides[i] = SIDE_FRONT;
		}
		else if (d < ON_EPSILON)
		{
			back = true;
			sides[i] = SIDE_BACK;
		}
		else
			sides[i] = SIDE_ON;
		dists[i] = d;
	}

	if (!front || !back)
	{
		// not clipped
		Sky_ClipPoly (nump, vecs, stage + 1);

		TEMP_FREE (dists);
		TEMP_FREE (sides);
		return;
	}

	// clip it
	sides[i] = sides[0];
	dists[i] = dists[0];
	VectorCopy (vecs, (vecs + (i * 3)));
	newc[0] = newc[1] = 0;

	// 2-dim vec3_t	 newv[2][MAX_CLIP_VERTS]; as 2 arrays
	TEMP_ALLOC (vec3_t, newv_0, MAX_CLIP_VERTS);
	TEMP_ALLOC (vec3_t, newv_1, MAX_CLIP_VERTS);

	for (i = 0, v = vecs; i < nump; i++, v += 3)
	{
		switch (sides[i])
		{
		case SIDE_FRONT:
			VectorCopy (v, newv_0[newc[0]]);
			newc[0]++;
			break;
		case SIDE_BACK:
			VectorCopy (v, newv_1[newc[1]]);
			newc[1]++;
			break;
		case SIDE_ON:
			VectorCopy (v, newv_0[newc[0]]);
			newc[0]++;
			VectorCopy (v, newv_1[newc[1]]);
			newc[1]++;
			break;
		}

		if (sides[i] == SIDE_ON || sides[i + 1] == SIDE_ON || sides[i + 1] == sides[i])
			continue;

		d = dists[i] / (dists[i] - dists[i + 1]);
		for (j = 0; j < 3; j++)
		{
			e = v[j] + d * (v[j + 3] - v[j]);
			newv_0[newc[0]][j] = e;
			newv_1[newc[1]][j] = e;
		}
		newc[0]++;
		newc[1]++;
	}

	// continue
	Sky_ClipPoly (newc[0], newv_0[0], stage + 1);
	Sky_ClipPoly (newc[1], newv_1[0], stage + 1);

	TEMP_FREE (dists);
	TEMP_FREE (sides);
	TEMP_FREE (newv_0);
	TEMP_FREE (newv_1);
}

/*
================
Sky_ProcessPoly
================
*/
void Sky_ProcessPoly (cb_context_t *cbx, glpoly_t *p, float color[3])
{
	float *poly_vert;

	// draw it
	DrawGLPoly (cbx, p, color, 1.0f);

	// update sky bounds
	if (need_bounds)
	{
		const size_t MAX_CLIP_VERTS = p->numverts + 2;
		const int	 num_verts = p->numverts;

		TEMP_ALLOC (vec3_t, verts, MAX_CLIP_VERTS);

		for (int i = 0; i < num_verts; i++)
		{
			poly_vert = &p->verts[0][0] + (i * VERTEXSIZE);
			VectorSubtract (poly_vert, r_origin, verts[i]);
		}
		Sky_ClipPoly (num_verts, verts[0], 0);

		TEMP_FREE (verts);
	}
}

/*
================
Sky_ProcessTextureChains -- handles sky polys in world model
================
*/
void Sky_ProcessTextureChains (cb_context_t *cbx, float color[3], int *skypolys)
{
	int			i;
	msurface_t *s;
	texture_t  *t;

	if (!r_drawworld_cheatsafe)
		return;

	for (i = 0; i < cl.worldmodel->numtextures; i++)
	{
		t = cl.worldmodel->textures[i];

		if (!t || !t->texturechains[chain_world] || !(t->texturechains[chain_world]->flags & SURF_DRAWSKY))
			continue;

		for (s = t->texturechains[chain_world]; s; s = s->texturechains[chain_world])
		{
			Sky_ProcessPoly (cbx, s->polys, color);
			++(*skypolys);
		}
	}
}

/*
================
Sky_DrawSkySurface
================
*/
static void Sky_DrawSkySurface (cb_context_t *cbx, float color[3], entity_t *e, msurface_t *s, qboolean rotated, vec3_t forward, vec3_t right, vec3_t up)
{
	// copy the polygon and translate manually, since Sky_ProcessPoly needs it to be in world space
	TEMP_ALLOC (glpoly_t, p, s->polys->numverts);
	p->numverts = s->polys->numverts;
	for (int k = 0; k < p->numverts; k++)
	{
		if (rotated)
		{
			p->verts[k][0] = e->origin[0] + s->polys->verts[k][0] * forward[0] - s->polys->verts[k][1] * right[0] + s->polys->verts[k][2] * up[0];
			p->verts[k][1] = e->origin[1] + s->polys->verts[k][0] * forward[1] - s->polys->verts[k][1] * right[1] + s->polys->verts[k][2] * up[1];
			p->verts[k][2] = e->origin[2] + s->polys->verts[k][0] * forward[2] - s->polys->verts[k][1] * right[2] + s->polys->verts[k][2] * up[2];
		}
		else
		{
			float *s_poly_vert = &s->polys->verts[0][0] + (k * VERTEXSIZE);
			float *poly_vert = &p->verts[0][0] + (k * VERTEXSIZE);
			VectorAdd (s_poly_vert, e->origin, poly_vert);
		}
	}
	Sky_ProcessPoly (cbx, p, color);
	TEMP_FREE (p);
}

/*
================
Sky_ProcessEntities -- handles sky polys on brush models
================
*/
void Sky_ProcessEntities (cb_context_t *cbx, float color[3])
{
	entity_t   *e;
	msurface_t *s;
	int			i, j;
	float		dot;
	qboolean	rotated;
	vec3_t		temp, forward, right, up;
	vec3_t		modelorg;

	if (!r_drawentities.value)
		return;

	for (i = 0; i < cl_numvisedicts; i++)
	{
		e = cl_visedicts[i];

		if (e->model->type != mod_brush)
			continue;

		if (!(e->model->used_specials & SURF_DRAWSKY))
			continue;

		if (R_IndirectBrush (e))
			continue;

		if (R_CullModelForEntity (e))
			continue;

		if (e->alpha == ENTALPHA_ZERO)
			continue;

		VectorSubtract (r_refdef.vieworg, e->origin, modelorg);
		if (e->angles[0] || e->angles[1] || e->angles[2])
		{
			rotated = true;
			AngleVectors (e->angles, forward, right, up);
			VectorCopy (modelorg, temp);
			modelorg[0] = DotProduct (temp, forward);
			modelorg[1] = -DotProduct (temp, right);
			modelorg[2] = DotProduct (temp, up);
		}
		else
			rotated = false;

		s = &e->model->surfaces[e->model->firstmodelsurface];

		for (j = 0; j < e->model->nummodelsurfaces; j++, s++)
		{
			if (s->flags & SURF_DRAWSKY)
			{
				dot = DotProduct (modelorg, s->plane->normal) - s->plane->dist;
				if (((s->flags & SURF_PLANEBACK) && (dot < -BACKFACE_EPSILON)) || (!(s->flags & SURF_PLANEBACK) && (dot > BACKFACE_EPSILON)))
					Sky_DrawSkySurface (cbx, color, e, s, rotated, forward, right, up);
			}
		}
	}
}

//==============================================================================
//
//  RENDER SKYBOX
//
//==============================================================================

/*
==============
Sky_EmitSkyBoxVertex
==============
*/
void Sky_EmitSkyBoxVertex (basicvertex_t *vertex, float s, float t, int axis)
{
	vec3_t v, b;
	int	   j, k;
	float  w, h;

	b[0] = s * gl_farclip.value / sqrt (3.0);
	b[1] = t * gl_farclip.value / sqrt (3.0);
	b[2] = gl_farclip.value / sqrt (3.0);

	for (j = 0; j < 3; j++)
	{
		k = st_to_vec[axis][j];
		if (k < 0)
			v[j] = -b[-k - 1];
		else
			v[j] = b[k - 1];
		v[j] += r_origin[j];
	}

	// convert from range [-1,1] to [0,1]
	s = (s + 1) * 0.5;
	t = (t + 1) * 0.5;

	// avoid bilerp seam
	w = skybox_textures[skytexorder[axis]]->width;
	h = skybox_textures[skytexorder[axis]]->height;
	s = s * (w - 1) / w + 0.5 / w;
	t = t * (h - 1) / h + 0.5 / h;

	t = 1.0 - t;

	vertex->position[0] = v[0];
	vertex->position[1] = v[1];
	vertex->position[2] = v[2];

	vertex->texcoord[0] = s;
	vertex->texcoord[1] = t;

	vertex->color[0] = 255;
	vertex->color[1] = 255;
	vertex->color[2] = 255;
	vertex->color[3] = 255;
}

/*
==============
Sky_DrawSkyDome

Draws a sky dome (hemisphere) for KD maps that don't have sky geometry.
Uses the same vertex format as sky surfaces.
==============
*/
static void Sky_DrawSkyDome (cb_context_t *cbx, int *skypolys)
{
	// Draw a simple hemisphere dome around the player
	const int   slices = 16;
	const int   stacks = 8;
	const float radius = 4096.0f; // Large enough to be beyond anything in the scene

	vkCmdBindIndexBuffer (cbx->cb, vulkan_globals.fan_index_buffer, 0, VK_INDEX_TYPE_UINT16);

	// Draw each stack as a triangle strip converted to triangles
	for (int stack = 0; stack < stacks; stack++)
	{
		float phi0 = (M_PI / 2.0f) * (float)stack / (float)stacks;       // 0 to PI/2
		float phi1 = (M_PI / 2.0f) * (float)(stack + 1) / (float)stacks;

		float z0 = sinf (phi0) * radius;
		float z1 = sinf (phi1) * radius;
		float r0 = cosf (phi0) * radius;
		float r1 = cosf (phi1) * radius;

		for (int slice = 0; slice < slices; slice++)
		{
			float theta0 = (2.0f * M_PI) * (float)slice / (float)slices;
			float theta1 = (2.0f * M_PI) * (float)(slice + 1) / (float)slices;

			// Create a quad (two triangles) for this segment
			VkBuffer	   buffer;
			VkDeviceSize   buffer_offset;
			basicvertex_t *vertices = (basicvertex_t *)R_VertexAllocate (4 * sizeof (basicvertex_t), &buffer, &buffer_offset);

			// Bottom left
			vertices[0].position[0] = r_origin[0] + r0 * cosf (theta0);
			vertices[0].position[1] = r_origin[1] + r0 * sinf (theta0);
			vertices[0].position[2] = r_origin[2] + z0;
			vertices[0].texcoord[0] = 0;
			vertices[0].texcoord[1] = 0;
			vertices[0].color[0] = vertices[0].color[1] = vertices[0].color[2] = vertices[0].color[3] = 255;

			// Top left
			vertices[1].position[0] = r_origin[0] + r1 * cosf (theta0);
			vertices[1].position[1] = r_origin[1] + r1 * sinf (theta0);
			vertices[1].position[2] = r_origin[2] + z1;
			vertices[1].texcoord[0] = 0;
			vertices[1].texcoord[1] = 1;
			vertices[1].color[0] = vertices[1].color[1] = vertices[1].color[2] = vertices[1].color[3] = 255;

			// Top right
			vertices[2].position[0] = r_origin[0] + r1 * cosf (theta1);
			vertices[2].position[1] = r_origin[1] + r1 * sinf (theta1);
			vertices[2].position[2] = r_origin[2] + z1;
			vertices[2].texcoord[0] = 1;
			vertices[2].texcoord[1] = 1;
			vertices[2].color[0] = vertices[2].color[1] = vertices[2].color[2] = vertices[2].color[3] = 255;

			// Bottom right
			vertices[3].position[0] = r_origin[0] + r0 * cosf (theta1);
			vertices[3].position[1] = r_origin[1] + r0 * sinf (theta1);
			vertices[3].position[2] = r_origin[2] + z0;
			vertices[3].texcoord[0] = 1;
			vertices[3].texcoord[1] = 0;
			vertices[3].color[0] = vertices[3].color[1] = vertices[3].color[2] = vertices[3].color[3] = 255;

			vkCmdBindVertexBuffers (cbx->cb, 0, 1, &buffer, &buffer_offset);
			vkCmdDrawIndexed (cbx->cb, 6, 1, 0, 0, 0);

			++(*skypolys);
		}
	}
}

/*
==============
Sky_DrawSkyBox

FIXME: eliminate cracks by adding an extra vert on tjuncs
==============
*/
void Sky_DrawSkyBox (cb_context_t *cbx, int *skypolys)
{
	int i;

	for (i = 0; i < 6; i++)
	{
		if (indirect) // Don't have bounds for the world polys. This type of sky (malformed cube) is very rare, so just draw the entire box (will
					  // be stencil-culled). Also note tjunctions avoidance below: this only culled entire cube faces in the first place.
		{
			skymins[0][i] = skymins[1][i] = -1;
			skymaxs[0][i] = skymaxs[1][i] = 1;
		}

		if (skymins[0][i] >= skymaxs[0][i] || skymins[1][i] >= skymaxs[1][i])
			continue;

		vkCmdBindDescriptorSets (
			cbx->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.sky_stencil_pipeline[indirect].layout.handle, 0, 1,
			&skybox_textures[skytexorder[i]]->descriptor_set, 0, NULL);

		VkBuffer	   buffer;
		VkDeviceSize   buffer_offset;
		basicvertex_t *vertices = (basicvertex_t *)R_VertexAllocate (4 * sizeof (basicvertex_t), &buffer, &buffer_offset);

#if 1 // FIXME: this is to avoid tjunctions until i can do it the right way
		skymins[0][i] = -1;
		skymins[1][i] = -1;
		skymaxs[0][i] = 1;
		skymaxs[1][i] = 1;
#endif
		Sky_EmitSkyBoxVertex (vertices + 0, skymins[0][i], skymins[1][i], i);
		Sky_EmitSkyBoxVertex (vertices + 1, skymins[0][i], skymaxs[1][i], i);
		Sky_EmitSkyBoxVertex (vertices + 2, skymaxs[0][i], skymaxs[1][i], i);
		Sky_EmitSkyBoxVertex (vertices + 3, skymaxs[0][i], skymins[1][i], i);

		vkCmdBindVertexBuffers (cbx->cb, 0, 1, &buffer, &buffer_offset);
		vkCmdDrawIndexed (cbx->cb, 6, 1, 0, 0, 0);

		++(*skypolys);
	}
}

//==============================================================================
//
//  RENDER CLOUDS
//
//==============================================================================

/*
==============
Sky_DrawSky

called once per frame after opaques before transparents, handles world + entities
==============
*/
void Sky_DrawSky (cb_context_t *cbx)
{
	int i;

	if (r_lightmap_cheatsafe)
		return;

	// For KD maps, force non-indirect mode to ensure correct vertex format
	// The indirect pipelines use worldvertex_t (28 bytes) but KD sky dome uses basicvertex_t (24 bytes)
	extern qboolean indirect;
	qboolean saved_indirect = indirect;
	if (KD_ShouldSkipBSPRendering ())
		indirect = false;

	R_BeginDebugUtilsLabel (cbx, "Sky");

	const qboolean flat_color = r_fastsky.value || (Fog_GetDensity () > 0 && skyfog >= 1);
	need_bounds = Sky_NeedStencil ();

	//
	// reset sky bounds
	//
	for (i = 0; i < 6; i++)
	{
		skymins[0][i] = skymins[1][i] = FLT_MAX;
		skymaxs[0][i] = skymaxs[1][i] = -FLT_MAX;
	}

	float fog_density = (Fog_GetDensity () > 0) ? skyfog : 0.0f;

	float color[4];
	if (Fog_GetDensity () > 0)
		Fog_GetColor (color); // color[3] is not used
	else
		memcpy (color, skyflatcolor, 3 * sizeof (float));

	float constant_values[25];
	memcpy (constant_values, vulkan_globals.view_projection_matrix, sizeof (vulkan_globals.view_projection_matrix));
	constant_values[16] = CLAMP (0.0f, color[0], 1.0f);
	constant_values[17] = CLAMP (0.0f, color[1], 1.0f);
	constant_values[18] = CLAMP (0.0f, color[2], 1.0f);
	constant_values[19] = fog_density;

	// With slow sky we first write stencil for the part of the screen that is covered by sky geometry and passes the depth test
	// Sky_DrawSkyBox then only fills the parts that had stencil written
	if (flat_color)
	{
		if (indirect)
			constant_values[19] = 1.0f;
		R_BindPipeline (cbx, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.sky_color_pipeline[indirect]);
		R_PushConstants (cbx, VK_SHADER_STAGE_ALL_GRAPHICS, 0, 20 * sizeof (float), constant_values);
	}
	else if (skybox_cubemap)
	{
		R_BindPipeline (cbx, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.sky_cube_pipeline[indirect]);
		vkCmdBindDescriptorSets (
			cbx->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.sky_cube_pipeline[indirect].layout.handle, 0, 1, &skybox_cubemap->descriptor_set, 0, NULL);
		memcpy (&constant_values[20], r_refdef.vieworg, sizeof (r_refdef.vieworg));
		R_PushConstants (cbx, VK_SHADER_STAGE_ALL_GRAPHICS, 0, 23 * sizeof (float), constant_values);
	}
	else if (!skybox_name[0])
	{
		// Use atmospheric sky if r_atmosphere is enabled
		if (r_atmosphere.value)
		{
			float atmosphere_constants[28];
			memcpy (atmosphere_constants, vulkan_globals.view_projection_matrix, 16 * sizeof (float));
			atmosphere_constants[16] = CLAMP (0.0f, color[0], 1.0f); // fog_color
			atmosphere_constants[17] = CLAMP (0.0f, color[1], 1.0f);
			atmosphere_constants[18] = CLAMP (0.0f, color[2], 1.0f);
			atmosphere_constants[19] = fog_density;
			memcpy (&atmosphere_constants[20], r_refdef.vieworg, sizeof (r_refdef.vieworg)); // eye_pos
			atmosphere_constants[23] = cl.time; // time

			vec3_t sun_dir;
			Sky_ComputeSunDir (sun_dir);
			atmosphere_constants[24] = sun_dir[0]; // sun_dir
			atmosphere_constants[25] = sun_dir[1];
			atmosphere_constants[26] = sun_dir[2];
			atmosphere_constants[27] = r_sun_intensity.value; // sun_intensity

			R_BindPipeline (cbx, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.sky_atmosphere_pipeline[indirect]);
			R_PushConstants (cbx, VK_SHADER_STAGE_ALL_GRAPHICS, 0, 28 * sizeof (float), atmosphere_constants);
		}
		else
		{
			// Fallback to scrolling sky layer
			if (!solidskytexture || !alphaskytexture)
			{
				R_EndDebugUtilsLabel (cbx);
				return;
			}
			R_BindPipeline (cbx, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.sky_layer_pipeline[indirect]);
			VkDescriptorSet descriptor_sets[2] = {solidskytexture->descriptor_set, alphaskytexture->descriptor_set};
			vkCmdBindDescriptorSets (
				cbx->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.sky_layer_pipeline[indirect].layout.handle, 0, 2, descriptor_sets, 0, NULL);
			memcpy (&constant_values[20], r_refdef.vieworg, sizeof (r_refdef.vieworg));
			constant_values[23] = cl.time - (int)cl.time / 16 * 16;
			constant_values[24] = r_skyalpha.value;
			R_PushConstants (cbx, VK_SHADER_STAGE_ALL_GRAPHICS, 0, 25 * sizeof (float), constant_values);
		}
	}
	else
	{
		R_BindPipeline (cbx, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.sky_stencil_pipeline[indirect]);
		R_PushConstants (cbx, VK_SHADER_STAGE_ALL_GRAPHICS, 0, 20 * sizeof (float), constant_values);
	}
	vkCmdBindIndexBuffer (cbx->cb, vulkan_globals.fan_index_buffer, 0, VK_INDEX_TYPE_UINT16);

	//
	// process world and bmodels: draw flat-shaded sky surfs, and update skybounds
	//
	int skypolys = 0;
	if (indirect)
	{
		R_DrawIndirectBrushes (cbx, false, false, true, -1);

		// Entities cannot use the indirect pipelines
		vkCmdBindIndexBuffer (cbx->cb, vulkan_globals.fan_index_buffer, 0, VK_INDEX_TYPE_UINT16);
		if (flat_color)
			R_BindPipeline (cbx, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.sky_color_pipeline[0]);
		else if (skybox_cubemap)
			R_BindPipeline (cbx, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.sky_cube_pipeline[0]);
		else if (!skybox_name[0])
		{
			if (r_atmosphere.value)
				R_BindPipeline (cbx, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.sky_atmosphere_pipeline[0]);
			else
				R_BindPipeline (cbx, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.sky_layer_pipeline[0]);
		}
		else
			R_BindPipeline (cbx, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.sky_stencil_pipeline[0]);
	}
	else
		Sky_ProcessTextureChains (cbx, color, &skypolys);

	Sky_ProcessEntities (cbx, color);

	// Process KD map sky surfaces
	KD_DrawSkySurfaces (cbx, color);

	// For KD maps without BSP sky geometry, draw a sky dome
	if (KD_ShouldSkipBSPRendering () && r_atmosphere.value && !flat_color && !skybox_cubemap && !skybox_name[0])
	{
		// Always use non-indirect pipeline (index 0) for sky dome - it uses basicvertex_t format
		// The indirect pipeline (index 1) expects worldvertex_t which has a different stride
		float kd_atmosphere_constants[28];
		memcpy (kd_atmosphere_constants, vulkan_globals.view_projection_matrix, 16 * sizeof (float));
		kd_atmosphere_constants[16] = CLAMP (0.0f, color[0], 1.0f);
		kd_atmosphere_constants[17] = CLAMP (0.0f, color[1], 1.0f);
		kd_atmosphere_constants[18] = CLAMP (0.0f, color[2], 1.0f);
		kd_atmosphere_constants[19] = fog_density;
		memcpy (&kd_atmosphere_constants[20], r_refdef.vieworg, sizeof (r_refdef.vieworg));
		kd_atmosphere_constants[23] = cl.time;
		vec3_t kd_sun_dir;
		Sky_ComputeSunDir (kd_sun_dir);
		kd_atmosphere_constants[24] = kd_sun_dir[0];
		kd_atmosphere_constants[25] = kd_sun_dir[1];
		kd_atmosphere_constants[26] = kd_sun_dir[2];
		kd_atmosphere_constants[27] = r_sun_intensity.value;

		R_BindPipeline (cbx, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.sky_atmosphere_pipeline[0]);
		R_PushConstants (cbx, VK_SHADER_STAGE_ALL_GRAPHICS, 0, 28 * sizeof (float), kd_atmosphere_constants);
		Sky_DrawSkyDome (cbx, &skypolys);
	}

	//
	// render slow sky: non-cubemap skybox
	//
	if (!flat_color && !skybox_cubemap && skybox_name[0])
	{
		R_BindPipeline (cbx, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.sky_box_pipeline);
		Sky_DrawSkyBox (cbx, &skypolys);
	}

	//
	// render volumetric clouds (over atmosphere or skybox)
	// Clouds are rendered by re-drawing sky geometry with the clouds pipeline
	//
	if (r_clouds.value && cloud_noise_texture && !flat_color)
	{
		R_BeginDebugUtilsLabel (cbx, "Volumetric Clouds");

		// Clouds push constants: mvp(16) + fog(4) + eye(3) + time(1) + sun(4) + cloud_params(5) = 33 floats
		float clouds_constants[33];
		memcpy (clouds_constants, vulkan_globals.view_projection_matrix, 16 * sizeof (float));
		clouds_constants[16] = CLAMP (0.0f, color[0], 1.0f);
		clouds_constants[17] = CLAMP (0.0f, color[1], 1.0f);
		clouds_constants[18] = CLAMP (0.0f, color[2], 1.0f);
		clouds_constants[19] = fog_density;
		memcpy (&clouds_constants[20], r_refdef.vieworg, sizeof (r_refdef.vieworg));
		clouds_constants[23] = cl.time;

		vec3_t sun_dir;
		Sky_ComputeSunDir (sun_dir);
		clouds_constants[24] = sun_dir[0];
		clouds_constants[25] = sun_dir[1];
		clouds_constants[26] = sun_dir[2];
		clouds_constants[27] = r_sun_intensity.value;
		clouds_constants[28] = r_clouds_height.value;
		clouds_constants[29] = r_clouds_thickness.value;
		clouds_constants[30] = r_clouds_density.value;
		clouds_constants[31] = r_clouds_coverage.value;
		clouds_constants[32] = r_clouds_speed.value;

		R_BindPipeline (cbx, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.sky_clouds_pipeline[indirect ? 1 : 0]);
		vkCmdBindDescriptorSets (
			cbx->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.sky_clouds_pipeline_layout.handle, 0, 1, &cloud_noise_texture->descriptor_set, 0, NULL);
		R_PushConstants (cbx, VK_SHADER_STAGE_ALL_GRAPHICS, 0, 33 * sizeof (float), clouds_constants);

		// Re-draw sky geometry with clouds pipeline (alpha blended over atmosphere)
		if (indirect)
		{
			R_DrawIndirectBrushes (cbx, false, false, true, -1);
		}
		else
		{
			// Re-process sky texture chains with the clouds pipeline bound
			Sky_ProcessTextureChains (cbx, color, &skypolys);
		}

		// Also process entities and KD sky with clouds
		Sky_ProcessEntities (cbx, color);
		KD_DrawSkySurfaces (cbx, color);

		// Draw clouds on sky dome for KD maps
		if (KD_ShouldSkipBSPRendering () && !skybox_cubemap && !skybox_name[0])
		{
			// Always use non-indirect pipeline (index 0) for sky dome - it uses basicvertex_t format
			R_BindPipeline (cbx, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.sky_clouds_pipeline[0]);
			R_PushConstants (cbx, VK_SHADER_STAGE_ALL_GRAPHICS, 0, 33 * sizeof (float), clouds_constants);
			Sky_DrawSkyDome (cbx, &skypolys);
		}

		R_EndDebugUtilsLabel (cbx);
	}

	Atomic_AddUInt32 (&rs_skypolys, skypolys);

	R_EndDebugUtilsLabel (cbx);

	// Restore indirect mode
	indirect = saved_indirect;
}

/*
==============
Sky_NeedStencil
==============
*/
qboolean Sky_NeedStencil ()
{
	const qboolean flat_color = r_fastsky.value || (Fog_GetDensity () > 0 && skyfog >= 1);
	return !flat_color && !skybox_cubemap && skybox_name[0];
}
