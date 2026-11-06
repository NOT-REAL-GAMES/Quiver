/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Production PEXT2 serializers must preserve entity indices across the old
 * 15/16-bit boundaries in baseline, delta/remove and attachment fields. */
#include "quakedef.h"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

sizebuf_t net_message;
client_static_t cls;
client_t *host_client;
const char *NET_QSocketGetTrueAddressString (const struct qsocket_s *sock) { (void)sock; return "LOCAL"; }
FUNC_NORETURN void Host_Error (const char *fmt, ...) { (void)fmt; abort (); }
FUNC_NORETURN void Sys_Error (const char *fmt, ...) { (void)fmt; abort (); }
void Con_Printf (const char *fmt, ...) { (void)fmt; }
void Con_DPrintf (const char *fmt, ...) { (void)fmt; }

static void begin_read (sizebuf_t *buf)
{
	net_message = *buf;
	MSG_BeginReading ();
}

int main (void)
{
	const unsigned ids[] = {1, 0x3fff, 0x4000, 32767, 32768, 65535, 65536, 1048575};
	for (size_t i = 0; i < sizeof (ids) / sizeof (ids[0]); ++i)
	{
		byte bytes[128] = {0};
		sizebuf_t buf = {0};
		buf.data = bytes; buf.maxsize = sizeof (bytes);
		MSG_WriteEntity (&buf, ids[i], PEXT2_REPLACEMENTDELTAS);
		begin_read (&buf);
		assert (MSG_ReadEntity (PEXT2_REPLACEMENTDELTAS) == ids[i]);
		assert (!msg_badread && msg_readcount == buf.cursize);

		for (int remove = 0; remove <= 1; ++remove)
		{
			buf.cursize = 0;
			MSG_WriteFTEEntityNumber (&buf, ids[i], remove);
			begin_read (&buf);
			qboolean decoded = !remove;
			assert (MSG_ReadFTEEntityNumber (&decoded) == ids[i]);
			assert (decoded == remove && !msg_badread && msg_readcount == buf.cursize);
		}

		/* Exercise the real server baseline writer, including a high parent
		 * reference, then read with the same primitives as CLFTE_ReadDelta. */
		buf.cursize = 0;
		entity_state_t state = nullentitystate;
		state.tagentity = ids[i];
		state.tagindex = 7;
		MSG_WriteStaticOrBaseLine (&buf, (int) ids[i], &state, PEXT2_REPLACEMENTDELTAS, PROTOCOL_RMQ, 0);
		begin_read (&buf);
		assert (MSG_ReadByte () == svcfte_spawnbaseline2);
		assert (MSG_ReadEntity (PEXT2_REPLACEMENTDELTAS) == ids[i]);
		unsigned flags = MSG_ReadByte ();
		if (flags & UF_EXTEND1) flags |= (unsigned) MSG_ReadByte () << 8;
		if (flags & UF_EXTEND2) flags |= (unsigned) MSG_ReadByte () << 16;
		if (flags & UF_EXTEND3) flags |= (unsigned) MSG_ReadByte () << 24;
		assert ((flags & UF_TAGINFO) && !(flags & ~(UF_EXTEND1 | UF_EXTEND2 | UF_EXTEND3 | UF_TAGINFO)));
		assert (MSG_ReadEntity (PEXT2_REPLACEMENTDELTAS) == ids[i]);
		assert (MSG_ReadByte () == 7 && !msg_badread && msg_readcount == buf.cursize);

		if (ids[i] <= 32767)
		{
			buf.cursize = 0;
			MSG_WriteEntity (&buf, ids[i], 0);
			assert (buf.cursize == 2);
			begin_read (&buf);
			assert (MSG_ReadEntity (0) == ids[i] && !msg_badread);
		}
	}
	/* Extended snapshot indices must reject a truncated high byte. */
	byte truncated[] = {0x00, 0x40};
	memset (&net_message, 0, sizeof (net_message));
	net_message.data = truncated; net_message.cursize = sizeof (truncated);
	MSG_BeginReading ();
	qboolean remove = false;
	assert (MSG_ReadFTEEntityNumber (&remove) == 0 && msg_badread);
	net_message.cursize = 1;
	MSG_BeginReading ();
	remove = true;
	assert (MSG_ReadFTEEntityNumber (&remove) == 0 && msg_badread && !remove);
	puts ("PASS: PEXT2 extended baseline, delta, remove and attachment indices");
	return 0;
}
