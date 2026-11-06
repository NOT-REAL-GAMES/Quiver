/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Retained controls integrated into the map editor. Include after quakedef.h. */
#ifndef QUIVER_EDITOR_CONTROLS_H
#define QUIVER_EDITOR_CONTROLS_H

void EditorControls_SetActive (qboolean active);
void EditorControls_CancelInput (void);
void EditorControls_Update (int x, int y);
qboolean EditorControls_ToggleInspector (void);
qboolean EditorControls_MouseButton (int button, qboolean down, int x, int y);
qboolean EditorControls_Scroll (int x, int y, float delta);
qboolean EditorControls_Key (int key, qboolean down, qboolean ctrl, qboolean shift);
qboolean EditorControls_Cancel (void);
void EditorControls_EscapeReleased (void);
qboolean EditorControls_BlocksEditor (void);
qboolean EditorControls_WantsText (void);
qboolean EditorControls_TextInput (const char *utf8);
void EditorControls_Draw (cb_context_t *cbx);

#endif
