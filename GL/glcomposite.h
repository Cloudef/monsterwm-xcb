#include <GL/glx.h>
#include <GL/gl.h>
#include <X11/Xlib-xcb.h> /* for XGetXCBConnection, link with libX11-xcb */

int setupgl(xcb_window_t root, int width, int height);
void setrootgl(xcb_window_t root);
void raisegl(xcb_window_t win);
void eventgl(xcb_generic_event_t *ev);
void loopgl();
void swapgl();
int connectiongl();
void closeconnectiongl();
