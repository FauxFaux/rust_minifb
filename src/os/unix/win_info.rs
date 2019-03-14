use std::mem;

use super::x11_dl::xlib;


const KEY_FUNCTION: u8 = 0xFF;
const KEY_ESC: u8 = 0x1B;
const Button6: u8 = 6;
const Button7: u8 = 7;

// window_handler.rs
const WINDOW_BORDERLESS: u32 = 1 << 1;
const WINDOW_RESIZE: u32 = 1 << 2;
const WINDOW_TITLE: u32 = 1 << 3;

#[derive(Default)]
struct Globals {
    window_count: usize,
    display: *const xlib::Display,
    screen: usize,
    gc: *const xlib::GC,
    depth: u32,
    setup_done: bool,
    visual: *const xlib::Visual,
    screen_width: usize,
    screen_height: usize,
    keyb_ext: i32,
    context: xlib::XContext,
    wm_delete_window: xlib::Atom,
}

#[derive(Default)]
struct SharedData {
    width: u32,
    height: u32,
    scale: f32,
    mouse_x: f32,
    mouse_y: f32,
    scroll_x: f32,
    scroll_y: f32,
    state: [u8; 3],
}

#[derive(Default)]
struct WindowInfo {
    //    void (*key_callback)(void* user_data, int key, int state);
    //    void (*char_callback)(void* user_data, unsigned int c);
    //    void* rust_data;
    shared_data: SharedData,
    window: xlib::Window,
    ximage: *mut xlib::XImage,
    draw_buffer: Box<[u32]>,
    scale: u32,
    width: u32,
    height: u32,
    update: bool,
    prev_cursor: i32,
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

fn setup_display(s: &mut Globals) -> Result<(), &'static str> {
    let major = 1;
    let minor = 0;
    let majorOpcode = 0;
    let eventBase = 0;
    let errorBase = 0;

    let depth;
    let i;
    let formatCount;
    let mut convDepth = -1;
    let formats: *const xlib::XPixmapFormatValues;

    if s_setup_done {
        return Ok(());
    }

    s_display = unsafe { XOpenDisplay(0) };

    if !s_display {
        return Err("Unable to open X11 display");
    }

    s.context = unsafe { xlib::XUniqueContext() };
    s.screen = DefaultScreen(s_display);
    s.visual = DefaultVisual(s_display, s_screen);
    formats = XListPixmapFormats(s_display, &formatCount);
    depth = DefaultDepth(s_display, s_screen);

    for i in 0..formatCount {
        if depth == formats[i].depth {
            convDepth = formats[i].bits_per_pixel;
            break;
        }
    }

    xlib::XFree(formats);

    // We only support 32-bit right now
    if convDepth != 32 {
        xlib::XCloseDisplay(s_display);
        return Err("Unable to find 32-bit format for X11 display");
    }

    s_depth = depth;

    s_gc = DefaultGC(s_display, s_screen);

    s_screen_width = DisplayWidth(s_display, s_screen);
    s_screen_height = DisplayHeight(s_display, s_screen);

    let wmDeleteWindowName = "WM_DELETE_WINDOW";
    xlib::XInternAtoms(
        s_display,
        /* ptr weirdness */ wmDeleteWindowName,
        1,
        false,
        &s_wm_delete_window,
    );

    s_setup_done = 1;

    init_cursors();

    s_keyb_ext = XkbQueryExtension(
        s_display,
        &majorOpcode,
        &eventBase,
        &errorBase,
        &major,
        &minor,
    );

    return Ok(());
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

fn mfb_open(
    title: &str,
    width: u32,
    height: u32,
    flags: u32,
    scale: u32,
) -> Result<some_ptr, &'static str> {
    let /*XSizeHints */ mut sizeHints;
    let /*XImage* */ mut image;
    let /*Window */ window;
    let /*WindowInfo* */ window_info;

    let mut s = Globals::default();

    setup_display(&mut s)?;

    //TODO: Handle no title/borderless

    width *= scale;
    height *= scale;

    let default_root_window = xlib::DefaultRootWindow(s_display);

    let mut window_attributes: xlib::XSetWindowAttributes = unsafe { mem::zeroed() };
    window_attributes.border_pixel = BlackPixel(s_display, s_screen);
    window_attributes.background_pixel = BlackPixel(s_display, s_screen);
    window_attributes.backing_store = xlib::NotUseful;

    window = XCreateWindow(
        s_display,
        default_root_window,
        (s_screen_width - width) / 2,
        (s_screen_height - height) / 2,
        width,
        height,
        0,
        s_depth,
        xlib::InputOutput,
        s_visual,
        xlib::CWBackPixel | xlib::CWBorderPixel | xlib::CWBackingStore,
        &window_attributes,
    );
    if (!window) {
        printf("Unable to create X11 Window\n");
        return 0;
    }

    //XSelectInput(s_display, s_window, KeyPressMask | KeyReleaseMask);
    XStoreName(s_display, window, title);

    XSelectInput(
        s_display,
        window,
        xlib::StructureNotifyMask
            | xlib::ButtonPressMask
            | xlib::KeyPressMask
            | xlib::KeyReleaseMask
            | xlib::ButtonReleaseMask,
    );

    if (!(flags & WINDOW_RESIZE)) {
        sizeHints.flags = xlib::PPosition | xlib::PMinSize | xlib::PMaxSize;
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

    image = XCreateImage(
        s_display,
        xlib::CopyFromParent,
        s_depth,
        xlib::ZPixmap,
        0,
        NULL,
        width,
        height,
        32,
        width * 4,
    );

    if (!image) {
        XDestroyWindow(s_display, window);
        printf("Unable to create XImage\n");
        return 0;
    }

    window_info = WindowInfo {
        shared_data: SharedData {
            width: 0,
            height: 0,
            scale: 0.0,
            mouse_x: 0.0,
            mouse_y: 0.0,
            scroll_x: 0.0,
            scroll_y: 0.0,
            state: [],
        },
        window,
        ximage,
        draw_buffer: vec![0u32; cast::usize(width * height)?].into_boxed_slice(),
        scale,
        width,
        height,
        update: true,
        prev_cursor: 0,
    };

    XSetWMProtocols(s_display, window, &s_wm_delete_window, 1);

    XSaveContext(
        s_display,
        window,
        s_context,
        &window_info as *const xlib::XPointer,
    );

    image.data = &window_info.draw_buffer;

    s_window_count += 1;

    return window_info;
}
