#define _POSIX_C_SOURCE 200809L

#include <GL/gl.h>
#include <GL/glx.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static int environment_dimension(const char* name, int fallback)
{
    const char* text = getenv(name);
    if (text == NULL || *text == '\0') {
        return fallback;
    }
    char* end = NULL;
    const long value = strtol(text, &end, 10);
    if (*end != '\0' || value < 64 || value > 4096) {
        return fallback;
    }
    return (int)value;
}

int main(int argc, char** argv)
{
    const char* title = argc > 1 ? argv[1] : "Mango Overlay GLX Test";
    Display* display = XOpenDisplay(NULL);
    if (display == NULL) {
        fprintf(stderr, "Could not open the X11 display.\n");
        return 69;
    }

    const int visual_attributes[] = {
        GLX_RGBA,
        GLX_DOUBLEBUFFER,
        GLX_RED_SIZE,
        8,
        GLX_GREEN_SIZE,
        8,
        GLX_BLUE_SIZE,
        8,
        None,
    };
    XVisualInfo* visual = glXChooseVisual(
        display,
        DefaultScreen(display),
        (int*)visual_attributes);
    if (visual == NULL) {
        fprintf(stderr, "Could not choose a double-buffered GLX visual.\n");
        XCloseDisplay(display);
        return 69;
    }

    XSetWindowAttributes attributes = { 0 };
    attributes.colormap = XCreateColormap(
        display,
        RootWindow(display, visual->screen),
        visual->visual,
        AllocNone);
    attributes.event_mask = KeyPressMask | StructureNotifyMask;

    int width = environment_dimension("MANGO_OVERLAY_DESKTOP_WIDTH", 960);
    int height = environment_dimension("MANGO_OVERLAY_DESKTOP_HEIGHT", 600);
    Window window = XCreateWindow(
        display,
        RootWindow(display, visual->screen),
        80,
        80,
        (unsigned int)width,
        (unsigned int)height,
        0,
        visual->depth,
        InputOutput,
        visual->visual,
        CWColormap | CWEventMask,
        &attributes);
    XStoreName(display, window, title);
    const Atom pid_atom = XInternAtom(display, "_NET_WM_PID", False);
    const unsigned long process_id = (unsigned long)getpid();
    XChangeProperty(
        display,
        window,
        pid_atom,
        XA_CARDINAL,
        32,
        PropModeReplace,
        (const unsigned char*)&process_id,
        1);
    Atom wm_delete = XInternAtom(display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(display, window, &wm_delete, 1);
    XMapWindow(display, window);

    GLXContext context = glXCreateContext(display, visual, NULL, True);
    if (context == NULL || !glXMakeCurrent(display, window, context)) {
        fprintf(stderr, "Could not create or activate the GLX context.\n");
        if (context != NULL) {
            glXDestroyContext(display, context);
        }
        XDestroyWindow(display, window);
        XFreeColormap(display, attributes.colormap);
        XFree(visual);
        XCloseDisplay(display);
        return 69;
    }

    int running = 1;
    float angle = 0.0F;
    const struct timespec frame_delay = { .tv_sec = 0, .tv_nsec = 16000000L };
    while (running) {
        while (XPending(display) > 0) {
            XEvent event;
            XNextEvent(display, &event);
            if (event.type == ConfigureNotify) {
                width = event.xconfigure.width;
                height = event.xconfigure.height;
            } else if (event.type == KeyPress
                && XLookupKeysym(&event.xkey, 0) == XK_Escape) {
                running = 0;
            } else if (event.type == ClientMessage
                && (Atom)event.xclient.data.l[0] == wm_delete) {
                running = 0;
            }
        }

        glViewport(0, 0, width, height);
        glClearColor(0.035F, 0.045F, 0.075F, 1.0F);
        glClear(GL_COLOR_BUFFER_BIT);
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glOrtho(-1.0, 1.0, -1.0, 1.0, -1.0, 1.0);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        glRotatef(angle, 0.0F, 0.0F, 1.0F);
        glBegin(GL_TRIANGLES);
        glColor3f(0.45F, 0.55F, 0.80F);
        glVertex2f(0.0F, 0.65F);
        glColor3f(0.75F, 0.48F, 0.25F);
        glVertex2f(-0.6F, -0.45F);
        glColor3f(0.35F, 0.70F, 0.65F);
        glVertex2f(0.6F, -0.45F);
        glEnd();
        glXSwapBuffers(display, window);

        angle += 0.7F;
        if (angle >= 360.0F) {
            angle -= 360.0F;
        }
        nanosleep(&frame_delay, NULL);
    }

    glXMakeCurrent(display, None, NULL);
    glXDestroyContext(display, context);
    XDestroyWindow(display, window);
    XFreeColormap(display, attributes.colormap);
    XFree(visual);
    XCloseDisplay(display);
    return 0;
}
