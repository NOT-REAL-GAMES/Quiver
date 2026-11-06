# Retained UI controls

`Quake/ui_control.h` provides a renderer-independent control tree. A context owns its root and descendants; controls can be added, removed, or reparented. Each control has anchors and offsets relative to its parent, a minimum size, visibility, enablement, and a mouse filter. Horizontal and vertical containers arrange children using minimum sizes, expansion, padding, and spacing.

The built-in controls are panels, labels, buttons, checkboxes, single-line text edits, horizontal and vertical boxes, scroll containers, and popups. `UI_CUSTOM` and `UI_SetDraw` provide a drawing callback for project-specific controls; it receives the control's rectangle and clip. Custom controls ignore mouse input by default; use `UI_SetMouseFilter` and `UI_SetEvent` to make them interactive. A theme supplies colors, spacing, and text metrics. The renderer callbacks receive a clip rectangle for every primitive; a backend must honor it. The editor's backend uses the engine's 2D renderer and Quake font. Text input is stored as UTF-8; glyphs outside the Quake font's ASCII range are displayed as `?` by that backend.

Create a context with `UI_Create`, add children to `UI_Root`, and set their anchors and offsets with `UI_SetRect`. Call `UI_Layout` after changing layout, text, theme, or viewport size, then `UI_Draw` when rendering. `UI_SetAction` registers a button/checkbox action or line-edit submission; `UI_SetChanged` registers a line-edit change callback. Remove a control with `UI_Remove` or destroy the whole tree with `UI_Destroy`.

Route pointer movement and buttons through `UI_MouseMove` and `UI_MouseButton`, wheel movement through `UI_Scroll`, keyboard controls through `UI_Key`, and UTF-8 text through `UI_TextInput`. Use `UI_WantsText` to enable the platform's text input only when a text edit has focus. Controls can stop, pass, or ignore mouse events. The tree handles hover, pointer capture, focus traversal, clipping, and enabled/visible state.

`UI_SCROLL` clips its content and supports pixel scroll offsets. `UI_POPUP` can be opened as a modal; modal popups block background input, support nested dialogs, close with Escape, and restore focus. `UI_SetPopupCloseOnOutside` controls outside-click dismissal.

The editor's view selector is built with this system. Its toolbar switches between 3D, top, front, and right views. Tab cycles views, 1–4 select them directly, and Ctrl+Tab moves focus between toolbar buttons. This framework is a foundation, not a complete implementation of Godot's scene system or every Godot Control widget.
