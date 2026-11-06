/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ui_control.h"
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define UI_MAX_DEPTH 256
#define UI_MAX_MODALS 32
#define UI_POINTER_BUTTONS 8

struct ui_control_s {
    ui_context_t *context;
    ui_control_t *parent, *first, *last, *prev, *next, *garbage_next;
    ui_kind_t kind;
    ui_edges_t anchors, offsets;
    ui_rect_t rect, clip;
    float min_width, min_height, measured_width, measured_height, padding, spacing;
    float scroll_x, scroll_y, scroll_max_x, scroll_max_y;
    char *text;
    size_t caret;
    ui_mouse_filter_t mouse_filter;
    bool visible, enabled, clip_children, focusable, expand_x, expand_y, checked, dead, pressed;
    bool custom_padding, custom_spacing;
    bool popup_close_outside;
    ui_action_fn action;
    void *action_user;
    ui_action_fn changed;
    void *changed_user;
    ui_draw_fn draw;
    void *draw_user;
    ui_event_fn event;
    void *event_user;
};
struct ui_context_s {
    ui_control_t *root, *focus, *hover, *capture[UI_POINTER_BUTTONS], *garbage;
    ui_control_t *modals[UI_MAX_MODALS], *restore_focus[UI_MAX_MODALS];
    unsigned modal_count;
    bool modal_outside_capture[UI_POINTER_BUTTONS], cancelled_capture[UI_POINTER_BUTTONS], popup_escape_capture;
    ui_theme_t theme;
    ui_renderer_t renderer;
    void *renderer_user;
    unsigned dispatch_depth;
    bool destroy_requested;
};

static float nonnegative(float x) { return isfinite(x) && x > 0 ? x : 0; }
static ui_rect_t intersection(ui_rect_t a, ui_rect_t b)
{
    float x=fmaxf(a.x,b.x), y=fmaxf(a.y,b.y);
    float r=fminf(a.x+a.width,b.x+b.width), d=fminf(a.y+a.height,b.y+b.height);
    ui_rect_t o={x,y,fmaxf(0,r-x),fmaxf(0,d-y)};return o;
}
static bool contains(ui_rect_t r,float x,float y)
{ return r.width>0 && r.height>0 && x>=r.x && y>=r.y && x<r.x+r.width && y<r.y+r.height; }
static bool subtree(const ui_control_t *node,const ui_control_t *root)
{ for(;node;node=node->parent)if(node==root)return true;return false; }
static void clear_captures(ui_context_t *c,const ui_control_t *root)
{
    for(int i=0;i<UI_POINTER_BUTTONS;++i)
        if(c->capture[i]&&(!root||subtree(c->capture[i],root))){c->capture[i]->pressed=false;c->capture[i]=NULL;c->cancelled_capture[i]=true;}
}
static bool has_capture(const ui_context_t *c)
{for(int i=0;i<UI_POINTER_BUTTONS;++i)if(c->capture[i])return true;return false;}
static bool effectively_active(const ui_control_t *node)
{ for(;node;node=node->parent)if(node->dead||!node->visible||!node->enabled)return false;return true; }
static bool effectively_visible(const ui_control_t *node)
{ for(;node;node=node->parent)if(node->dead||!node->visible)return false;return true; }
/* Strict UTF-8, excluding controls and embedded line breaks. */
static bool valid_line_text(const char *s)
{
    const unsigned char *p=(const unsigned char *)s;size_t length=strlen(s);
    if(length>4096)return false;
    while(*p){unsigned char a=*p++;
        if(a<0x80){if(a<0x20||a==0x7f)return false;continue;}
        if(a>=0xc2&&a<=0xdf){if(p[0]<0x80||p[0]>0xbf)return false;p+=1;continue;}
        if(a>=0xe0&&a<=0xef){unsigned char b=p[0];if(!b)return false;
            if(b<0x80||b>0xbf||(a==0xe0&&b<0xa0)||(a==0xed&&b>0x9f))return false;
            if(p[1]<0x80||p[1]>0xbf)return false;
            p+=2;continue;}
        if(a>=0xf0&&a<=0xf4){unsigned char b=p[0];if(!b)return false;
            if(b<0x80||b>0xbf||(a==0xf0&&b<0x90)||(a==0xf4&&b>0x8f))return false;
            if(!p[1]||p[1]<0x80||p[1]>0xbf||p[2]<0x80||p[2]>0xbf)return false;
            p+=3;continue;}
        return false;
    }
    return true;
}
static void detach(ui_control_t *n)
{
    ui_control_t *p=n->parent;if(!p)return;
    if(n->prev)n->prev->next=n->next;else p->first=n->next;
    if(n->next)n->next->prev=n->prev;else p->last=n->prev;
    n->parent=n->prev=n->next=NULL;
}
static void attach(ui_control_t *p,ui_control_t *n)
{
    n->parent=p;n->prev=p->last;n->next=NULL;
    if(p->last)p->last->next=n;else p->first=n;p->last=n;
}
static void free_tree(ui_control_t *n)
{
    while(n->first){ui_control_t *c=n->first;detach(c);free_tree(c);}
    free(n->text);free(n);
}
static void collect(ui_context_t *c)
{
    if(c->dispatch_depth)return;
    while(c->garbage){ui_control_t *n=c->garbage;c->garbage=n->garbage_next;free_tree(n);}
}
static void begin(ui_context_t *c){++c->dispatch_depth;}
static void end(ui_context_t *c)
{
    --c->dispatch_depth;collect(c);
    if(!c->dispatch_depth&&c->destroy_requested){free_tree(c->root);free(c);}
}
/* Removal/hiding must never leave modal stack entries or saved-focus pointers
 * referring to controls which are about to become inaccessible. */
static void dismiss_subtree_modals(ui_context_t *c,ui_control_t *n)
{
    for(unsigned i=0;i<c->modal_count;++i)if(subtree(c->restore_focus[i],n))c->restore_focus[i]=NULL;
    for(unsigned i=0;i<c->modal_count;++i)if(subtree(c->modals[i],n)){
        ui_control_t *restore=c->restore_focus[i];
        for(unsigned j=i;j<c->modal_count;++j){
            ui_control_t *m=c->modals[j];m->visible=false;
            if(subtree(c->focus,m))c->focus=NULL;
            if(subtree(c->hover,m))c->hover=NULL;
            clear_captures(c,m);
            c->modals[j]=c->restore_focus[j]=NULL;
        }
        c->modal_count=i;
        if(restore&&!subtree(restore,n)&&effectively_active(restore)&&
           (!i||subtree(restore,c->modals[i-1])))c->focus=restore;
        break;
    }
}

ui_theme_t UI_DefaultTheme(void)
{
    ui_theme_t t={0};
    t.panel=(ui_color_t){0.12f,0.13f,0.16f,0.96f};
    t.text=(ui_color_t){0.92f,0.92f,0.92f,1};
    t.button=(ui_color_t){0.24f,0.26f,0.30f,1};
    t.button_hover=(ui_color_t){0.32f,0.35f,0.40f,1};
    t.button_pressed=(ui_color_t){0.16f,0.18f,0.21f,1};
    t.button_disabled=(ui_color_t){0.17f,0.18f,0.20f,0.7f};
    t.focus=(ui_color_t){0.95f,0.75f,0.25f,1};
    t.check=(ui_color_t){0.43f,0.75f,0.48f,1};
    t.padding=4;t.spacing=4;t.glyph_width=8;t.line_height=8;return t;
}
ui_context_t *UI_Create(const ui_theme_t *theme,const ui_renderer_t *renderer,void *user)
{
    ui_context_t *c=calloc(1,sizeof(*c));if(!c)return NULL;
    c->theme=theme?*theme:UI_DefaultTheme();if(renderer)c->renderer=*renderer;c->renderer_user=user;
    c->root=calloc(1,sizeof(*c->root));if(!c->root){free(c);return NULL;}
    c->root->context=c;c->root->kind=UI_ROOT;c->root->visible=c->root->enabled=true;
    c->root->clip_children=true;c->root->mouse_filter=UI_MOUSE_IGNORE;return c;
}
void UI_Destroy(ui_context_t *c)
{
    if(!c)return;
    if(c->dispatch_depth){c->destroy_requested=true;return;}
    collect(c);free_tree(c->root);free(c);
}
ui_control_t *UI_Root(ui_context_t *c){return c?c->root:NULL;}
ui_control_t *UI_Add(ui_control_t *p,ui_kind_t kind,const char *text)
{
    if(!p||p->dead||p->context->destroy_requested||kind<=UI_ROOT||kind>UI_CUSTOM)return NULL;
    unsigned depth=0;for(ui_control_t *q=p;q;q=q->parent)if(++depth>=UI_MAX_DEPTH)return NULL;
    ui_control_t *n=calloc(1,sizeof(*n));if(!n)return NULL;
    n->context=p->context;n->kind=kind;n->visible=n->enabled=true;
    n->mouse_filter=(kind==UI_BUTTON||kind==UI_CHECKBOX||kind==UI_LINE_EDIT||kind==UI_SCROLL||kind==UI_POPUP)?UI_MOUSE_STOP:UI_MOUSE_IGNORE;
    n->focusable=kind==UI_BUTTON||kind==UI_CHECKBOX||kind==UI_LINE_EDIT;
    if(kind==UI_SCROLL)n->clip_children=true;
    if(kind==UI_POPUP){n->visible=false;n->popup_close_outside=true;n->clip_children=true;}
    n->padding=n->context->theme.padding;n->spacing=n->context->theme.spacing;
    if(!UI_SetText(n,text?text:"")){free(n);return NULL;}
    attach(p,n);return n;
}
void UI_Remove(ui_control_t *n)
{
    if(!n||n->dead||!n->parent)return;
    ui_context_t *c=n->context;
    dismiss_subtree_modals(c,n);
    if(subtree(c->focus,n))c->focus=NULL;
    if(subtree(c->hover,n))c->hover=NULL;
    clear_captures(c,n);
    detach(n);n->dead=true;n->garbage_next=c->garbage;c->garbage=n;collect(c);
}
static bool fits_depth(const ui_control_t *n,unsigned depth)
{
    if(depth>=UI_MAX_DEPTH)return false;
    for(const ui_control_t *c=n->first;c;c=c->next)if(!fits_depth(c,depth+1))return false;
    return true;
}
bool UI_Reparent(ui_control_t *n,ui_control_t *p)
{
    if(!n||!p||n->dead||p->dead||n->context->destroy_requested||!n->parent||n->context!=p->context||subtree(p,n))return false;
    unsigned depth=0;for(ui_control_t *q=p;q;q=q->parent)if(++depth>=UI_MAX_DEPTH)return false;
    /* Include the moved subtree's depth, so later tree operations remain bounded. */
    if(!fits_depth(n,depth))return false;
    detach(n);attach(p,n);
    ui_context_t *c=n->context;
    for(unsigned i=0;i<c->modal_count;++i)if(!effectively_active(c->modals[i])){dismiss_subtree_modals(c,c->modals[i]);break;}
    if(c->focus&&!effectively_active(c->focus))c->focus=NULL;
    if(c->hover&&!effectively_active(c->hover))c->hover=NULL;
    for(int i=0;i<UI_POINTER_BUTTONS;++i)if(c->capture[i]&&!effectively_active(c->capture[i]))
    {c->capture[i]->pressed=false;c->capture[i]=NULL;c->cancelled_capture[i]=true;}
    return true;
}
void UI_SetRect(ui_control_t *n,ui_edges_t a,ui_edges_t o)
{
    if(!n||n->dead||!isfinite(a.left)||!isfinite(a.top)||!isfinite(a.right)||!isfinite(a.bottom)||
       !isfinite(o.left)||!isfinite(o.top)||!isfinite(o.right)||!isfinite(o.bottom))return;
    n->anchors=a;n->offsets=o;
}
void UI_SetMinSize(ui_control_t *n,float w,float h){if(n&&!n->dead){n->min_width=nonnegative(w);n->min_height=nonnegative(h);}}
void UI_SetExpand(ui_control_t *n,bool x,bool y){if(n&&!n->dead){n->expand_x=x;n->expand_y=y;}}
void UI_SetPadding(ui_control_t *n,float v){if(n&&!n->dead){n->padding=nonnegative(v);n->custom_padding=true;}}
void UI_SetSpacing(ui_control_t *n,float v){if(n&&!n->dead){n->spacing=nonnegative(v);n->custom_spacing=true;}}
static void clear_interaction(ui_control_t *n)
{
    ui_context_t *c=n->context;
    if(subtree(c->focus,n))c->focus=NULL;
    if(subtree(c->hover,n))c->hover=NULL;
    clear_captures(c,n);
    n->pressed=false;
}
void UI_SetVisible(ui_control_t *n,bool v){if(n&&!n->dead){if(!v)dismiss_subtree_modals(n->context,n);n->visible=v;if(!v)clear_interaction(n);}}
void UI_SetEnabled(ui_control_t *n,bool v){if(n&&!n->dead){if(!v)dismiss_subtree_modals(n->context,n);n->enabled=v;if(!v)clear_interaction(n);}}
void UI_SetClipChildren(ui_control_t *n,bool v){if(n&&!n->dead)n->clip_children=v;}
void UI_SetScroll(ui_control_t *n,float x,float y)
{
    if(!n||n->dead||n->kind!=UI_SCROLL)return;
    n->scroll_x=nonnegative(x);n->scroll_y=nonnegative(y);
    if(n->context->root->rect.width>0&&n->context->root->rect.height>0)
        UI_Layout(n->context,n->context->root->rect.width,n->context->root->rect.height);
}
void UI_GetScroll(const ui_control_t *n,float *x,float *y)
{
    if(x)*x=n&&!n->dead&&n->kind==UI_SCROLL?n->scroll_x:0;
    if(y)*y=n&&!n->dead&&n->kind==UI_SCROLL?n->scroll_y:0;
}
void UI_SetMouseFilter(ui_control_t *n,ui_mouse_filter_t f){if(n&&!n->dead&&f>=UI_MOUSE_STOP&&f<=UI_MOUSE_IGNORE)n->mouse_filter=f;}
void UI_SetFocusable(ui_control_t *n,bool v){if(n&&!n->dead){n->focusable=v;if(!v&&n->context->focus==n)n->context->focus=NULL;}}
bool UI_SetText(ui_control_t *n,const char *text)
{
    if(!n||n->dead||!text||(n->kind==UI_LINE_EDIT&&!valid_line_text(text)))return false;
    size_t len=strlen(text);if(len==SIZE_MAX)return false;char *p=malloc(len+1);if(!p)return false;
    memcpy(p,text,len+1);free(n->text);n->text=p;n->caret=len;return true;
}
const char *UI_GetText(const ui_control_t *n){return n&&!n->dead&&n->text?n->text:"";}
void UI_SetChecked(ui_control_t *n,bool v){if(n&&!n->dead&&n->kind==UI_CHECKBOX)n->checked=v;}
bool UI_IsChecked(const ui_control_t *n){return n&&!n->dead&&n->kind==UI_CHECKBOX&&n->checked;}
void UI_SetAction(ui_control_t *n,ui_action_fn f,void *u){if(n&&!n->dead){n->action=f;n->action_user=u;}}
void UI_SetChanged(ui_control_t *n,ui_action_fn f,void *u){if(n&&!n->dead){n->changed=f;n->changed_user=u;}}
void UI_SetDraw(ui_control_t *n,ui_draw_fn f,void *u){if(n&&!n->dead){n->draw=f;n->draw_user=u;}}
void UI_SetEvent(ui_control_t *n,ui_event_fn f,void *u){if(n&&!n->dead){n->event=f;n->event_user=u;}}
static void apply_theme(ui_control_t *n,const ui_theme_t *t)
{
    if(!n->custom_padding)n->padding=nonnegative(t->padding);
    if(!n->custom_spacing)n->spacing=nonnegative(t->spacing);
    for(ui_control_t *child=n->first;child;child=child->next)apply_theme(child,t);
}
void UI_SetTheme(ui_context_t *c,const ui_theme_t *t){if(c&&t){c->theme=*t;apply_theme(c->root,t);}}
ui_rect_t UI_GetRect(const ui_control_t *n){return n&&!n->dead?n->rect:(ui_rect_t){0,0,0,0};}
ui_control_t *UI_GetFocus(const ui_context_t *c){return c?c->focus:NULL;}
ui_control_t *UI_GetHover(const ui_context_t *c){return c?c->hover:NULL;}
bool UI_WantsText(const ui_context_t *c)
{return c&&c->focus&&c->focus->kind==UI_LINE_EDIT&&effectively_active(c->focus);}

static void measure(ui_control_t *n,unsigned depth)
{
    if(depth>=UI_MAX_DEPTH)return;
    float w=0,h=0;size_t count=0;
    for(ui_control_t *x=n->first;x;x=x->next)if(x->visible&&!x->dead){
        measure(x,depth+1);++count;
        if(n->kind==UI_HBOX){w+=x->measured_width;h=fmaxf(h,x->measured_height);}
        else if(n->kind==UI_VBOX){h+=x->measured_height;w=fmaxf(w,x->measured_width);}
    }
    if(n->kind==UI_HBOX||n->kind==UI_VBOX){if(count>1){if(n->kind==UI_HBOX)w+=(count-1)*n->spacing;else h+=(count-1)*n->spacing;}w+=2*n->padding;h+=2*n->padding;}
    else if(n->kind==UI_LABEL||n->kind==UI_BUTTON||n->kind==UI_CHECKBOX||n->kind==UI_LINE_EDIT){
        const ui_theme_t *t=&n->context->theme;
        w=(float)strlen(n->text?n->text:"")*nonnegative(t->glyph_width);h=nonnegative(t->line_height);
        if(n->kind!=UI_LABEL){w+=2*n->padding;h+=2*n->padding;}
        if(n->kind==UI_CHECKBOX)w+=nonnegative(t->line_height)+n->spacing;
        if(n->kind==UI_LINE_EDIT)w=fmaxf(w,96);
    }
    n->measured_width=fmaxf(n->min_width,w);n->measured_height=fmaxf(n->min_height,h);
}
static void arrange(ui_control_t *n,ui_rect_t rect,ui_rect_t clip,unsigned depth)
{
    if(depth>=UI_MAX_DEPTH)return;
    n->rect=rect;n->clip=clip;
    ui_rect_t child_clip=(n->clip_children||n->kind==UI_SCROLL)?intersection(clip,rect):clip;
    if(n->kind==UI_HBOX||n->kind==UI_VBOX){
        bool horizontal=n->kind==UI_HBOX;float inner_w=fmaxf(0,rect.width-2*n->padding),inner_h=fmaxf(0,rect.height-2*n->padding);
        float available=horizontal?inner_w:inner_h,total=0;size_t count=0,expanding=0;
        for(ui_control_t *c=n->first;c;c=c->next)if(c->visible&&!c->dead){++count;total+=horizontal?c->measured_width:c->measured_height;if(horizontal?c->expand_x:c->expand_y)++expanding;}
        if(count>1)total+=(count-1)*n->spacing;
        float extra=expanding?fmaxf(0,available-total)/(float)expanding:0,position=0;
        for(ui_control_t *c=n->first;c;c=c->next)if(c->visible&&!c->dead){
            float size=(horizontal?c->measured_width:c->measured_height)+((horizontal?c->expand_x:c->expand_y)?extra:0);
            ui_rect_t cr={rect.x+n->padding,rect.y+n->padding,inner_w,inner_h};
            if(horizontal){cr.x+=position;cr.width=size;cr.height=fmaxf(inner_h,c->measured_height);}
            else {cr.y+=position;cr.height=size;cr.width=fmaxf(inner_w,c->measured_width);}
            arrange(c,cr,child_clip,depth+1);position+=size+n->spacing;
        }
    }else{
        ui_rect_t inner={rect.x+n->padding,rect.y+n->padding,fmaxf(0,rect.width-2*n->padding),fmaxf(0,rect.height-2*n->padding)};
        if(n->kind==UI_ROOT)inner=rect;
        if(n->kind==UI_SCROLL){
            float extent_x=inner.width,extent_y=inner.height;
            for(ui_control_t *c=n->first;c;c=c->next)if(c->visible&&!c->dead){
                float l=inner.width*c->anchors.left+c->offsets.left;
                float t=inner.height*c->anchors.top+c->offsets.top;
                float r=inner.width*c->anchors.right+c->offsets.right;
                float b=inner.height*c->anchors.bottom+c->offsets.bottom;
                extent_x=fmaxf(extent_x,l+fmaxf(c->measured_width,r-l));
                extent_y=fmaxf(extent_y,t+fmaxf(c->measured_height,b-t));
            }
            n->scroll_max_x=fmaxf(0,extent_x-inner.width);n->scroll_max_y=fmaxf(0,extent_y-inner.height);
            n->scroll_x=fminf(n->scroll_x,n->scroll_max_x);n->scroll_y=fminf(n->scroll_y,n->scroll_max_y);
        }
        for(ui_control_t *c=n->first;c;c=c->next)if(c->visible&&!c->dead){
            float l=inner.x+inner.width*c->anchors.left+c->offsets.left;
            float t=inner.y+inner.height*c->anchors.top+c->offsets.top;
            float r=inner.x+inner.width*c->anchors.right+c->offsets.right;
            float b=inner.y+inner.height*c->anchors.bottom+c->offsets.bottom;
            ui_rect_t cr={l,t,fmaxf(c->measured_width,r-l),fmaxf(c->measured_height,b-t)};
            if(n->kind==UI_SCROLL){cr.x-=n->scroll_x;cr.y-=n->scroll_y;}
            arrange(c,cr,child_clip,depth+1);
        }
    }
}
void UI_Layout(ui_context_t *c,float width,float height)
{
    if(!c||c->destroy_requested)return;
    measure(c->root,0);ui_rect_t r={0,0,nonnegative(width),nonnegative(height)};arrange(c->root,r,r,0);
}
static void paint_rect(ui_context_t *c,ui_rect_t r,ui_rect_t clip,ui_color_t color)
{if(!c->destroy_requested&&c->renderer.rect&&intersection(r,clip).width>0&&intersection(r,clip).height>0)c->renderer.rect(c->renderer_user,r,clip,color);}
static bool is_modal(const ui_context_t *c,const ui_control_t *n)
{for(unsigned i=0;i<c->modal_count;++i)if(c->modals[i]==n)return true;return false;}
static void draw_control(ui_context_t *c,ui_control_t *n,unsigned depth,bool modal_root)
{
    if(depth>=UI_MAX_DEPTH||c->destroy_requested||!effectively_visible(n)||(!modal_root&&is_modal(c,n)))return;
    ui_theme_t *t=&c->theme;ui_rect_t r=n->rect,clip=n->clip;
    if(n->kind==UI_PANEL||n->kind==UI_POPUP||n->kind==UI_SCROLL)paint_rect(c,r,clip,t->panel);
    else if(n->kind==UI_BUTTON||n->kind==UI_CHECKBOX||n->kind==UI_LINE_EDIT){
        ui_color_t col=!n->enabled?t->button_disabled:n->pressed?t->button_pressed:c->hover==n?t->button_hover:t->button;
        paint_rect(c,r,clip,col);
        if(c->focus==n&&!c->destroy_requested){ui_rect_t edge={r.x,r.y,r.width,1};paint_rect(c,edge,clip,t->focus);edge.y=r.y+r.height-1;paint_rect(c,edge,clip,t->focus);edge=(ui_rect_t){r.x,r.y,1,r.height};paint_rect(c,edge,clip,t->focus);edge.x=r.x+r.width-1;paint_rect(c,edge,clip,t->focus);}
        if(n->kind==UI_CHECKBOX){float size=fminf(nonnegative(t->line_height),fmaxf(0,r.height-2*n->padding));ui_rect_t box={r.x+n->padding,r.y+(r.height-size)*0.5f,size,size};paint_rect(c,box,clip,n->checked?t->check:t->panel);}
    }
    if(!c->destroy_requested&&effectively_visible(n)&&(n->kind==UI_LABEL||n->kind==UI_BUTTON||n->kind==UI_CHECKBOX||n->kind==UI_LINE_EDIT)&&c->renderer.text&&n->text&&*n->text){
        float x=r.x+(n->kind==UI_LABEL?0:n->padding),y=r.y+(r.height-nonnegative(t->line_height))*0.5f;
        if(n->kind==UI_CHECKBOX)x+=nonnegative(t->line_height)+n->spacing;
        c->renderer.text(c->renderer_user,x,y,n->text,intersection(clip,r),t->text);
    }
    if(!c->destroy_requested&&effectively_visible(n)&&n->kind==UI_LINE_EDIT&&c->focus==n&&n->enabled){
        size_t glyphs=0;for(size_t i=0;i<n->caret;++i)if(((unsigned char)n->text[i]&0xc0)!=0x80)++glyphs;
        ui_rect_t caret={r.x+n->padding+glyphs*nonnegative(t->glyph_width),r.y+n->padding,1,fmaxf(0,r.height-2*n->padding)};
        paint_rect(c,caret,intersection(clip,r),t->focus);
    }
    if(!c->destroy_requested&&effectively_visible(n)&&n->draw)
        n->draw(n,r,intersection(clip,r),n->draw_user);
    for(ui_control_t *x=n->first,*next;x&&!c->destroy_requested&&effectively_visible(n);x=next){next=x->next;draw_control(c,x,depth+1,false);}
}
void UI_Draw(ui_context_t *c)
{
    if(!c||c->destroy_requested)return;
    begin(c);draw_control(c,c->root,0,false);
    for(unsigned i=0;i<c->modal_count&&!c->destroy_requested;++i)draw_control(c,c->modals[i],0,true);
    end(c);
}
static ui_control_t *hit(ui_control_t *n,float x,float y,unsigned depth)
{
    if(depth>=UI_MAX_DEPTH||n->dead||!n->visible||!n->enabled||!contains(n->clip,x,y))return NULL;
    for(ui_control_t *q=n->last;q;q=q->prev){ui_control_t *found=hit(q,x,y,depth+1);if(found)return found;}
    return n->mouse_filter!=UI_MOUSE_IGNORE&&contains(n->rect,x,y)?n:NULL;
}
ui_control_t *UI_GetModal(const ui_context_t *c)
{return c&&c->modal_count?c->modals[c->modal_count-1]:NULL;}
bool UI_CanCancel(const ui_context_t *c)
{
    if(!c||c->destroy_requested)return false;
    if(c->modal_count||c->popup_escape_capture)return true;
    for(const ui_control_t *n=c->focus;n;n=n->parent)
        if(n->kind==UI_POPUP&&effectively_active(n))return true;
    return false;
}
bool UI_Scroll(ui_context_t *c,float x,float y,float dx,float dy)
{
    if(!c||c->destroy_requested||!isfinite(x)||!isfinite(y)||!isfinite(dx)||!isfinite(dy))return false;
    begin(c);ui_control_t *base=UI_GetModal(c);ui_control_t *target=hit(base?base:c->root,x,y,0);
    bool consumed=base!=NULL;
    for(ui_control_t *n=target;n;n=n->parent){
        if(n->kind==UI_SCROLL){
            consumed=true;float old_x=n->scroll_x,old_y=n->scroll_y;
            UI_SetScroll(n,n->scroll_x+dx,n->scroll_y+dy);
            if(n->scroll_x!=old_x||n->scroll_y!=old_y)break;
        }
        if(n==base)break;
    }
    end(c);return consumed;
}
static bool send(ui_control_t *n,const ui_event_t *e)
{
    bool handled=false;
    while(n&&!n->dead&&!n->context->destroy_requested){ui_control_t *parent=n->parent;ui_mouse_filter_t filter=n->mouse_filter;
        if(filter!=UI_MOUSE_IGNORE&&n->event&&n->event(n,e,n->event_user))handled=true;
        if(handled||filter==UI_MOUSE_STOP)break;
        n=parent;
    }
    return handled;
}
static void focus_inner(ui_context_t *c,ui_control_t *n)
{
    if(n&&(!n->focusable||n->context!=c||!effectively_active(n)))n=NULL;
    if(n&&c->modal_count&&!subtree(n,c->modals[c->modal_count-1]))return;
    if(c->focus==n)return;
    ui_control_t *old=c->focus;c->focus=NULL;
    if(old&&!old->dead&&old->event){ui_event_t e={.type=UI_EVENT_FOCUS_OUT};old->event(old,&e,old->event_user);}
    if(!c->destroy_requested&&n&&!n->dead&&effectively_active(n)){c->focus=n;if(n->event){ui_event_t e={.type=UI_EVENT_FOCUS_IN};n->event(n,&e,n->event_user);}}
}
void UI_Focus(ui_context_t *c,ui_control_t *n){if(!c||c->destroy_requested)return;begin(c);focus_inner(c,n);end(c);}
bool UI_MouseMove(ui_context_t *c,float x,float y)
{
    if(!c||c->destroy_requested||!isfinite(x)||!isfinite(y))return false;
    begin(c);ui_control_t *base=c->modal_count?c->modals[c->modal_count-1]:c->root;
    ui_control_t *target=hit(base,x,y,0),*old=c->hover;
    if(old!=target){c->hover=target;
        if(old&&!old->dead&&old->event){ui_event_t e={.type=UI_EVENT_MOUSE_EXIT,.x=x,.y=y};old->event(old,&e,old->event_user);}
        if(!c->destroy_requested&&target&&effectively_active(target)&&target->event){ui_event_t e={.type=UI_EVENT_MOUSE_ENTER,.x=x,.y=y};target->event(target,&e,target->event_user);}}
    bool captured=has_capture(c);ui_event_t e={.type=UI_EVENT_MOUSE_MOVE,.x=x,.y=y};
    if(captured){
        ui_control_t *seen[UI_POINTER_BUTTONS];int count=0;
        for(int i=0;i<UI_POINTER_BUTTONS&&!c->destroy_requested;++i){ui_control_t *receiver=c->capture[i];bool duplicate=false;
            for(int j=0;j<count;++j)if(seen[j]==receiver)duplicate=true;
            if(receiver&&!duplicate&&effectively_active(receiver)){seen[count++]=receiver;send(receiver,&e);}}
    }else if(!c->destroy_requested&&c->hover&&effectively_active(c->hover))send(c->hover,&e);
    bool consumed=captured||c->hover||c->modal_count;end(c);return consumed;
}
bool UI_MouseButton(ui_context_t *c,int button,bool down,float x,float y)
{
    if(!c||c->destroy_requested||!isfinite(x)||!isfinite(y))return false;
    begin(c);
    int slot=button>=1&&button<=UI_POINTER_BUTTONS?button-1:-1;
    if(slot>=0&&!down&&c->cancelled_capture[slot]){c->cancelled_capture[slot]=false;end(c);return true;}
    if(slot>=0&&!down&&c->modal_outside_capture[slot]){c->modal_outside_capture[slot]=false;end(c);return true;}
    if(slot>=0&&down&&c->capture[slot]){end(c);return true;}
    if(slot>=0&&down){c->modal_outside_capture[slot]=false;c->cancelled_capture[slot]=false;}
    if(down&&button==1&&!c->modal_count){
        for(ui_control_t *p=c->focus;p;p=p->parent)
            if(p->kind==UI_POPUP){
                if(p->popup_close_outside&&(!contains(p->rect,x,y)||!contains(p->clip,x,y)))UI_ClosePopup(p);
                break;
            }
    }
    ui_control_t *base=c->modal_count?c->modals[c->modal_count-1]:c->root;
    ui_control_t *target=down?hit(base,x,y,0):((slot>=0&&c->capture[slot])?c->capture[slot]:hit(base,x,y,0));
    if(!target&&c->modal_count){
        if(down&&slot>=0)c->modal_outside_capture[slot]=true;
        if(down&&button==1&&base->popup_close_outside)UI_ClosePopup(base);
        end(c);return true;
    }
    if(!target){if(down&&button==1)focus_inner(c,NULL);end(c);return false;}
    if(down&&slot>=0)c->capture[slot]=target;
    if(down&&button==1){target->pressed=true;focus_inner(c,target->focusable?target:NULL);
        if(!target->dead&&target->kind==UI_LINE_EDIT){
            float cw=nonnegative(c->theme.glyph_width);float delta=x-target->rect.x-target->padding;
            size_t want=cw>0&&delta>0?(size_t)(delta/cw+0.5f):0,pos=0;
            while(want&&target->text[pos]){++pos;while(((unsigned char)target->text[pos]&0xc0)==0x80)++pos;--want;}
            target->caret=pos;
        }}
    ui_event_t e={.type=down?UI_EVENT_MOUSE_DOWN:UI_EVENT_MOUSE_UP,.x=x,.y=y,.button=button};send(target,&e);
    if(!down&&button==1){
        bool activate=!c->destroy_requested&&c->capture[0]==target&&!target->dead&&target->pressed&&effectively_active(target)&&contains(target->rect,x,y)&&contains(target->clip,x,y);
        c->capture[0]=NULL;target->pressed=false;
        if(activate&&(target->kind==UI_BUTTON||target->kind==UI_CHECKBOX)){
            if(target->kind==UI_CHECKBOX)target->checked=!target->checked;
            if(target->action)target->action(target,target->action_user);
        }
    }
    else if(!down&&slot>=0)c->capture[slot]=NULL;
    end(c);return true;
}
static void focus_collect(ui_control_t *n,ui_control_t **first,ui_control_t **last,ui_control_t *current,ui_control_t **previous,ui_control_t **next,unsigned depth)
{
    if(depth>=UI_MAX_DEPTH||n->dead||!n->visible||!n->enabled)return;
    if(n->focusable){if(!*first)*first=n;if(*last==current&&!*next)*next=n;if(n==current)*previous=*last;*last=n;}
    for(ui_control_t *x=n->first;x;x=x->next)focus_collect(x,first,last,current,previous,next,depth+1);
}
void UI_SetPopupCloseOnOutside(ui_control_t *n,bool enabled)
{if(n&&!n->dead&&n->kind==UI_POPUP)n->popup_close_outside=enabled;}
bool UI_OpenPopup(ui_control_t *n,bool modal)
{
    if(!n||n->dead||n->context->destroy_requested||n->kind!=UI_POPUP||!n->parent||!n->enabled)return false;
    ui_context_t *c=n->context;
    for(unsigned i=0;i<c->modal_count;++i)if(c->modals[i]==n)return modal&&i+1==c->modal_count;
    if(modal&&c->modal_count==UI_MAX_MODALS)return false;
    if(!modal&&c->modal_count&&!subtree(n,c->modals[c->modal_count-1]))return false;
    begin(c);bool old_visible=n->visible;n->visible=true;
    if(!effectively_active(n)){n->visible=old_visible;end(c);return false;}
    ui_control_t *prior=c->focus;
    if(modal){clear_captures(c,NULL);c->hover=NULL;c->modals[c->modal_count]=n;c->restore_focus[c->modal_count]=prior;++c->modal_count;}
    UI_Layout(c,c->root->rect.width,c->root->rect.height);
    ui_control_t *first=NULL,*last=NULL,*previous=NULL,*next=NULL;
    focus_collect(n,&first,&last,NULL,&previous,&next,0);
    if(modal||first)focus_inner(c,first);
    bool result=!c->destroy_requested&&!n->dead&&n->visible;
    end(c);return result;
}
void UI_ClosePopup(ui_control_t *n)
{
    if(!n||n->dead||n->context->destroy_requested||n->kind!=UI_POPUP)return;
    ui_context_t *c=n->context;begin(c);
    unsigned index=0;while(index<c->modal_count&&c->modals[index]!=n)++index;
    if(index<c->modal_count){
        ui_control_t *restore=c->restore_focus[index];
        for(unsigned i=index;i<c->modal_count;++i){ui_control_t *m=c->modals[i];m->visible=false;
            if(subtree(c->hover,m))c->hover=NULL;
            clear_captures(c,m);
            c->modals[i]=c->restore_focus[i]=NULL;
        }
        c->modal_count=index;focus_inner(c,restore);
    }else{
        dismiss_subtree_modals(c,n);n->visible=false;
        if(subtree(c->focus,n))focus_inner(c,NULL);
        if(subtree(c->hover,n))c->hover=NULL;
        clear_captures(c,n);
    }
    end(c);
}
bool UI_Key(ui_context_t *c,ui_key_t key,bool down,bool shift)
{
    if(!c||c->destroy_requested)return false;
    begin(c);
    if(key==UI_KEY_ESCAPE&&c->popup_escape_capture){if(!down)c->popup_escape_capture=false;end(c);return true;}
    if(down&&key==UI_KEY_ESCAPE&&c->modal_count){c->popup_escape_capture=true;UI_ClosePopup(c->modals[c->modal_count-1]);end(c);return true;}
    if(down&&key==UI_KEY_ESCAPE&&!c->modal_count){
        for(ui_control_t *n=c->focus;n;n=n->parent)if(n->kind==UI_POPUP){c->popup_escape_capture=true;UI_ClosePopup(n);end(c);return true;}
    }
    if(down&&key==UI_KEY_TAB){ui_control_t *first=NULL,*last=NULL,*prev=NULL,*next=NULL;
        focus_collect(c->modal_count?c->modals[c->modal_count-1]:c->root,&first,&last,c->focus,&prev,&next,0);
        if(first){focus_inner(c,shift?(prev?prev:last):(next?next:first));end(c);return true;}}
    ui_control_t *n=c->focus;if(!n||n->dead||!effectively_active(n)){bool consumed=c->modal_count!=0;end(c);return consumed;}
    ui_event_t e={.type=down?UI_EVENT_KEY_DOWN:UI_EVENT_KEY_UP,.key=key,.shift=shift};bool handled=send(n,&e);
    if(down&&!handled&&!c->destroy_requested&&effectively_active(n)&&n->kind==UI_LINE_EDIT){
        size_t len=strlen(n->text),at=n->caret,next=at;
        if(key==UI_KEY_LEFT&&at){--n->caret;while(n->caret&&((unsigned char)n->text[n->caret]&0xc0)==0x80)--n->caret;}
        else if(key==UI_KEY_RIGHT&&at<len){++n->caret;while(n->caret<len&&((unsigned char)n->text[n->caret]&0xc0)==0x80)++n->caret;}
        else if(key==UI_KEY_HOME)n->caret=0;
        else if(key==UI_KEY_END)n->caret=len;
        else if((key==UI_KEY_BACKSPACE&&at)||(key==UI_KEY_DELETE&&at<len)){
            if(key==UI_KEY_BACKSPACE){--at;while(at&&((unsigned char)n->text[at]&0xc0)==0x80)--at;}
            else {++next;while(next<len&&((unsigned char)n->text[next]&0xc0)==0x80)++next;}
            memmove(n->text+at,n->text+next,len-next+1);n->caret=at;
            if(n->changed)n->changed(n,n->changed_user);
        }else if(key==UI_KEY_ENTER&&n->action)n->action(n,n->action_user);
    }
    else if(down&&!handled&&!c->destroy_requested&&effectively_active(n)&&(key==UI_KEY_ENTER||key==UI_KEY_SPACE)&&(n->kind==UI_BUTTON||n->kind==UI_CHECKBOX)){
        if(n->kind==UI_CHECKBOX)n->checked=!n->checked;
        if(n->action)n->action(n,n->action_user);
    }
    end(c);return true;
}
bool UI_TextInput(ui_context_t *c,const char *text)
{
    if(!c||c->destroy_requested||!text||!*text||!valid_line_text(text))return false;
    begin(c);ui_control_t *n=c->focus;
    if(!n||n->dead||n->kind!=UI_LINE_EDIT||!effectively_active(n)){end(c);return false;}
    size_t len=strlen(n->text),add=strlen(text);
    if(add>4096-len){end(c);return false;}
    /* Input may alias the line's current text; preserve it across realloc and
     * overlapping insertions. */
    char *insert=malloc(add);if(!insert){end(c);return false;}memcpy(insert,text,add);
    char *next=realloc(n->text,len+add+1);if(!next){free(insert);end(c);return false;}
    n->text=next;memmove(next+n->caret+add,next+n->caret,len-n->caret+1);memcpy(next+n->caret,insert,add);n->caret+=add;free(insert);
    if(n->changed)n->changed(n,n->changed_user);
    end(c);return true;
}
