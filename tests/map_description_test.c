/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Exercise the production map-description parser with an independently
 * positioned memory-backed file handle, as used for embedded pak entries. */
#include "quakedef.h"
#include <assert.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

THREAD_LOCAL char com_token[COM_PARSE_MAX_TOKEN_SIZE];
static byte file_data[4096];
static qfileofs_t file_base, file_position;
static qfilesize_t entry_size, actual_size;
static int file_open, force_short_read;
static int identity (int value) { return value; }
int (*LittleLong) (int) = identity;

int q_snprintf (char *dst, size_t size, const char *format, ...)
{
	va_list args;
	va_start (args, format);
	int result = vsnprintf (dst, size, format, args);
	va_end (args);
	return result;
}

qfilesize_t COM_OpenFile (const char *path, int *handle, unsigned int *path_id)
{
	(void)path_id;
	if (strcmp (path, "maps/test.bsp"))
	{
		*handle = -1;
		return -1;
	}
	assert (!file_open);
	file_open = 1;
	file_position = file_base;
	*handle = 1;
	return entry_size;
}

void COM_CloseFile (int handle) { assert (handle == 1 && file_open); file_open = 0; }
qfileofs_t Sys_FilePos (int handle) { assert (handle == 1 && file_open); return file_position; }
int Sys_FileSeek (int handle, qfileofs_t position)
{
	assert (handle == 1 && file_open);
	if (position < 0 || position > (qfileofs_t)sizeof (file_data)) return 1;
	file_position = position;
	return 0;
}
int Sys_FileRead (int handle, void *dst, int count)
{
	assert (handle == 1 && file_open && count >= 0);
	qfilesize_t remaining = actual_size - file_position;
	if (remaining < 0) remaining = 0;
	if (count > remaining) count = (int)remaining;
	if (force_short_read && file_position >= file_base + (qfileofs_t)sizeof (dheader_t) && count > 0) --count;
	memcpy (dst, file_data + file_position, (size_t)count);
	file_position += count;
	return count;
}

const char *COM_ParseEx (const char *data, cpe_mode mode)
{
	(void)mode;
	if (!data) return NULL;
	while (*data && (unsigned char)*data <= ' ') ++data;
	if (!*data) return NULL;
	char *out = com_token;
	if (*data == '{' || *data == '}') { *out++ = *data++; *out = 0; return data; }
	if (*data == '"')
	{
		++data;
		while (*data && *data != '"' && (size_t)(out - com_token) < sizeof (com_token) - 1) *out++ = *data++;
		if (*data == '"') ++data;
	}
	else while (*data && (unsigned char)*data > ' ' && *data != '{' && *data != '}' && (size_t)(out - com_token) < sizeof (com_token) - 1) *out++ = *data++;
	*out = 0;
	return data;
}
const char *COM_Parse (const char *data) { return COM_ParseEx (data, CPE_NOTRUNC); }

static void make_map (qfileofs_t base)
{
	const char entities[] = "{\n\"classname\" \"worldspawn\"\n\"message\" \"Stress Room\"\n}\n{\n\"classname\" \"info_player_start\"\n}\n";
	dheader_t header = {0};
	header.version = BSPVERSION;
	header.lumps[LUMP_ENTITIES].fileofs = sizeof (header);
	header.lumps[LUMP_ENTITIES].filelen = sizeof (entities) - 1;
	memset (file_data, 0, sizeof (file_data));
	memcpy (file_data + base, &header, sizeof (header));
	memcpy (file_data + base + sizeof (header), entities, sizeof (entities));
	file_base = base;
	entry_size = sizeof (header) + sizeof (entities) - 1;
	actual_size = base + entry_size;
	file_open = force_short_read = 0;
}

int main (void)
{
	char desc[128];
	/* A loose map begins at zero. Both disk and in-memory pak members can
	 * begin at a nonzero offset and use independently positioned handles. */
	const int offsets[] = {0, 137, 1023};
	for (unsigned i = 0; i < sizeof (offsets) / sizeof (offsets[0]); ++i)
	{
		make_map (offsets[i]);
		assert (Mod_LoadMapDescription (desc, sizeof (desc), "test"));
		assert (!strcmp (desc, "Stress Room") && !file_open);
	}
	make_map (137);
	entry_size = sizeof (dheader_t);
	assert (!Mod_LoadMapDescription (desc, sizeof (desc), "test") && !file_open);
	make_map (137);
	dheader_t bad;
	memcpy (&bad, file_data + file_base, sizeof (bad));
	bad.lumps[LUMP_ENTITIES].fileofs = INT_MAX;
	bad.lumps[LUMP_ENTITIES].filelen = INT_MAX;
	memcpy (file_data + file_base, &bad, sizeof (bad));
	assert (!Mod_LoadMapDescription (desc, sizeof (desc), "test") && !file_open);
	make_map (137);
	force_short_read = 1;
	assert (!Mod_LoadMapDescription (desc, sizeof (desc), "test") && !file_open);
	assert (!Mod_LoadMapDescription (desc, sizeof (desc), "missing") && !file_open);
	puts ("PASS: loose and pak map descriptions, malformed/truncated bounds");
	return 0;
}
