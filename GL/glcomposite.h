#include <GL/glx.h>
#include <GL/gl.h>
#include <X11/Xlib-xcb.h> /* for XGetXCBConnection, link with libX11-xcb */

int setupgl(Window root, int width, int height);
void loopgl(xcb_window_t win);
int connectiongl();
void closeconnectiongl();
