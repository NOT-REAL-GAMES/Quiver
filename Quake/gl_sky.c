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
#include "octree_engine.h"
#include "cloud_coordinates.h"
#include "sky_profile.h"
#include "sky_atmosphere.h"

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

extern cvar_t r_showtris;
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
cvar_t r_clouds_resolution = {"r_clouds_resolution", "2", CVAR_ARCHIVE};
cvar_t r_clouds_filter = {"r_clouds_filter", "0", CVAR_ARCHIVE};
// Experimental measurements remain local to the current run.
cvar_t r_clouds_cascades = {"r_clouds_cascades", "0", CVAR_NONE};
cvar_t r_clouds_near_resolution = {"r_clouds_near_resolution", "2", CVAR_NONE};
cvar_t r_clouds_far_resolution = {"r_clouds_far_resolution", "4", CVAR_NONE};
cvar_t r_clouds_split = {"r_clouds_split", "4096", CVAR_NONE};
cvar_t r_clouds_band = {"r_clouds_band", "0", CVAR_NONE};			   // 0 all, 1 near, 2 far
cvar_t r_clouds_density = {"r_clouds_density", "0.5", CVAR_NONE};	   // Cloud opacity
cvar_t r_clouds_coverage = {"r_clouds_coverage", "0.4", CVAR_NONE};	   // Sky coverage amount
cvar_t r_clouds_speed = {"r_clouds_speed", "0.01", CVAR_NONE};		   // Wind animation speed
cvar_t r_clouds_height = {"r_clouds_height", "2000", CVAR_NONE};	   // Cloud layer base height (Quake units)
cvar_t r_clouds_thickness = {"r_clouds_thickness", "1500", CVAR_NONE}; // Cloud layer thickness
cvar_t r_clouds_scale = {"r_clouds_scale", "0.000075", CVAR_NONE};	   // Cloud UV scale (smaller = larger clouds)

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
		const int noise_size = 256;
		byte	 *noise_data = Noise_Generate3DTexture (noise_size);
		if (noise_data)
		{
			cloud_noise_texture = TexMgr_LoadImage3D (
				NULL, "cloud_noise", noise_size, noise_size, noise_size, SRC_R8_3D, noise_data,
				TEXPREF_PERSIST | (r_clouds_filter.value ? TEXPREF_LINEAR : TEXPREF_NEAREST));
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
static void Sky_CloudFilterChanged (cvar_t *var)
{
	if (!cloud_noise_texture)
		return;
	GL_WaitForDeviceIdle ();
	cloud_noise_texture->flags =
		static_cast<textureflags_t> ((cloud_noise_texture->flags & ~(TEXPREF_LINEAR | TEXPREF_NEAREST)) | (var->value ? TEXPREF_LINEAR : TEXPREF_NEAREST));
	TexMgr_UpdateTextureDescriptorSets ();
}

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
	Sky_AtmosphereInit ();
	Cvar_RegisterVariable (&r_atmosphere_quality);
	Cvar_RegisterVariable (&r_sun_angle);
	Cvar_RegisterVariable (&r_sun_azimuth);
	Cvar_RegisterVariable (&r_sun_intensity);

	// Volumetric cloud cvars
	Cvar_RegisterVariable (&r_clouds);
	Cvar_RegisterVariable (&r_clouds_resolution);
	Cvar_RegisterVariable (&r_clouds_filter);
	Cvar_SetCallback (&r_clouds_filter, Sky_CloudFilterChanged);
	Cvar_RegisterVariable (&r_clouds_cascades);
	Cvar_RegisterVariable (&r_clouds_near_resolution);
	Cvar_RegisterVariable (&r_clouds_far_resolution);
	Cvar_RegisterVariable (&r_clouds_split);
	Cvar_RegisterVariable (&r_clouds_band);
	SkyProfile_Init ();
	Cvar_RegisterVariable (&r_clouds_density);
	Cvar_RegisterVariable (&r_clouds_coverage);
	Cvar_RegisterVariable (&r_clouds_speed);
	Cvar_RegisterVariable (&r_clouds_height);
	Cvar_RegisterVariable (&r_clouds_thickness);
	Cvar_RegisterVariable (&r_clouds_scale);

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
	// Generate the 256^3 R8 cloud-noise texture (16 MiB).
	if (cloud_noise_texture)
	{
		TexMgr_FreeTexture (cloud_noise_texture);
		cloud_noise_texture = NULL;
	}

	const int noise_size = 256;
	byte	 *noise_data = Noise_Generate3DTexture (noise_size);
	if (noise_data)
	{
		cloud_noise_texture = TexMgr_LoadImage3D (
			NULL, "cloud_noise", noise_size, noise_size, noise_size, SRC_R8_3D, noise_data,
			TEXPREF_PERSIST | (r_clouds_filter.value ? TEXPREF_LINEAR : TEXPREF_NEAREST));
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
static void Sky_ProcessEntities (cb_context_t *cbx, float color[3], bool sky_indirect)
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

		if (sky_indirect && R_IndirectBrush (e))
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

	const float half_side = R_FarClip () / sqrtf (3.0f);
	b[0] = s * half_side;
	b[1] = t * half_side;
	b[2] = half_side;

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
static void Sky_DrawSkyDome (cb_context_t *cbx, int *skypolys, const float *flat_color = nullptr)
{
	constexpr int slices = 16;
	constexpr int stacks = 8;
	constexpr int num_vertices = (slices + 1) * (stacks + 1);
	constexpr int num_indices = slices * stacks * 6;
	const bool	  full_sphere = cl.worldmodel && Mod_IsVirtualWorld (cl.worldmodel->name);
	const float	  radius = q_min (R_FarClip (), 4096.0f);

	struct dome_mesh_t
	{
		basicvertex_t vertices[num_vertices];
		uint16_t	  indices[num_indices];
	};

	// Cache both the legacy hemisphere and a complete empty-world sky.
	static const std::array<dome_mesh_t, 2> meshes = [] ()
	{
		std::array<dome_mesh_t, 2> result = {};
		for (int sphere = 0; sphere < 2; ++sphere)
		{
			dome_mesh_t &mesh = result[sphere];
			for (int stack = 0; stack <= stacks; stack++)
			{
				float phi = sphere ? -M_PI / 2.0f + M_PI * stack / stacks : (M_PI / 2.0f) * stack / stacks;
				float z = sinf (phi);
				float r = cosf (phi);
				for (int slice = 0; slice <= slices; slice++)
				{
					float		   theta = (2.0f * M_PI) * slice / slices;
					basicvertex_t *vertex = &mesh.vertices[stack * (slices + 1) + slice];
					vertex->position[0] = r * cosf (theta);
					vertex->position[1] = r * sinf (theta);
					vertex->position[2] = z;
					vertex->texcoord[0] = (float)slice / slices;
					vertex->texcoord[1] = (float)stack / stacks;
					memset (vertex->color, 255, sizeof (vertex->color));
				}
			}
			int index = 0;
			for (int stack = 0; stack < stacks; stack++)
			{
				for (int slice = 0; slice < slices; slice++)
				{
					uint16_t bottom = stack * (slices + 1) + slice;
					uint16_t top = bottom + slices + 1;
					// The camera sees the dome from inside. This winding matches
					// the sky pipelines' clockwise front faces after projection.
					mesh.indices[index++] = bottom;
					mesh.indices[index++] = top + 1;
					mesh.indices[index++] = top;
					mesh.indices[index++] = bottom;
					mesh.indices[index++] = bottom + 1;
					mesh.indices[index++] = top + 1;
				}
			}
		}
		return result;
	}();
	const dome_mesh_t &mesh = meshes[full_sphere ? 1 : 0];

	VkBuffer	   vertex_buffer, index_buffer;
	VkDeviceSize   vertex_offset, index_offset;
	basicvertex_t *vertices = (basicvertex_t *)R_VertexAllocate (sizeof (mesh.vertices), &vertex_buffer, &vertex_offset);
	memcpy (vertices, mesh.vertices, sizeof (mesh.vertices));
	for (int i = 0; i < num_vertices; i++)
		for (int axis = 0; axis < 3; ++axis)
		{
			vertices[i].position[axis] = vertices[i].position[axis] * radius + r_origin[axis];
			if (flat_color)
				vertices[i].color[axis] = static_cast<byte> (CLAMP (0.0f, flat_color[axis], 1.0f) * 255.0f);
		}

	uint16_t *indices = (uint16_t *)R_IndexAllocate (sizeof (mesh.indices), &index_buffer, &index_offset);
	memcpy (indices, mesh.indices, sizeof (mesh.indices));
	vkCmdBindVertexBuffers (cbx->cb, 0, 1, &vertex_buffer, &vertex_offset);
	vkCmdBindIndexBuffer (cbx->cb, index_buffer, index_offset, VK_INDEX_TYPE_UINT16);
	// This generated backdrop has no physical surface. Keep its screen position
	// and view direction, but put it at reverse-Z far depth even at wide FOVs.
	// Real BSP portals continue to test and write their actual geometry depth.
	float backdrop[16];
	memcpy (backdrop, vulkan_globals.view_projection_matrix, sizeof (backdrop));
	backdrop[2] = backdrop[6] = backdrop[10] = backdrop[14] = 0.0f;
	R_PushConstants (cbx, VK_SHADER_STAGE_ALL_GRAPHICS, 0, sizeof (backdrop), backdrop);
	vkCmdDrawIndexed (cbx->cb, num_indices, 1, 0, 0, 0);
	R_PushConstants (cbx, VK_SHADER_STAGE_ALL_GRAPHICS, 0, sizeof (backdrop), vulkan_globals.view_projection_matrix);
	vkCmdBindIndexBuffer (cbx->cb, vulkan_globals.fan_index_buffer, 0, VK_INDEX_TYPE_UINT16);
	*skypolys += slices * stacks;
}

/*
==============
Sky_DrawSkyBox

FIXME: eliminate cracks by adding an extra vert on tjuncs
==============
*/
static void Sky_DrawSkyBox (cb_context_t *cbx, int *skypolys, bool sky_indirect)
{
	int i;

	for (i = 0; i < 6; i++)
	{
		if (sky_indirect) // Don't have bounds for the world polys. This type of sky (malformed cube) is very rare, so just draw the entire box (will
					  // be stencil-culled). Also note tjunctions avoidance below: this only culled entire cube faces in the first place.
		{
			skymins[0][i] = skymins[1][i] = -1;
			skymaxs[0][i] = skymaxs[1][i] = 1;
		}

		if (skymins[0][i] >= skymaxs[0][i] || skymins[1][i] >= skymaxs[1][i])
			continue;

		vkCmdBindDescriptorSets (
			cbx->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.sky_stencil_pipeline[sky_indirect].layout.handle, 0, 1,
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

// Build the sampling eye from canonical coordinates, independently of local
// view/depth reconstruction. Wrapped planets fit whole cloud periods around
// their extent, so crossing the zero seam preserves the same density field.
static qboolean Sky_CloudSamplingEye (float eye[3], float *scale)
{
	const auto					  *world = OW_GetWorld ();
	const int64_t				   extent = world && world->Config ().generator == quiver::Generator::Planet ? world->Config ().planetSettings.extentChunks : 0;
	const quiver::CloudCoordinates coordinates (*scale, extent);
	*scale = coordinates.scale;
	if (!OW_Active ())
		return true;
	const quiver::WorldPosition position = OW_GetRenderPosition ();
	// The atmosphere is a finite band around world altitude zero. At enormous
	// altitudes its thickness is below the renderer's depth precision.
	if (position.chunk.z < -1024 || position.chunk.z > 1024)
		return false;
	for (int axis = 0; axis < 2; ++axis)
		eye[axis] = coordinates.Position (position.chunk[axis], position.local[axis]);
	eye[2] = static_cast<float> (position.chunk.z * quiver::ChunkSize) + position.local[2];
	return true;
}

static float Sky_DisplayScale ()
{
	// Match the final postprocess contrast clamp. Quantize down so cloud fog's
	// spare alpha byte can carry the same safe highlight headroom as atmosphere.
	return floorf (255.0f / CLAMP (1.0f, vid_contrast.value, 2.0f)) / 255.0f;
}

static bool Sky_FlatColor ()
{
	return r_fastsky.value || (Fog_GetDensity () > 0 && skyfog >= 1);
}

static bool Sky_LightmapView (bool virtual_sky)
{
	return r_lightmap_cheatsafe || (virtual_sky && r_world_debug_lighting == WORLD_DEBUG_LIGHTMAP);
}

static bool Sky_HasDome (bool virtual_sky)
{
	return r_drawworld_cheatsafe &&
		   (virtual_sky || (KD_ShouldSkipBSPRendering () && r_atmosphere.value && !Sky_FlatColor () && !skybox_cubemap && !skybox_name[0]));
}

// r_showtris is an overlay. The cloud volume has no mesh; show the generated
// sky's actual triangles using the same x-ray/visible-edge modes as BSP.
void Sky_DrawShowTris (cb_context_t *cbx)
{
	const bool virtual_sky = cl.worldmodel && Mod_IsVirtualWorld (cl.worldmodel->name);
	if (!cl.worldmodel || Sky_LightmapView (virtual_sky) || !Sky_HasDome (virtual_sky))
		return;
	R_BindPipeline (
		cbx, VK_PIPELINE_BIND_POINT_GRAPHICS, r_showtris.value == 1 ? vulkan_globals.showtris_pipeline : vulkan_globals.showtris_depth_test_pipeline);
	int skypolys = 0;
	Sky_DrawSkyDome (cbx, &skypolys);
}

/*
==============
Sky_DrawSky

called once per frame after opaques before transparents, handles world + entities
==============
*/
static void Sky_DrawSkyContents (cb_context_t *cbx)
{
	int i;

	const qboolean virtual_sky = cl.worldmodel && Mod_IsVirtualWorld (cl.worldmodel->name);
	if (!cl.worldmodel || Sky_LightmapView (virtual_sky))
		return;
	const qboolean use_atmosphere = r_atmosphere.value || (virtual_sky && (!solidskytexture || !alphaskytexture));

	// For KD maps, force non-indirect mode to ensure correct vertex format
	// The indirect pipelines use worldvertex_t (28 bytes) but KD sky dome uses basicvertex_t (24 bytes)
	const bool sky_indirect = indirect && !KD_ShouldSkipBSPRendering () && !virtual_sky;

	R_BeginDebugUtilsLabel (cbx, "Sky");

	const qboolean flat_color = Sky_FlatColor ();
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
		if (sky_indirect)
			constant_values[19] = 1.0f;
		R_BindPipeline (cbx, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.sky_color_pipeline[sky_indirect]);
		R_PushConstants (cbx, VK_SHADER_STAGE_ALL_GRAPHICS, 0, 20 * sizeof (float), constant_values);
	}
	else if (skybox_cubemap)
	{
		R_BindPipeline (cbx, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.sky_cube_pipeline[sky_indirect]);
		vkCmdBindDescriptorSets (
			cbx->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.sky_cube_pipeline[sky_indirect].layout.handle, 0, 1, &skybox_cubemap->descriptor_set, 0, NULL);
		memcpy (&constant_values[20], r_refdef.vieworg, sizeof (r_refdef.vieworg));
		R_PushConstants (cbx, VK_SHADER_STAGE_ALL_GRAPHICS, 0, 23 * sizeof (float), constant_values);
	}
	else if (!skybox_name[0])
	{
		// Use atmospheric sky if r_atmosphere is enabled
		if (use_atmosphere)
		{
			float atmosphere_constants[29];
			memcpy (atmosphere_constants, vulkan_globals.view_projection_matrix, 16 * sizeof (float));
			atmosphere_constants[16] = CLAMP (0.0f, color[0], 1.0f); // fog_color
			atmosphere_constants[17] = CLAMP (0.0f, color[1], 1.0f);
			atmosphere_constants[18] = CLAMP (0.0f, color[2], 1.0f);
			atmosphere_constants[19] = fog_density;
			memcpy (&atmosphere_constants[20], r_refdef.vieworg, sizeof (r_refdef.vieworg)); // eye_pos
			atmosphere_constants[23] = Sky_DisplayScale ();

			vec3_t sun_dir;
			Sky_ComputeSunDir (sun_dir);
			atmosphere_constants[24] = sun_dir[0]; // sun_dir
			atmosphere_constants[25] = sun_dir[1];
			atmosphere_constants[26] = sun_dir[2];
			atmosphere_constants[27] = r_sun_intensity.value; // sun_intensity
			atmosphere_constants[28] = Sky_AtmosphereUseLUT () ? 1.0f : 0.0f;

			R_BindPipeline (cbx, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.sky_atmosphere_pipeline[sky_indirect]);
			Sky_AtmosphereBind (cbx);
			R_PushConstants (cbx, VK_SHADER_STAGE_ALL_GRAPHICS, 0, 29 * sizeof (float), atmosphere_constants);
		}
		else
		{
			// Fallback to scrolling sky layer
			if (!solidskytexture || !alphaskytexture)
			{
				R_EndDebugUtilsLabel (cbx);
				return;
			}
			R_BindPipeline (cbx, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.sky_layer_pipeline[sky_indirect]);
			VkDescriptorSet descriptor_sets[2] = {solidskytexture->descriptor_set, alphaskytexture->descriptor_set};
			vkCmdBindDescriptorSets (
				cbx->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.sky_layer_pipeline[sky_indirect].layout.handle, 0, 2, descriptor_sets, 0, NULL);
			memcpy (&constant_values[20], r_refdef.vieworg, sizeof (r_refdef.vieworg));
			constant_values[23] = cl.time - (int)cl.time / 16 * 16;
			constant_values[24] = r_skyalpha.value;
			R_PushConstants (cbx, VK_SHADER_STAGE_ALL_GRAPHICS, 0, 25 * sizeof (float), constant_values);
		}
	}
	else
	{
		R_BindPipeline (cbx, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.sky_stencil_pipeline[sky_indirect]);
		R_PushConstants (cbx, VK_SHADER_STAGE_ALL_GRAPHICS, 0, 20 * sizeof (float), constant_values);
	}
	vkCmdBindIndexBuffer (cbx->cb, vulkan_globals.fan_index_buffer, 0, VK_INDEX_TYPE_UINT16);

	//
	// process world and bmodels: draw flat-shaded sky surfs, and update skybounds
	//
	int skypolys = 0;
	if (sky_indirect)
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
			if (use_atmosphere)
				R_BindPipeline (cbx, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.sky_atmosphere_pipeline[0]);
			else
				R_BindPipeline (cbx, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.sky_layer_pipeline[0]);
		}
		else
			R_BindPipeline (cbx, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.sky_stencil_pipeline[0]);
	}
	else
		Sky_ProcessTextureChains (cbx, color, &skypolys);

	Sky_ProcessEntities (cbx, color, sky_indirect);

	// Process KD map sky surfaces
	if (r_drawworld_cheatsafe)
		KD_DrawSkySurfaces (cbx, color);

	if (virtual_sky && Sky_HasDome (virtual_sky))
	{
		Sky_DrawSkyDome (cbx, &skypolys, flat_color ? color : nullptr);
		for (i = 0; i < 6; ++i)
		{
			skymins[0][i] = skymins[1][i] = -1.0f;
			skymaxs[0][i] = skymaxs[1][i] = 1.0f;
		}
	}

	// For KD maps without BSP sky geometry, draw a sky dome
	if (!virtual_sky && Sky_HasDome (virtual_sky))
	{
		// Always use non-indirect pipeline (index 0) for sky dome - it uses basicvertex_t format
		// The indirect pipeline (index 1) expects worldvertex_t which has a different stride
		float kd_atmosphere_constants[29];
		memcpy (kd_atmosphere_constants, vulkan_globals.view_projection_matrix, 16 * sizeof (float));
		kd_atmosphere_constants[16] = CLAMP (0.0f, color[0], 1.0f);
		kd_atmosphere_constants[17] = CLAMP (0.0f, color[1], 1.0f);
		kd_atmosphere_constants[18] = CLAMP (0.0f, color[2], 1.0f);
		kd_atmosphere_constants[19] = fog_density;
		memcpy (&kd_atmosphere_constants[20], r_refdef.vieworg, sizeof (r_refdef.vieworg));
		kd_atmosphere_constants[23] = Sky_DisplayScale ();
		vec3_t kd_sun_dir;
		Sky_ComputeSunDir (kd_sun_dir);
		kd_atmosphere_constants[24] = kd_sun_dir[0];
		kd_atmosphere_constants[25] = kd_sun_dir[1];
		kd_atmosphere_constants[26] = kd_sun_dir[2];
		kd_atmosphere_constants[27] = r_sun_intensity.value;
		kd_atmosphere_constants[28] = Sky_AtmosphereUseLUT () ? 1.0f : 0.0f;

		R_BindPipeline (cbx, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.sky_atmosphere_pipeline[0]);
		Sky_AtmosphereBind (cbx);
		R_PushConstants (cbx, VK_SHADER_STAGE_ALL_GRAPHICS, 0, 29 * sizeof (float), kd_atmosphere_constants);
		Sky_DrawSkyDome (cbx, &skypolys);
	}

	//
	// render slow sky: non-cubemap skybox
	//
	if (!flat_color && !skybox_cubemap && skybox_name[0])
	{
		R_BindPipeline (cbx, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.sky_box_pipeline);
		Sky_DrawSkyBox (cbx, &skypolys, sky_indirect);
	}

	Atomic_AddUInt32 (&rs_skypolys, skypolys);

	R_EndDebugUtilsLabel (cbx);

}

void Sky_DrawSky (cb_context_t *cbx)
{
	// The main render pass permits secondary commands only. Record its sky
	// timestamps here, including balanced markers around all early returns.
	SkyProfile_BeginPass (cbx->cb, SKY_PROFILE_ATMOSPHERE);
	Sky_DrawSkyContents (cbx);
	SkyProfile_EndPass (cbx->cb, SKY_PROFILE_ATMOSPHERE);
}

/*
==============
Sky_DrawClouds

Composite the volume after opaque geometry and before transparent surfaces.
The full-screen pass clips every ray to scene depth, including terrain.
==============
*/
static float			 cloud_frame_constants[32];
static VkDescriptorSet	 cloud_frame_uniform_sets[3];
static uint32_t			 cloud_frame_uniform_offsets[3];
static sky_cloud_stats_t cloud_frame_stats;

sky_cloud_stats_t Sky_GetCloudStats (void)
{
	return cloud_frame_stats;
}

// Record the expensive trace into the reduced target between opaque rendering
// and the depth-aware composition secondary command buffer.
void Sky_RenderCloudsReduced (cb_context_t *cbx, int band)
{
	const int  width = band == 2 ? cloud_frame_stats.far_width : (band == 1 ? cloud_frame_stats.near_width : cloud_frame_stats.width);
	const int  height = band == 2 ? cloud_frame_stats.far_height : (band == 1 ? cloud_frame_stats.near_height : cloud_frame_stats.height);
	VkViewport viewport = {};
	viewport.width = width;
	viewport.height = height;
	viewport.maxDepth = 1.0f;
	VkRect2D scissor = {{0, 0}, {static_cast<uint32_t> (width), static_cast<uint32_t> (height)}};
	vkCmdSetViewport (cbx->cb, 0, 1, &viewport);
	vkCmdSetScissor (cbx->cb, 0, 1, &scissor);
	R_BindPipeline (cbx, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.sky_clouds_reduced_pipeline);
	VkDescriptorSet sets[3] = {cloud_noise_texture->descriptor_set, vulkan_globals.clouds_sampled_depth_set, cloud_frame_uniform_sets[band]};
	vkCmdBindDescriptorSets (
		cbx->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.sky_clouds_reduced_layout.handle, 0, 3, sets, 1, &cloud_frame_uniform_offsets[band]);
	R_PushConstants (cbx, VK_SHADER_STAGE_ALL_GRAPHICS, 0, sizeof (cloud_frame_constants), cloud_frame_constants);
	vkCmdDraw (cbx->cb, 3, 1, 0, 0);
}

static void Sky_AllocateCloudUniform (const float values[16], int index)
{
	VkBuffer buffer;
	byte	*destination = R_UniformAllocate (16 * sizeof (float), &buffer, &cloud_frame_uniform_offsets[index], &cloud_frame_uniform_sets[index]);
	memcpy (destination, values, 16 * sizeof (float));
}

void Sky_DrawClouds (cb_context_t *cbx)
{
	cloud_frame_stats = {};
	cloud_frame_stats.allocated_bytes = vulkan_globals.clouds_allocated_bytes;
	vulkan_globals.clouds_reduced_active = false;
	if (!cl.worldmodel || !r_drawworld_cheatsafe)
		return;
	const qboolean virtual_sky = Mod_IsVirtualWorld (cl.worldmodel->name);
	if (Sky_LightmapView (virtual_sky) || Sky_FlatColor ())
		return;
	const float	   fog_density = (Fog_GetDensity () > 0) ? skyfog : 0.0f;
	float		   color[4];
	if (Fog_GetDensity () > 0)
		Fog_GetColor (color);
	else
		memcpy (color, skyflatcolor, 3 * sizeof (float));

	float cloud_eye[3];
	VectorCopy (r_refdef.vieworg, cloud_eye);
	float		   cloud_scale = r_clouds_scale.value;
	const qboolean clouds_reachable = Sky_CloudSamplingEye (cloud_eye, &cloud_scale);
	if (clouds_reachable && r_clouds.value && r_clouds_density.value > 0.0f && r_clouds_coverage.value > 0.0f && r_clouds_thickness.value > 0.0f &&
		cloud_noise_texture)
	{
		R_BeginDebugUtilsLabel (cbx, "Volumetric Clouds");

		// Pack fog RGB and display headroom into RGBA8 to retain the 128-byte push constant range.
		// Keep the cloud sampling eye separate from local view reconstruction.
		float clouds_constants[32];
		static_assert (sizeof (clouds_constants) <= MAX_PUSH_CONSTANT_SIZE, "Cloud constants exceed the renderer's push constant buffer");
		memcpy (clouds_constants, vulkan_globals.view_projection_matrix, 16 * sizeof (float));
		uint32_t packed_fog = 0;
		for (int i = 0; i < 3; i++)
			packed_fog |= (uint32_t)(CLAMP (0.0f, color[i], 1.0f) * 255.0f + 0.5f) << (i * 8);
		packed_fog |= (uint32_t)(Sky_DisplayScale () * 255.0f + 0.5f) << 24;
		memcpy (&clouds_constants[16], &packed_fog, sizeof (packed_fog));
		clouds_constants[17] = fog_density;
		clouds_constants[18] = r_clouds_speed.value;
		clouds_constants[19] = cloud_scale;
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
		clouds_constants[31] = std::isfinite (r_clouds_coverage.value) ? CLAMP (0.0f, r_clouds_coverage.value, 1.0f) : 0.5f;
		// Coverage is nonnegative. Its sign bit carries the world-curvature flag
		// without expanding the guaranteed 128-byte cloud push-constant range.
		if (r_planet_bend_active)
			clouds_constants[31] = -fabsf (clouds_constants[31]);
		if (virtual_sky)
		{
			// Only the sampling eye moves to the periodic cloud frame. Scene-depth
			// reconstruction remains in the unchanged local render frame.
			memcpy (&clouds_constants[20], cloud_eye, sizeof (cloud_eye));
		}

		const int	   divisor = r_clouds_resolution.value >= 4 ? 4 : (r_clouds_resolution.value >= 2 ? 2 : 1);
		const qboolean reduced = divisor > 1 && vulkan_globals.clouds_reduced_image != VK_NULL_HANDLE;
		const qboolean cascades = reduced && r_clouds_cascades.value != 0 && vulkan_globals.clouds_far_image != VK_NULL_HANDLE;
		cloud_frame_stats.reduced = reduced;
		cloud_frame_stats.cascades = cascades;
		cloud_frame_stats.band = reduced ? (r_clouds_band.value >= 2 ? 2 : (r_clouds_band.value >= 1 ? 1 : 0)) : 0;
		cloud_frame_stats.split = std::isfinite (r_clouds_split.value) ? q_max (1.0f, r_clouds_split.value) : 4096.0f;
		cloud_frame_stats.near_divisor = cascades ? (r_clouds_near_resolution.value >= 4 ? 4 : 2) : (reduced ? divisor : 1);
		cloud_frame_stats.far_divisor = cascades ? (r_clouds_far_resolution.value >= 4 ? 4 : 2) : cloud_frame_stats.near_divisor;
		cloud_frame_stats.near_width = (r_refdef.vrect.width + cloud_frame_stats.near_divisor - 1) / cloud_frame_stats.near_divisor;
		cloud_frame_stats.near_height = (r_refdef.vrect.height + cloud_frame_stats.near_divisor - 1) / cloud_frame_stats.near_divisor;
		cloud_frame_stats.far_width = (r_refdef.vrect.width + cloud_frame_stats.far_divisor - 1) / cloud_frame_stats.far_divisor;
		cloud_frame_stats.far_height = (r_refdef.vrect.height + cloud_frame_stats.far_divisor - 1) / cloud_frame_stats.far_divisor;
		const qboolean far_only = cascades && cloud_frame_stats.band == 2;
		cloud_frame_stats.divisor = far_only ? cloud_frame_stats.far_divisor : cloud_frame_stats.near_divisor;
		cloud_frame_stats.width = far_only ? cloud_frame_stats.far_width : cloud_frame_stats.near_width;
		cloud_frame_stats.height = far_only ? cloud_frame_stats.far_height : cloud_frame_stats.near_height;
		vulkan_globals.clouds_reduced_active = reduced;
		if (reduced)
		{
			memcpy (cloud_frame_constants, clouds_constants, sizeof (cloud_frame_constants));
			float uniform[16] = {};
			uniform[0] = r_refdef.vrect.x;
			uniform[1] = vid.height - glheight + r_refdef.vrect.y;
			uniform[2] = r_refdef.vrect.width;
			uniform[3] = r_refdef.vrect.height;
			uniform[4] = cloud_frame_stats.near_width;
			uniform[5] = cloud_frame_stats.near_height;
			uniform[6] = cloud_frame_stats.near_divisor;
			uniform[7] = r_clouds_filter.value != 0;
			uniform[8] = cloud_frame_stats.far_width;
			uniform[9] = cloud_frame_stats.far_height;
			uniform[10] = cloud_frame_stats.far_divisor;
			uniform[11] = uniform[7];
			uniform[12] = cloud_frame_stats.split;
			uniform[13] = cascades;
			uniform[14] = cloud_frame_stats.band;
			uniform[15] = cascades ? 0 : cloud_frame_stats.band;
			Sky_AllocateCloudUniform (uniform, 0);
			if (cascades)
			{
				for (int band = 1; band <= 2; ++band)
				{
					if (cloud_frame_stats.band != 0 && cloud_frame_stats.band != band)
						continue;
					uniform[15] = band;
					Sky_AllocateCloudUniform (uniform, band);
				}
			}
			R_BindPipeline (cbx, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.sky_clouds_composite_pipeline);
			VkDescriptorSet radiance_set = vulkan_globals.clouds_reduced_set;
			if (cascades && cloud_frame_stats.band == 0)
				radiance_set = vulkan_globals.clouds_cascade_set;
			else if (far_only)
				radiance_set = vulkan_globals.clouds_far_set;
			VkDescriptorSet sets[4] = {cloud_noise_texture->descriptor_set, vulkan_globals.clouds_sampled_depth_set, cloud_frame_uniform_sets[0], radiance_set};
			vkCmdBindDescriptorSets (
				cbx->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.sky_clouds_reduced_layout.handle, 0, 4, sets, 1, &cloud_frame_uniform_offsets[0]);
		}
		else
		{
			R_BindPipeline (cbx, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.sky_clouds_pipeline);
			VkDescriptorSet descriptors[2] = {cloud_noise_texture->descriptor_set, vulkan_globals.clouds_depth_descriptor_set};
			vkCmdBindDescriptorSets (cbx->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.sky_clouds_pipeline_layout.handle, 0, 2, descriptors, 0, NULL);
		}
		R_PushConstants (cbx, VK_SHADER_STAGE_ALL_GRAPHICS, 0, sizeof (clouds_constants), clouds_constants);
		vkCmdDraw (cbx->cb, 3, 1, 0, 0);
		vulkan_globals.clouds_drawn = true;

		R_EndDebugUtilsLabel (cbx);
	}
}

/*
==============
Sky_DrawDepth

Sky portals must still hide later transparent geometry. Delay just their depth
writes until the cloud pass has read real opaque scene depth.
==============
*/
void Sky_DrawDepth (cb_context_t *cbx)
{
	const bool virtual_sky = cl.worldmodel && Mod_IsVirtualWorld (cl.worldmodel->name);
	if (!cl.worldmodel || Sky_LightmapView (virtual_sky))
		return;
	const bool sky_indirect = indirect && !KD_ShouldSkipBSPRendering () && !virtual_sky;
	const bool saved_bounds = need_bounds;
	need_bounds = false;

	R_BeginDebugUtilsLabel (cbx, "Sky Depth");
	R_BindPipeline (cbx, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.sky_depth_pipeline[sky_indirect]);
	R_PushConstants (cbx, VK_SHADER_STAGE_ALL_GRAPHICS, 0, sizeof (vulkan_globals.view_projection_matrix), vulkan_globals.view_projection_matrix);
	vkCmdBindIndexBuffer (cbx->cb, vulkan_globals.fan_index_buffer, 0, VK_INDEX_TYPE_UINT16);
	float color[3] = {0, 0, 0};
	int	  skypolys = 0;
	if (sky_indirect)
	{
		R_DrawIndirectBrushes (cbx, false, false, true, -1);
		R_BindPipeline (cbx, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.sky_depth_pipeline[0]);
		vkCmdBindIndexBuffer (cbx->cb, vulkan_globals.fan_index_buffer, 0, VK_INDEX_TYPE_UINT16);
	}
	else
		Sky_ProcessTextureChains (cbx, color, &skypolys);
	Sky_ProcessEntities (cbx, color, sky_indirect);
	if (r_drawworld_cheatsafe)
		KD_DrawSkySurfaces (cbx, color);
	// A synthetic dome sits at clear depth and needs no depth write.
	R_EndDebugUtilsLabel (cbx);
	need_bounds = saved_bounds;
}

/*
==============
Sky_NeedStencil
==============
*/
qboolean Sky_NeedStencil ()
{
	const qboolean flat_color = Sky_FlatColor ();
	return !flat_color && !skybox_cubemap && skybox_name[0];
}
