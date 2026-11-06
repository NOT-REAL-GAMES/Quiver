/* In-engine source-map editor session. Called from the main thread. */
#ifndef QUAKE_EDITOR_H
#define QUAKE_EDITOR_H
void Editor_Init (void);
void Editor_Frame (void);
void Editor_Shutdown (void);
/* Called for F9 events; returns true only while an editor session owns it. */
qboolean Editor_ToggleKey (qboolean down);
#endif
