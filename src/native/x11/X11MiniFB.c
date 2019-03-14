#include <X11/Xresource.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xcursor/Xcursor.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#define KEY_FUNCTION 0xFF
#define KEY_ESC 0x1B
#define Button6 6
#define Button7 7

// window_handler.rs
const uint32_t WINDOW_BORDERLESS = 1 << 1;
const uint32_t WINDOW_RESIZE = 1 << 2;
const uint32_t WINDOW_TITLE = 1 << 3;

void mfb_close(void* window_info);

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static int s_window_count = 0;
static Display* s_display;
static int s_screen;
static GC s_gc;
static int s_depth;
static int s_setup_done = 0;
static Visual* s_visual;
static int s_screen_width;
static int s_screen_height;
static int s_keyb_ext = 0;
static XContext s_context;
static Atom s_wm_delete_window;

// Needs to match lib.rs enum
enum CursorStyle {
    CursorStyle_Arrow,
    CursorStyle_Ibeam,
    CursorStyle_Crosshair,
    CursorStyle_ClosedHand,
    CursorStyle_OpenHand,
    CursorStyle_ResizeLeftRight,
    CursorStyle_ResizeUpDown,
    CursorStyle_SizeAll,
    CursorStyle_Count,
};

static Cursor s_cursors[CursorStyle_Count];

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

typedef struct SharedData {
    uint32_t width;
    uint32_t height;
    float scale;
    float mouse_x;
    float mouse_y;
    float scroll_x;
    float scroll_y;
    uint8_t state[3];
} SharedData;

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

typedef struct WindowInfo {
    void (*key_callback)(void* user_data, int key, int state);
    void (*char_callback)(void* user_data, unsigned int c);
    void* rust_data;
    SharedData* shared_data;
    Window window;
    XImage* ximage;
    void* draw_buffer;
    int scale;
    int width;
    int height;
    int update;
    int prev_cursor;
} WindowInfo;

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void init_cursors() {
    s_cursors[CursorStyle_Arrow] = XcursorLibraryLoadCursor(s_display, "arrow");
    s_cursors[CursorStyle_Ibeam] = XcursorLibraryLoadCursor(s_display, "xterm");
    s_cursors[CursorStyle_Crosshair] = XcursorLibraryLoadCursor(s_display, "crosshair");
    s_cursors[CursorStyle_ClosedHand] = XcursorLibraryLoadCursor(s_display, "hand2");
    s_cursors[CursorStyle_OpenHand] = XcursorLibraryLoadCursor(s_display, "hand2");
    s_cursors[CursorStyle_ResizeLeftRight] = XcursorLibraryLoadCursor(s_display, "sb_h_double_arrow");
    s_cursors[CursorStyle_ResizeUpDown] = XcursorLibraryLoadCursor(s_display, "sb_v_double_arrow");
    s_cursors[CursorStyle_SizeAll] = XcursorLibraryLoadCursor(s_display, "diamond_cross");
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static int setup_display() {
    int major = 1;
    int minor = 0;
	int majorOpcode = 0;
	int eventBase = 0;
	int errorBase = 0;

    int depth, i, formatCount, convDepth = -1;
    XPixmapFormatValues* formats;

    if (s_setup_done) {
        return 1;
    }

    s_display = XOpenDisplay(0);

    if (!s_display) {
        printf("Unable to open X11 display\n");
        return 0;
    }

    s_context = XUniqueContext();
    s_screen = DefaultScreen(s_display);
    s_visual = DefaultVisual(s_display, s_screen);
    formats = XListPixmapFormats(s_display, &formatCount);
    depth = DefaultDepth(s_display, s_screen);

    for (i = 0; i < formatCount; ++i) {
        if (depth == formats[i].depth) {
            convDepth = formats[i].bits_per_pixel;
            break;
        }
    }

    XFree(formats);

    // We only support 32-bit right now
    if (convDepth != 32) {
        printf("Unable to find 32-bit format for X11 display\n");
        XCloseDisplay(s_display);
        return 0;
    }

    s_depth = depth;

    s_gc = DefaultGC(s_display, s_screen);

    s_screen_width = DisplayWidth(s_display, s_screen);
    s_screen_height = DisplayHeight(s_display, s_screen);

    const char* wmDeleteWindowName = "WM_DELETE_WINDOW";
    XInternAtoms(s_display, (char**)&wmDeleteWindowName, 1, False, &s_wm_delete_window);

    s_setup_done = 1;

    init_cursors();

	s_keyb_ext = XkbQueryExtension(s_display, &majorOpcode, &eventBase, &errorBase, &major, &minor);

    return 1;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void* mfb_open(const char* title, int width, int height, unsigned int flags, int scale)
{
    XSetWindowAttributes windowAttributes;
    XSizeHints sizeHints;
    XImage* image;
    Window window;
    WindowInfo* window_info;


    if (!setup_display()) {
        return 0;
    }

    //TODO: Handle no title/borderless
    (void)flags;

    width *= scale;
    height *= scale;

    Window defaultRootWindow = DefaultRootWindow(s_display);

    windowAttributes.border_pixel = BlackPixel(s_display, s_screen);
    windowAttributes.background_pixel = BlackPixel(s_display, s_screen);
    windowAttributes.backing_store = NotUseful;

    window = XCreateWindow(s_display, defaultRootWindow, (s_screen_width - width) / 2,
                    (s_screen_height - height) / 2, width, height, 0, s_depth, InputOutput,
                    s_visual, CWBackPixel | CWBorderPixel | CWBackingStore,
                    &windowAttributes);
    if (!window) {
        printf("Unable to create X11 Window\n");
        return 0;
    }

    //XSelectInput(s_display, s_window, KeyPressMask | KeyReleaseMask);
    XStoreName(s_display, window, title);

    XSelectInput(s_display, window,
        StructureNotifyMask |
        ButtonPressMask | KeyPressMask | KeyReleaseMask | ButtonReleaseMask);

    if (!(flags & WINDOW_RESIZE)) {
        sizeHints.flags = PPosition | PMinSize | PMaxSize;
        sizeHints.x = 0;
        sizeHints.y = 0;
        sizeHints.min_width = width;
        sizeHints.max_width = width;
        sizeHints.min_height = height;
        sizeHints.max_height = height;
        XSetWMNormalHints(s_display, window, &sizeHints);
    }

    XClearWindow(s_display, window);
    XMapRaised(s_display, window);
    XFlush(s_display);

    image = XCreateImage(s_display, CopyFromParent, s_depth, ZPixmap, 0, NULL, width, height, 32, width * 4);

    if (!image) {
        XDestroyWindow(s_display, window);
        printf("Unable to create XImage\n");
        return 0;
    }

    window_info = (WindowInfo*)malloc(sizeof(WindowInfo));
    window_info->key_callback = 0;
    window_info->char_callback = 0;
    window_info->rust_data = 0;
    window_info->window = window;
    window_info->ximage = image;
    window_info->scale = scale;
    window_info->width = width;
    window_info->height = height;
    window_info->draw_buffer = malloc(width * height * 4);
    window_info->update = 1;

    XSetWMProtocols(s_display, window, &s_wm_delete_window, 1);

    XSaveContext(s_display, window, s_context, (XPointer) window_info);

    image->data = (char*)window_info->draw_buffer;

    s_window_count += 1;

    return (void*)window_info;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void mfb_set_title(void* window_info, const char* title)
{
    WindowInfo* info = (WindowInfo*)window_info;
    XStoreName(s_display, info->window, title);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static WindowInfo* find_handle(Window handle)
{
    WindowInfo* info;

    if (XFindContext(s_display, handle, s_context, (XPointer*) &info) != 0) {
        return 0;
    }

    return info;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void mfb_set_cursor_style(void* window_info, int cursor)
{
    WindowInfo* info = (WindowInfo*)window_info;

	if (info->prev_cursor == cursor)
		return;

	if (cursor < 0 || cursor >= CursorStyle_Count) {
		printf("cursor out of range %d\n", cursor);
		return;
	}

    XDefineCursor(s_display, info->window, s_cursors[cursor]);

	info->prev_cursor = cursor;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static int handle_special_keys(WindowInfo* info, XEvent* event, int down) {
	int keySym;

	if (!s_keyb_ext)
		return 0;

	keySym = XkbKeycodeToKeysym(s_display, event->xkey.keycode, 0, 1);

	switch (keySym)
	{
		case XK_KP_0:
		case XK_KP_1:
		case XK_KP_2:
		case XK_KP_3:
		case XK_KP_4:
		case XK_KP_5:
		case XK_KP_6:
		case XK_KP_7:
		case XK_KP_8:
		case XK_KP_9:
		case XK_KP_Separator:
		case XK_KP_Decimal:
		case XK_KP_Equal:
		case XK_KP_Enter:
		{
			if (info->key_callback) {
				info->key_callback(info->rust_data, keySym, down);
				return 1;
			}
		}
	}

	return 0;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static int process_event(XEvent* event) {
    KeySym sym;

    WindowInfo* info = find_handle(event->xany.window);

    if (!info)
        return 1;

    if (event->type == ClientMessage) {
        if ((Atom)event->xclient.data.l[0] == s_wm_delete_window) {
            info->update = 0;
            mfb_close(info);

            return 0;
        }
    }

    switch (event->type)
    {
        case KeyPress:
        {
            sym = XLookupKeysym(&event->xkey, 0);

			if (handle_special_keys(info, event, 1))
				break;

            if (info->key_callback)
                info->key_callback(info->rust_data, sym, 1);

            if (info->char_callback) {
                info->char_callback(info->rust_data, sym);
            }

            break;
        }

        case KeyRelease:
        {
			if (handle_special_keys(info, event, 0))
				break;

            sym = XLookupKeysym(&event->xkey, 0);

            if (info->key_callback)
                info->key_callback(info->rust_data, sym, 0);
            break;
        }

        case ButtonPress:
        {
            if (!info->shared_data)
                break;

            if (event->xbutton.button == Button1)
                info->shared_data->state[0] = 1;
            else if (event->xbutton.button == Button2)
                info->shared_data->state[1] = 1;
            else if (event->xbutton.button == Button3)
                info->shared_data->state[2] = 1;
            else if (event->xbutton.button == Button4)
                info->shared_data->scroll_y = 10.0f;
            else if (event->xbutton.button == Button5)
                info->shared_data->scroll_y = -10.0f;
            else if (event->xbutton.button == Button6)
                info->shared_data->scroll_x = 10.0f;
            else if (event->xbutton.button == Button7)
                info->shared_data->scroll_y = -10.0f;

            break;
        }

        case ButtonRelease:
        {
            if (!info->shared_data)
                break;

            if (event->xbutton.button == Button1)
                info->shared_data->state[0] = 0;
            else if (event->xbutton.button == Button2)
                info->shared_data->state[1] = 0;
            else if (event->xbutton.button == Button3)
                info->shared_data->state[2] = 0;

            break;
        }

        case ConfigureNotify:
        {
            info->width = event->xconfigure.width;
            info->height = event->xconfigure.height;
            break;
        }
    }

    return 1;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void get_mouse_pos(WindowInfo* info) {
    Window root, child;
    int rootX, rootY, childX, childY;
    unsigned int mask;

    XQueryPointer(s_display, info->window,
                    &root, &child,
                    &rootX, &rootY, &childX, &childY,
                    &mask);

    if (info->shared_data) {
        info->shared_data->mouse_x = (float)childX;
        info->shared_data->mouse_y = (float)childY;
    }
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static int process_events()
{
    int count;
    XEvent event;
    KeySym sym;

    count = XPending(s_display);

    while (count--)
    {
        XEvent event;
        XNextEvent(s_display, &event);

        // Don't process any more messages if event is 0
        if (process_event(&event) == 0)
            return 0;
    }

    return 0;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void scale_2x(unsigned int* dest, unsigned int* source, int width, int height, int scale) {
    int x, y;
    for (y = 0; y < height; y += scale) {
        for (x = 0; x < width; x += scale) {
            const unsigned int t = *source++;
            dest[0] = t;
            dest[1] = t;
            dest[width + 0] = t;
            dest[width + 1] = t;
            dest += scale;
        }

        dest += width * (scale - 1);
    }
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void scale_4x(unsigned int* dest, unsigned int* source, int width, int height, int scale) {
    int x, y;
    for (y = 0; y < height; y += scale) {
        for (x = 0; x < width; x += scale) {
            const unsigned int t = *source++;
            dest[(width * 0) + 0] = t;
            dest[(width * 0) + 1] = t;
            dest[(width * 0) + 2] = t;
            dest[(width * 0) + 3] = t;
            dest[(width * 1) + 0] = t;
            dest[(width * 1) + 1] = t;
            dest[(width * 1) + 2] = t;
            dest[(width * 1) + 3] = t;
            dest[(width * 2) + 0] = t;
            dest[(width * 2) + 1] = t;
            dest[(width * 2) + 2] = t;
            dest[(width * 2) + 3] = t;
            dest[(width * 3) + 0] = t;
            dest[(width * 3) + 1] = t;
            dest[(width * 3) + 2] = t;
            dest[(width * 3) + 3] = t;
            dest += scale;
        }

        dest += width * (scale - 1);
    }
}


#define write_8(offset) \
    dest[(width * offset) + 0] = t; \
    dest[(width * offset) + 1] = t; \
    dest[(width * offset) + 2] = t; \
    dest[(width * offset) + 3] = t; \
    dest[(width * offset) + 4] = t; \
    dest[(width * offset) + 5] = t; \
    dest[(width * offset) + 6] = t; \
    dest[(width * offset) + 7] = t;

#define write_16(offset) \
    dest[(width * offset) + 0] = t; \
    dest[(width * offset) + 1] = t; \
    dest[(width * offset) + 2] = t; \
    dest[(width * offset) + 3] = t; \
    dest[(width * offset) + 4] = t; \
    dest[(width * offset) + 5] = t; \
    dest[(width * offset) + 6] = t; \
    dest[(width * offset) + 7] = t; \
    dest[(width * offset) + 8] = t; \
    dest[(width * offset) + 9] = t; \
    dest[(width * offset) + 10] = t; \
    dest[(width * offset) + 11] = t; \
    dest[(width * offset) + 12] = t; \
    dest[(width * offset) + 13] = t; \
    dest[(width * offset) + 14] = t; \
    dest[(width * offset) + 15] = t;

#define write_32(offset) \
    dest[(width * offset) + 0] = t; \
    dest[(width * offset) + 1] = t; \
    dest[(width * offset) + 2] = t; \
    dest[(width * offset) + 3] = t; \
    dest[(width * offset) + 4] = t; \
    dest[(width * offset) + 5] = t; \
    dest[(width * offset) + 6] = t; \
    dest[(width * offset) + 7] = t; \
    dest[(width * offset) + 8] = t; \
    dest[(width * offset) + 9] = t; \
    dest[(width * offset) + 10] = t; \
    dest[(width * offset) + 11] = t; \
    dest[(width * offset) + 12] = t; \
    dest[(width * offset) + 13] = t; \
    dest[(width * offset) + 14] = t; \
    dest[(width * offset) + 15] = t; \
    dest[(width * offset) + 16] = t; \
    dest[(width * offset) + 17] = t; \
    dest[(width * offset) + 18] = t; \
    dest[(width * offset) + 19] = t; \
    dest[(width * offset) + 20] = t; \
    dest[(width * offset) + 21] = t; \
    dest[(width * offset) + 22] = t; \
    dest[(width * offset) + 23] = t; \
    dest[(width * offset) + 24] = t; \
    dest[(width * offset) + 25] = t; \
    dest[(width * offset) + 26] = t; \
    dest[(width * offset) + 27] = t; \
    dest[(width * offset) + 28] = t; \
    dest[(width * offset) + 29] = t; \
    dest[(width * offset) + 30] = t; \
    dest[(width * offset) + 31] = t;

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void scale_8x(unsigned int* dest, unsigned int* source, int width, int height, int scale) {
    int x, y;
    scale = 8;
    for (y = 0; y < height; y += 8) {
        for (x = 0; x < width; x += 8) {
            const unsigned int t = *source++;

            write_8(0);
            write_8(1);
            write_8(2);
            write_8(3);
            write_8(4);
            write_8(5);
            write_8(6);
            write_8(7);

            dest += scale;
        }

        dest += width * (scale - 1);
    }
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void scale_16x(unsigned int* dest, unsigned int* source, int width, int height, int scale) {
    int x, y;
    scale = 16;
    for (y = 0; y < height; y += scale) {
        for (x = 0; x < width; x += scale) {
            const unsigned int t = *source++;

            write_16(0);
            write_16(1);
            write_16(2);
            write_16(3);
            write_16(4);
            write_16(5);
            write_16(6);
            write_16(7);
            write_16(8);
            write_16(9);
            write_16(10);
            write_16(11);
            write_16(12);
            write_16(13);
            write_16(14);
            write_16(15);

            dest += scale;
        }

        dest += width * (scale - 1);
    }
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void scale_32x(unsigned int* dest, unsigned int* source, int width, int height, int scale) {
    int x, y;

    for (y = 0; y < scale; y += scale) {
        for (x = 0; x < width; x += scale) {
            const unsigned int t = *source++;

            write_32(0);
            write_32(1);
            write_32(2);
            write_32(3);
            write_32(4);
            write_32(5);
            write_32(6);
            write_32(7);
            write_32(8);
            write_32(9);
            write_32(10);
            write_32(11);
            write_32(12);
            write_32(13);
            write_32(14);
            write_32(15);

            write_32(16);
            write_32(17);
            write_32(18);
            write_32(19);
            write_32(20);
            write_32(21);
            write_32(22);
            write_32(23);
            write_32(24);
            write_32(25);
            write_32(26);
            write_32(27);
            write_32(28);
            write_32(29);
            write_32(30);
            write_32(31);

            dest += scale;
        }

        dest += width * (scale - 1);
    }
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void mfb_update_with_buffer(void* window_info, void* buffer)
{
    WindowInfo* info = (WindowInfo*)window_info;
    int width = info->width;
    int height = info->height;
    int scale = info->scale;

    if (info->update && buffer) {
        switch (scale) {
            case 1: {
                memcpy(info->draw_buffer, buffer, width * height * 4);
                break;
            }
            case 2: {
                scale_2x(info->draw_buffer, buffer, width, height, scale);
                break;
            }

            case 4: {
                scale_4x(info->draw_buffer, buffer, width, height, scale);
                break;
            }

            case 8: {
                scale_8x(info->draw_buffer, buffer, width, height, scale);
                break;
            }

            case 16: {
                scale_16x(info->draw_buffer, buffer, width, height, scale);
                break;
            }

            case 32: {
                scale_32x(info->draw_buffer, buffer, width, height, scale);
                break;
            }
        }

        XPutImage(s_display, info->window, s_gc, info->ximage, 0, 0, 0, 0, width, height);
        XFlush(s_display);
    }

    // clear before processing new events

    if (info->shared_data) {
        info->shared_data->scroll_x = 0.0f;
        info->shared_data->scroll_y = 0.0f;
    }

    get_mouse_pos(info);
    process_events();
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void mfb_set_position(void* window, int x, int y)
{
    WindowInfo* info = (WindowInfo*)window;
    XMoveWindow(s_display, info->window, x, y);
    XFlush(s_display);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void mfb_close(void* window_info)
{
    WindowInfo* info = (WindowInfo*)window_info;

    if (!info->draw_buffer)
        return;

    XSaveContext(s_display, info->window, s_context, (XPointer)0);

    free(info->draw_buffer);

    info->ximage->data = NULL;
    info->draw_buffer = 0;

    XDestroyImage(info->ximage);
    XDestroyWindow(s_display, info->window);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void mfb_set_key_callback(void* window, void* rust_data,
						  void (*key_callback)(void* user_data, int key, int state),
						  void (*char_callback)(void* user_data, uint32_t key))

{
    WindowInfo* win = (WindowInfo*)window;
    win->key_callback = key_callback;
    win->char_callback = char_callback;
    win->rust_data = rust_data;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void mfb_set_shared_data(void* window, SharedData* data)
{
    WindowInfo* win = (WindowInfo*)window;
    win->shared_data = data;
    win->shared_data->width = win->width;
    win->shared_data->height = win->height;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

int mfb_should_close(void* window) {
    WindowInfo* win = (WindowInfo*)window;
    return !!win->update;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

unsigned int mfb_get_screen_size() {
    setup_display();
    return (s_screen_width << 16) | s_screen_height;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void* mfb_get_window_handle(void* window) {
    WindowInfo* win = (WindowInfo*)window;
    return (void*)(uintptr_t)win->window;
}
