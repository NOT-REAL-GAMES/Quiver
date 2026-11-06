/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Small retained-mode UI tree. Independent of the renderer and window system. */
#ifndef QUIVER_UI_CONTROL_H
#define QUIVER_UI_CONTROL_H

#include <stdbool.h>
#include <stddef.h>

typedef struct ui_context_s ui_context_t;
typedef struct ui_control_s ui_control_t;
typedef struct { float x, y, width, height; } ui_rect_t;
typedef struct { float left, top, right, bottom; } ui_edges_t;
typedef struct { float r, g, b, a; } ui_color_t;

typedef enum {
    UI_ROOT, UI_PANEL, UI_LABEL, UI_BUTTON, UI_CHECKBOX, UI_HBOX, UI_VBOX, UI_LINE_EDIT, UI_SCROLL, UI_POPUP, UI_CUSTOM
} ui_kind_t;
typedef enum { UI_MOUSE_STOP, UI_MOUSE_PASS, UI_MOUSE_IGNORE } ui_mouse_filter_t;
typedef enum {
    UI_EVENT_MOUSE_ENTER, UI_EVENT_MOUSE_EXIT, UI_EVENT_MOUSE_MOVE,
    UI_EVENT_MOUSE_DOWN, UI_EVENT_MOUSE_UP, UI_EVENT_KEY_DOWN, UI_EVENT_KEY_UP,
    UI_EVENT_FOCUS_IN, UI_EVENT_FOCUS_OUT
} ui_event_type_t;
typedef enum { UI_KEY_TAB = 1, UI_KEY_ENTER, UI_KEY_SPACE, UI_KEY_LEFT, UI_KEY_RIGHT, UI_KEY_UP, UI_KEY_DOWN,
    UI_KEY_HOME, UI_KEY_END, UI_KEY_BACKSPACE, UI_KEY_DELETE, UI_KEY_ESCAPE } ui_key_t;
typedef struct {
    ui_event_type_t type;
    float x, y;
    int button, key;
    bool shift;
} ui_event_t;
typedef struct {
    ui_color_t panel, text, button, button_hover, button_pressed, button_disabled, focus, check;
    float padding, spacing, glyph_width, line_height;
} ui_theme_t;
typedef struct {
    /* Backends must clip each primitive to clip. Coordinates are in pixels. */
    void (*rect)(void *user, ui_rect_t rect, ui_rect_t clip, ui_color_t color);
    void (*text)(void *user, float x, float y, const char *text, ui_rect_t clip, ui_color_t color);
} ui_renderer_t;
typedef void (*ui_action_fn)(ui_control_t *control, void *user);
/* Draw a custom control in pixel coordinates. Drawing must honor clip. */
typedef void (*ui_draw_fn)(ui_control_t *control, ui_rect_t rect, ui_rect_t clip, void *user);
/* Return true to consume the event. PASS bubbles to the parent; STOP consumes. */
typedef bool (*ui_event_fn)(ui_control_t *control, const ui_event_t *event, void *user);

ui_theme_t UI_DefaultTheme(void);
ui_context_t *UI_Create(const ui_theme_t *theme, const ui_renderer_t *renderer, void *renderer_user);
/* Safe from input/action/render callbacks: freeing is deferred until the
 * outermost dispatch returns, after which the context pointer is invalid. */
void UI_Destroy(ui_context_t *context);
ui_control_t *UI_Root(ui_context_t *context);
ui_control_t *UI_Add(ui_control_t *parent, ui_kind_t kind, const char *text);
/* Removes the complete subtree. Safe from an event/action callback. */
void UI_Remove(ui_control_t *control);
bool UI_Reparent(ui_control_t *control, ui_control_t *new_parent);
/* Each edge is parent-origin + parent-size*anchor + offset. */
void UI_SetRect(ui_control_t *control, ui_edges_t anchors, ui_edges_t offsets);
void UI_SetMinSize(ui_control_t *control, float width, float height);
void UI_SetExpand(ui_control_t *control, bool horizontal, bool vertical);
void UI_SetPadding(ui_control_t *control, float padding);
void UI_SetSpacing(ui_control_t *control, float spacing);
void UI_SetVisible(ui_control_t *control, bool visible);
void UI_SetEnabled(ui_control_t *control, bool enabled);
void UI_SetClipChildren(ui_control_t *control, bool clip);
/* Scroll offsets are positive pixels from the top-left of scroll content. */
void UI_SetScroll(ui_control_t *control, float x, float y);
void UI_GetScroll(const ui_control_t *control, float *x, float *y);
/* Scrolls the innermost scroll container under the pointer; positive dy
 * moves content upward. Modal popups also consume scroll outside themselves. */
bool UI_Scroll(ui_context_t *context, float x, float y, float dx, float dy);
/* Popups are initially hidden. Modal popups stack, block background input,
 * close with Escape, and restore the prior focus on close. */
bool UI_OpenPopup(ui_control_t *popup, bool modal);
void UI_ClosePopup(ui_control_t *popup);
void UI_SetPopupCloseOnOutside(ui_control_t *popup, bool enabled);
ui_control_t *UI_GetModal(const ui_context_t *context);
/* True when Escape should be routed here, including a pending key release. */
bool UI_CanCancel(const ui_context_t *context);
void UI_SetMouseFilter(ui_control_t *control, ui_mouse_filter_t filter);
void UI_SetFocusable(ui_control_t *control, bool focusable);
bool UI_SetText(ui_control_t *control, const char *text);
const char *UI_GetText(const ui_control_t *control);
void UI_SetChecked(ui_control_t *control, bool checked);
bool UI_IsChecked(const ui_control_t *control);
void UI_SetAction(ui_control_t *control, ui_action_fn action, void *user);
/* LINE_EDIT invokes changed after a text edit and action on Enter. */
void UI_SetChanged(ui_control_t *control, ui_action_fn changed, void *user);
/* Called after built-in drawing and before child controls are drawn. */
void UI_SetDraw(ui_control_t *control, ui_draw_fn draw, void *user);
void UI_SetEvent(ui_control_t *control, ui_event_fn handler, void *user);
void UI_SetTheme(ui_context_t *context, const ui_theme_t *theme);
ui_rect_t UI_GetRect(const ui_control_t *control);
ui_control_t *UI_GetFocus(const ui_context_t *context);
ui_control_t *UI_GetHover(const ui_context_t *context);
bool UI_WantsText(const ui_context_t *context);
void UI_Focus(ui_context_t *context, ui_control_t *control);
void UI_Layout(ui_context_t *context, float width, float height);
void UI_Draw(ui_context_t *context);
bool UI_MouseMove(ui_context_t *context, float x, float y);
/* Buttons 1 through 8 retain their press target until their matching release. */
bool UI_MouseButton(ui_context_t *context, int button, bool down, float x, float y);
bool UI_Key(ui_context_t *context, ui_key_t key, bool down, bool shift);
/* UTF-8 text input for the focused LINE_EDIT; rejects invalid/control text. */
bool UI_TextInput(ui_context_t *context, const char *utf8);

#endif
