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

float Fog_GetDensity (void);
void  Fog_GetColor (float *c);

extern atomic_uint32_t rs_skypolys; // for scr_speeds readout
static float		   skyflatcolor[3];
static float sky_texture_radiance[3];
static qboolean sky_texture_radiance_valid;
static float		   skymins[2][6], skymaxs[2][6];

static gltexture_t *solidskytexture, *alphaskytexture;

extern cvar_t gl_farclip;
cvar_t		  r_fastsky = {"r_fastsky", "0", CVAR_NONE};
cvar_t		  r_sky_quality = {"r_sky_quality", "12", CVAR_NONE};
cvar_t		  r_skyalpha = {"r_skyalpha", "1", CVAR_NONE};
cvar_t		  r_skyfog = {"r_skyfog", "0.5", CVAR_NONE};
cvar_t		  r_skywind = {"r_skywind", "1", CVAR_ARCHIVE};

static const int skytexorder[6] = {0, 2, 1, 3, 4, 5}; // for skybox

static const vec3_t skyclip[6] = {{1, 1, 0}, {1, -1, 0}, {0, -1, 1}, {0, 1, 1}, {1, 0, 1}, {-1, 0, 1}};

static const int st_to_vec[6][3] = {
	{3, -1, 2}, {-3, 1, 2}, {1, 3, 2}, {-1, -3, 2}, {-2, -1, 3}, // straight up
	{2, -1, -3}													 // straight down
};

static const int vec_to_st[6][3] = {{-2, 3, 1}, {2, 3, -1}, {1, 3, 2}, {-1, 3, -2}, {-2, -1, 3}, {-2, 1, -3}};

static float skyfog; // ericw

#define SKYWIND_CFG "wind.cfg"

static SDL_Mutex *load_skytexture_mutex;
static int		  max_skytexture_index = -1;

qboolean need_bounds;

typedef struct
{
	float position[3];
	float texcoord1[2];
	float texcoord2[2];
	byte  color[4];
} skylayervertex_t;

typedef struct skybox_s
{
	char		 name[1024]; // name of current skybox, or "" if no skybox
	char		 name_worldspawn[1024];
	gltexture_t *textures[6];
	gltexture_t *cubemap;
	float		 wind_dist;
	float		 wind_yaw;
	float		 wind_pitch;
	float		 wind_period;
	float radiance[3];
	qboolean radiance_valid;
} skybox_t;

static skybox_t	 skybox;
static skybox_t *skybox_cache; // skyboxes loaded for the current map, to avoid reloading when a map switches between them

/* Approximate the visible sky in linear RGB. This is a small diffuse fallback,
 * never an inferred sun direction or a substitute for authored sky lighting. */
static const float sky_radiance_linear[256] = {
	0.000000000f, 0.000303527f, 0.000607054f, 0.000910581f, 0.001214108f, 0.001517635f, 0.001821162f, 0.002124689f,
	0.002428216f, 0.002731743f, 0.003035270f, 0.003346536f, 0.003676507f, 0.004024717f, 0.004391442f, 0.004776953f,
	0.005181517f, 0.005605392f, 0.006048833f, 0.006512091f, 0.006995410f, 0.007499032f, 0.008023193f, 0.008568126f,
	0.009134059f, 0.009721217f, 0.010329823f, 0.010960094f, 0.011612245f, 0.012286488f, 0.012983032f, 0.013702083f,
	0.014443844f, 0.015208514f, 0.015996293f, 0.016807376f, 0.017641954f, 0.018500220f, 0.019382361f, 0.020288563f,
	0.021219010f, 0.022173885f, 0.023153366f, 0.024157632f, 0.025186860f, 0.026241222f, 0.027320892f, 0.028426040f,
	0.029556834f, 0.030713444f, 0.031896033f, 0.033104767f, 0.034339807f, 0.035601315f, 0.036889450f, 0.038204372f,
	0.039546235f, 0.040915197f, 0.042311411f, 0.043735029f, 0.045186204f, 0.046665086f, 0.048171824f, 0.049706566f,
	0.051269458f, 0.052860647f, 0.054480276f, 0.056128490f, 0.057805430f, 0.059511238f, 0.061246054f, 0.063010018f,
	0.064803267f, 0.066625939f, 0.068478170f, 0.070360096f, 0.072271851f, 0.074213568f, 0.076185381f, 0.078187422f,
	0.080219820f, 0.082282707f, 0.084376212f, 0.086500462f, 0.088655586f, 0.090841711f, 0.093058963f, 0.095307467f,
	0.097587347f, 0.099898728f, 0.102241733f, 0.104616484f, 0.107023103f, 0.109461711f, 0.111932428f, 0.114435374f,
	0.116970668f, 0.119538428f, 0.122138772f, 0.124771818f, 0.127437680f, 0.130136477f, 0.132868322f, 0.135633330f,
	0.138431615f, 0.141263291f, 0.144128471f, 0.147027266f, 0.149959790f, 0.152926152f, 0.155926464f, 0.158960835f,
	0.162029376f, 0.165132195f, 0.168269400f, 0.171441101f, 0.174647404f, 0.177888416f, 0.181164244f, 0.184474995f,
	0.187820772f, 0.191201683f, 0.194617830f, 0.198069320f, 0.201556254f, 0.205078736f, 0.208636870f, 0.212230757f,
	0.215860500f, 0.219526200f, 0.223227957f, 0.226965874f, 0.230740049f, 0.234550582f, 0.238397574f, 0.242281122f,
	0.246201327f, 0.250158285f, 0.254152094f, 0.258182853f, 0.262250658f, 0.266355605f, 0.270497791f, 0.274677312f,
	0.278894263f, 0.283148740f, 0.287440838f, 0.291770650f, 0.296138271f, 0.300543794f, 0.304987314f, 0.309468923f,
	0.313988713f, 0.318546778f, 0.323143209f, 0.327778098f, 0.332451536f, 0.337163615f, 0.341914425f, 0.346704056f,
	0.351532600f, 0.356400144f, 0.361306780f, 0.366252596f, 0.371237680f, 0.376262123f, 0.381326011f, 0.386429434f,
	0.391572478f, 0.396755231f, 0.401977780f, 0.407240212f, 0.412542613f, 0.417885071f, 0.423267670f, 0.428690497f,
	0.434153636f, 0.439657174f, 0.445201195f, 0.450785783f, 0.456411023f, 0.462077000f, 0.467783796f, 0.473531496f,
	0.479320183f, 0.485149940f, 0.491020850f, 0.496932995f, 0.502886458f, 0.508881321f, 0.514917665f, 0.520995573f,
	0.527115126f, 0.533276404f, 0.539479489f, 0.545724461f, 0.552011402f, 0.558340390f, 0.564711506f, 0.571124829f,
	0.577580440f, 0.584078418f, 0.590618841f, 0.597201788f, 0.603827339f, 0.610495571f, 0.617206562f, 0.623960392f,
	0.630757136f, 0.637596874f, 0.644479682f, 0.651405637f, 0.658374817f, 0.665387298f, 0.672443157f, 0.679542470f,
	0.686685312f, 0.693871761f, 0.701101892f, 0.708375780f, 0.715693501f, 0.723055129f, 0.730460740f, 0.737910409f,
	0.745404210f, 0.752942217f, 0.760524505f, 0.768151147f, 0.775822218f, 0.783537792f, 0.791297940f, 0.799102738f,
	0.806952258f, 0.814846572f, 0.822785754f, 0.830769877f, 0.838799012f, 0.846873232f, 0.854992608f, 0.863157213f,
	0.871367119f, 0.879622397f, 0.887923118f, 0.896269353f, 0.904661174f, 0.913098652f, 0.921581856f, 0.930110858f,
	0.938685728f, 0.947306537f, 0.955973353f, 0.964686248f, 0.973445290f, 0.982250550f, 0.991102097f, 1.000000000f
};

static float Sky_RadianceChannel (byte value)
{
	return sky_radiance_linear[value];
}

static void Sky_AverageClassicRadiance (const texture_t *texture, qboolean q64)
{
    const byte *pixels = (const byte *)(texture + 1);
    unsigned width = q64 ? texture->width : texture->width / 2;
    unsigned height = q64 ? texture->height / 2 : texture->height;
    if (!width || !height) { sky_texture_radiance_valid = false; return; }
    double sum[3] = {0};
    for (unsigned y = 0; y < height; ++y)
    for (unsigned x = 0; x < width; ++x)
    {
        unsigned front = pixels[(size_t)y * texture->width + x];
        unsigned back = q64 ? pixels[(size_t)(y + height) * texture->width + x] :
            pixels[(size_t)y * texture->width + x + width];
        const byte *f = (const byte *)&d_8to24table[front], *b = (const byte *)&d_8to24table[back];
        float alpha = q64 ? 0.5f : front ? 1.0f : 0.0f;
        for (int channel = 0; channel < 3; ++channel)
            sum[channel] += Sky_RadianceChannel (f[channel]) * alpha + Sky_RadianceChannel (b[channel]) * (1.0f - alpha);
    }
    for (int channel = 0; channel < 3; ++channel)
        sky_texture_radiance[channel] = (float)(sum[channel] / ((double)width * height));
    sky_texture_radiance_valid = true;
}

qboolean Sky_GetRadianceColor (float color[3])
{
    if (!color) return false;
    if (skybox.name[0])
    {
        if (!skybox.radiance_valid) return false;
        memcpy (color, skybox.radiance, sizeof (skybox.radiance));
        return true;
    }
    if (!sky_texture_radiance_valid) return false;
    memcpy (color, sky_texture_radiance, sizeof (sky_texture_radiance));
    return true;
}

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
		Sky_AverageClassicRadiance (mt, false);
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
		Sky_AverageClassicRadiance (mt, true);
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

static bool Skywind_is_enabled (void)
{
	return skybox.name[0] && (r_skywind.value > 0.0f) &&
		   ((skybox.wind_dist != 0.0f) || (skybox.wind_period != 0.0f) || (skybox.wind_pitch != 0.0f) || (skybox.wind_yaw != 0.0f));
}

/*
=================
Skywind_Clear
=================
*/
static void Skywind_Clear (void)
{
	// reset
	skybox.wind_dist = skybox.wind_period = skybox.wind_pitch = skybox.wind_yaw = 0.0f;
}

/*
=================
Skywind_Load_f
=================
*/
static void Skywind_Load_f (void)
{
	char		relname[MAX_QPATH];
	char	   *buf;
	const char *data;

	Skywind_Clear ();

	if (!skybox.name[0])
	{
		Con_Printf ("No skybox loaded\n");
		return;
	}

	q_snprintf (relname, sizeof (relname), "gfx/env/%s" SKYWIND_CFG, skybox.name);
	buf = (char *)COM_LoadFile (relname, NULL);
	if (!buf)
	{
		Con_DPrintf ("Sky wind config not found '%s'.\n", relname);
		return;
	}

	data = COM_Parse (buf);
	if (!data)
		goto done;

	if (strcmp (com_token, "skywind") != 0)
	{
		Con_Printf ("Skywind_Load_f: first token must be 'skywind'.\n");
		goto done;
	}

	if ((data = COM_Parse (data)) != NULL)
		skybox.wind_dist = CLAMP (-2.0, atof (com_token), 2.0);

	if ((data = COM_Parse (data)) != NULL)
		skybox.wind_yaw = fmod (atof (com_token), 360.0);

	if ((data = COM_Parse (data)) != NULL)
		skybox.wind_period = atof (com_token);

	if ((data = COM_Parse (data)) != NULL)
		skybox.wind_pitch = fmod (atof (com_token) + 90.0, 180.0) - 90.0;

done:
	Mem_Free (buf);
}

/*
=================
Skywind_Save_f
=================
*/
static void Skywind_Save_f (void)
{
	char  relname[MAX_QPATH];
	char  path[MAX_OSPATH];
	FILE *f;

	if (!skybox.name[0])
	{
		Con_Printf ("No skybox loaded\n");
		return;
	}

	q_snprintf (relname, sizeof (relname), "gfx/env/%s" SKYWIND_CFG, skybox.name);
	q_snprintf (path, sizeof (path), "%s/%s", com_gamedir, relname);
	f = Sys_fopen (path, "wt");
	if (!f)
	{
		Con_Printf ("Couldn't write '%s'.\n", relname);
		return;
	}

	fprintf (
		f,
		"// distance yaw period pitch\n"
		"skywind %g %g %g %g\n",
		skybox.wind_dist, skybox.wind_yaw, skybox.wind_period, skybox.wind_pitch);

	fclose (f);

	Con_SafePrintf ("Wrote ");
	Con_LinkPrintf (path, "%s", relname);
	Con_SafePrintf ("\n");
}

/*
=================
Skywind_LookDir_f
=================
*/
static void Skywind_LookDir_f (void)
{
	if (cls.state != ca_connected)
		return;

	if (!skybox.name[0])
	{
		Con_Printf ("No skybox loaded\n");
		return;
	}

	// invert view direction so that clouds move towards the player, not away from them
	skybox.wind_yaw = fmod (cl.viewangles[YAW] + 180.0, 360.0);
	skybox.wind_pitch = -cl.viewangles[PITCH];

	// first argument, if present, overrides the loop duration (default: 30 seconds)
	if (Cmd_Argc () >= 2)
		skybox.wind_period = atof (Cmd_Argv (1));
	else if (!skybox.wind_period)
		skybox.wind_period = 30.f;

	// second argument, if present, overrides the amplitude of the movement (default: 1.0)
	if (Cmd_Argc () >= 3)
		skybox.wind_dist = CLAMP (-2.0, atof (Cmd_Argv (2)), 2.0);
	else if (!skybox.wind_dist)
		skybox.wind_dist = 1.f;
}

/*
=================
Skywind_Rotate_f
=================
*/
static void Skywind_Rotate_f (void)
{
	if (cls.state != ca_connected)
		return;

	if (!skybox.name[0])
	{
		Con_Printf ("No skybox loaded\n");
		return;
	}

	if (Cmd_Argc () < 2)
	{
		Con_Printf (
			"usage:\n"
			"   %s <yawdelta> [pitchdelta]\n",
			Cmd_Argv (0));
		return;
	}

	skybox.wind_yaw = fmod (skybox.wind_yaw + atof (Cmd_Argv (1)), 360.0);
	if (Cmd_Argc () >= 3)
		skybox.wind_pitch = fmod (skybox.wind_pitch + atof (Cmd_Argv (2)) + 90.0, 180.0) - 90.0;
}

/*
=================
Skywind_f
=================
*/
static void Skywind_f (void)
{
	if (cls.state != ca_connected)
		return;

	if (!skybox.name[0])
	{
		Con_Printf ("No skybox loaded\n");
		return;
	}

	if (Cmd_Argc () < 2)
	{
		Con_Printf (
			"usage:\n"
			"   %s [distance] [yaw] [period] [pitch]\n"
			"current values:\n"
			"   \"distance\" is \"%g\"\n"
			"   \"yaw\"      is \"%g\"\n"
			"   \"period\"   is \"%g\"\n"
			"   \"pitch\"    is \"%g\"\n",
			Cmd_Argv (0), skybox.wind_dist, skybox.wind_yaw, skybox.wind_period, skybox.wind_pitch);
		return;
	}

	skybox.wind_dist = CLAMP (-2.0, atof (Cmd_Argv (1)), 2.0);
	if (Cmd_Argc () >= 3)
		skybox.wind_yaw = fmod (atof (Cmd_Argv (2)), 360.0);
	if (Cmd_Argc () >= 4)
		skybox.wind_period = atof (Cmd_Argv (3));
	if (Cmd_Argc () >= 5)
		skybox.wind_pitch = fmod (atof (Cmd_Argv (4)) + 90.0, 180.0) - 90.0;
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

	if (strcmp (skybox.name, name) == 0)
		return; // no change

	// keep wind changes made to the outgoing skybox
	for (i = 0; i < (int)VEC_SIZE (skybox_cache); i++)
	{
		if (strcmp (skybox_cache[i].name, skybox.name) == 0)
		{
			skybox_cache[i].wind_dist = skybox.wind_dist;
			skybox_cache[i].wind_yaw = skybox.wind_yaw;
			skybox_cache[i].wind_pitch = skybox.wind_pitch;
			skybox_cache[i].wind_period = skybox.wind_period;
			break;
		}
	}

	// the textures stay alive in the cache
	for (i = 0; i < 6; i++)
		skybox.textures[i] = NULL;
	skybox.cubemap = NULL;
	skybox.radiance_valid = false;
	Skywind_Clear ();

	// turn off skybox if sky is set to ""
	if (name[0] == 0)
	{
		skybox.name[0] = 0;
		return;
	}

	// check if already loaded
	for (i = 0; i < (int)VEC_SIZE (skybox_cache); i++)
	{
		if (strcmp (skybox_cache[i].name, name) == 0)
		{
			q_strlcpy (skybox.name, name, sizeof (skybox.name));
			memcpy (skybox.textures, skybox_cache[i].textures, sizeof (skybox.textures));
			skybox.cubemap = skybox_cache[i].cubemap;
			memcpy (skybox.radiance, skybox_cache[i].radiance, sizeof (skybox.radiance));
			skybox.radiance_valid = skybox_cache[i].radiance_valid;
			skybox.wind_dist = skybox_cache[i].wind_dist;
			skybox.wind_yaw = skybox_cache[i].wind_yaw;
			skybox.wind_pitch = skybox_cache[i].wind_pitch;
			skybox.wind_period = skybox_cache[i].wind_period;
			return;
		}
	}

	// load textures
	for (i = 0; i < 6; i++)
	{
		q_snprintf (filename[i], sizeof (filename[i]), "gfx/env/%s%s", name, suf[i]);
		if ((data[i] = Image_LoadImage (filename[i], &width[i], &height[i], &fmt[i], 0)))
			nonefound = false;
		if (!data[i] || (width[i] != height[i]) || (width[i] != width[0]) || (fmt[i] != SRC_RGBA))
			cubemap = false;
	}

	/* Average the four side faces and upper face, each equally weighted.
	 * The lower face is intentionally excluded from upper-sky illumination. */
	double sky_sum[3] = {0};
	unsigned sky_faces = 0;
	for (i = 0; i < 5; ++i)
	{
		if (!data[i] || width[i] <= 0 || height[i] <= 0 ||
			(fmt[i] != SRC_RGBA && fmt[i] != SRC_INDEXED)) continue;
		size_t count = (size_t)width[i] * height[i];
		double face[3] = {0};
		for (size_t pixel = 0; pixel < count; ++pixel)
		{
			const byte *rgba = fmt[i] == SRC_RGBA ? data[i] + pixel * 4 : (const byte *)&d_8to24table[data[i][pixel]];
			float alpha = fmt[i] == SRC_RGBA ? rgba[3] / 255.0f : 1.0f;
			for (int c = 0; c < 3; ++c) face[c] += Sky_RadianceChannel (rgba[c]) * alpha;
		}
		for (int c = 0; c < 3; ++c) sky_sum[c] += face[c] / count;
		++sky_faces;
	}
	if (sky_faces)
	{
		for (int c = 0; c < 3; ++c) skybox.radiance[c] = (float)(sky_sum[c] / sky_faces);
		skybox.radiance_valid = true;
	}

	if (cubemap)
	{
		q_snprintf (filename[0], sizeof (filename[0]), "gfx/env/%scube", name);
		skybox.cubemap = TexMgr_LoadImage (cl.worldmodel, filename[0], width[0], height[0], SRC_RGBA_CUBEMAP, (byte *)data, filename[0], 0, TEXPREF_NONE);
	}
	else
		for (i = 0; i < 6; i++)
		{
			if (data[i])
				skybox.textures[i] = TexMgr_LoadImage (cl.worldmodel, filename[i], width[i], height[i], fmt[i], data[i], filename[i], 0, TEXPREF_NONE);
			else
			{
				Con_Printf ("Couldn't load %s\n", filename[i]);
				skybox.textures[i] = notexture;
			}
		}

	for (i = 0; i < 6; i++)
		if (data[i])
			Mem_Free (data[i]);

	if (nonefound) // go back to scrolling sky if skybox is totally missing
	{
		for (i = 0; i < 6; i++)
		{
			if (skybox.textures[i] && skybox.textures[i] != notexture)
				TexMgr_FreeTexture (skybox.textures[i]);
			skybox.textures[i] = NULL;
		}
		skybox.name[0] = 0;
		return;
	}

	q_strlcpy (skybox.name, name, sizeof (skybox.name));

	Skywind_Load_f ();

	VEC_PUSH (skybox_cache, skybox);
}

/*
=================
Sky_GetSkyCommand

To preserve dynamic skies in demos and savegames
=================
*/
const char *Sky_GetSkyCommand (qboolean always)
{
	qboolean need_sky = always || strcmp (skybox.name, skybox.name_worldspawn);
	qboolean need_skyfog = always; // no safe way to record skyfog in demos; r_skyfog is user pref

	if (need_sky || need_skyfog)
	{
		char sky[128];
		char fog[128];
		q_strlcpy (sky, va ("sky \"%s\"", skybox.name), sizeof (sky));
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
	skybox.name[0] = 0;

	for (int i = 0; i < 6; i++)
		skybox.textures[i] = NULL;

	skybox.cubemap = NULL;

	// Free cached sky textures before model cleanup, including those loaded
	// while cl.worldmodel was NULL and therefore have no model owner.
	for (int i = 0; i < (int)VEC_SIZE (skybox_cache); i++)
	{
		for (int j = 0; j < 6; j++)
		{
			if (skybox_cache[i].textures[j] && skybox_cache[i].textures[j] != notexture)
				TexMgr_FreeTexture (skybox_cache[i].textures[j]);
		}
		if (skybox_cache[i].cubemap)
			TexMgr_FreeTexture (skybox_cache[i].cubemap);
	}

	VEC_CLEAR (skybox_cache);

	Skywind_Clear ();

	solidskytexture = NULL;
	alphaskytexture = NULL;
	max_skytexture_index = -1;
	sky_texture_radiance_valid = false;
	Cvar_SetQuick (&r_skyfog, r_skyfog.default_string);
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

	q_strlcpy (skybox.name_worldspawn, skybox.name, sizeof (skybox.name_worldspawn));
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
	{
		const char *wind_params_str = (Skywind_is_enabled ()) ? va (", wind dist %.3f yaw %.3f period %.3f pitch %.3f", skybox.wind_dist, skybox.wind_yaw,
																	skybox.wind_period, skybox.wind_pitch)
															  : "";
		Con_Printf ("sky is \"%s\"%s\n", skybox.name, wind_params_str);
	}
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
	Cvar_RegisterVariable (&r_fastsky);
	Cvar_RegisterVariable (&r_sky_quality);
	Cvar_RegisterVariable (&r_skyalpha);
	Cvar_RegisterVariable (&r_skyfog);
	Cvar_SetCallback (&r_skyfog, R_SetSkyfog_f);
	Cvar_RegisterVariable (&r_skywind);

	Cmd_AddCommand ("sky", Sky_SkyCommand_f);
	Cmd_AddCommand ("skywind", Skywind_f);
	Cmd_AddCommand ("skywind_save", Skywind_Save_f);
	Cmd_AddCommand ("skywind_load", Skywind_Load_f);
	Cmd_AddCommand ("skywind_lookdir", Skywind_LookDir_f);
	Cmd_AddCommand ("skywind_rotate", Skywind_Rotate_f);

	load_skytexture_mutex = SDL_CreateMutex ();
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

	for (i = cl.worldmodel->texofs[TEXTYPE_SKY]; i < cl.worldmodel->texofs[TEXTYPE_SKY + 1]; i++)
	{
		t = cl.worldmodel->textures[cl.worldmodel->usedtextures[i]];

		if (!t || !t->texturechains[chain_world])
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
	w = skybox.textures[skytexorder[axis]]->width;
	h = skybox.textures[skytexorder[axis]]->height;
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

		const main_render_pass_variant_t variant = cbx->pipeline_variant;
		vkCmdBindDescriptorSets (
			cbx->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.sky_stencil_pipeline[variant][indirect].layout.handle, 0, 1,
			&skybox.textures[skytexorder[i]]->descriptor_set, 0, NULL);

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
Skywind_UpdateParams
==============
*/
static void Skywind_UpdateParams (float *wind_phase, vec3_t wind_dir)
{
	if (Skywind_is_enabled ())
	{
		float  yaw = DEG2RAD (skybox.wind_yaw);
		float  pitch = DEG2RAD (skybox.wind_pitch);
		float  sy = sinf (yaw);
		float  sp = sinf (pitch);
		float  cy = cosf (yaw);
		float  cp = cosf (pitch);
		float  dist = CLAMP (-2.f, skybox.wind_dist, 2.f);
		float  period = r_skywind.value ? skybox.wind_period / r_skywind.value : 0.0;
		double phase = period ? cl.time * 0.5 / period : 0.5;

		phase -= floor (phase) + 0.5; // [-0.5, 0.5)

		wind_dir[0] = dist * cp * sy;
		wind_dir[1] = dist * sp;
		wind_dir[2] = -dist * cp * cy;
		*wind_phase = phase;
	}
	else
	{
		// reset
		wind_dir[0] = 0.0f;
		wind_dir[1] = 0.0f;
		wind_dir[2] = 0.0f;
		*wind_phase = 0.0f;
	}
}

/*
==============
Sky_DrawSky

called once per frame after opaques before transparents, handles world + entities
==============
*/
void Sky_DrawSky (cb_context_t *cbx)
{
	int								 i;
	const main_render_pass_variant_t variant = cbx->pipeline_variant;

	if (r_lightmap_cheatsafe)
		return;

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

	float constant_values[27];
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
		R_BindPipeline (cbx, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.sky_color_pipeline[variant][indirect]);
		R_PushConstants (cbx, VK_SHADER_STAGE_ALL_GRAPHICS, 0, 20 * sizeof (float), constant_values);
	}
	else if (skybox.cubemap)
	{
		R_BindPipeline (cbx, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.sky_cube_pipeline[variant][indirect]);
		vkCmdBindDescriptorSets (
			cbx->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.sky_cube_pipeline[variant][indirect].layout.handle, 0, 1, &skybox.cubemap->descriptor_set,
			0, NULL);
		memcpy (&constant_values[20], r_refdef.vieworg, sizeof (r_refdef.vieworg));

		Skywind_UpdateParams (&constant_values[23], &constant_values[24]);

		R_PushConstants (cbx, VK_SHADER_STAGE_ALL_GRAPHICS, 0, 27 * sizeof (float), constant_values);
	}
	else if (!skybox.name[0])
	{
		if (!solidskytexture || !alphaskytexture)
		{
			R_EndDebugUtilsLabel (cbx);
			return;
		}
		R_BindPipeline (cbx, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.sky_layer_pipeline[variant][indirect]);
		VkDescriptorSet descriptor_sets[2] = {solidskytexture->descriptor_set, alphaskytexture->descriptor_set};
		vkCmdBindDescriptorSets (
			cbx->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.sky_layer_pipeline[variant][indirect].layout.handle, 0, 2, descriptor_sets, 0, NULL);
		memcpy (&constant_values[20], r_refdef.vieworg, sizeof (r_refdef.vieworg));
		constant_values[23] = cl.time - (int)cl.time / 16 * 16;
		constant_values[24] = r_skyalpha.value;
		R_PushConstants (cbx, VK_SHADER_STAGE_ALL_GRAPHICS, 0, 25 * sizeof (float), constant_values);
	}
	else
	{
		R_BindPipeline (cbx, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.sky_stencil_pipeline[variant][indirect]);
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
			R_BindPipeline (cbx, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.sky_color_pipeline[variant][0]);
		else if (skybox.cubemap)
			R_BindPipeline (cbx, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.sky_cube_pipeline[variant][0]);
		else if (!skybox.name[0])
			R_BindPipeline (cbx, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.sky_layer_pipeline[variant][0]);
		else
			R_BindPipeline (cbx, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.sky_stencil_pipeline[variant][0]);
	}
	else
		Sky_ProcessTextureChains (cbx, color, &skypolys);

	Sky_ProcessEntities (cbx, color);

	//
	// render slow sky: non-cubemap skybox
	//
	if (!flat_color && !skybox.cubemap && skybox.name[0])
	{
		R_BindPipeline (cbx, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_globals.sky_box_pipeline[variant]);
		Sky_DrawSkyBox (cbx, &skypolys);
	}

	Atomic_AddUInt32 (&rs_skypolys, skypolys);

	R_EndDebugUtilsLabel (cbx);
}

/*
==============
Sky_NeedStencil
==============
*/
qboolean Sky_NeedStencil ()
{
	const qboolean flat_color = r_fastsky.value || (Fog_GetDensity () > 0 && skyfog >= 1);
	return !flat_color && !skybox.cubemap && skybox.name[0];
}
