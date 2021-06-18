//////////////////////////////////////////////////////////////////////////////
//       Author: Josh Thibodaux <josh@thibodaux.net>
//    Copyright: Josh Thibodaux <josh@thibodaux.net>
//
//  Description: Finds fonts that support a given glyph.
//
//      Created: Fri 24 May 2019 12:29:02 PM PDT
//    Revisions:
//      1.0 - File created
//
//////////////////////////////////////////////////////////////////////////////
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <poll.h>
#include <sys/time.h>

#include <fontconfig/fontconfig.h>
#include <errno.h>
#include <stdint.h>
#include <iconv.h>
#include <locale.h>
#include <math.h>
#include <X11/Xft/Xft.h>
#include <uninameslist.h>
#include <X11/Xatom.h>
#include <X11/Xmu/Atoms.h>
#include <X11/extensions/Xdbe.h>

#define DBG(...) { if(args.debug) fprintf(stderr, __VA_ARGS__); }

#define DBG_P(...) { if(args.debug) { XFlush(global.dpy); fprintf(stderr, __VA_ARGS__); } }

// Refresh rate for window
#define TIMEOUT 100

// Global storage of command line flags.
struct {
  int display;
  int maxfonts;
  int debug;
  int showname;
  int showannot;
  int printfonts;
  int fixed;
} args = { 1, 0, 0, 0, 0, 0, 0 };

struct {
  // X11 Elements
  Display *dpy;
  FcFontSet *fs;
  Drawable win;
  XftDraw *xdraw;
  XftColor ftblack;
  XColor white;
  XColor black;
  GC xgc;
  Drawable backbuf; // Backing buffer if DBE enabled
  Drawable draw;    // Drawable to draw to (window or back buffer)
  double lastpaint; // Last time window was painted.
  int dirty;        // If window needs to be repainted.
  int quitdims[4];
  // Character information from libuninameslist
  struct unicode_nameannot info;
  // Desired character in UTF32
  uint32_t character;
  // Desired character as hex string
  char hexchar[11];
} global;


/** Parses arguments and returns location of first non-option argument.
 *
 *  Returns a negative value on failure.
 */
int parse_arguments(int argc, char *argv[])
{
    while(1)
    {
        int option_index = 0;

        static struct option long_options[] = {
          {"nodisplay"  , no_argument      , 0, 'N'},
          {"help"       , no_argument      , 0, 'h'},
          {"maxfonts"   , required_argument, 0, 'm'},
          {"debug"      , no_argument      , 0, 'd'},
          {"name"       , no_argument      , 0, 'n'},
          {"annotation" , no_argument      , 0, 'a'},
          {"print"      , no_argument      , 0, 'p'},
          {"fixed"      , no_argument      , 0, 'f'},
          {0            , 0                , 0, 0}
        };

        int c = getopt_long(argc, argv, "Nnhm:dapc::", long_options, &option_index);

        switch(c)
        {
        case 'h':
          printf("fc-char v" VERSION "\n");
          printf("Usage: %s [options] {<hex codepoint>|<character>}\n", argv[0]);
          printf("Options:\n");
          printf("--help         / -h        :  Show help (this)\n");
          printf("--nodisplay    / -N        :  Don't display found glyphs.\n");
          printf("--maxfonts #   / -m#       :  Maximum number of fonts to return/show.\n");
          printf("--debug        / -d        :  Print debugging information.\n");
          printf("--name         / -n        :  Print unicode character name.\n");
          printf("--annotation   / -a        :  Print unicode character annotation string.\n");
          printf("--print        / -p        :  Print list of fonts with character.\n");
          printf("--fixed        / -f        :  Include fixed-size fonts.\n");
          return -1;
          break;

        case 'N':
          args.display = 0;
          break;

        case 'm':
          args.maxfonts = atoi(optarg);
          break;

        case 'd':
          args.debug = 1;
          break;

        case 'n':
          args.showname = 1;
          break;

        case 'a':
          args.showannot = 1;
          break;

        case 'p':
          args.printfonts = 1;

        case 'f':
          args.fixed = 1;
          break;

        default:
          if(optind < argc) {
            return optind;
          } else {
            return -1;
          }
          break;
        }
    }
    return 0;
}

// Initial font size to use in scaling.
#define INITFTSZ 12.0

/** Determines font size for the title string.
 *
 * This iterates through the list of fonts, determining
 * the maximum size that will render all family names
 * within the given width and height.
 *
 * \param family Font family to use in rendering.
 * \param width Width of render box.
 * \param height Height of render box.
 * \return Newly allocated font at appropriate size or NULL on error.
 */
XftFont *gen_scale_title_font(const char *family, int width, int height)
{
    XftFont *font = XftFontOpen(global.dpy, XDefaultScreen(global.dpy),
                                FC_FAMILY, XftTypeString, family,
                                FC_SIZE, XftTypeDouble, INITFTSZ,
                                NULL);
    if(font == NULL) {
      return NULL;
    }

    double scale = -1.0;

    for(int i = 0; i < global.fs->nfont; i++) {
      FcChar8 *famname = FcPatternFormat(global.fs->fonts[i], (FcChar8 *)"%{family[0]}");
      if(famname == NULL) {
        XftFontClose(global.dpy, font);
        return NULL;
      }

      XGlyphInfo extents;
      XftTextExtents8(global.dpy, font, famname, strlen((char *)famname), &extents);
      free(famname);

      double xscale = (double)width / (double)extents.width;
      /*double yscale = (double)height / (double)extents.height;*/
      double yscale = (double)height / (double)font->height;
      double newscale = xscale < yscale ? xscale : yscale;

      if((scale < 0) || (newscale < scale)) {
        scale = newscale;
      }
    }

    XftFontClose(global.dpy, font);

    return XftFontOpen(global.dpy, XDefaultScreen(global.dpy),
                       FC_FAMILY, XftTypeString, family,
                       FC_SIZE, XftTypeDouble, INITFTSZ * scale,
                       NULL);
}

/** Draws the grid of characters.
 *
 * \param width Width of grid area
 * \param height Height of grid area
 * \param yoffset Y offset from top of drawable to grid area.
 * \param character The character to render.
 */
int generate_grid(unsigned int width, unsigned int height, int yoffset, FcChar32 *character)
{
// Vertical padding (pixels)
#define VPADDING 5
// Horizontal padding (pixels)
#define HPADDING 5
// How much vertical space for font name (percentage)
#define FTSPACE 0.2
// Font name font
#define FTNAMEFT "charter"
// Border width (pixels)
#define BDRWIDTH 2

    // Number of fonts to render
    int c = (args.maxfonts && (args.maxfonts < global.fs->nfont)) ? args.maxfonts : global.fs->nfont;
    // Number of columns
    int nw = (int)round(sqrt((double)c * (double)width / (double)height));
    // Number of rows
    int nh = (int)round((double)c / (double)nw);
    // Width of each box
    int bw = (int)floor((double)width / (double)nw);
    // Height of each box
    int bh = (int)floor((double)height / (double)nh);
    // Height of font name portion
    int fh = (int)((double)bh * FTSPACE);
    // Height of character portion
    int ch = bh - fh;
    // Height of font name rendering area
    int frh = fh - 2*VPADDING;
    // Height of character rendering area
    int crh = ch - 2*VPADDING;
    // Width of font name & character rendering area
    int cw = bw - 2*HPADDING;

    DBG("nw %d nh %d\n", nw, nh);
    DBG("bw %d bh %d\n", bw, bh);

    if((frh <= 0) || (crh <= 0) || (cw <= 0)) {
      return -1;
    }

    // Determine font size to use when rendering font names
    XftFont *fnfont = gen_scale_title_font(FTNAMEFT, cw, frh);
    if(fnfont == NULL) {
      return -1;
    }

    // Render characters, names, and grid squares.
    for(int i = 0; i < c; i++) {
      // Render grid rectangle
      int rx = (i % nw) * bw;
      int ry = (i / nw) * bh + yoffset;
      XDrawRectangle(global.dpy, global.draw, global.xgc, rx, ry, bw, bh);
      DBG_P("Rectangle (%d, %d) %d x %d\n", rx, ry, bw, bh);

      int xcoord = (i % nw) * bw + HPADDING;
      int ycoord = (i / nw) * bh + frh + VPADDING + yoffset;
      XGlyphInfo extents;
      FcChar8 *family = FcPatternFormat(global.fs->fonts[i], (FcChar8 *)"%{family[0]}");
      XftTextExtentsUtf8(global.dpy, fnfont, family, strlen((char *)family), &extents);
      int xadjust = (cw - extents.width) / 2;
      if(xadjust > 0)
        xcoord += xadjust;

      XftDrawStringUtf8(global.xdraw, &global.ftblack, fnfont,
                        xcoord, ycoord, family, strlen((char *)family));

      DBG_P("Family name: %s\n", family);

      XftFont *cfont = XftFontOpen(global.dpy, XDefaultScreen(global.dpy),
                                   FC_FAMILY, XftTypeString, family,
                                   FC_PIXEL_SIZE, XftTypeDouble, (double)crh,
                                   NULL);

      free(family);

      XftTextExtents32(global.dpy, cfont, character, 1, &extents);

      DBG("extents at new size w %d h %d x %d y %d xoff %d yoff %d\n",
          extents.width, extents.height, extents.x, extents.y,
          extents.xOff, extents.yOff);
      DBG("font info: height %d ascent %d descent %d\n", cfont->height,
          cfont->ascent, cfont->descent);

      xcoord = (i % nw) * bw + HPADDING;
      ycoord = (i / nw) * bh + fh + crh + VPADDING + yoffset - cfont->descent;
      xadjust = (cw - extents.width) / 2;
      if(xadjust > 0)
        xcoord += xadjust;
      // Render character
      DBG("Rendering character at (%d,%d)\n", xcoord, ycoord);
      DBG_P("Character render\n");

      XftDrawString32(global.xdraw, &global.ftblack, cfont,
                      xcoord, ycoord, character, 1);
      XftFontClose(global.dpy, cfont);
    }

    XftFontClose(global.dpy, fnfont);

    return 0;
}

/** Converts a character in the local code page to UTF32LE.
 *
 * \param input Buffer containing the character to convert.
 * \param output Destination buffer, should be at least 5 bytes long.
 * \param outsize The size of the destination buffer.
 * \param uchar Buffer to store character encoded as 32-bit unsigned
 */
int convert_char(char *input, char *output, int outsize, uint32_t *uchar)
{
  iconv_t it = iconv_open("UTF32LE", "");
  if(it == (iconv_t)-1) {
    fprintf(stderr, "Error initializing iconv: %s\n", strerror(errno));
    return -1;
  }

  size_t inbytesleft = strlen(input);
  size_t outbytesleft = 10;
  char outbuf[10] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  char *obufaddr = &outbuf[0];

  size_t converted = iconv(it, &input, &inbytesleft, &obufaddr, &outbytesleft);

  if(converted < 0) {
    fprintf(stderr, "Error converting glyph: %s\n", strerror(errno));
    return -1;
  }

  *uchar = *(uint32_t *)outbuf;
  if(*uchar <= 0xFFFF) {
    snprintf(output, outsize, "U+%04x", *(uint32_t *)outbuf);
  } else {
    snprintf(output, outsize, "U+%08x", *(uint32_t *)outbuf);
  }

  iconv_close(it);

  if(*(uint32_t *)outbuf == 0) {
    printf("Glyph result was 0.\n");
    return 0;
  }

  return 1;
}

/** Connects to X and creates the application's window.
 *
 * \return Zero on success, negative on failure.
 */
int initialize_x11()
{
  global.dpy = XOpenDisplay(NULL);

  memset(&global.white, 0, sizeof(global.white));
  global.white.red = global.white.green = global.white.blue = 65535;
  Status stat = XAllocColor(global.dpy, XDefaultColormap(global.dpy, XDefaultScreen(global.dpy)),
                            &global.white);
  if(stat == 0) {
    fprintf(stderr, "Could not allocate background color\n");
    return -1;
  }

  memset(&global.black, 0, sizeof(global.black));
  stat = XAllocColor(global.dpy, XDefaultColormap(global.dpy, XDefaultScreen(global.dpy)),
                     &global.black);
  if(stat == 0) {
    fprintf(stderr, "Could not allocate foreground color\n");
    return -1;
  }

  XSetWindowAttributes winattr;
  winattr.backing_store = Always;
  winattr.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask;
  winattr.background_pixel = global.white.pixel;
  global.win = XCreateWindow(global.dpy, RootWindow(global.dpy, XDefaultScreen(global.dpy)),
                             0, 0, 800, 600, 1,
                             CopyFromParent, InputOutput,
                             CopyFromParent, CWBackPixel | CWEventMask | CWBackingStore,
                             &winattr);

  global.draw = global.win;

  // Allocate back buffer
  int maj, min;
  global.backbuf = None;
//if(XdbeQueryExtension(global.dpy, &maj, &min)) {
// FIXME: Get Visual info and check support for db on screens
//    global.backbuf = XdbeAllocateBackBufferName(global.dpy, global.win, XdbeCopied);
//    if(global.backbuf  == None) {
//      DBG("Could not allocate back buffer.\n");
//    } else {
//      global.draw = global.backbuf;
//    }
//  }

  // Setup drawing surfaces for fonts and grid.
  global.xdraw = XftDrawCreate(global.dpy, global.draw, XDefaultVisual(global.dpy, XDefaultScreen(global.dpy)),
                               XDefaultColormap(global.dpy, XDefaultScreen(global.dpy)));
  XRenderColor xrc = { 0, 0, 0, 65535 };
  XftColorAllocValue(global.dpy, XDefaultVisual(global.dpy, XDefaultScreen(global.dpy)),
                     XDefaultColormap(global.dpy, XDefaultScreen(global.dpy)), &xrc, &global.ftblack);

  // Setup graphic context for grid.
  XGCValues gcvalues;
  gcvalues.foreground = global.black.pixel;
  gcvalues.line_width = BDRWIDTH;
  global.xgc = XCreateGC(global.dpy, global.draw, GCForeground | GCLineWidth, &gcvalues);

  // Tell the WM about us
  char *title;
  if(global.info.name != NULL) {
    int tsize = 16 + strlen(global.info.name);
    title = (char *)malloc(tsize);
    snprintf(title, tsize, "fc-char %s %s", global.hexchar, global.info.name);
  } else {
    title = (char *)malloc(16);
    snprintf(title, 16, "fc-char %s", global.hexchar);
  }
  XTextProperty xtitle;
  XStringListToTextProperty(&title, 1, &xtitle);
  XSetWMName(global.dpy, global.win, &xtitle);
  free(title);
  // XFree(&xtext);

  char smtitle[] = "fc-char";
  char *psmtitle = &smtitle[0];
  XTextProperty xsmtitle;
  XStringListToTextProperty(&psmtitle, 1, &xsmtitle);
  XSetWMIconName(global.dpy, global.win, &xsmtitle);
  // XFree(&xsmtitle);

  Atom delmsg = XInternAtom(global.dpy, "WM_DELETE_WINDOW", True);
  stat = XSetWMProtocols(global.dpy, global.win, &delmsg, 1);
  DBG("WMProtocol Status %d\n", stat);


  DBG("Exposing window\n");

  // Expose the window
  XMapWindow(global.dpy, global.win);

  return 0;
}

/** Free X resources.
 */
void close_x11()
{
  XftColorFree(global.dpy, XDefaultVisual(global.dpy, XDefaultScreen(global.dpy)),
               XDefaultColormap(global.dpy, XDefaultScreen(global.dpy)), &global.ftblack);
  XftDrawDestroy(global.xdraw);
  XFreeGC(global.dpy, global.xgc);

  if(global.backbuf != None) {
    XdbeDeallocateBackBufferName(global.dpy, global.backbuf);
  }

  XUnmapWindow(global.dpy, global.win);
  XDestroyWindow(global.dpy, global.win);
  XCloseDisplay(global.dpy);
}

/** Draws application window contents.
 */
void paint_window()
{
#define TITLEFONT "charter"
#define TITLEFONTSZ 14.0
  Window root;
  int x, y;
  unsigned int width, height, bwidth, depth;
  XGetGeometry(global.dpy, global.draw, &root, &x, &y,
               &width, &height, &bwidth, &depth);
  DBG("Geometry (%d, %d) (%u, %u)\n", x, y, width, height);

  XGCValues gcvalues;
  gcvalues.foreground = global.white.pixel;

  XChangeGC(global.dpy, global.xgc, GCForeground, &gcvalues);
  XFillRectangle(global.dpy, global.draw, global.xgc, 0, 0, width, height);

  gcvalues.foreground = global.black.pixel;
  XChangeGC(global.dpy, global.xgc, GCForeground, &gcvalues);

  // Add a quit button
  XftFont *font = XftFontOpen(global.dpy, XDefaultScreen(global.dpy),
                              FC_FAMILY, XftTypeString, TITLEFONT,
                              FC_SIZE, XftTypeDouble, TITLEFONTSZ,
                              NULL);
  XGlyphInfo extents;
  char quit[] = "Quit";
  XftTextExtents8(global.dpy, font, &quit[0], strlen(quit), &extents);
  global.quitdims[0] = HPADDING;
  global.quitdims[1] = VPADDING;
  int w = 2 * HPADDING + extents.width;
  int h = 2 * VPADDING + font->height;
  global.quitdims[2] = w;
  global.quitdims[3] = h;
  XDrawRectangle(global.dpy, global.draw, global.xgc, HPADDING, VPADDING, w, h);
  XftDrawStringUtf8(global.xdraw, &global.ftblack, font,
                    2 * HPADDING, 2 * VPADDING + font->height - font->descent,
                    quit, strlen(quit));

  DBG_P("Quit button\n");

  char *title;
  if(global.info.name != NULL) {
    int tsize = 16 + strlen(global.info.name);
    title = (char *)malloc(tsize);
    snprintf(title, tsize, "%s %s", global.hexchar, global.info.name);
  } else {
    title = (char *)malloc(16);
    snprintf(title, 16, "%s", global.hexchar);
  }
  XftDrawStringUtf8(global.xdraw, &global.ftblack, font,
                    2 * HPADDING + w, 2 * VPADDING + font->height - font->descent,
                    title, strlen(title));

  DBG_P("Title\n");

  free(title);
  XftFontClose(global.dpy, font);

  // Draw character grid
  int offset = h + 2 * VPADDING;
  generate_grid(width, height - offset, offset, &global.character);

  if(global.backbuf != None) {
    XdbeSwapInfo sinfo;
    sinfo.swap_window = global.win;
    sinfo.swap_action = XdbeCopied;
    XdbeSwapBuffers(global.dpy, &sinfo, 1);
  }

  XFlush(global.dpy);
}

void maybe_paint_window()
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    double now = (double)tv.tv_sec + ((double)tv.tv_usec / 1e6);
    now *= 1e3; // Adjust to milliseconds
    double diff = now - global.lastpaint;
    DBG("maybe: diff %g dirty %d\n", diff, global.dirty);

  if(((diff) > TIMEOUT) && (global.dirty)) {
    paint_window();
    global.dirty = 0;
    global.lastpaint = now;
  }
}


/** Checks if the given coordinates are within
 *  the bounds of the quit button.
 */
int check_quit_bounds(int x, int y)
{
    int dx = x - global.quitdims[0];
    if((dx < 0) || (dx > global.quitdims[2]))
      return 0;

    int dy = y - global.quitdims[1];
    if((dy < 0) || (dy > global.quitdims[3]))
      return 0;

    return 1;
}

/** Determines how a user has supplied a character
 *  and converts it to UTF32 and a hex string.
 */
int parse_character(char *cchar)
{
  int hexstring = 0;
  if(strstr(cchar, "0x") == cchar) {
    hexstring = 1;
  } else if(strstr(cchar, "0X") == cchar) {
    hexstring = 1;
  } else if(strstr(cchar, "U+") == cchar) {
    hexstring = 1;
  }

  if(hexstring) {
    strncpy(global.hexchar, cchar, 11);
    global.hexchar[0] = 'U';
    global.hexchar[1] = '+';
    global.character = (uint32_t)strtol(cchar + 2, NULL, 16);
  } else {
    if(convert_char(cchar, global.hexchar, 11, &global.character) == 0) {
      fprintf(stderr, "Failed to convert character encoding.\n");
      return -1;
    }
  }

  return 0;
}

/** Searches for fonts containing the desired
 *  character and returns the font set found.
 */
int generate_fontset()
{
    global.info = UnicodeNameAnnot[(global.character>>16)&0x1f][(global.character>>8)&0xff][global.character&0xff];

    FcPattern *pat = FcPatternCreate();

    if(!args.fixed) {
      FcPatternAddBool(pat, FC_SCALABLE, FcTrue);
    }

    FcCharSet *charset = FcCharSetCreate();
    FcCharSetAddChar(charset, global.character);
    FcPatternAddCharSet(pat, FC_CHARSET, charset);

    FcObjectSet *os = FcObjectSetBuild(FC_FAMILY, FC_STYLE, FC_FILE, (char *)0);

    global.fs = FcFontList(0, pat, os);

    FcObjectSetDestroy(os);
    FcPatternDestroy(pat);
    FcCharSetDestroy(charset);

    return 0;
}


int XNextEventTimed(Display *disp, XEvent *event_return, int timeout)
{
    if(XPending(disp) == 0) {
      struct pollfd pfd;
      pfd.fd = ConnectionNumber(disp);
      pfd.events = POLLIN | POLLOUT | POLLPRI;
      pfd.revents = 0;
      int nr = poll(&pfd, 1, timeout);
      if(nr > 0) {
        if(XPending(disp) > 0) {
          XNextEvent(disp, event_return);
          return 1;
        }
      }
    } else {
      XNextEvent(disp, event_return);
      return 1;
    }

    return 0;
}


int main(int argc, char *argv[])
{
    setlocale(LC_ALL, "");
    FcInit();

    int cindex = parse_arguments(argc, argv);
    if(cindex < 0) {
      fprintf(stderr, "Must supply a character value.\n");
      return 1;
    }

    parse_character(argv[cindex]);
    generate_fontset();

    if(args.display) {
      global.lastpaint = -1.0;
      global.dirty = 1;

      initialize_x11();

      int quitclicked = 0;
      int quit = 0;
      while(!quit) {
        XEvent event;
        int nr = XNextEventTimed(global.dpy, &event, TIMEOUT);
        if(nr == 0) {
          DBG("timeout\n");
          maybe_paint_window();
        } else if(nr > 0) {
          switch(event.type) {
        case Expose:
          DBG("expose\n");
          global.dirty = 1;
          maybe_paint_window();
          break;

        case ButtonPress:
          if(check_quit_bounds(event.xbutton.x, event.xbutton.y)) {
            quitclicked = 1;
          }
          break;

        case ButtonRelease:
          if(check_quit_bounds(event.xbutton.x, event.xbutton.y)) {
            quit = 1;
          } else {
            quitclicked = 0;
          }
          break;

        default:
          fprintf(stderr, "Unhandled X11 message %d. Exiting.\n", event.type);
          quit = 1;
          break;
          }
        }
      }

      close_x11();
    }

    if(args.showname) {
      if(global.info.name != NULL) {
        printf("Name: %s\n", global.info.name);
      } else {
        printf("Name lookup failed.\n");
      }
    }

    if(args.showannot) {
      if(global.info.annot != NULL) {
        printf("%s\n", global.info.annot);
      } else {
        printf("Annotation lookup failed.\n");
      }
    }

    int end = global.fs->nfont;
    if((args.maxfonts > 0) && (args.maxfonts < end))
      end = args.maxfonts;

    // Print font families
    if(args.printfonts) {
      for(int j = 0; j < end; j++)
      {
          FcChar8 *family = FcPatternFormat(global.fs->fonts[j], (FcChar8 *)"%{family[0]}");
          if(family == NULL) {
            fprintf(stderr, "Error formatting font family.\n");
            return 1;
          }
          printf("%s\n", family);
          free(family);
      }
    }

    FcFontSetDestroy(global.fs);

    FcFini();

    return 0;
}
