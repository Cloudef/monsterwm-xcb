/* see license for copyright and license */

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <sys/wait.h>
#include <X11/keysym.h>
#include <xcb/xcb.h>
#include <xcb/xcb_atom.h>
#include <xcb/xcb_icccm.h>
#include <xcb/xcb_keysyms.h>
#if XINERAMA
#  include <xcb/xinerama.h>
#endif

#include <cairo/cairo.h>
#include <pango/pangocairo.h>
#ifndef CAIRO_HAS_XCB_SURFACE
#  error Cairo was not compiled with XCB support
#endif
#include <cairo/cairo-xcb.h>

#define OPENGL 1
#if OPENGL
#  include "GL/glcomposite.h"
#endif

/* TODO: Reduce SLOC */

/* set this to 1 to enable debug prints */
#if 1
#  define DEBUG(x)      puts(x);
#  define DEBUGP(x,...) printf(x, ##__VA_ARGS__);
#else
#  define DEBUG(x)      ;
#  define DEBUGP(x,...) ;
#endif

/* upstream compatility */
#if !OPENGL
#  define True  true
#  define False false
#  define Mod1Mask     XCB_MOD_MASK_1
#  define Mod4Mask     XCB_MOD_MASK_4
#  define ShiftMask    XCB_MOD_MASK_SHIFT
#  define ControlMask  XCB_MOD_MASK_CONTROL
#  define Button1      XCB_BUTTON_INDEX_1
#  define Button2      XCB_BUTTON_INDEX_2
#  define Button3      XCB_BUTTON_INDEX_3
#endif

#define XCB_MOVE_RESIZE XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT
#define XCB_MOVE        XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y
#define XCB_RESIZE      XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT

static char *WM_ATOM_NAME[]   = { "WM_PROTOCOLS", "WM_DELETE_WINDOW" };
static char *NET_ATOM_NAME[]  = { "_NET_SUPPORTED", "_NET_WM_STATE_FULLSCREEN", "_NET_WM_STATE", "_NET_ACTIVE_WINDOW" };

#define LENGTH(x) (sizeof(x)/sizeof(*x))
#define CLEANMASK(mask) (mask & ~(numlockmask | XCB_MOD_MASK_LOCK))
#define BUTTONMASK      XCB_EVENT_MASK_BUTTON_PRESS|XCB_EVENT_MASK_BUTTON_RELEASE

enum { PREV = -1, NEXT = 1, RESIZE, MOVE };
enum { TILE, MONOCLE, BSTACK, GRID, MODES };
enum { WM_PROTOCOLS, WM_DELETE_WINDOW, WM_COUNT };
enum { NET_SUPPORTED, NET_FULLSCREEN, NET_WM_STATE, NET_ACTIVE, NET_COUNT };

/* argument structure to be passed to function by config.h
 * com  - a command to run
 * i    - an integer to indicate different states
 */
typedef union {
    const char** com;
    const int i;
} Arg;

/* a key struct represents a combination of
 * mod      - a modifier mask
 * keysym   - and the key pressed
 * func     - the function to be triggered because of the above combo
 * arg      - the argument to the function
 */
typedef struct {
    unsigned int mod;
    xcb_keysym_t keysym;
    void (*func)(const Arg *);
    const Arg arg;
} key;

/* a button struct represents a combination of
 * mask     - a modifier mask
 * button   - and the mouse button pressed
 * func     - the function to be triggered because of the above combo
 * arg      - the argument to the function
 */
typedef struct {
    unsigned int mask;
    xcb_button_t button;
    void (*func)(const Arg *);
    const Arg arg;
} Button;

/* a client is a wrapper to a window that additionally
 * holds some properties for that window
 *
 * next        - the client after this one, or NULL if the current is the only or last client
 * isurgent    - the window received an urgent hint
 * istransient - the window is transient
 * isfullscrn  - the window is fullscreen
 * isfloating  - the window is floating
 * win         - the window
 *
 * istransient is separate from isfloating as floating window can be reset
 * to their tiling positions, while the transients will always be floating
 */
typedef struct client {
    struct client *next;
    int monitor;
    bool isurgent, istransient, isfullscrn, isfloating;
    xcb_window_t win;
    char name[255];
} client;

/* properties of each desktop
 * master_size  - the size of the master window
 * mode         - the desktop's tiling layout mode
 * growth       - growth factor of the first stack window
 * head         - the start of the client list
 * current      - the currently highlighted window
 * prevfocus    - the client that previously had focus
 * showpanel    - the visibility status of the panel
 */
typedef struct {
    int master_size, mode, growth;
    client *head, *current, *prevfocus;
    bool showpanel;
} desktop;

/* properties of each monitor */
typedef struct {
    bool showpanel;
    int current_desktop;
    int previous_desktop;
    int growth;
    int mode;
    int master_size;
    int wh, ww, wx, wy;
    client *head, *prevfocus, *current;
    desktop *desktops;

    /* bar */
    xcb_window_t     bar_win;
    xcb_pixmap_t     bar_pixmap;
    xcb_gcontext_t   bar_gc;
    cairo_surface_t  *bar_cr_s;
    cairo_t          *bar_cr;
} monitor;

/* define behavior of certain applications
 * configured in config.h
 * class    - the class or name of the instance
 * desktop  - what desktop it should be spawned at
 * follow   - whether to change desktop focus to the specified desktop
 */
typedef struct {
    const char *class;
    const int desktop;
    const bool follow;
    const bool floating;
} AppRule;

 /* function prototypes sorted alphabetically */
static client* addwindow(xcb_window_t w);
static void buttonpress(xcb_generic_event_t *e);
static void change_monitor(const Arg *arg);
static void change_desktop(const Arg *arg);
static void cleanup(void);
static void client_to_monitor(const Arg *arg);
static void client_to_desktop(const Arg *arg);
static void clientmessage(xcb_generic_event_t *e);
static void configurerequest(xcb_generic_event_t *e);
static void deletewindow(xcb_window_t w);
static void desktopinfo(void);
static void destroynotify(xcb_generic_event_t *e);
static void die(const char* errstr, ...);
static void enternotify(xcb_generic_event_t *e);
static void motionnotify(xcb_generic_event_t *e);
static void focusurgent();
static unsigned int getcolor(char* color);
static void grabbuttons(client *c);
static void grabkeys(void);
static void grid(int h, int y);
static void keypress(xcb_generic_event_t *e);
static void killclient();
static void last_monitor();
static void last_desktop();
static void maprequest(xcb_generic_event_t *e);
static void monocle(int h, int y);
static void move_down();
static void move_up();
static void mousemotion(const Arg *arg);
static void next_win();
static void prev_win();
static void propertynotify(xcb_generic_event_t *e);
static void quit(const Arg *arg);
static void removeclient(client *c);
static void resize_master(const Arg *arg);
static void resize_stack(const Arg *arg);
static void rotate_monitor(const Arg *arg);
static void rotate(const Arg *arg);
static void rotate_filled(const Arg *arg);
static void run(void);
static void select_monitor(int i);
static void save_desktop(int i);
static void select_desktop(int i);
static void setfullscreen(client *c, bool fullscrn);
static int setup(int window, int default_screen);
static void sigchld();
static void spawn(const Arg *arg);
static void stack(int h, int y);
static void swap_master();
static void switch_mode(const Arg *arg);
static void tile(void);
static void togglepanel();
static void update_current(client *c);
static void unmapnotify(xcb_generic_event_t *e);
static client* wintoclient(xcb_window_t w);
static int areatomonitor(int x, int y);
static void drawbars();
static void drawbar(bool active_monitor);

#include "config.h"

/* variables */
static bool running = true;
static int retval = 0;
static int current_monitor = 0;
static int previous_monitor = 0;
static int MONITORS = 1; /* always at least 1 monitor */
static unsigned int win_unfocus, win_focus;
static unsigned int numlockmask = 0; /* dynamic key lock mask */
static monitor *monitors;
static monitor *CM; /* comes from current_monitor, shortened to CM to avoid code clutter */
static xcb_connection_t *dis;
static xcb_screen_t *screen;
static xcb_atom_t wmatoms[WM_COUNT], netatoms[NET_COUNT];
static char statustext[255];

/* events array
 * on receival of a new event, call the appropriate function to handle it
 */
static void (*events[XCB_NO_OPERATION])(xcb_generic_event_t *e);

/* layout array - given the current layout mode, tile the windows
 * h (or hh) is the avaible height that windows have to expand
 * y (or cy) is the num of pixels from top to place the windows (y coordinate)
 */
static void (*layout[MODES])(int h, int y) = {
    [TILE] = stack, [BSTACK]  = stack,
    [GRID] = grid,  [MONOCLE] = monocle,
};

/* get screen of display */
static xcb_screen_t *xcb_screen_of_display(xcb_connection_t *con, int screen) {
    xcb_screen_iterator_t iter;
    iter = xcb_setup_roots_iterator(xcb_get_setup(con));
    for (; iter.rem; --screen, xcb_screen_next(&iter)) if (screen == 0) return iter.data;
    return NULL;
}

/* wrapper to get root visual type from xcb screen */
xcb_visualtype_t *xcb_get_root_visual_type(xcb_screen_t *s) {
    xcb_visualtype_t       *visual_type = NULL;
    xcb_depth_iterator_t   depth_iter;

    depth_iter = xcb_screen_allowed_depths_iterator(s);
    for(;depth_iter.rem;xcb_depth_next(&depth_iter)) {
        xcb_visualtype_iterator_t visual_iter;

        visual_iter = xcb_depth_visuals_iterator(depth_iter.data);
        for(;visual_iter.rem;xcb_visualtype_next(&visual_iter))
            if(s->root_visual == visual_iter.data->visual_id) {
                visual_type = visual_iter.data;
                break;
            }
    }

    return visual_type;
}

static char *PANGOFONT = "Terminus";
static void setfont(char *font)
{
    PANGOFONT = font;
}

static void textsize(const char *str, int *w, int *h, cairo_t *cr)
{
    PangoLayout  *layout;
    PangoFontDescription *desc;
    layout = pango_cairo_create_layout(cr);
    pango_layout_set_text(layout, str, -1);
    desc = pango_font_description_from_string(PANGOFONT);
    pango_layout_set_font_description(layout, desc);
    pango_font_description_free(desc);
    pango_layout_get_pixel_size(layout, w, h);
    g_object_unref(layout);
}

/* render text */
static void drawtext(const char *str, int x, int y, cairo_t *cr)
{
    PangoLayout *layout;
    PangoFontDescription *desc;
    int width, height;

    layout = pango_cairo_create_layout(cr);
    pango_layout_set_text(layout, str, -1);
    desc = pango_font_description_from_string(PANGOFONT);
    pango_layout_set_font_description(layout, desc);
    pango_font_description_free(desc);

    pango_layout_get_pixel_size(layout, &width, &height);
    cairo_move_to(CM->bar_cr, x, y - (height/3));

    pango_cairo_update_layout(cr, layout);
    pango_cairo_show_layout_line (cr, pango_layout_get_line (layout, 0));

    g_object_unref(layout);
}

/* wrapper to move and resize window */
static inline void xcb_move_resize(xcb_connection_t *con, xcb_window_t win, int x, int y, int w, int h) {
    unsigned int pos[4] = { x, y, w, h };
    xcb_configure_window(con, win, XCB_MOVE_RESIZE, pos);
}

/* wrapper to move window */
static inline void xcb_move(xcb_connection_t *con, xcb_window_t win, int x, int y) {
    unsigned int pos[2] = { x, y };
    xcb_configure_window(con, win, XCB_MOVE, pos);
}

/* wrapper to resize window */
static inline void xcb_resize(xcb_connection_t *con, xcb_window_t win, int w, int h) {
    unsigned int pos[2] = { w, h };
    xcb_configure_window(con, win, XCB_RESIZE, pos);
}

/* wrapper to raise window */
static inline void xcb_raise_window(xcb_connection_t *con, xcb_window_t win) {
    unsigned int arg[1] = { XCB_STACK_MODE_ABOVE };
    xcb_configure_window(con, win, XCB_CONFIG_WINDOW_STACK_MODE, arg);
}

/* wrapper to set xcb border width */
static inline void xcb_border_width(xcb_connection_t *con, xcb_window_t win, int w) {
    unsigned int arg[1] = { w };
    xcb_configure_window(con, win, XCB_CONFIG_WINDOW_BORDER_WIDTH, arg);
}

/* wrapper to change single gc using xcb */
static inline void xcb_change_gc_single(xcb_connection_t *conn, xcb_gcontext_t gc, uint32_t mask, uint32_t value) {
        xcb_change_gc(conn, gc, mask, &value);
}

/* wrapper to get xcb keysymbol from keycode */
static xcb_keysym_t xcb_get_keysym(xcb_keycode_t keycode) {
    xcb_key_symbols_t *keysyms;
    xcb_keysym_t       keysym;

    if (!(keysyms = xcb_key_symbols_alloc(dis))) return 0;
    keysym = xcb_key_symbols_get_keysym(keysyms, keycode, 0);
    xcb_key_symbols_free(keysyms);

    return keysym;
}

/* wrapper to get xcb keycodes from keysymbol */
static xcb_keycode_t* xcb_get_keycodes(xcb_keysym_t keysym) {
    xcb_key_symbols_t *keysyms;
    xcb_keycode_t     *keycode;

    if (!(keysyms = xcb_key_symbols_alloc(dis))) return NULL;
    keycode = xcb_key_symbols_get_keycode(keysyms, keysym);
    xcb_key_symbols_free(keysyms);

    return keycode;
}

/* retieve RGB color from hex (think of html) */
static unsigned int xcb_get_colorpixel(char *hex) {
    char strgroups[3][3]  = {{hex[1], hex[2], '\0'}, {hex[3], hex[4], '\0'}, {hex[5], hex[6], '\0'}};
    unsigned int rgb16[3] = {(strtol(strgroups[0], NULL, 16)), (strtol(strgroups[1], NULL, 16)), (strtol(strgroups[2], NULL, 16))};
    return (rgb16[0] << 16) + (rgb16[1] << 8) + rgb16[2];
}

/* wrapper to get atoms using xcb */
static void xcb_get_atoms(char **names, xcb_atom_t *atoms, unsigned int count) {
    xcb_intern_atom_cookie_t cookies[count];
    xcb_intern_atom_reply_t  *reply;

    for (unsigned int i = 0; i < count; i++) cookies[i] = xcb_intern_atom(dis, 0, strlen(names[i]), names[i]);
    for (unsigned int i = 0; i < count; i++) {
        reply = xcb_intern_atom_reply(dis, cookies[i], NULL); /* TODO: Handle error */
        if (reply) {
            DEBUGP("%s : %d\n", names[i], reply->atom);
            atoms[i] = reply->atom; free(reply);
        } else puts("WARN: monsterwm failed to register %s atom.\nThings might not work right.");
    }
}

/* wrapper to window get attributes using xcb */
static void xcb_get_attributes(xcb_window_t *windows, xcb_get_window_attributes_reply_t **reply, unsigned int count) {
    xcb_get_window_attributes_cookie_t cookies[count];
    for (unsigned int i = 0; i < count; i++) cookies[i] = xcb_get_window_attributes(dis, windows[i]);
    for (unsigned int i = 0; i < count; i++) reply[i]   = xcb_get_window_attributes_reply(dis, cookies[i], NULL); /* TODO: Handle error */
}

/* check if other wm exists */
static int xcb_checkotherwm(void) {
    xcb_generic_error_t *error;
    unsigned int values[1] = {XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT|XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY|XCB_EVENT_MASK_PROPERTY_CHANGE|
                              XCB_EVENT_MASK_BUTTON_PRESS|(FOLLOW_MONITOR?XCB_EVENT_MASK_POINTER_MOTION:0)};

    error = xcb_request_check(dis, xcb_change_window_attributes_checked(dis, screen->root, XCB_CW_EVENT_MASK, values));
    xcb_flush(dis);
    if (error) return 1;
    return 0;
}

int areatomonitor(int x, int y) {
    for (int m=0; m<MONITORS; m++)
        if (monitors[m].wx < x && (monitors[m].wx + monitors[m].ww) > x &&
            monitors[m].wy < y && (monitors[m].wy + monitors[m].wh) > y) return m;
    return current_monitor;
}

/* create a new client and add the new window
 * window should notify of property change events
 */
client* addwindow(xcb_window_t w) {
    client *c, *t;
    if (!(c = (client *)calloc(1, sizeof(client))))
        die("error: could not calloc() %u bytes\n", sizeof(client));

    c->monitor = current_monitor;
    if (!CM->head) CM->head = c;
    else if (ATTACH_ASIDE) {
        for(t=CM->head; t->next; t=t->next); /* get the last client */
        t->next = c;
    } else {
        c->next = CM->head;
        CM->head = c;
    }

    unsigned int values[1] = { XCB_EVENT_MASK_PROPERTY_CHANGE|(FOLLOW_MOUSE?XCB_EVENT_MASK_ENTER_WINDOW:0) };
    xcb_change_window_attributes_checked(dis, (c->win = w), XCB_CW_EVENT_MASK, values);
    return c;
}

/* on the press of a button check to see if there's a binded function to call */
void buttonpress(xcb_generic_event_t *e) {
    xcb_button_press_event_t *ev = (xcb_button_press_event_t*)e;
    DEBUGP("xcb: button press: %d state: %d\n", ev->detail, ev->state);

    client *c = wintoclient(ev->event);
    if (!c) return;
    if (CLICK_TO_FOCUS && CM->current != c && ev->detail == XCB_BUTTON_INDEX_1) update_current(c);

    for (unsigned int i=0; i<LENGTH(buttons); i++)
        if (buttons[i].func && buttons[i].button == ev->detail &&
            CLEANMASK(buttons[i].mask) == CLEANMASK(ev->state)) {
            update_current(c);
            buttons[i].func(&(buttons[i].arg));
        }
}

void change_monitor(const Arg *arg) {
    if (arg->i == current_monitor) return;
    previous_monitor = current_monitor;
    select_monitor(arg->i);
    update_current(CM->current);
    desktopinfo();
}

/* focus another desktop
 * to avoid flickering
 * first map the new windows
 * if the layout mode is fullscreen map only one window
 * then unmap previous windows
 */
void change_desktop(const Arg *arg) {
    if (arg->i == CM->current_desktop) return;
    CM->previous_desktop = CM->current_desktop;
    select_desktop(arg->i);
    tile();
    if (CM->mode == MONOCLE && CM->current) xcb_map_window(dis, CM->current->win);
    else for (client *c=CM->head; c; c=c->next) xcb_map_window(dis, c->win);
    update_current(CM->current);
    select_desktop(CM->previous_desktop);
    for (client *c=CM->head; c; c=c->next) xcb_unmap_window(dis, c->win);
    select_desktop(arg->i);
    desktopinfo();
}

/* remove all windows in all desktops by sending a delete message */
void cleanup(void) {
    xcb_query_tree_reply_t  *reply;
    unsigned int nchildren;

    for (int m=0; m<MONITORS; m++) {
        cairo_destroy(monitors[m].bar_cr);
        free(monitors[m].desktops);
    }
    free(monitors);

    xcb_ungrab_key(dis, XCB_GRAB_ANY, screen->root, XCB_MOD_MASK_ANY);
    reply = xcb_query_tree_reply(dis, xcb_query_tree(dis, screen->root), NULL); /* TODO: error handling */
    if (reply) {
        nchildren = reply[0].children_len;
        for (unsigned int i = 0; i<nchildren; i++) deletewindow(reply[i].parent);
        free(reply);
    }
    xcb_set_input_focus(dis, XCB_NONE, XCB_INPUT_FOCUS_POINTER_ROOT, XCB_CURRENT_TIME);
}

/* move a client to another monitor
 * store the client's window
 * remove the client
 * add the window to the new monitor
 * if defined change focus to the new monitor
 *
 * keep in mind that current pointer might
 * change with each select_monitor() invocation
 */
void client_to_monitor(const Arg *arg) {
    if (CM->current && arg->i < MONITORS && CM->current->monitor == arg->i) return;
    xcb_window_t w = CM->current->win;
    int OLDM = current_monitor;

    bool wfloating = CM->current->isfloating;
    bool wfullscrn = CM->current->isfullscrn;
    bool transient = CM->current->istransient;

    xcb_unmap_window(dis, CM->current->win);
    removeclient(CM->current);

    select_monitor(arg->i);
    CM->current = addwindow(w);
    CM->current->isfloating = wfloating;
    CM->current->isfullscrn = wfullscrn;
    CM->current->istransient = transient;

    tile();
    xcb_map_window(dis, CM->current->win);
    select_monitor(OLDM);
    tile();
    update_current(CM->current);

    if (FOLLOW_WINDOW) change_monitor(arg);
    desktopinfo();
}

/* move a client to another desktop
 * store the client's window
 * remove the client
 * add the window to the new desktop
 * if defined change focus to the new desktop
 *
 * keep in mind that current pointer might
 * change with each select_desktop() invocation
 */
void client_to_desktop(const Arg *arg) {
    if (arg->i == CM->current_desktop || !CM->current) return;
    int cd = CM->current_desktop;
    client *c = CM->current;

    /* add the window to the new desktop keeping the client's properties */
    select_desktop(arg->i);
    CM->current = addwindow(c->win);
    CM->current->isfloating  = c->isfloating;
    CM->current->isfullscrn  = c->isfullscrn;
    CM->current->istransient = c->istransient;

    /* remove the window and client from the current desktop */
    select_desktop(cd);
    xcb_unmap_window(dis, c->win);
    removeclient(c);
    tile();
    update_current(CM->current);

    if (FOLLOW_WINDOW) change_desktop(arg);
    desktopinfo();
}

/* check if window requested fullscreen or activation
 * To change the state of a mapped window, a client MUST
 * send a _NET_WM_STATE client message to the root window
 * message_type must be _NET_WM_STATE
 *   data.l[0] is the action to be taken
 *   data.l[1] is the property to alter three actions:
 *     remove/unset _NET_WM_STATE_REMOVE=0
 *     add/set _NET_WM_STATE_ADD=1,
 *     toggle _NET_WM_STATE_TOGGLE=2
 */
void clientmessage(xcb_generic_event_t *e) {
    xcb_client_message_event_t *ev = (xcb_client_message_event_t*)e;
    client *c = wintoclient(ev->window);
    if (ev->format != 32) return;
    DEBUGP("xcb: client message: %d, %d, %d\n", ev->data.data32[0], ev->data.data32[1], ev->data.data32[2]);
    if (c && ev->type == netatoms[NET_WM_STATE] && ((xcb_atom_t)ev->data.data32[1]
        == netatoms[NET_FULLSCREEN] || (xcb_atom_t)ev->data.data32[2] == netatoms[NET_FULLSCREEN]))
        setfullscreen(c, (ev->data.data32[0] == 1 || (ev->data.data32[0] == 2 && !c->isfullscrn)));
    else if (c && ev->type == netatoms[NET_ACTIVE]) CM->current = c;
    tile();
    update_current(CM->current);
}

/* a configure request means that the window requested changes in its geometry
 * state. if the window is fullscreen discard and fill the screen, else set the
 * appropriate values as requested, and tile the window again so that it fills
 * the gaps that otherwise could have been created
 */
void configurerequest(xcb_generic_event_t *e) {
    xcb_configure_request_event_t *ev = (xcb_configure_request_event_t*)e;
    client *c = wintoclient(ev->window);
    if (c && c->isfullscrn) setfullscreen(c, true);
    else {
        unsigned int v[7];
        unsigned int i = 0;
        if (ev->value_mask & XCB_CONFIG_WINDOW_X)              v[i++] = ev->x;
        if (ev->value_mask & XCB_CONFIG_WINDOW_Y)              v[i++] = ev->y + (CM->showpanel && TOP_PANEL) ? PANEL_HEIGHT : 0;
        if (ev->value_mask & XCB_CONFIG_WINDOW_WIDTH)          v[i++] = (ev->width  < CM->ww - BORDER_WIDTH) ? ev->width  : CM->ww + BORDER_WIDTH;
        if (ev->value_mask & XCB_CONFIG_WINDOW_HEIGHT)         v[i++] = (ev->height < CM->wh - BORDER_WIDTH) ? ev->height : CM->wh + BORDER_WIDTH;
        if (ev->value_mask & XCB_CONFIG_WINDOW_BORDER_WIDTH)   v[i++] = ev->border_width;
        if (ev->value_mask & XCB_CONFIG_WINDOW_SIBLING)        v[i++] = ev->sibling;
        if (ev->value_mask & XCB_CONFIG_WINDOW_STACK_MODE)     v[i++] = ev->stack_mode;
        xcb_configure_window(dis, ev->window, ev->value_mask, v);
    }
    tile();
    if (c && c == CM->current) update_current(c);
}

/* close the window */
void deletewindow(xcb_window_t w) {
    xcb_client_message_event_t ev;
    ev.response_type = XCB_CLIENT_MESSAGE;
    ev.window = w;
    ev.format = 32;
    ev.sequence = 0;
    ev.type = wmatoms[WM_PROTOCOLS];
    ev.data.data32[0] = wmatoms[WM_DELETE_WINDOW];
    ev.data.data32[1] = XCB_CURRENT_TIME;
    xcb_send_event(dis, 0, w, XCB_EVENT_MASK_NO_EVENT, (char*)&ev);
}

/* output info about the desktops on standard output stream
 *
 * the info is a list of ':' separated values for each desktop
 * desktop to desktop info is separated by ' ' single spaces
 * the info values are
 *   the monitor number/id
 *   whether the monitor is the current focused (1) or not (0)
 *   the desktop number/id
 *   the desktop's client count
 *   the desktop's tiling layout mode/id
 *   whether the desktop is the current focused (1) or not (0)
 *   whether any client in that desktop has received an urgent hint
 *
 * once the info is collected, immediately flush the stream
 */
void desktopinfo(void) {
    bool urgent = false;
    int OLDM = current_monitor, cd, n=0, d=0, m=0;
    for (; m<MONITORS; m++) {
        select_monitor(m); d = 0; cd = CM->current_desktop;
        for (client *c; d<DESKTOPS; d++) {
            for (select_desktop(d), c=CM->head, n=0, urgent=false; c; c=c->next, ++n) if (c->isurgent) urgent = true;
            fprintf(stdout, "%d:%d:%d:%d:%d:%d:%d%c", m, current_monitor == OLDM, d, n, CM->mode, CM->current_desktop == cd, urgent, (m+1==MONITORS && d+1==DESKTOPS)?'\n':' ');
        }
        select_desktop(cd);
        drawbar(m == OLDM);
    }
    fflush(stdout);
    select_monitor(OLDM);
}

/* a destroy notification is received when a window is being closed
 * on receival, remove the appropriate client that held that window
 */
void destroynotify(xcb_generic_event_t *e) {
    DEBUG("xcb: destoroy notify");
    xcb_destroy_notify_event_t *ev = (xcb_destroy_notify_event_t*)e;
    client *c = wintoclient(ev->window);
    if (c) removeclient(c); else { DEBUG("fail destroy"); }
    desktopinfo();
}

/* print a message on standard error stream
 * and exit with failure exit code
 */
void die(const char *errstr, ...) {
    va_list ap;
    va_start(ap, errstr);
    vfprintf(stderr, errstr, ap);
    va_end(ap);
    exit(EXIT_FAILURE);
}

/* when the mouse enters a window's borders
 * the window, if notifying of such events (EnterWindowMask)
 * will notify the wm and will get focus
 */
void enternotify(xcb_generic_event_t *e) {
    xcb_enter_notify_event_t *ev = (xcb_enter_notify_event_t*)e;
    if (!FOLLOW_MOUSE) return;
    DEBUG("xcb: enter notify");
    client *c = wintoclient(ev->event);
    if (c && ev->mode == XCB_NOTIFY_MODE_NORMAL && ev->detail != XCB_NOTIFY_DETAIL_INFERIOR) update_current(c);
}

void motionnotify(xcb_generic_event_t *e) {
    xcb_motion_notify_event_t *ev = (xcb_motion_notify_event_t*)e;
    int area_monitor;
    if (!FOLLOW_MONITOR) return;
    DEBUG("xcb: motion notify");
    if ((area_monitor = areatomonitor(ev->root_x, ev->root_y)) != current_monitor)
        change_monitor(&(Arg){.i = area_monitor});
}

/* find and focus the client which received
 * the urgent hint in the current desktop
 */
void focusurgent() {
    for (int m = 0; m<MONITORS; m++)
        for (client *c=monitors[m].head; c; c=c->next) if (c->isurgent) update_current(c);
}

/* get a pixel with the requested color
 * to fill some window area - borders
 */
unsigned int getcolor(char* color) {
    xcb_colormap_t map = screen->default_colormap;
    xcb_alloc_color_reply_t *c;
    unsigned int r, g, b, rgb, pixel;

    rgb = xcb_get_colorpixel(color);
    r = rgb >> 16; g = rgb >> 8 & 0xFF; b = rgb & 0xFF;
    c = xcb_alloc_color_reply(dis, xcb_alloc_color(dis, map, r * 257, g * 257, b * 257), NULL);
    if (!c)
        die("error: cannot allocate color '%s'\n", c);

    pixel = c->pixel; free(c);
    return pixel;
}

/* set the given client to listen to button events (presses / releases) */
void grabbuttons(client *c) {
    unsigned int modifiers[] = { 0, XCB_MOD_MASK_LOCK, numlockmask, numlockmask|XCB_MOD_MASK_LOCK };
    for (unsigned int b=0; b<LENGTH(buttons); b++)
        for (unsigned int m=0; m<LENGTH(modifiers); m++)
            xcb_grab_button(dis, 1, c->win, XCB_EVENT_MASK_BUTTON_PRESS, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC,
                    screen->root, XCB_NONE, buttons[b].button, buttons[b].mask|modifiers[m]);
}

/* the wm should listen to key presses */
void grabkeys(void) {
    xcb_keycode_t *keycode;
    unsigned int modifiers[] = { 0, XCB_MOD_MASK_LOCK, numlockmask, numlockmask|XCB_MOD_MASK_LOCK };
    xcb_ungrab_key(dis, XCB_GRAB_ANY, screen->root, XCB_MOD_MASK_ANY);
    for (unsigned int i=0; i<LENGTH(keys); i++) {
        keycode = xcb_get_keycodes(keys[i].keysym);
        for (unsigned int k=0; keycode[k] != XCB_NO_SYMBOL; k++)
            for (unsigned int m=0; m<LENGTH(modifiers); m++)
                xcb_grab_key(dis, 1, screen->root, keys[i].mod | modifiers[m], keycode[k], XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
    }
}

/* arrange windows in a grid */
void grid(int hh, int cy) {
    int n = 0, cols = 0;
    for (client *c = CM->head; c; c=c->next) if (!c->istransient && !c->isfullscrn && !c->isfloating) ++n;
    for (cols=0; cols <= n/2; cols++) if (cols*cols >= n) break; /* emulate square root */
    if (n == 5) cols = 2;

    int rows = n/cols, cn = 0, rn = 0, i = 0, ch = hh - BORDER_WIDTH, cw = (CM->ww - BORDER_WIDTH)/(cols?cols:1);
    for (client *c=CM->head; c; c=c->next, i++) {
        if (c->isfullscrn || c->istransient || c->isfloating) { i--; continue; }
        if (i/rows + 1 > cols - n%cols) rows = n/cols + 1;
        xcb_move_resize(dis, c->win, CM->wx + cn*cw, CM->wy + cy + rn*ch/rows, cw - BORDER_WIDTH, ch/rows - BORDER_WIDTH);
        if (++rn >= rows) { rn = 0; cn++; }
    }
}

/* on the press of a key check to see if there's a binded function to call */
void keypress(xcb_generic_event_t *e) {
    xcb_key_press_event_t *ev       = (xcb_key_press_event_t *)e;
    xcb_keysym_t           keysym   = xcb_get_keysym(ev->detail);
    DEBUGP("xcb: keypress: code: %d mod: %d\n", ev->detail, ev->state);
    for (unsigned int i=0; i<LENGTH(keys); i++)
        if (keysym == keys[i].keysym && CLEANMASK(keys[i].mod) == CLEANMASK(ev->state) && keys[i].func)
                keys[i].func(&keys[i].arg);
}

/* explicitly kill a client - close the highlighted window
 * send a delete message and remove the client
 */
void killclient() {
    if (!CM->current) return;
    xcb_icccm_get_wm_protocols_reply_t reply; unsigned int n = 0; bool got = false;
    if (xcb_icccm_get_wm_protocols_reply(dis,
        xcb_icccm_get_wm_protocols(dis, CM->current->win, wmatoms[WM_PROTOCOLS]),
        &reply, NULL)) { /* TODO: Handle error? */
        for(; n != reply.atoms_len; ++n) if ((got = reply.atoms[n] == wmatoms[WM_DELETE_WINDOW])) break;
        xcb_icccm_get_wm_protocols_reply_wipe(&reply);
    }
    if (got) deletewindow(CM->current->win);
    else xcb_kill_client(dis, CM->current->win);
    removeclient(CM->current);
}

/* focus the previously focused monitor */
void last_monitor() {
    change_monitor(&(Arg){.i = previous_monitor});
}

/* focus the previously focused desktop */
void last_desktop() {
    change_desktop(&(Arg){.i = CM->previous_desktop});
}

/* a map request is received when a window wants to display itself
 * if the window has override_redirect flag set then it should not be handled
 * by the wm. if the window already has a client then there is nothing to do.
 *
 * get the window class and name instance and try to match against an app rule.
 * create a client for the window, that client will always be current.
 * check for transient state, and fullscreen state and the appropriate values.
 * if the desktop in which the window was spawned is the current desktop then
 * display the window, else, if set, focus the new desktop.
 */
void maprequest(xcb_generic_event_t *e) {
    xcb_map_request_event_t            *ev = (xcb_map_request_event_t*)e;
    xcb_window_t                       windows[] = { ev->window }, transient = 0;
    xcb_get_window_attributes_reply_t  *attr[1];
    xcb_icccm_get_wm_class_reply_t     ch;
    xcb_get_geometry_reply_t           *geometry;
    xcb_get_property_reply_t           *prop_reply;
    xcb_icccm_get_text_property_reply_t text_reply;

    DEBUG("xcb: map request");
    xcb_get_attributes(windows, attr, 1);
    if (attr[0]->override_redirect) return;
    if (wintoclient(ev->window))    return;
    DEBUG("xcb: manage");

    bool follow = false, floating = false; char name[255]; memset(name, 0, 255);
    int cd = CM->current_desktop, newdsk = CM->current_desktop;
    if (xcb_icccm_get_wm_class_reply(dis, xcb_icccm_get_wm_class(dis, ev->window), &ch, NULL)) { /* TODO: error handling */
        DEBUGP("class: %s instance: %s\n", ch.class_name, ch.instance_name);
        for (unsigned int i=0; i<LENGTH(rules); i++)
            if (!strcmp(ch.class_name, rules[i].class) || !strcmp(ch.instance_name, rules[i].class)) {
                follow = rules[i].follow;
                newdsk = (rules[i].desktop < 0) ? CM->current_desktop : rules[i].desktop;
                floating = rules[i].floating;
                break;
            }
        xcb_icccm_get_wm_class_reply_wipe(&ch);
    }

    /* might be useful in future */
    geometry = xcb_get_geometry_reply(dis, xcb_get_geometry(dis, ev->window), NULL); /* TODO: error handling */
    if (geometry) {
        DEBUGP("geom: %ux%u+%d+%d\n", geometry->width, geometry->height,
                                      geometry->x,     geometry->y);
        free(geometry);
    }

    if (xcb_icccm_get_wm_name_reply(dis, xcb_icccm_get_wm_name_unchecked(dis, ev->window), &text_reply, NULL)) { /* TODO: error handling */
        memcpy(name, text_reply.name, text_reply.name_len);
        xcb_icccm_get_text_property_reply_wipe(&text_reply);
    } else strcpy(name, "broken");

    select_desktop(newdsk);
    CM->prevfocus = CM->current;
    CM->current   = addwindow(ev->window);
    memset(CM->current->name, 0, 255);
    strcpy(CM->current->name, name);

    xcb_icccm_get_wm_transient_for_reply(dis, xcb_icccm_get_wm_transient_for_unchecked(dis, ev->window), &transient, NULL); /* TODO: error handling */
    CM->current->istransient = transient?true:false;
    CM->current->isfloating  = floating || CM->current->istransient;

    prop_reply  = xcb_get_property_reply(dis, xcb_get_property_unchecked(dis, 0, ev->window, netatoms[NET_WM_STATE], XCB_ATOM_ATOM, 0, 1), NULL); /* TODO: error handling */
    if (prop_reply) {
        if (prop_reply->format == 32) {
            xcb_atom_t *v = xcb_get_property_value(prop_reply);
            for (unsigned int i=0; i<prop_reply->value_len; i++)
                DEBUGP("%d : %d\n", i, v[0]);
            setfullscreen(CM->current, (v[0] == netatoms[NET_FULLSCREEN]));
        }
        free(prop_reply);
    }

    /** information for stdout **/
    DEBUGP("transient: %d\n", CM->current->istransient);
    DEBUGP("floating:  %d\n", CM->current->isfloating);

    select_desktop(cd);
    if (cd == newdsk) {
        tile();
        xcb_map_window(dis, ev->window);
        update_current(CM->current);
        grabbuttons(CM->current);
    } else if (follow) change_desktop(&(Arg){.i = newdsk});
    desktopinfo();
}

/* grab the pointer and get it's current position
 * all pointer movement events will be reported until it's ungrabbed
 * until the mouse button has not been released,
 * grab the interesting events - button press/release and pointer motion
 * and on on pointer movement resize or move the window under the curson.
 * if the received event is a map request or a configure request call the
 * appropriate handler, and stop listening for other events.
 * Ungrab the poitner and event handling is passed back to run() function.
 * Once a window has been moved or resized, it's marked as floating.
 */
void mousemotion(const Arg *arg) {
    xcb_get_geometry_reply_t  *geometry;
    xcb_query_pointer_reply_t *pointer;
    xcb_grab_pointer_reply_t  *grab_reply;
    int mx, my, winx, winy, winw, winh, xw, yh;

    if (!CM->current) return;
    geometry = xcb_get_geometry_reply(dis, xcb_get_geometry(dis, CM->current->win), NULL); /* TODO: error handling */
    if (geometry) {
        winx = geometry->x;     winy = geometry->y;
        winw = geometry->width; winh = geometry->height;
        free(geometry);
    } else return;

    pointer = xcb_query_pointer_reply(dis, xcb_query_pointer(dis, screen->root), 0);
    if (!pointer) return;
    mx = pointer->root_x; my = pointer->root_y;

    grab_reply = xcb_grab_pointer_reply(dis, xcb_grab_pointer(dis, 0, screen->root, BUTTONMASK|XCB_EVENT_MASK_BUTTON_MOTION|XCB_EVENT_MASK_POINTER_MOTION,
            XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC, XCB_NONE, XCB_NONE, XCB_CURRENT_TIME), NULL);
    if (!grab_reply || grab_reply->status != XCB_GRAB_STATUS_SUCCESS) return;

    if (CM->current->isfullscrn) setfullscreen(CM->current, False);
    if (!CM->current->isfloating) CM->current->isfloating = True;

    xcb_generic_event_t *e = NULL;
    xcb_motion_notify_event_t *ev = NULL;
    int area_monitor; bool ungrab = false;
    do {
        if (e) free(e); xcb_flush(dis);
        while(!(e = xcb_wait_for_event(dis))) xcb_flush(dis);
        switch (e->response_type & ~0x80) {
            case XCB_CONFIGURE_REQUEST: case XCB_MAP_REQUEST:
                events[e->response_type & ~0x80](e);
                break;
            case XCB_MOTION_NOTIFY:
                ev = (xcb_motion_notify_event_t*)e;
                xw = (arg->i == MOVE ? winx : winw) + ev->root_x - mx;
                yh = (arg->i == MOVE ? winy : winh) + ev->root_y - my;
                if (arg->i == RESIZE) xcb_resize(dis, CM->current->win, xw>MINWSZ?xw:winw, yh>MINWSZ?yh:winh);
                else if (arg->i == MOVE) {
                    xcb_move(dis, CM->current->win, xw, yh);
                    if ((area_monitor = areatomonitor(xw, yh)) != current_monitor) {
                        client_to_monitor(&(Arg){.i = area_monitor});
                        change_monitor(&(Arg){.i = area_monitor});
                    }
                }
                drawbars();
                break;
            case XCB_KEY_PRESS:
            case XCB_KEY_RELEASE:
            case XCB_BUTTON_PRESS:
            case XCB_BUTTON_RELEASE:
                ungrab = true;
                break;
        }
    } while(!ungrab && CM->current);
    DEBUG("xcb: ungrab");
    xcb_ungrab_pointer(dis, XCB_CURRENT_TIME);
    update_current(CM->current);
    tile();
}

/* each window should cover all the available screen space */
void monocle(int hh, int cy) {
    for (client *c=CM->head; c; c=c->next) if (!c->isfullscrn && !c->isfloating && !c->istransient)
        xcb_move_resize(dis, c->win, CM->wx, CM->wy + cy, CM->ww, hh);
}

/* move the current client, to current->next
 * and current->next to current client's position
 */
void move_down() {
    if (!CM->current || !CM->head->next) return;

    /* p is previous, n is next, if current is head n is last, c is current */
    client *p = NULL, *n = (CM->current->next) ? CM->current->next : CM->head;
    for (p=CM->head; p && p->next != CM->current; p=p->next);
    /* if there's a previous client then p->next should be what's after c
     * ..->[p]->[c]->[n]->..  ==>  ..->[p]->[n]->[c]->..
     */
    if (p) p->next = CM->current->next;
    /* else if no p client, then c is head, swapping with n should update head
     * [c]->[n]->..  ==>  [n]->[c]->..
     *  ^head              ^head
     */
    else CM->head = n;
    /* if c is the last client, c will be the current head
     * [n]->..->[p]->[c]->NULL  ==>  [c]->[n]->..->[p]->NULL
     *  ^head                         ^head
     * else c will take the place of n, so c-next will be n->next
     * ..->[p]->[c]->[n]->..  ==>  ..->[p]->[n]->[c]->..
     */
    CM->current->next = (CM->current->next) ? n->next : n;
    /* if c was swapped with n then they now point to the same ->next. n->next should be c
     * ..->[p]->[c]->[n]->..  ==>  ..->[p]->[n]->..  ==>  ..->[p]->[n]->[c]->..
     *                                        [c]-^
     */
    if (CM->current->next == n->next) n->next = CM->current;
    /* else c is the last client and n is head,
     * so c will be move to be head, no need to update n->next
     * [n]->..->[p]->[c]->NULL  ==>  [c]->[n]->..->[p]->NULL
     *  ^head                         ^head
     */
    else CM->head = CM->current;

    tile();
    update_current(CM->current);
}

/* move the current client, to the previous from current
 * and the previous from  current to current client's position
 */
void move_up() {
    if (!CM->current || !CM->head->next) return;

    client *pp = NULL, *p;
    /* p is previous from current or last if current is head */
    for (p=CM->head; p->next && p->next != CM->current; p=p->next);
    /* pp is previous from p, or null if current is head and thus p is last */
    if (p->next) for (pp=CM->head; pp; pp=pp->next) if (pp->next == p) break;
    /* if p has a previous client then the next client should be current (current is c)
     * ..->[pp]->[p]->[c]->..  ==>  ..->[pp]->[c]->[p]->..
     */
    if (pp) pp->next = CM->current;
    /* if p doesn't have a previous client, then p might be head, so head must change to c
     * [p]->[c]->..  ==>  [c]->[p]->..
     *  ^head              ^head
     * if p is not head, then c is head (and p is last), so the new head is next of c
     * [c]->[n]->..->[p]->NULL  ==>  [n]->..->[p]->[c]->NULL
     *  ^head         ^last           ^head         ^last
     */
    else CM->head = (CM->current == CM->head) ? CM->current->next : CM->current;
    /* next of p should be next of c
     * ..->[pp]->[p]->[c]->[n]->..  ==>  ..->[pp]->[c]->[p]->[n]->..
     * except if c was head (now c->next is head), so next of p should be c
     * [c]->[n]->..->[p]->NULL  ==>  [n]->..->[p]->[c]->NULL
     *  ^head         ^last           ^head         ^last
     */
    p->next = (CM->current->next == CM->head) ? CM->current : CM->current->next;
    /* next of c should be p
     * ..->[pp]->[p]->[c]->[n]->..  ==>  ..->[pp]->[c]->[p]->[n]->..
     * except if c was head (now c->next is head), so c is must be last
     * [c]->[n]->..->[p]->NULL  ==>  [n]->..->[p]->[c]->NULL
     *  ^head         ^last           ^head         ^last
     */
    CM->current->next = (CM->current->next == CM->head) ? NULL : p;

    tile();
    update_current(CM->current);
}

/* cyclic focus the next window
 * if the window is the last on stack, focus head
 */
void next_win() {
    if (!CM->current || !CM->head->next) return;
    CM->current = (CM->prevfocus = CM->current)->next ? CM->current->next : CM->head;
    if (CM->mode == MONOCLE) xcb_map_window(dis, CM->current->win);
    update_current(CM->current);
}

/* cyclic focus the previous window
 * if the window is the head, focus the last stack window
 */
void prev_win() {
    if (!CM->current || !CM->head->next) return;
    if (CM->head == (CM->prevfocus = CM->current)) while (CM->current->next) CM->current=CM->current->next;
    else for (client *t=CM->head; t; t=t->next) if (t->next == CM->current) { CM->current = t; break; }
    if (CM->mode == MONOCLE) xcb_map_window(dis, CM->current->win);
    update_current(CM->current);
}

/* update status text */
void updatestatus(client *c)
{
    xcb_icccm_get_text_property_reply_t text_reply;
    char name[255];
    memset(name, 0, 255);
    if (xcb_icccm_get_wm_name_reply(dis, xcb_icccm_get_wm_name_unchecked(dis, c?c->win:screen->root), &text_reply, NULL)) { /* TODO: error handling */
        memcpy(name, text_reply.name, text_reply.name_len);
        xcb_icccm_get_text_property_reply_wipe(&text_reply);
    } else strcpy(name, "broken");
    if (c) strncpy(c->name, name, 254);
    else   strncpy(statustext, name, 254);
    drawbars();
}

/* property notify is called when one of the window's properties
 * is changed, such as an urgent hint is received
 */
void propertynotify(xcb_generic_event_t *e) {
    xcb_property_notify_event_t *ev = (xcb_property_notify_event_t*)e;
    xcb_icccm_wm_hints_t wmh;
    client *c;

    DEBUG("xcb: property notify");

    /* update status */
    if (ev->window == screen->root && ev->atom == XCB_ATOM_WM_NAME)
        updatestatus(NULL);

    /* get client */
    if (!(c = wintoclient(ev->window))) return;

    /* get name */
    if (ev->atom == XCB_ATOM_WM_NAME)
        updatestatus(c);

    if (ev->atom != XCB_ICCCM_WM_ALL_HINTS) return;
    DEBUG("xcb: got hint!");
    if (xcb_icccm_get_wm_hints_reply(dis, xcb_icccm_get_wm_hints(dis, ev->window), &wmh, NULL)) /* TODO: error handling */
        c->isurgent = (wmh.flags & XCB_ICCCM_WM_HINT_X_URGENCY);
    desktopinfo();
}

/* to quit just stop receiving events
 * run() is stopped and control is back to main()
 */
void quit(const Arg *arg) {
    retval = arg->i;
    running = false;
}

/* remove the specified client
 * the previous client must point to the next client of the given
 * the removing client can be on any desktop, so we must return
 * back the current focused desktop
 *
 * keep in mind that the current set and the current update may
 * differ. current pointer changes in every select_desktop()
 * invocation.
 */
void removeclient(client *c) {
    DEBUG("xcb: removeclient");
    client **p = NULL;
    int OLDM = current_monitor;
    select_monitor(c->monitor);
    DEBUGP("xcb: remove monitor: %d\n", c->monitor);
    int nd = 0, cd = CM->current_desktop;
    for (bool found = false; nd<DESKTOPS && !found; nd++)
        for (select_desktop(nd), p = &CM->head; *p && !(found = *p == c); p = &(*p)->next);
    *p = c->next;
    CM->current = (CM->prevfocus && CM->prevfocus != c) ? CM->prevfocus : (*p) ? (CM->prevfocus = *p) : (CM->prevfocus = CM->head);
    select_desktop(cd);
    tile();
    if (CM->mode == MONOCLE && cd == --nd && CM->current) xcb_map_window(dis, CM->current->win);
    update_current(CM->current);
    free(c);
    if (OLDM != current_monitor) {
       select_monitor(OLDM);
       update_current(CM->current);
    }
}

/* resize the master window - check for boundary size limits
 * the size of a window can't be less than MINWSZ
 */
void resize_master(const Arg *arg) {
    int msz = CM->master_size + arg->i;
    if ((CM->mode == BSTACK ? CM->wh : CM->ww) - msz <= MINWSZ || msz <= MINWSZ) return;
    CM->master_size = msz;
    tile();
}

/* resize the first stack window - no boundary checks */
void resize_stack(const Arg *arg) {
    CM->growth += arg->i;
    tile();
}

void rotate_monitor(const Arg *arg) {
    change_monitor(&(Arg){.i = (current_monitor + MONITORS + arg->i) % MONITORS});
}

/* jump and focus the next or previous desktop */
void rotate(const Arg *arg) {
    change_desktop(&(Arg){.i = (DESKTOPS + CM->current_desktop + arg->i) % DESKTOPS});
}

/* jump and focus the next or previous desktop that has clients */
void rotate_filled(const Arg *arg) {
    int n = arg->i;
    while (n < DESKTOPS && !CM->desktops[(DESKTOPS + CM->current_desktop + n) % DESKTOPS].head) (n += arg->i);
    change_desktop(&(Arg){.i = (DESKTOPS + CM->current_desktop + n) % DESKTOPS});
}

/* main event loop - on receival of an event call the appropriate event handler */
void run(void) {
    xcb_generic_event_t *ev;
    while(running) {
        xcb_flush(dis);
        if (xcb_connection_has_error(dis)) die("error: X11 connection got interrupted\n");
        if ((ev = xcb_wait_for_event(dis))) {
            if (events[ev->response_type & ~0x80]) events[ev->response_type & ~0x80](ev);
            else { DEBUGP("xcb: unimplented event: %d\n", ev->response_type & ~0x80); }
            free(ev);
        }
#if OPENGL
        loopgl(CM->bar_win); /* enter opengl loop */
#endif
    }
}

void select_monitor(int i) {
   if (i >= MONITORS) return;
   CM = &monitors[i];
   current_monitor = i;
}

/* save specified desktop's properties */
void save_desktop(int i) {
    if (i < 0 || i >= DESKTOPS) return;
    CM->desktops[i].master_size = CM->master_size;
    CM->desktops[i].mode        = CM->mode;
    CM->desktops[i].growth      = CM->growth;
    CM->desktops[i].head        = CM->head;
    CM->desktops[i].current     = CM->current;
    CM->desktops[i].showpanel   = CM->showpanel;
    CM->desktops[i].prevfocus   = CM->prevfocus;
}

/* set the specified desktop's properties */
void select_desktop(int i) {
    if (i < 0 || i >= DESKTOPS) return;
    save_desktop(CM->current_desktop);
    CM->master_size     = CM->desktops[i].master_size;
    CM->mode            = CM->desktops[i].mode;
    CM->growth          = CM->desktops[i].growth;
    CM->head            = CM->desktops[i].head;
    CM->current         = CM->desktops[i].current;
    CM->showpanel       = CM->desktops[i].showpanel;
    CM->prevfocus       = CM->desktops[i].prevfocus;
    CM->current_desktop = i;
}

/* set or unset fullscreen state of client */
void setfullscreen(client *c, bool fullscrn) {
    DEBUGP("xcb: set fullscreen: %d\n", fullscrn);
    long data[] = { fullscrn ? netatoms[NET_FULLSCREEN] : XCB_NONE };
    if (fullscrn != c->isfullscrn) xcb_change_property(dis, XCB_PROP_MODE_REPLACE, c->win, netatoms[NET_WM_STATE], XCB_ATOM_ATOM, 32, fullscrn, data);
    if ((c->isfullscrn = fullscrn)) xcb_move_resize(dis, c->win, CM->wx, CM->wy, CM->ww, CM->wh + PANEL_HEIGHT);
    else drawbars();
}

/* get numlock modifier using xcb */
int setup_keyboard(void)
{
    xcb_get_modifier_mapping_reply_t *reply;
    xcb_keycode_t                    *modmap;
    xcb_keycode_t                    *numlock;

    reply   = xcb_get_modifier_mapping_reply(dis, xcb_get_modifier_mapping_unchecked(dis), NULL); /* TODO: error checking */
    if (!reply) return -1;

    modmap = xcb_get_modifier_mapping_keycodes(reply);
    if (!modmap) return -1;

    numlock = xcb_get_keycodes(XK_Num_Lock);
    for (unsigned int i=0; i<8; i++)
       for (unsigned int j=0; j<reply->keycodes_per_modifier; j++) {
           xcb_keycode_t keycode = modmap[i * reply->keycodes_per_modifier + j];
           if (keycode == XCB_NO_SYMBOL) continue;
           for (unsigned int n=0; numlock[n] != XCB_NO_SYMBOL; n++)
               if (numlock[n] == keycode) {
                   DEBUGP("xcb: found num-lock %d\n", 1 << i);
                   numlockmask = 1 << i;
                   break;
               }
       }

    return 0;
}

void drawbar(bool active_monitor) {
    int offsetx = 0, width = 0, height = 0;

    /* background */
    xcb_rectangle_t panel_vis[] = {
        {0, 0, CM->ww + BORDER_WIDTH, PANEL_HEIGHT}, /* BG */
    };
    xcb_change_gc_single(dis, CM->bar_gc, XCB_GC_FOREGROUND, xcb_get_colorpixel(BAR_BACKGROUND));
    xcb_poly_fill_rectangle(dis, CM->bar_pixmap, CM->bar_gc, 1, panel_vis);

    /* tags */
    for (int d=0; d<DESKTOPS; d++) {
        xcb_rectangle_t tag_vis[] = {
            { 5 + (15 * d), 5, 10, PANEL_HEIGHT - 10 }
        };
        xcb_change_gc_single(dis, CM->bar_gc, XCB_GC_FOREGROUND, xcb_get_colorpixel(!active_monitor ? "#9d9d9d" : d == CM->current_desktop ? "#44ddff" : "#9d9d9d"));
        xcb_poly_fill_rectangle(dis, CM->bar_pixmap, CM->bar_gc, 1, tag_vis);
    }
    offsetx = 5 + (15 * DESKTOPS);

    /* layout */
    xcb_rectangle_t layout_vis[] = {
        { offsetx, 5, 10, PANEL_HEIGHT - 10 }
    };
    xcb_change_gc_single(dis, CM->bar_gc, XCB_GC_FOREGROUND, xcb_get_colorpixel(
                CM->mode == TILE ? "#ff0000" : CM->mode == BSTACK ? "#00FF00" : CM->mode == MONOCLE ? "#000000" : "#FFFFFF"));
    xcb_poly_fill_rectangle(dis, CM->bar_pixmap, CM->bar_gc, 1, layout_vis);
    offsetx += 15;

    /* render */
    xcb_copy_area(dis, CM->bar_pixmap, CM->bar_win, CM->bar_gc, 0, 0, 0, 0, CM->ww + BORDER_WIDTH, PANEL_HEIGHT);

    /* cairo draw */
    unsigned int rgb = xcb_get_colorpixel(CM->mode == TILE ? "#ff0000" : CM->mode == BSTACK ? "#00FF00" : CM->mode == MONOCLE ? "#000000" : "#FFFFFF");
    const char *text = CM->mode == TILE ? "TILE" : CM->mode == BSTACK ? "BSTACK" : CM->mode == MONOCLE ? "MONOCLE" : "GRID";
    cairo_set_source_rgb(CM->bar_cr, (double)((rgb >> 16) / 255.0), (double)((rgb >> 8 & 0xFF) / 255.0), (double)((rgb & 0xFF) / 255.0));
    setfont("IPAGothic 10");
    drawtext(text, offsetx, PANEL_HEIGHT, CM->bar_cr);
    textsize(text, &width, &height, CM->bar_cr);
    offsetx += 5 + width;

    /* draw status */
    rgb = xcb_get_colorpixel("#9d9d9d");
    cairo_set_source_rgb(CM->bar_cr, (double)((rgb >> 16) / 255.0), (double)((rgb >> 8 & 0xFF) / 255.0), (double)((rgb & 0xFF) / 255.0));
    textsize(statustext, &width, &height, CM->bar_cr);
    drawtext(statustext, (CM->ww + BORDER_WIDTH) - width, PANEL_HEIGHT, CM->bar_cr);

    /* get client count */
    int n = 0;
    for (client *c=CM->head; c; c=c->next) ++n;
    if (!n) return;

    /* get total width of clients */
    int client_area = (CM->ww + BORDER_WIDTH) - offsetx - width - 10;
    int i = 0, extra = 0, tw = 0, mw = client_area / n;
    for (client *c=CM->head; c; c=c->next) {
        textsize(c->name, &width, &height, CM->bar_cr);
        tw = 5 + width;
        if (tw < mw) extra += (mw - tw); else i++;
    }
    if (i > 0) mw += extra / i;

    /* draw clients */
    char name[255]; bool bar_full = false;
    for (client *c=CM->head; c; c=c->next) {
        unsigned int gap = 0; bool get_gap = true;
        while ((width && 5 + width > mw) || get_gap) {
            strcpy(name, c->name); get_gap = false;
            if (gap && gap+3<=strlen(c->name)) {
                for (int i=1; i<4; ++i) name[strlen(c->name) - gap - i] = '.';
                name[strlen(c->name) - gap] = 0;
            }
            textsize(name, &width, &height, CM->bar_cr);
            if ((bar_full = (++gap >= strlen(c->name) - 2)))
            { c = CM->current; snprintf(name, 254, "%s [ + %d other clients ]", c->name, n-1); break; }
        }

        rgb = xcb_get_colorpixel(c == CM->current ? "#44ddff" : "#9d9d9d");
        cairo_set_source_rgb(CM->bar_cr, (double)((rgb >> 16) / 255.0), (double)((rgb >> 8 & 0xFF) / 255.0), (double)((rgb & 0xFF) / 255.0));
        drawtext(name, offsetx, PANEL_HEIGHT, CM->bar_cr);
        offsetx += 5 + width; width = 0;
        if (bar_full) break;
    }
}

void drawbars() {
    int OLDM = current_monitor;
    for (int m=0; m<MONITORS; m++)
    { select_monitor(m); drawbar(m == OLDM); }
    select_monitor(OLDM);
}

static void setup_monitor(int i, int x, int y, int w, int h)
{
    select_monitor(i);
    if (!(CM->desktops = calloc(DESKTOPS, sizeof(desktop)))) die("error: could not allocate memory for desktops @ monitor %d\n", i);
    CM->ww = w - BORDER_WIDTH + 2; CM->wh = h - (SHOW_PANEL ? PANEL_HEIGHT : 0) - BORDER_WIDTH + 2;
    CM->wx = x; CM->wy = y; CM->showpanel = SHOW_PANEL; CM->mode = DEFAULT_MODE;
    CM->master_size = ((CM->mode == BSTACK) ? CM->wh : CM->ww) * MASTER_SIZE;

    CM->bar_win = xcb_generate_id(dis);
    xcb_create_window(dis, XCB_COPY_FROM_PARENT, CM->bar_win, screen->root, x, y + (TOP_PANEL ? 0 : h - PANEL_HEIGHT), w, PANEL_HEIGHT,
                      0, XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, 0, NULL);
    xcb_map_window(dis, CM->bar_win);

    /* normal xcb GC context (maybe do all drawing in cairo?) */
    CM->bar_pixmap = xcb_generate_id(dis);
    CM->bar_gc     = xcb_generate_id(dis);
    xcb_create_pixmap(dis, screen->root_depth, CM->bar_pixmap, CM->bar_win, w, PANEL_HEIGHT);
    xcb_create_gc(dis, CM->bar_gc, CM->bar_pixmap, 0, 0);

    /* cairo context */
    CM->bar_cr_s = cairo_xcb_surface_create(dis, CM->bar_win, xcb_get_root_visual_type(screen), w, PANEL_HEIGHT);
    CM->bar_cr   = cairo_create(CM->bar_cr_s);

    for (int d=0; d<DESKTOPS; d++) save_desktop(d);
    change_desktop(&(Arg){.i = DEFAULT_DESKTOP});

    DEBUGP("%d: %dx%d+%d,%d\n", i, CM->ww, CM->wh, CM->wx, CM->wy);
}

static void setup_monitors(int window)
{
#if XINERAMA
    xcb_xinerama_query_screens_reply_t *xinerama_reply;
    xcb_xinerama_screen_info_iterator_t xinerama_iter;
    if (!window) {
        if (!(xinerama_reply = xcb_xinerama_query_screens_reply(dis, xcb_xinerama_query_screens(dis), NULL))) /* TODO: check error */
            die("error: xinerama failed to query screens\n");
        xinerama_iter = xcb_xinerama_query_screens_screen_info_iterator(xinerama_reply);
        MONITORS = xinerama_iter.rem?xinerama_iter.rem:MONITORS;
        free(xinerama_reply);
    } else MONITORS = 1; /* window mode */
#endif /* XINERAMA */

    /* alloc monitors */
    DEBUGP("MONITORS: %d\n", MONITORS);
    if (!(monitors = calloc(MONITORS, sizeof(monitor)))) die("error: could not allocate memory for monitors");

#if XINERAMA
    if (xinerama_iter.rem && !window) {
        for (int i=0; xinerama_iter.rem; xcb_xinerama_screen_info_next(&xinerama_iter)) {
            setup_monitor(i++, xinerama_iter.data->x_org, xinerama_iter.data->y_org,
                    xinerama_iter.data->width,
                    xinerama_iter.data->height - (SHOW_PANEL ? PANEL_HEIGHT : 0));
        }
    } else setup_monitor(0, 0, 0, screen->width_in_pixels, screen->height_in_pixels);
#else /* XINERAMA */
    setup_monitor(0, 0, 0, screen->width_in_pixels, screen->height_in_pixels);
#endif /* XINERAMA */
}

/* set initial values
 * root window - screen height/width - atoms - xerror handler
 * set masks for reporting events handled by the wm
 * and propagate the suported net atoms
 */
int setup(int window, int default_screen) {
    sigchld();
    strcpy(statustext, WMNAME" "VERSION);

    screen = xcb_screen_of_display(dis, default_screen);
    if (!screen) die("error: cannot aquire screen\n");

    /* window mode */
    if (window) {
        window = xcb_generate_id(dis);
        xcb_create_window(dis, XCB_COPY_FROM_PARENT, window, screen->root, 0, 0, 640, 480, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, 0, NULL);
        xcb_map_window(dis, (screen->root = window));
        screen->width_in_pixels = 640; screen->height_in_pixels = 480;
    }

    /* check if another wm is running */
    if (xcb_checkotherwm()) die("error: other wm is running\n");

    /* setup monitors */
    setup_monitors(window);
    win_focus   = getcolor(FOCUS);
    win_unfocus = getcolor(UNFOCUS);

#if OPENGL
    if (setupgl(screen->root, screen->width_in_pixels, screen->height_in_pixels) == -1)
        die("error: failed to enable composition\n");
#endif

    /* setup keyboard */
    if (setup_keyboard() == -1)
        die("error: failed to setup keyboard\n");

    /* set up atoms for dialog/notification windows */
    xcb_get_atoms(WM_ATOM_NAME, wmatoms, WM_COUNT);
    xcb_get_atoms(NET_ATOM_NAME, netatoms, NET_COUNT);

    xcb_change_property(dis, XCB_PROP_MODE_REPLACE, screen->root, netatoms[NET_SUPPORTED], XCB_ATOM_ATOM, 32, NET_COUNT, netatoms);
    grabkeys();

    /* set events */
    for (unsigned int i=0; i<XCB_NO_OPERATION; i++) events[i] = NULL;
    events[XCB_BUTTON_PRESS]        = buttonpress;
    events[XCB_CLIENT_MESSAGE]      = clientmessage;
    events[XCB_CONFIGURE_REQUEST]   = configurerequest;
    events[XCB_DESTROY_NOTIFY]      = destroynotify;
    events[XCB_ENTER_NOTIFY]        = enternotify;
    events[XCB_KEY_PRESS]           = keypress;
    events[XCB_MAP_REQUEST]         = maprequest;
    events[XCB_PROPERTY_NOTIFY]     = propertynotify;
    events[XCB_UNMAP_NOTIFY]        = unmapnotify;
    events[XCB_MOTION_NOTIFY]       = motionnotify;

    change_monitor(&(Arg){.i = DEFAULT_MONITOR});
    change_desktop(&(Arg){.i = DEFAULT_DESKTOP});
    drawbars();
    return 0;
}

void sigchld() {
    if (signal(SIGCHLD, sigchld) == SIG_ERR)
        die("error: can't install SIGCHLD handler\n");
    while(0 < waitpid(-1, NULL, WNOHANG));
}

/* execute a command */
void spawn(const Arg *arg) {
    if (fork()) return;
    if (dis) close(screen->root);
    setsid();
    execvp((char*)arg->com[0], (char**)arg->com);
    fprintf(stderr, "error: execvp %s", (char *)arg->com[0]);
    perror(" failed"); /* also prints the err msg */
    exit(EXIT_SUCCESS);
}

/* arrange windows in normal or bottom stack tile */
void stack(int hh, int cy) {
    client *c;
    int n = 0, d = 0, z = (CM->mode == BSTACK ? CM->ww : hh), cx = 0, cw = 0, ch = 0;

    /* count stack windows - start from head->next */
    for (n=0, c=CM->head->next; c; c=c->next) if (!c->isfullscrn && !c->isfloating && !c->istransient) ++n;

    /* grab the first non-floating, non-fullscreen window and place it on master
     * if it's a stack window, remove it from the stack count (--n)
     */
    for (c=CM->head; c && (c->isfullscrn || c->isfloating || c->istransient); c=c->next, --n);

    /* if there is only one window, it should cover the available screen space
     * if there is only one stack window (n == 1) then we don't care about growth
     * if more than one stack windows (n > 1) on screen then adjustments may be needed
     *   - d is the num of pixels than remain on the bottom of the screen plus the growth
     *   - z is the clients' height/width
     *
     *      ----------  -.    --------------------.
     *      |   |----| --|--> growth               `}--> first client will get (z+d) height/width
     *      |   |    |   |                          |
     *      |   |----|   }--> screen height - hh  --'
     *      |   |    | }-|--> client height - z       :: 2 stack clients on tile mode ..looks like a spaceship
     *      ----------  -'                            :: piece of aart by c00kiemon5ter o.O om nom nom nom nom
     *
     *     what we do is, remove the growth from the screen height  : (z - growth)
     *     and then divide that space with the windows on the stack : (z - growth)/n
     *     so all windows have equal height/width (z)
     *     growth is left out and will later be added to the first's client height/width
     *     before that, there will be cases when the num of windows is not perfectly
     *     divided with then available screen height/width (ie 100px scr. height, and 3 windows)
     *     so we get that remaining space and merge growth to it (d): (z - growth) % n + growth
     *     finally we know each client's height, and how many pixels should be added to
     *     the first stack window so that it satisfies growth, and doesn't create gaps
     *     on the bottom of the screen.
     */
    if (c && n < 1) {
        xcb_move_resize(dis, c->win, CM->wx + cx, CM->wy + cy, CM->ww - 2*BORDER_WIDTH, hh - 2*BORDER_WIDTH);
        return;
    } else if (c && n > 1) { d = (z - CM->growth) % n + CM->growth; z = (z - CM->growth) / n; }

    /* tile the first non-floating, non-fullscreen window to cover the master area */
    if (c) (CM->mode == BSTACK) ? xcb_move_resize(dis, c->win, CM->wx + cx, CM->wy + cy, CM->ww - 2*BORDER_WIDTH, CM->master_size - BORDER_WIDTH)
                                : xcb_move_resize(dis, c->win, CM->wx + cx, CM->wy + cy, CM->master_size - BORDER_WIDTH, hh - 2*BORDER_WIDTH);

    /* tile the next non-floating, non-fullscreen stack window with growth/d */
    if (c) for (c=c->next; c && (c->isfullscrn || c->isfloating || c->istransient); c=c->next);
    if (c) (CM->mode == BSTACK) ? xcb_move_resize(dis, c->win, CM->wx + cx, CM->wy + (cy += CM->master_size),
                              (cw =  z - BORDER_WIDTH)  - BORDER_WIDTH + d,
                              (ch = hh - BORDER_WIDTH*2 - CM->master_size))
                            : xcb_move_resize(dis, c->win, CM->wx + (cx += CM->master_size), CM->wy + cy,
                              (cw = CM->ww - BORDER_WIDTH*2 - CM->master_size),
                              (ch =  z - BORDER_WIDTH)  - BORDER_WIDTH + d);

    /* tile the rest of the non-floating, non-fullscreen stack windows */
    if (c) for (CM->mode==BSTACK?(cx+=cw+d):(cy+=ch+d), c=c->next; c; c=c->next)
        if (!c->isfullscrn && !c->isfloating && !c->istransient) {
            xcb_move_resize(dis, c->win, CM->wx + cx, CM->wy + cy, cw, ch);
            (CM->mode == BSTACK) ? (cx+=z) : (cy+=z);
        }
}

/* swap master window with current or
 * if current is head swap with next
 * if current is not head, then head
 * is behind us, so move_up until we
 * are the head
 */
void swap_master() {
    if (!CM->current || !CM->head->next || CM->mode == MONOCLE) return;
    if (CM->current == CM->head) move_down();
    else while (CM->current != CM->head) move_up();
    update_current(CM->head);
    tile();
}

/* switch the tiling mode and reset all floating windows */
void switch_mode(const Arg *arg) {
    if (CM->mode == arg->i) for (client *c=CM->head; c; c=c->next) c->isfloating = False;
    if (CM->mode == MONOCLE) for (client *c=CM->head; c; c=c->next) xcb_map_window(dis, c->win);
    CM->mode = arg->i;
    CM->master_size = (CM->mode == BSTACK ? CM->wh : CM->ww) * MASTER_SIZE;
    tile();
    update_current(CM->current);
    desktopinfo();
}

/* tile all windows of current desktop - call the handler tiling function */
void tile(void) {
    if (!CM->head) return; /* nothing to arange */
    layout[CM->head->next ? CM->mode : MONOCLE](CM->wh + (CM->showpanel ? 0 : PANEL_HEIGHT),
                                          (TOP_PANEL && CM->showpanel ? PANEL_HEIGHT : 0));
}

/* toggle visibility state of the panel */
void togglepanel() {
    CM->showpanel = !CM->showpanel;
    tile();
    if (CM->showpanel) { xcb_map_window(dis, CM->bar_win); drawbar(true); }
    else xcb_unmap_window(dis, CM->bar_win);
}

/* windows that request to unmap should lose their
 * client, so no invisible windows exist on screen
 */
void unmapnotify(xcb_generic_event_t *e) {
    xcb_unmap_notify_event_t *ev = (xcb_unmap_notify_event_t *)e;
    client *c = wintoclient(ev->window);
    if (c && ev->event != screen->root) removeclient(c);
    desktopinfo();
}

/* update client - set highlighted borders and active window
 * if no client is given update current
 * if current is NULL then delete the active window property
 *
 * a window should have borders in any case except if
 *  - the window is not floating or transient
 *  - the window is fullscreen
 *  - the window is the only window on screen
 *  - the mode is MONOCLE and non of the above applies
 */
void update_current(client *c) {
    if (!c) {
        xcb_delete_property(dis, screen->root, netatoms[NET_ACTIVE]);
        return;
    } else CM->current = c;

    for (int m=0; m<MONITORS; m++) {
        for (c=monitors[m].head; c; c=c->next) {
            xcb_border_width(dis, c->win, (!monitors[m].head->next || c->isfullscrn || (monitors[m].mode == MONOCLE && (!c->isfloating && !c->istransient))) ? 0 : BORDER_WIDTH);
            xcb_change_window_attributes(dis, c->win, XCB_CW_BORDER_PIXEL, (CM->current == c ? &win_focus : &win_unfocus));
            if (CLICK_TO_FOCUS) xcb_grab_button(dis, 1, c->win, XCB_EVENT_MASK_BUTTON_PRESS, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC,
               screen->root, XCB_NONE, XCB_BUTTON_INDEX_1, XCB_BUTTON_MASK_ANY);
        }
    }

    xcb_change_property(dis, XCB_PROP_MODE_REPLACE, screen->root, netatoms[NET_ACTIVE], XCB_ATOM_WINDOW, 32, 1, &CM->current->win);
    xcb_set_input_focus(dis, XCB_INPUT_FOCUS_POINTER_ROOT, CM->current->win, XCB_CURRENT_TIME);
    if (CLICK_TO_FOCUS) xcb_ungrab_button(dis, XCB_BUTTON_INDEX_1, CM->current->win, XCB_BUTTON_MASK_ANY);

    /* keep transient and floating windows on top of regular windows */
    bool r = false;
    if (CM->current->isfloating || CM->current->istransient)                { xcb_raise_window(dis, CM->current->win); r = true; }
    else for(c=CM->head; c; c=c->next) if (c->istransient || c->isfloating) { xcb_raise_window(dis, c->win);           r = true; }
    if (!r) xcb_raise_window(dis, CM->current->win);

    if (CLICK_TO_FOCUS) grabbuttons(CM->current);
    drawbar(true);
}

/* find to which client the given window belongs to */
client* wintoclient(xcb_window_t w) {
    client *c = NULL;
    int d = 0, m = 0, OLDM = current_monitor, cd; bool found = false;
    for (; m<MONITORS && !found; ++m) {
        select_monitor(m); d = 0; cd = CM->current_desktop;
        for (; d<DESKTOPS && !found; ++d)
            for (select_desktop(d), c=monitors[m].head; c && !(found = (w == c->win)); c=c->next);
        select_desktop(cd);
    }
    select_monitor(OLDM);
    return c;
}

/* open xcb connection */
int openconnection(int *default_screen)
{
#if !OPENGL
    return !xcb_connection_has_error((dis = xcb_connect(NULL, default_screen)));
#else
    return connectiongl(&dis, default_screen);
#endif
}

/* close xcb connection */
void closeconnection()
{
#if !OPENGL
    xcb_disconnect(dis);
#else
    closeconnectiongl();
#endif
}

int main(int argc, char *argv[]) {
    int default_screen, window = 0;
    if (argc == 2 && !strcmp("-v", argv[1])) {
        fprintf(stdout, "%s-%s\n", WMNAME, VERSION);
        return EXIT_SUCCESS;
    } else if (argc == 2 && !strcmp("-t", argv[1])) window = 1;
      else if (argc != 1) die("usage: %s [-v][-t]\n", WMNAME);
    if (!openconnection(&default_screen))
        die("error: cannot open xcb connection.\n");
    if (setup(window, default_screen) != -1) {
      desktopinfo(); /* zero out every desktop on (re)start */
      run();
    }
    cleanup();
    closeconnection();
    return retval;
}

/* vim: set ts=4 sw=4 :*/
