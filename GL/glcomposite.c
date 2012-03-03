#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "glcomposite.h"
#include <xcb/xcb.h>
#include <xcb/composite.h>

static xcb_connection_t    *dis;
static Display             *gldis;
static int                 glscrn;
static Window              glroot;
static GLXContext          glctx;
static GLXFBConfig         pixconfig;
static int glwidth, glheight;

/* GL extensions */
static PFNGLXBINDTEXIMAGEEXTPROC    glXBindTexImageEXT      = NULL;
static PFNGLXRELEASETEXIMAGEEXTPROC glXReleaseTexImageEXT   = NULL;

typedef struct glwin
{
   xcb_window_t win;
   unsigned int tex;
   GLXPixmap    pix;
   int x, y, w, h;
   struct glwin *prev, *next;
} glwin;

glwin *glstack = NULL;

static glwin* alloc_glwin(xcb_window_t win)
{
   int pixatt[] = { GLX_TEXTURE_TARGET_EXT, GLX_TEXTURE_RECTANGLE_EXT,
                    GLX_TEXTURE_FORMAT_EXT, GLX_TEXTURE_FORMAT_RGB_EXT,
                    None };
   xcb_pixmap_t xcb_pix;
   glwin *w = malloc(sizeof(glwin));
   w->win  = win;
   w->tex  = 0;
   w->pix  = 0;
   w->w    = 0;
   w->h    = 0;
   w->x    = 0;
   w->y    = 0;
   w->next = NULL;
   w->prev = NULL;

   /* redirect window */
   xcb_composite_redirect_window(dis, win, XCB_COMPOSITE_REDIRECT_MANUAL);

   /* create gl texture from pixmap */
   xcb_pix = xcb_generate_id(dis);
   xcb_composite_name_window_pixmap(dis, win, xcb_pix);
   w->pix = glXCreatePixmap(gldis, pixconfig, xcb_pix, pixatt);
   xcb_free_pixmap(dis, xcb_pix);

   glGenTextures(1, &w->tex);
   glBindTexture(GL_TEXTURE_RECTANGLE_ARB, w->tex);
   glXBindTexImageEXT(gldis, w->pix, GLX_FRONT_LEFT_EXT, NULL);

   glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
   glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
   glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
   glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

   /* we handle this window now with opengl :) */
   //xcb_unmap_window(dis, win);

   return w;
}

static glwin* dealloc_glwin(glwin *w)
{
   glwin *prev;

   if (!(prev = w->prev)) glstack = NULL;
   else w->prev->next = w->next;

   /* release */
   xcb_composite_unredirect_window(dis, w->win, XCB_COMPOSITE_REDIRECT_MANUAL);

   /* free */
   glXReleaseTexImageEXT(gldis, w->pix, GLX_FRONT_LEFT_EXT);
   glDeleteTextures(1, &w->tex);
   free(w);
   return prev;
}

static glwin* win_to_glwin(xcb_window_t win)
{
   for (glwin *w = glstack; w; w = w->next)
      if (w->win == win) return w;
   return NULL;
}

static glwin* add_glwin(xcb_window_t win)
{
   glwin *w;
   if ((w = win_to_glwin(win))) return w; /* already exists */
   if (!glstack)                return (glstack = alloc_glwin(win));
   for (w = glstack; w && w->next;  w = w->next);
   return (w->next = alloc_glwin(win));
}

static void update_glwin(glwin *win)
{
   xcb_get_geometry_reply_t *geometry;
   if (!(geometry = xcb_get_geometry_reply(dis, xcb_get_geometry(dis, win->win), NULL)))
      return;

   printf("geom: %ux%u+%d+%d\n", geometry->width, geometry->height, geometry->x, geometry->y);

   win->w = geometry->width; win->h = geometry->height,
   win->x = geometry->x;     win->y = geometry->y;
   free(geometry);
}

static void draw_glwin(glwin *win)
{
   float wx, wy, ww, hh;

   ww  = (float)win->w/glwidth; hh = (float)win->h/glheight;
   wx  = (float)win->x/glwidth; wy = (float)win->y/glheight;
   wy += 1-hh; /* shifts the draw so we don't start from center */

   /* TODO: use modern drawing methods, FBO's and such */
   glBindTexture(GL_TEXTURE_RECTANGLE_ARB, win->tex);
   glBegin(GL_TRIANGLE_STRIP);
   glTexCoord2f(win->w,      0); glVertex3f( wx+ww, wy+hh, 0.);
   glTexCoord2f(0,           0); glVertex3f( wx-ww, wy+hh, 0.);
   glTexCoord2f(win->w, win->h); glVertex3f( wx+ww, wy-hh, 0.);
   glTexCoord2f(0,      win->h); glVertex3f( wx-ww, wy-hh, 0.);
   glEnd();
}

/* print a message on standard error stream
 * and exit with failure exit code
 */
static void die(const char *errstr, ...) {
   va_list ap;
   va_start(ap, errstr);
   vfprintf(stderr, errstr, ap);
   va_end(ap);
   exit(EXIT_FAILURE);
}

static GLXFBConfig ChoosePixmapFBConfig()
{
   GLXFBConfig *confs;
   int i, nconfs, value;

   confs = glXGetFBConfigs(gldis, glscrn, &nconfs);
   for (i = 0; i != nconfs; i++) {
      glXGetFBConfigAttrib(gldis, confs[i], GLX_DRAWABLE_TYPE, &value);
      if (!(value & GLX_PIXMAP_BIT))
         continue;

      glXGetFBConfigAttrib(gldis, confs[i], GLX_BIND_TO_TEXTURE_TARGETS_EXT, &value);
      if (!(value & GLX_TEXTURE_2D_BIT_EXT))
         continue;

      glXGetFBConfigAttrib(gldis, confs[i], GLX_BIND_TO_TEXTURE_RGBA_EXT, &value);
      if (value == False) {
         glXGetFBConfigAttrib(gldis, confs[i], GLX_BIND_TO_TEXTURE_RGB_EXT, &value);
         if (value == False)
            continue;
      }

      glXGetFBConfigAttrib(gldis, confs[i], GLX_Y_INVERTED_EXT, &value);
      /* if value == TRUE, invert */

      break;
   }

   return confs[i];
}

void redirectgl(Window root)
{
   xcb_window_t *c;
   xcb_query_tree_reply_t *query;
   xcb_grab_server(dis);
   if (!(query = xcb_query_tree_reply(dis,xcb_query_tree(dis,root),0))) {
      xcb_ungrab_server(dis);
      return;
   }
   c = xcb_query_tree_children(query);
   for (unsigned int i = 0; i != query->children_len; ++i) add_glwin(c[i]);
   free(query);
   xcb_ungrab_server(dis);
}

int setupgl(Window root, int width, int height)
{
   GLint att[] = { GLX_RGBA, GLX_DOUBLEBUFFER, None };
   XVisualInfo *vi;
   GLXContext  glctx;
   const char *extensions;

   glroot = root;
   if (!(vi = glXChooseVisual(gldis, 0, att))) {
      puts("glXChooseVisual failed.");
      return 0;
   }
   glctx = glXCreateContext(gldis, vi, NULL, GL_TRUE);
   if (!glXMakeCurrent(gldis, glroot, glctx)) {
      puts("glXMakeCurrent failed.");
      return 0;
   }
   glViewport(0,0,(glwidth = width), (glheight = height));

   extensions = glXQueryExtensionsString(gldis, glscrn);
   if (!strstr(extensions, "GLX_EXT_texture_from_pixmap")) {
      puts("GLX_EXT_texture_from_pixmap extension is not supported on your driver.");
      return 0;
   }

   glXBindTexImageEXT    = (PFNGLXBINDTEXIMAGEEXTPROC)    glXGetProcAddress((GLubyte*) "glXBindTexImageEXT");
   glXReleaseTexImageEXT = (PFNGLXRELEASETEXIMAGEEXTPROC) glXGetProcAddress((GLubyte*) "glXReleaseTexImageEXT");

   if (!glXBindTexImageEXT || !glXReleaseTexImageEXT)
   {
      puts("glXGetProcAddress failed.");
      return 0;
   }

   /* redirect all windows */
   // xcb_composite_redirect_subwindows(dis, glroot, XCB_COMPOSITE_REDIRECT_MANUAL);

   /* get framebuffer configuration for pixmaps */
   pixconfig = ChoosePixmapFBConfig(gldis);
   glEnable(GL_TEXTURE_RECTANGLE_ARB);

   /* setup bg color */
   glClearColor(.0, .0, .2, 1.0);

   return 1;
}

void swapgl()
{
   glXSwapBuffers(gldis, glroot);
   glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void loopgl(int update, xcb_window_t window)
{
   glwin *win;

   if (!(win = add_glwin(window))) return;
   update_glwin(win);
   draw_glwin(win);
}

/* open openGL connection which needs x11-xcb */
int connectiongl(xcb_connection_t **wmcon, int *screen)
{
   xcb_composite_query_version_reply_t *ver;

   if (!(gldis = XOpenDisplay(0)))
      die("error: cannot open display\n");
   glscrn = DefaultScreen(gldis);
   if (xcb_connection_has_error((dis = XGetXCBConnection(gldis)))) {
      XCloseDisplay(gldis);
      return 0;
   }
   XSetEventQueueOwner(gldis, XCBOwnsEventQueue);

   if (!(ver = xcb_composite_query_version_reply(dis,
            xcb_composite_query_version_unchecked(dis, 0, 2), NULL)))
      die("error: could not query composite extension version\n");

   if (ver->minor_version < 2)
      die("error: composite extension 0.2 or newer needed!\n");

   *screen = glscrn; *wmcon = dis;
   return 1;
}

void closeconnectiongl()
{
   glwin *wn;

   /* free all windows */
   for (glwin *w = glstack; w; w = wn) wn = dealloc_glwin(w);

   glXDestroyContext(gldis, glctx);
   XCloseDisplay(gldis);
}
