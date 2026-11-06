#include "ui_control.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static int rects, texts, clicks, changes, enters, exits, bubbled;
static ui_rect_t last_clip;
static void rect(void *u,ui_rect_t r,ui_rect_t clip,ui_color_t color)
{ (void)u;(void)r;(void)color;++rects;last_clip=clip; }
static void text(void *u,float x,float y,const char *s,ui_rect_t clip,ui_color_t color)
{ (void)u;(void)x;(void)y;(void)s;(void)color;++texts;last_clip=clip; }
static void action(ui_control_t *n,void *u){(void)n;(*(int *)u)++;}
static void remove_self(ui_control_t *n,void *u){(*(int *)u)++;UI_Remove(n);}
static void destroy_context(ui_control_t *n,void *u){(void)n;UI_Destroy((ui_context_t *)u);}
static bool destroy_on_down(ui_control_t *n,const ui_event_t *e,void *u)
{(void)n;if(e->type==UI_EVENT_MOUSE_DOWN)UI_Destroy((ui_context_t *)u);return false;}
typedef struct {ui_context_t *context;int calls;} destroy_renderer_t;
static void destroy_during_draw(void *u,ui_rect_t r,ui_rect_t clip,ui_color_t color)
{(void)r;(void)clip;(void)color;destroy_renderer_t *d=u;++d->calls;UI_Destroy(d->context);}
typedef struct {ui_context_t *context;ui_rect_t rect,clip;int calls;bool remove,destroy;} custom_state_t;
static void custom_draw(ui_control_t *n,ui_rect_t rect,ui_rect_t clip,void *user)
{
    custom_state_t *state=user;state->rect=rect;state->clip=clip;++state->calls;
    if(state->remove)UI_Remove(n);
    if(state->destroy)UI_Destroy(state->context);
}
static bool events(ui_control_t *n,const ui_event_t *e,void *u)
{(void)n;(void)u;if(e->type==UI_EVENT_MOUSE_ENTER)++enters;if(e->type==UI_EVENT_MOUSE_EXIT)++exits;if(e->type==UI_EVENT_MOUSE_DOWN)++bubbled;return false;}
typedef struct {int downs[9],ups[9],moves;} pointer_counts_t;
static bool pointer_events(ui_control_t *n,const ui_event_t *e,void *u)
{
    (void)n;pointer_counts_t *counts=u;
    if(e->button>=1&&e->button<=8){if(e->type==UI_EVENT_MOUSE_DOWN)++counts->downs[e->button];if(e->type==UI_EVENT_MOUSE_UP)++counts->ups[e->button];}
    if(e->type==UI_EVENT_MOUSE_MOVE)++counts->moves;
    return false;
}
static int closef(float a,float b){return fabsf(a-b)<0.001f;}
static void anchors_and_clip(void)
{
    ui_renderer_t renderer={rect,text};ui_context_t *c=UI_Create(NULL,&renderer,NULL);assert(c);
    ui_control_t *p=UI_Add(UI_Root(c),UI_PANEL,"");assert(p);UI_SetPadding(p,0);UI_SetClipChildren(p,true);
    UI_SetRect(p,(ui_edges_t){0,0,1,1},(ui_edges_t){10,20,-10,-20});
    ui_control_t *x=UI_Add(p,UI_BUTTON,"OK");assert(x);UI_SetRect(x,(ui_edges_t){1,1,1,1},(ui_edges_t){-60,-30,20,10});
    UI_Layout(c,200,120);ui_rect_t pr=UI_GetRect(p),xr=UI_GetRect(x);
    assert(closef(pr.x,10)&&closef(pr.y,20)&&closef(pr.width,180)&&closef(pr.height,80));
    assert(closef(xr.x,130)&&closef(xr.y,70)&&closef(xr.width,80)&&closef(xr.height,40));
    UI_Draw(c);assert(rects>=2&&texts==1);assert(closef(last_clip.x,130)&&closef(last_clip.y,70)&&closef(last_clip.width,60)&&closef(last_clip.height,30));
    UI_Layout(c,300,160);xr=UI_GetRect(x);assert(closef(xr.x,230)&&closef(xr.y,110));
    assert(!UI_MouseButton(c,1,true,295,130)); /* child outside clipped panel */
    UI_Destroy(c);
}
static void containers(void)
{
    ui_context_t *c=UI_Create(NULL,NULL,NULL);assert(c);ui_control_t *box=UI_Add(UI_Root(c),UI_HBOX,"");
    UI_SetRect(box,(ui_edges_t){0,0,1,0},(ui_edges_t){0,0,0,40});UI_SetPadding(box,2);UI_SetSpacing(box,3);
    ui_control_t *a=UI_Add(box,UI_PANEL,""),*b=UI_Add(box,UI_PANEL,""),*hidden=UI_Add(box,UI_PANEL,"");
    UI_SetMinSize(a,20,10);UI_SetMinSize(b,30,10);UI_SetExpand(b,true,false);UI_SetVisible(hidden,false);
    UI_Layout(c,200,100);ui_rect_t ar=UI_GetRect(a),br=UI_GetRect(b);
    assert(closef(ar.x,2)&&closef(ar.y,2)&&closef(ar.width,20));
    assert(closef(br.x,25)&&closef(br.width,173)&&closef(br.height,36));
    assert(!UI_Reparent(box,a));assert(UI_Reparent(a,UI_Root(c)));UI_Layout(c,200,100);
    UI_Destroy(c);
}
static void input_and_lifecycle(void)
{
    ui_context_t *c=UI_Create(NULL,NULL,NULL);assert(c);ui_control_t *p=UI_Add(UI_Root(c),UI_PANEL,"");UI_SetPadding(p,0);
    UI_SetRect(p,(ui_edges_t){0,0,1,1},(ui_edges_t){0,0,0,0});UI_SetEvent(p,events,NULL);UI_SetMouseFilter(p,UI_MOUSE_PASS);
    ui_control_t *a=UI_Add(p,UI_BUTTON,"A"),*b=UI_Add(p,UI_CHECKBOX,"B");
    UI_SetRect(a,(ui_edges_t){0,0,0,0},(ui_edges_t){0,0,50,30});UI_SetRect(b,(ui_edges_t){0,0,0,0},(ui_edges_t){60,0,120,30});
    UI_SetAction(a,action,&clicks);UI_SetAction(b,action,&clicks);UI_SetEvent(a,events,NULL);UI_SetMouseFilter(a,UI_MOUSE_PASS);
    UI_Layout(c,200,100);assert(UI_MouseMove(c,10,10));assert(enters==1&&UI_GetHover(c)==a);
    assert(UI_MouseButton(c,1,true,10,10));assert(UI_GetFocus(c)==a);assert(bubbled==2);
    assert(UI_MouseMove(c,180,80));assert(exits==1);assert(UI_MouseButton(c,1,false,180,80));assert(clicks==0);
    assert(UI_MouseButton(c,1,true,10,10));assert(UI_MouseButton(c,1,false,10,10));assert(clicks==1);
    assert(UI_Key(c,UI_KEY_TAB,true,false));assert(UI_GetFocus(c)==b);
    assert(UI_Key(c,UI_KEY_SPACE,true,false));assert(UI_IsChecked(b)&&clicks==2);
    assert(UI_Key(c,UI_KEY_TAB,true,true));assert(UI_GetFocus(c)==a);
    UI_SetEnabled(a,false);assert(UI_GetFocus(c)==NULL);assert(UI_Key(c,UI_KEY_TAB,true,false));assert(UI_GetFocus(c)==b);
    UI_SetAction(b,remove_self,&clicks);assert(UI_MouseButton(c,1,true,65,5));assert(UI_MouseButton(c,1,false,65,5));assert(clicks==3&&UI_GetFocus(c)==NULL);
    UI_Destroy(c);
}
static void line_edit(void)
{
    ui_context_t *c=UI_Create(NULL,NULL,NULL);assert(c);ui_control_t *n=UI_Add(UI_Root(c),UI_LINE_EDIT,"abc");assert(n);
    UI_SetRect(n,(ui_edges_t){0,0,0,0},(ui_edges_t){0,0,160,25});UI_SetChanged(n,action,&changes);UI_SetAction(n,action,&clicks);
    UI_Layout(c,200,100);assert(!UI_WantsText(c));UI_Focus(c,n);assert(UI_GetFocus(c)==n&&UI_WantsText(c));
    assert(UI_TextInput(c,"\xc3\xa9"));assert(!strcmp(UI_GetText(n),"abc\xc3\xa9"));
    assert(UI_Key(c,UI_KEY_LEFT,true,false));assert(UI_TextInput(c,"X"));assert(!strcmp(UI_GetText(n),"abcX\xc3\xa9"));
    assert(UI_Key(c,UI_KEY_DELETE,true,false));assert(!strcmp(UI_GetText(n),"abcX"));
    assert(UI_Key(c,UI_KEY_BACKSPACE,true,false));assert(!strcmp(UI_GetText(n),"abc"));
    assert(UI_Key(c,UI_KEY_HOME,true,false));assert(UI_Key(c,UI_KEY_DELETE,true,false));assert(!strcmp(UI_GetText(n),"bc"));
    assert(!UI_TextInput(c,"\xc0\x80")&&!UI_TextInput(c,"\xf0\x90")&&!UI_TextInput(c,"\n"));assert(changes==5);
    assert(UI_Key(c,UI_KEY_END,true,false));assert(UI_TextInput(c,UI_GetText(n)));
    assert(!strcmp(UI_GetText(n),"bcbc")&&changes==6); /* input can alias current text */
    int before=clicks;assert(UI_Key(c,UI_KEY_ENTER,true,false));assert(clicks==before+1);
    UI_Remove(n);assert(!UI_WantsText(c)&&!UI_TextInput(c,"x"));UI_Destroy(c);
}
static void scrolling(void)
{
    ui_context_t *c=UI_Create(NULL,NULL,NULL);assert(c);
    ui_control_t *s=UI_Add(UI_Root(c),UI_SCROLL,"");assert(s);UI_SetPadding(s,0);
    UI_SetRect(s,(ui_edges_t){0,0,0,0},(ui_edges_t){0,0,100,60});
    ui_control_t *list=UI_Add(s,UI_VBOX,"");UI_SetPadding(list,0);UI_SetSpacing(list,2);
    UI_SetRect(list,(ui_edges_t){0,0,1,0},(ui_edges_t){0,0,0,0});
    ui_control_t *buttons[4];for(int i=0;i<4;++i){buttons[i]=UI_Add(list,UI_BUTTON,"item");UI_SetMinSize(buttons[i],40,30);}
    UI_Layout(c,200,150);ui_rect_t a=UI_GetRect(buttons[0]),b=UI_GetRect(buttons[2]);
    assert(closef(a.y,0)&&closef(b.y,64));
    assert(UI_Scroll(c,10,10,0,40));float x,y;UI_GetScroll(s,&x,&y);assert(closef(x,0)&&closef(y,40));
    a=UI_GetRect(buttons[0]);b=UI_GetRect(buttons[2]);assert(closef(a.y,-40)&&closef(b.y,24));
    assert(!UI_MouseButton(c,1,true,10,70)); /* outside viewport clip */
    UI_SetScroll(s,0,10000);UI_GetScroll(s,&x,&y);assert(closef(y,66));
    assert(UI_Scroll(c,10,10,0,-10000));UI_GetScroll(s,&x,&y);assert(closef(y,0));
    assert(!UI_Scroll(c,150,100,0,20));UI_Destroy(c);
}
static void popups(void)
{
    ui_context_t *c=UI_Create(NULL,NULL,NULL);assert(c);int background_clicks=0,dialog_clicks=0;
    ui_control_t *bg=UI_Add(UI_Root(c),UI_BUTTON,"background");UI_SetRect(bg,(ui_edges_t){0,0,1,1},(ui_edges_t){0,0,0,0});
    UI_SetAction(bg,action,&background_clicks);
    ui_control_t *p=UI_Add(UI_Root(c),UI_POPUP,"");UI_SetPadding(p,0);UI_SetRect(p,(ui_edges_t){0,0,0,0},(ui_edges_t){20,20,120,100});
    ui_control_t *inside=UI_Add(p,UI_BUTTON,"inside");UI_SetRect(inside,(ui_edges_t){0,0,0,0},(ui_edges_t){0,0,60,30});UI_SetAction(inside,action,&dialog_clicks);
    ui_control_t *nested=UI_Add(UI_Root(c),UI_POPUP,"");UI_SetPadding(nested,0);UI_SetRect(nested,(ui_edges_t){0,0,0,0},(ui_edges_t){30,30,100,90});
    ui_control_t *nested_button=UI_Add(nested,UI_BUTTON,"nested");UI_SetRect(nested_button,(ui_edges_t){0,0,0,0},(ui_edges_t){0,0,60,30});
    UI_Layout(c,200,150);UI_Focus(c,bg);assert(UI_GetFocus(c)==bg&&!UI_CanCancel(c));
    assert(UI_OpenPopup(p,true)&&UI_GetModal(c)==p&&UI_GetFocus(c)==inside&&UI_CanCancel(c));
    UI_Focus(c,bg);assert(UI_GetFocus(c)==inside); /* modal traps programmatic focus */
    UI_SetPopupCloseOnOutside(p,false);assert(UI_MouseButton(c,1,true,180,130));assert(UI_MouseButton(c,1,false,180,130));
    assert(UI_GetModal(c)==p&&background_clicks==0);
    assert(UI_MouseButton(c,1,true,25,25));assert(UI_MouseButton(c,1,false,25,25));assert(dialog_clicks==1);
    assert(UI_OpenPopup(nested,true)&&UI_GetModal(c)==nested&&UI_GetFocus(c)==nested_button);
    assert(UI_Key(c,UI_KEY_ESCAPE,true,false)&&UI_GetModal(c)==p&&UI_GetFocus(c)==inside);
    assert(UI_Key(c,UI_KEY_ESCAPE,true,false)&&UI_GetModal(c)==p); /* repeat does not close next popup */
    assert(UI_Key(c,UI_KEY_ESCAPE,false,false)); /* release is not sent to restored focus */
    UI_SetPopupCloseOnOutside(p,true);assert(UI_MouseButton(c,1,true,180,130));assert(UI_GetModal(c)==NULL&&UI_GetFocus(c)==bg);
    assert(UI_MouseButton(c,1,false,180,130));assert(background_clicks==0); /* closing click not replayed */
    assert(UI_MouseButton(c,1,true,180,130));assert(UI_MouseButton(c,1,false,180,130));assert(background_clicks==1);
    assert(UI_OpenPopup(p,true));UI_Remove(p);assert(UI_GetModal(c)==NULL&&UI_GetFocus(c)==bg);
    assert(UI_OpenPopup(nested,true));UI_SetVisible(nested,false);assert(UI_GetModal(c)==NULL&&UI_GetFocus(c)==bg);
    assert(UI_OpenPopup(nested,false)&&UI_CanCancel(c));assert(UI_Key(c,UI_KEY_ESCAPE,true,false));assert(UI_GetModal(c)==NULL&&UI_CanCancel(c));
    assert(UI_Key(c,UI_KEY_ESCAPE,false,false)&&!UI_CanCancel(c));
    UI_Destroy(c);
}
static void pointer_capture(void)
{
    ui_context_t *c=UI_Create(NULL,NULL,NULL);assert(c);pointer_counts_t a={0},b={0};
    ui_control_t *one=UI_Add(UI_Root(c),UI_BUTTON,"A"),*two=UI_Add(UI_Root(c),UI_BUTTON,"B");
    UI_SetRect(one,(ui_edges_t){0,0,0,0},(ui_edges_t){0,0,50,30});UI_SetRect(two,(ui_edges_t){0,0,0,0},(ui_edges_t){60,0,110,30});
    UI_SetEvent(one,pointer_events,&a);UI_SetEvent(two,pointer_events,&b);UI_Layout(c,200,100);
    assert(UI_MouseButton(c,2,true,10,10));assert(UI_MouseButton(c,3,true,70,10));
    assert(UI_MouseMove(c,150,80));assert(a.moves==1&&b.moves==1);
    assert(UI_MouseButton(c,2,false,70,10));assert(UI_MouseButton(c,3,false,10,10));
    assert(a.downs[2]==1&&a.ups[2]==1&&b.downs[3]==1&&b.ups[3]==1&&a.ups[3]==0&&b.ups[2]==0);
    assert(UI_MouseButton(c,2,true,10,10));UI_Remove(one);
    assert(UI_MouseButton(c,2,false,70,10));assert(b.ups[2]==0); /* removed target's release is swallowed */
    UI_Destroy(c);
}
static void deferred_context_destruction(void)
{
    ui_context_t *c=UI_Create(NULL,NULL,NULL);assert(c);
    ui_control_t *button=UI_Add(UI_Root(c),UI_BUTTON,"destroy");UI_SetRect(button,(ui_edges_t){0,0,0,0},(ui_edges_t){0,0,100,30});
    UI_SetAction(button,destroy_context,c);UI_Layout(c,200,100);
    assert(UI_MouseButton(c,1,true,10,10));assert(UI_MouseButton(c,1,false,10,10));
    /* c is freed only as the outermost event returns; do not use it again. */
    c=UI_Create(NULL,NULL,NULL);button=UI_Add(UI_Root(c),UI_BUTTON,"destroy");
    UI_SetRect(button,(ui_edges_t){0,0,0,0},(ui_edges_t){0,0,100,30});UI_SetEvent(button,destroy_on_down,c);UI_Layout(c,200,100);
    assert(UI_MouseButton(c,1,true,10,10));
    destroy_renderer_t state={0};ui_renderer_t renderer={destroy_during_draw,NULL};
    c=UI_Create(NULL,&renderer,&state);state.context=c;
    ui_control_t *panel=UI_Add(UI_Root(c),UI_PANEL,"");UI_SetRect(panel,(ui_edges_t){0,0,1,1},(ui_edges_t){0,0,0,0});UI_Layout(c,200,100);
    UI_Draw(c);assert(state.calls==1);
}
static void custom_controls(void)
{
    ui_context_t *c=UI_Create(NULL,NULL,NULL);assert(c);
    ui_control_t *panel=UI_Add(UI_Root(c),UI_PANEL,"");UI_SetPadding(panel,0);UI_SetClipChildren(panel,true);
    UI_SetRect(panel,(ui_edges_t){0,0,0,0},(ui_edges_t){0,0,80,80});
    ui_control_t *custom=UI_Add(panel,UI_CUSTOM,"");assert(custom);UI_SetRect(custom,(ui_edges_t){0,0,0,0},(ui_edges_t){60,10,110,40});
    custom_state_t state={.context=c};UI_SetDraw(custom,custom_draw,&state);UI_SetMouseFilter(custom,UI_MOUSE_STOP);
    pointer_counts_t input={0};UI_SetEvent(custom,pointer_events,&input);UI_Layout(c,200,100);UI_Draw(c);
    assert(state.calls==1&&closef(state.rect.x,60)&&closef(state.rect.width,50));
    assert(closef(state.clip.x,60)&&closef(state.clip.width,20));
    assert(UI_MouseButton(c,2,true,65,15)&&UI_MouseButton(c,2,false,150,90));assert(input.downs[2]==1&&input.ups[2]==1);
    state.remove=true;UI_Draw(c);assert(state.calls==2);UI_Draw(c);assert(state.calls==2);UI_Destroy(c);
    c=UI_Create(NULL,NULL,NULL);custom=UI_Add(UI_Root(c),UI_CUSTOM,"");
    UI_SetRect(custom,(ui_edges_t){0,0,0,0},(ui_edges_t){0,0,50,30});
    state=(custom_state_t){.context=c,.destroy=true};UI_SetDraw(custom,custom_draw,&state);
    ui_control_t *later=UI_Add(UI_Root(c),UI_CUSTOM,"");custom_state_t after={0};UI_SetDraw(later,custom_draw,&after);
    UI_Layout(c,100,100);UI_Draw(c);assert(state.calls==1&&after.calls==0); /* context freed after draw returns */
}
int main(void){anchors_and_clip();containers();input_and_lifecycle();line_edit();scrolling();popups();pointer_capture();deferred_context_destruction();custom_controls();puts("retained UI control tests passed");return 0;}
