/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Editor-facing adapter for the renderer-independent retained control tree. */
#include "quakedef.h"
#include "editor_ui.h"
#include "editor_controls.h"
#include "ui_control.h"
#include <stdint.h>
#include <limits.h>
#include <math.h>

static struct
{
	ui_context_t *context;
	ui_control_t *bar, *views[4];
	ui_control_t *inspector, *inspector_selection, *collision, *inspector_hint;
	ui_control_t *script_label, *script_edit;
	ui_control_t *model_add_label, *model_add_path, *model_add;
	ui_control_t *model_labels[5], *model_fields[5];
	ui_control_t *light_label, *light_labels[3], *light_fields[3], *light_swatch;
	cb_context_t *cbx;
	int width, height, view;
	unsigned int captured;
	qboolean escape_capture;
	qboolean inspector_open, inspector_visible, inspector_release, inspector_press_valid;
	editor_inspector_state_t selection, inspector_press;
	editor_inspector_state_t script_focus;
	editor_inspector_state_t model_focus[5];
	editor_inspector_state_t light_focus[3];
} controls = {.view = -1};

static void EditorControls_Rect (void *user, ui_rect_t rect, ui_rect_t clip, ui_color_t color)
{
	(void)user; if (!controls.cbx) return;
	float left = q_max (rect.x, clip.x), top = q_max (rect.y, clip.y);
	float right = q_min (rect.x + rect.width, clip.x + clip.width);
	float bottom = q_min (rect.y + rect.height, clip.y + clip.height);
	if (right > left && bottom > top)
		Draw_FillRGBA (controls.cbx, left, top, right - left, bottom - top, color.r, color.g, color.b, color.a);
}
static void EditorControls_Text (void *user, float x, float y, const char *text, ui_rect_t clip, ui_color_t color)
{
	(void)user; if (!controls.cbx || !text) return;
	GL_SetCanvasColor (color.r, color.g, color.b, color.a);
	for (const unsigned char *p = (const unsigned char *)text; *p;)
	{
		unsigned char glyph = *p++;
		if (glyph >= 0x80) { while ((*p & 0xc0) == 0x80) ++p; glyph = '?'; }
		if (x >= clip.x && x + 8 <= clip.x + clip.width && y >= clip.y && y + 8 <= clip.y + clip.height)
			Draw_Character (controls.cbx, x, y, glyph);
		x += 8;
		if (x >= clip.x + clip.width) break;
	}
	GL_SetCanvasColor (1, 1, 1, 1);
}
static void EditorControls_ViewAction (ui_control_t *control, void *user)
{
	(void)control;
	EditorUI_SetView ((editor_ui_view_t)((intptr_t)user - 1));
}
static void EditorControls_CollisionAction (ui_control_t *control, void *user)
{
	(void)user;
	const editor_inspector_state_t *selection = &controls.selection;
	if (controls.inspector_release)
	{
		if (!controls.inspector_press_valid)
		{
			UI_SetChecked (control, controls.selection.collision_enabled);
			return;
		}
		selection = &controls.inspector_press;
	}
	if (!EditorUI_SetInspectorCollision (selection, UI_IsChecked (control)))
	{
		editor_inspector_state_t current; EditorUI_GetInspectorState (&current);
		UI_SetChecked (control, current.collision_enabled);
	}
}
static bool EditorControls_ScriptEvent (ui_control_t *control, const ui_event_t *event, void *user)
{
	(void)control; (void)user;
	if (event->type == UI_EVENT_FOCUS_IN) controls.script_focus = controls.selection;
	return false;
}
static void EditorControls_ScriptAction (ui_control_t *control, void *user)
{
	(void)user;
	if (EditorUI_SetInspectorScript (&controls.script_focus, UI_GetText (control)))
	{
		EditorUI_GetInspectorState (&controls.selection);
		controls.script_focus = controls.selection;
		UI_Focus (controls.context, NULL);
	}
}
static bool EditorControls_ModelEvent (ui_control_t *control, const ui_event_t *event, void *user)
{
	(void)control; unsigned field=(unsigned)(uintptr_t)user;
	if(event->type==UI_EVENT_FOCUS_IN && field<5) controls.model_focus[field]=controls.selection;
	return false;
}
static qboolean EditorControls_Numbers (const char *text,double *out,unsigned count)
{
	for(unsigned i=0;i<count;++i)
	{
		char *end;out[i]=strtod(text,&end);
		if(end==text || !isfinite(out[i]))return false;
		text=end;
	}
	while(*text==' ' || *text=='\t')++text;
	return !*text;
}
static bool EditorControls_LightEvent (ui_control_t *control, const ui_event_t *event, void *user)
{
	(void)control; unsigned channel = (unsigned)(uintptr_t)user;
	if (event->type == UI_EVENT_FOCUS_IN && channel < 3) controls.light_focus[channel] = controls.selection;
	return false;
}
static void EditorControls_LightAction (ui_control_t *control, void *user)
{
	unsigned channel = (unsigned)(uintptr_t)user;
	if (channel >= 3) return;
	double value, rgb[3];
	if (!EditorControls_Numbers (UI_GetText (control), &value, 1) || value < 0 || value > 255)
	{ EditorUI_SetStatus ("Light RGB channels must be between 0 and 255"); return; }
	memcpy (rgb, controls.light_focus[channel].light_color, sizeof (rgb));
	rgb[channel] = value / 255;
	if (EditorUI_SetInspectorLightColor (&controls.light_focus[channel], rgb))
	{
		EditorUI_GetInspectorState (&controls.selection);
		UI_Focus (controls.context, NULL);
	}
}
static void EditorControls_LightSwatch (ui_control_t *control, ui_rect_t rect, ui_rect_t clip, void *user)
{
	(void)control; (void)user;
	EditorControls_Rect (NULL, rect, clip, (ui_color_t){0.7f, 0.7f, 0.7f, 1});
	rect.x += 1; rect.y += 1; rect.width -= 2; rect.height -= 2;
	EditorControls_Rect (NULL, rect, clip, (ui_color_t){
		(float)controls.selection.light_color[0], (float)controls.selection.light_color[1], (float)controls.selection.light_color[2], 1});
}
static void EditorControls_ModelAction (ui_control_t *control,void *user)
{
	unsigned field=(unsigned)(uintptr_t)user;
	if(field>=5)return;
	editor_map_model_t model=controls.model_focus[field].model;const char *text=UI_GetText(control);
	qboolean valid=true;
	if(field==0)
	{
		if(strlen(text)>=sizeof(model.model))valid=false;
		else q_strlcpy(model.model,text,sizeof(model.model));
	}
	else if(field==1)valid=EditorControls_Numbers(text,model.origin,3);
	else if(field==2)valid=EditorControls_Numbers(text,model.angles,3);
	else if(field==3)valid=EditorControls_Numbers(text,&model.modelscale,1);
	else
	{
		double skin;valid=EditorControls_Numbers(text,&skin,1) && skin>=0 && skin<=INT_MAX && floor(skin)==skin;
		if(valid)model.skin=(int)skin;
	}
	if(!valid){EditorUI_SetStatus("Model field needs a valid numeric value or model path");return;}
	if(EditorUI_SetInspectorModel(&controls.model_focus[field],&model))
	{
		EditorUI_GetInspectorState(&controls.selection);UI_Focus(controls.context,NULL);
	}
}
static void EditorControls_AddModel (ui_control_t *control,void *user)
{
	(void)control;(void)user;
	if(EditorUI_CreateModel(UI_GetText(controls.model_add_path)))UI_Focus(controls.context,NULL);
}
static void EditorControls_Create (void)
{
	if (controls.context) return;
	ui_theme_t theme = UI_DefaultTheme ();
	theme.button = (ui_color_t){0.14f, 0.16f, 0.20f, 0.96f};
	theme.button_hover = (ui_color_t){0.28f, 0.32f, 0.39f, 1};
	theme.button_pressed = (ui_color_t){0.38f, 0.43f, 0.52f, 1};
	theme.text = (ui_color_t){1, 1, 1, 1}; theme.padding = 2; theme.spacing = 2;
	theme.glyph_width = 8; theme.line_height = 8;
	ui_renderer_t renderer = {EditorControls_Rect, EditorControls_Text};
	controls.context = UI_Create (&theme, &renderer, NULL);
	if (!controls.context) return;
	controls.bar = UI_Add (UI_Root (controls.context), UI_HBOX, "");
	if (!controls.bar) { EditorControls_SetActive (false); return; }
	UI_SetRect (controls.bar, (ui_edges_t){1, 0, 1, 0}, (ui_edges_t){-300, 3, -4, 23});
	UI_SetPadding (controls.bar, 0); UI_SetSpacing (controls.bar, 2); UI_SetClipChildren (controls.bar, true);
	static const char *const labels[] = {"3D", "TOP", "FRONT", "RIGHT"};
	for (int i = 0; i < 4; ++i)
	{
		controls.views[i] = UI_Add (controls.bar, UI_BUTTON, labels[i]);
		if (!controls.views[i]) { EditorControls_SetActive (false); return; }
		UI_SetMinSize (controls.views[i], 56, 18);
		UI_SetExpand (controls.views[i], true, true);
		UI_SetAction (controls.views[i], EditorControls_ViewAction, (void *)(intptr_t)(i + 1));
	}
	controls.inspector = UI_Add (UI_Root (controls.context), UI_SCROLL, "");
	if (!controls.inspector) { EditorControls_SetActive (false); return; }
	UI_SetRect (controls.inspector, (ui_edges_t){1, 0, 1, 0}, (ui_edges_t){-224, 52, -4, 154});
	UI_SetPadding (controls.inspector, 0); UI_SetClipChildren (controls.inspector, true);
	UI_SetMouseFilter (controls.inspector, UI_MOUSE_STOP);
	ui_control_t *heading = UI_Add (controls.inspector, UI_LABEL, "PROPERTIES");
	controls.inspector_selection = UI_Add (controls.inspector, UI_LABEL, "Nothing selected");
	controls.collision = UI_Add (controls.inspector, UI_CHECKBOX, "Collision");
	controls.script_label = UI_Add (controls.inspector, UI_LABEL, "World script (.qs):");
	controls.script_edit = UI_Add (controls.inspector, UI_LINE_EDIT, "");
	controls.inspector_hint = UI_Add (controls.inspector, UI_LABEL, "I: hide inspector");
	if (!heading || !controls.inspector_selection || !controls.collision || !controls.script_label || !controls.script_edit || !controls.inspector_hint)
	{ EditorControls_SetActive (false); return; }
	UI_SetRect (heading, (ui_edges_t){0,0,1,0}, (ui_edges_t){8,6,-8,18});
	UI_SetRect (controls.inspector_selection, (ui_edges_t){0,0,1,0}, (ui_edges_t){8,22,-8,34});
	UI_SetRect (controls.collision, (ui_edges_t){0,0,1,0}, (ui_edges_t){8,42,-8,66});
	UI_SetRect (controls.script_label, (ui_edges_t){0,0,1,0}, (ui_edges_t){8,35,-8,47});
	UI_SetRect (controls.script_edit, (ui_edges_t){0,0,1,0}, (ui_edges_t){8,49,-8,72});
	UI_SetRect (controls.inspector_hint, (ui_edges_t){0,0,1,0}, (ui_edges_t){8,76,-8,88});
	UI_SetAction (controls.collision, EditorControls_CollisionAction, NULL);
	UI_SetEvent (controls.script_edit, EditorControls_ScriptEvent, NULL);
	UI_SetAction (controls.script_edit, EditorControls_ScriptAction, NULL);
	UI_SetVisible (controls.script_label, false); UI_SetVisible (controls.script_edit, false);
	controls.light_label = UI_Add (controls.inspector, UI_LABEL, "Light RGB (0-255):");
	controls.light_swatch = UI_Add (controls.inspector, UI_CUSTOM, "");
	if (!controls.light_label || !controls.light_swatch) { EditorControls_SetActive (false); return; }
	UI_SetRect (controls.light_label, (ui_edges_t){0,0,1,0}, (ui_edges_t){8,35,-8,47});
	UI_SetRect (controls.light_swatch, (ui_edges_t){0,0,1,0}, (ui_edges_t){8,129,-8,149});
	UI_SetDraw (controls.light_swatch, EditorControls_LightSwatch, NULL);
	UI_SetMouseFilter (controls.light_swatch, UI_MOUSE_IGNORE);
	UI_SetVisible (controls.light_label, false); UI_SetVisible (controls.light_swatch, false);
	static const char *const channel_names[] = {"Red:", "Green:", "Blue:"};
	for (unsigned i = 0; i < 3; ++i)
	{
		controls.light_labels[i] = UI_Add (controls.inspector, UI_LABEL, channel_names[i]);
		controls.light_fields[i] = UI_Add (controls.inspector, UI_LINE_EDIT, "");
		if (!controls.light_labels[i] || !controls.light_fields[i]) { EditorControls_SetActive (false); return; }
		float top = 49 + i * 26;
		UI_SetRect (controls.light_labels[i], (ui_edges_t){0,0,0,0}, (ui_edges_t){8,top,64,top+22});
		UI_SetRect (controls.light_fields[i], (ui_edges_t){0,0,1,0}, (ui_edges_t){66,top,-16,top+22});
		UI_SetEvent (controls.light_fields[i], EditorControls_LightEvent, (void *)(uintptr_t)i);
		UI_SetAction (controls.light_fields[i], EditorControls_LightAction, (void *)(uintptr_t)i);
		UI_SetVisible (controls.light_labels[i], false); UI_SetVisible (controls.light_fields[i], false);
	}
	controls.model_add_label=UI_Add(controls.inspector,UI_LABEL,"Place static model:");
	controls.model_add_path=UI_Add(controls.inspector,UI_LINE_EDIT,"progs/");
	controls.model_add=UI_Add(controls.inspector,UI_BUTTON,"Add model ahead of view");
	if(!controls.model_add_label || !controls.model_add_path || !controls.model_add){EditorControls_SetActive(false);return;}
	UI_SetRect(controls.model_add_label,(ui_edges_t){0,0,1,0},(ui_edges_t){8,78,-8,90});
	UI_SetRect(controls.model_add_path,(ui_edges_t){0,0,1,0},(ui_edges_t){8,92,-8,114});
	UI_SetRect(controls.model_add,(ui_edges_t){0,0,1,0},(ui_edges_t){8,116,-8,138});
	UI_SetAction(controls.model_add,EditorControls_AddModel,NULL);
	UI_SetAction(controls.model_add_path,EditorControls_AddModel,NULL);
	static const char *const model_names[]={"Model path:","Origin: X Y Z","Angles: pitch yaw roll","Scale:","Skin:"};
	for(unsigned i=0;i<5;++i)
	{
		controls.model_labels[i]=UI_Add(controls.inspector,UI_LABEL,model_names[i]);
		controls.model_fields[i]=UI_Add(controls.inspector,UI_LINE_EDIT,"");
		if(!controls.model_labels[i] || !controls.model_fields[i]){EditorControls_SetActive(false);return;}
		float top=146+i*40;
		UI_SetRect(controls.model_labels[i],(ui_edges_t){0,0,1,0},(ui_edges_t){8,top,-8,top+12});
		UI_SetRect(controls.model_fields[i],(ui_edges_t){0,0,1,0},(ui_edges_t){8,top+14,-8,top+36});
		UI_SetEvent(controls.model_fields[i],EditorControls_ModelEvent,(void *)(uintptr_t)i);
		UI_SetAction(controls.model_fields[i],EditorControls_ModelAction,(void *)(uintptr_t)i);
		UI_SetVisible(controls.model_labels[i],false);UI_SetVisible(controls.model_fields[i],false);
	}
	UI_SetVisible(controls.model_add_label,false);UI_SetVisible(controls.model_add_path,false);UI_SetVisible(controls.model_add,false);
	controls.inspector_open = true;
	UI_SetVisible (controls.inspector, false);
	controls.view = -1; controls.width = controls.height = -1;
}
void EditorControls_SetActive (qboolean active)
{
	if (active) { EditorControls_Create (); EditorControls_Update (0, 0); return; }
	UI_Destroy (controls.context); memset (&controls, 0, sizeof (controls)); controls.view = -1;
}
void EditorControls_CancelInput (void)
{
	if (!controls.context) return;
	for (int button = 1; button <= 3; ++button)
		if (controls.captured & (1u << button)) UI_MouseButton (controls.context, button, false, -1, -1);
	controls.captured = false; controls.inspector_press_valid = false; UI_Focus (controls.context, NULL);
}
qboolean EditorControls_ToggleInspector (void)
{
	if (!controls.context) return false;
	controls.inspector_open = !controls.inspector_open;
	controls.width = -1;
	return true;
}
void EditorControls_Update (int x, int y)
{
	if (!controls.context) return;
	editor_inspector_state_t selection; EditorUI_GetInspectorState (&selection);
	qboolean panel_visible = controls.inspector_open && selection.supported && glwidth >= 320 && glheight >= 200;
	qboolean selection_changed = selection.kind != controls.selection.kind || selection.index != controls.selection.index;
	if (selection_changed)
	{
		qboolean editing_model=false;for(unsigned i=0;i<5;++i)editing_model|=UI_GetFocus(controls.context)==controls.model_fields[i];
		for (unsigned i = 0; i < 3; ++i) editing_model |= UI_GetFocus (controls.context) == controls.light_fields[i];
		if (UI_GetFocus (controls.context) == controls.script_edit || editing_model) UI_Focus (controls.context, NULL);
		static const char *const names[] = {"Nothing selected", "Brush", "World sector", "prop_static", "prop_dynamic", "prop_movable", "Light", "Static model"};
		char label[96];
		if (selection.kind == EDITOR_INSPECT_NONE) q_strlcpy (label, names[0], sizeof (label));
		else q_snprintf (label, sizeof (label), "%s %d", names[selection.kind], selection.index);
		UI_SetText (controls.inspector_selection, label);
		UI_SetVisible (controls.collision, selection.kind == EDITOR_INSPECT_STATIC || selection.kind == EDITOR_INSPECT_MODEL);
		qboolean show_script = (selection.kind == EDITOR_INSPECT_WORLD || selection.kind == EDITOR_INSPECT_BRUSH) && selection.script_supported;
		UI_SetVisible (controls.script_label, show_script);
		UI_SetVisible (controls.script_edit, show_script);
		UI_SetText (controls.inspector_hint, show_script ? "Enter: save   I: hide" : "I: hide inspector");
		controls.width = -1;
	}
	qboolean show_light = selection.kind == EDITOR_INSPECT_LIGHT && selection.light_color_supported;
	UI_SetVisible (controls.light_label, show_light); UI_SetVisible (controls.light_swatch, show_light);
	for (unsigned i = 0; i < 3; ++i)
	{
		UI_SetVisible (controls.light_labels[i], show_light); UI_SetVisible (controls.light_fields[i], show_light);
		if (show_light && UI_GetFocus (controls.context) != controls.light_fields[i])
		{
			char value[64]; q_snprintf (value, sizeof (value), "%.9g", selection.light_color[i] * 255);
			if (strcmp (UI_GetText (controls.light_fields[i]), value)) UI_SetText (controls.light_fields[i], value);
		}
	}
	qboolean show_model=selection.kind==EDITOR_INSPECT_MODEL && selection.model_supported;
	UI_SetVisible(controls.model_add_label,selection.model_supported);UI_SetVisible(controls.model_add_path,selection.model_supported);
	UI_SetVisible(controls.model_add,selection.model_supported);
	for(unsigned i=0;i<5;++i)
	{
		UI_SetVisible(controls.model_labels[i],show_model);UI_SetVisible(controls.model_fields[i],show_model);
		if(show_model && UI_GetFocus(controls.context)!=controls.model_fields[i])
		{
			char value[256];
			if(i==0)q_strlcpy(value,selection.model.model,sizeof(value));
			else if(i==1)q_snprintf(value,sizeof(value),"%.9g %.9g %.9g",selection.model.origin[0],selection.model.origin[1],selection.model.origin[2]);
			else if(i==2)q_snprintf(value,sizeof(value),"%.9g %.9g %.9g",selection.model.angles[0],selection.model.angles[1],selection.model.angles[2]);
			else if(i==3)q_snprintf(value,sizeof(value),"%.9g",selection.model.modelscale);
			else q_snprintf(value,sizeof(value),"%d",selection.model.skin);
			if(strcmp(UI_GetText(controls.model_fields[i]),value))UI_SetText(controls.model_fields[i],value);
		}
	}
	{
		float add_top = show_light ? 178 : 78;
		UI_SetRect (controls.model_add_label, (ui_edges_t){0,0,1,0}, (ui_edges_t){8,add_top,-8,add_top+12});
		UI_SetRect (controls.model_add_path, (ui_edges_t){0,0,1,0}, (ui_edges_t){8,add_top+14,-8,add_top+36});
		UI_SetRect (controls.model_add, (ui_edges_t){0,0,1,0}, (ui_edges_t){8,add_top+38,-8,add_top+60});
		float height = show_model ? 368 : show_light ? (selection.model_supported ? 266 : 186) : selection.model_supported ? 166 : 102;
		height = q_min (height, (float)glheight - 88);
		UI_SetRect (controls.inspector, (ui_edges_t){1,0,1,0}, (ui_edges_t){-224,52,-4,52+height});
		float hint = show_model ? 348 : show_light ? (selection.model_supported ? 246 : 158) : selection.model_supported ? 146 : 76;
		UI_SetRect (controls.inspector_hint, (ui_edges_t){0,0,1,0}, (ui_edges_t){8,hint,-8,hint+12});
		UI_SetText (controls.inspector_hint, selection.model_supported || show_light ? "Enter: apply  I: hide" : "I: hide inspector");
		controls.width = -1;
	}
	controls.selection = selection;
	if (UI_GetFocus (controls.context) != controls.script_edit && strcmp (UI_GetText (controls.script_edit), selection.world_script))
		UI_SetText (controls.script_edit, selection.world_script);
	UI_SetChecked (controls.collision, selection.collision_enabled);
	if (panel_visible != controls.inspector_visible)
	{
		UI_SetVisible (controls.inspector, panel_visible);
		controls.inspector_visible = panel_visible;
		controls.width = -1;
	}
	int view = (int)EditorUI_GetView ();
	if (view != controls.view)
	{
		static const char *const normal[] = {"3D", "TOP", "FRONT", "RIGHT"};
		static const char *const active[] = {"[3D]", "[TOP]", "[FRONT]", "[RIGHT]"};
		for (int i = 0; i < 4; ++i) UI_SetText (controls.views[i], i == view ? active[i] : normal[i]);
		controls.view = view; controls.width = -1;
	}
	if (controls.width != glwidth || controls.height != glheight)
	{
		UI_SetVisible (controls.bar, glwidth >= 840 && glheight >= 80);
		UI_Layout (controls.context, (float)glwidth, (float)glheight);
		controls.width = glwidth; controls.height = glheight;
	}
	UI_MouseMove (controls.context, (float)x, (float)y);
}
qboolean EditorControls_MouseButton (int button, qboolean down, int x, int y)
{
	if (!controls.context || button < 1 || button > 3) return false;
	unsigned int mask = 1u << button;
	if (down)
	{
		if (button == 1)
		{
			controls.inspector_press_valid = UI_GetHover (controls.context) == controls.collision && controls.inspector_visible;
			if (controls.inspector_press_valid) controls.inspector_press = controls.selection;
		}
		qboolean handled = UI_MouseButton (controls.context, button, true, (float)x, (float)y);
		if (handled) controls.captured |= mask;
		else if (button == 1) UI_Focus (controls.context, NULL);
		return handled;
	}
	if (!(controls.captured & mask)) return false;
	controls.inspector_release = button == 1;
	UI_MouseButton (controls.context, button, false, (float)x, (float)y);
	controls.inspector_release = false;
	if (button == 1) controls.inspector_press_valid = false;
	controls.captured &= ~mask; return true;
}
qboolean EditorControls_Scroll (int x, int y, float delta)
{
	if (!controls.context) return false;
	if (UI_Scroll (controls.context, (float)x, (float)y, 0, delta)) return true;
	if (controls.inspector_visible)
	{
		ui_rect_t r = UI_GetRect (controls.inspector);
		if (x >= r.x && y >= r.y && x < r.x + r.width && y < r.y + r.height) return true;
	}
	return false;
}
qboolean EditorControls_Key (int key, qboolean down, qboolean ctrl, qboolean shift)
{
	if (!controls.context) return false;
	qboolean text = UI_WantsText (controls.context), modal = UI_GetModal (controls.context) != NULL;
	ui_key_t mapped;
	switch (key)
	{
	case K_TAB: if (!ctrl && !modal) return false; mapped = UI_KEY_TAB; break;
	case K_ENTER: case K_KP_ENTER: mapped = UI_KEY_ENTER; break;
	case K_SPACE: mapped = UI_KEY_SPACE; break;
	case K_LEFTARROW: if (!text && !modal) return false; mapped = UI_KEY_LEFT; break;
	case K_RIGHTARROW: if (!text && !modal) return false; mapped = UI_KEY_RIGHT; break;
	case K_UPARROW: if (!text && !modal) return false; mapped = UI_KEY_UP; break;
	case K_DOWNARROW: if (!text && !modal) return false; mapped = UI_KEY_DOWN; break;
	case K_HOME: if (!text && !modal) return false; mapped = UI_KEY_HOME; break;
	case K_END: if (!text && !modal) return false; mapped = UI_KEY_END; break;
	case K_BACKSPACE: if (!text && !modal) return false; mapped = UI_KEY_BACKSPACE; break;
	case K_DEL: if (!text && !modal) return false; mapped = UI_KEY_DELETE; break;
	default: return text || modal;
	}
	return UI_Key (controls.context, mapped, down, shift);
}
qboolean EditorControls_Cancel (void)
{
	if (!controls.context) return false;
	if (UI_CanCancel (controls.context))
	{
		qboolean handled = UI_Key (controls.context, UI_KEY_ESCAPE, true, false);
		controls.escape_capture = handled; return handled;
	}
	if (UI_WantsText (controls.context)) { UI_Focus (controls.context, NULL); return true; }
	return false;
}
void EditorControls_EscapeReleased (void)
{
	if (controls.context && controls.escape_capture) UI_Key (controls.context, UI_KEY_ESCAPE, false, false);
	controls.escape_capture = false;
}
qboolean EditorControls_BlocksEditor (void)
{ return controls.context && (UI_GetModal (controls.context) != NULL || UI_WantsText (controls.context)); }
qboolean EditorControls_WantsText (void) { return controls.context && UI_WantsText (controls.context); }
qboolean EditorControls_TextInput (const char *utf8) { return controls.context && UI_TextInput (controls.context, utf8); }
void EditorControls_Draw (cb_context_t *cbx)
{
	if (!controls.context) return;
	controls.cbx = cbx; UI_Draw (controls.context); controls.cbx = NULL;
}
