#define _POSIX_C_SOURCE 200809L

#include <EGL/egl.h>
#include <GL/gl.h>
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

static int egl_success(EGLBoolean result, const char* action)
{
    if (result == EGL_TRUE) {
        return 1;
    }
    fprintf(stderr, "%s failed with EGL error 0x%04x.\n", action, eglGetError());
    return 0;
}

int main(int argc, char** argv)
{
    const char* title = argc > 1 ? argv[1] : "Mango Overlay EGL Test";
    Display* x_display = XOpenDisplay(NULL);
    if (x_display == NULL) {
        fprintf(stderr, "Could not open the X11 display.\n");
        return 69;
    }

    const int width = environment_dimension("MANGO_OVERLAY_DESKTOP_WIDTH", 960);
    const int height = environment_dimension("MANGO_OVERLAY_DESKTOP_HEIGHT", 600);
    Window window = XCreateSimpleWindow(
        x_display,
        RootWindow(x_display, DefaultScreen(x_display)),
        80,
        80,
        (unsigned int)width,
        (unsigned int)height,
        0,
        BlackPixel(x_display, DefaultScreen(x_display)),
        BlackPixel(x_display, DefaultScreen(x_display)));
    XSelectInput(x_display, window, KeyPressMask | StructureNotifyMask);
    XStoreName(x_display, window, title);
    const Atom pid_atom = XInternAtom(x_display, "_NET_WM_PID", False);
    const unsigned long process_id = (unsigned long)getpid();
    XChangeProperty(
        x_display,
        window,
        pid_atom,
        XA_CARDINAL,
        32,
        PropModeReplace,
        (const unsigned char*)&process_id,
        1);
    Atom wm_delete = XInternAtom(x_display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(x_display, window, &wm_delete, 1);
    XMapWindow(x_display, window);
    XFlush(x_display);

    EGLDisplay egl_display = eglGetDisplay((EGLNativeDisplayType)x_display);
    if (egl_display == EGL_NO_DISPLAY) {
        fprintf(stderr, "eglGetDisplay failed with EGL error 0x%04x.\n", eglGetError());
        XDestroyWindow(x_display, window);
        XCloseDisplay(x_display);
        return 69;
    }
    EGLint major = 0;
    EGLint minor = 0;
    if (!egl_success(
            eglInitialize(egl_display, &major, &minor),
            "eglInitialize")
        || !egl_success(eglBindAPI(EGL_OPENGL_API), "eglBindAPI")) {
        eglTerminate(egl_display);
        XDestroyWindow(x_display, window);
        XCloseDisplay(x_display);
        return 69;
    }
    printf("Using EGL %d.%d with desktop OpenGL.\n", major, minor);

    const EGLint config_attributes[] = {
        EGL_SURFACE_TYPE,
        EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE,
        EGL_OPENGL_BIT,
        EGL_RED_SIZE,
        8,
        EGL_GREEN_SIZE,
        8,
        EGL_BLUE_SIZE,
        8,
        EGL_NONE,
    };
    EGLConfig config = NULL;
    EGLint config_count = 0;
    if (!egl_success(
            eglChooseConfig(
                egl_display,
                config_attributes,
                &config,
                1,
                &config_count),
            "eglChooseConfig")
        || config_count == 0) {
        fprintf(stderr, "No EGL desktop OpenGL window config is available.\n");
        eglTerminate(egl_display);
        XDestroyWindow(x_display, window);
        XCloseDisplay(x_display);
        return 69;
    }

    EGLSurface surface = eglCreateWindowSurface(
        egl_display,
        config,
        (EGLNativeWindowType)window,
        NULL);
    EGLContext context = eglCreateContext(
        egl_display,
        config,
        EGL_NO_CONTEXT,
        NULL);
    if (surface == EGL_NO_SURFACE || context == EGL_NO_CONTEXT
        || !egl_success(
            eglMakeCurrent(egl_display, surface, surface, context),
            "eglMakeCurrent")) {
        fprintf(stderr, "Could not create the EGL surface/context.\n");
        if (context != EGL_NO_CONTEXT) {
            eglDestroyContext(egl_display, context);
        }
        if (surface != EGL_NO_SURFACE) {
            eglDestroySurface(egl_display, surface);
        }
        eglTerminate(egl_display);
        XDestroyWindow(x_display, window);
        XCloseDisplay(x_display);
        return 69;
    }
    eglSwapInterval(egl_display, 1);

    int current_width = width;
    int current_height = height;
    int running = 1;
    float angle = 0.0F;
    const struct timespec frame_delay = { .tv_sec = 0, .tv_nsec = 1000000L };
    while (running) {
        while (XPending(x_display) > 0) {
            XEvent event;
            XNextEvent(x_display, &event);
            if (event.type == ConfigureNotify) {
                current_width = event.xconfigure.width;
                current_height = event.xconfigure.height;
            } else if (event.type == KeyPress
                && XLookupKeysym(&event.xkey, 0) == XK_Escape) {
                running = 0;
            } else if (event.type == ClientMessage
                && (Atom)event.xclient.data.l[0] == wm_delete) {
                running = 0;
            }
        }

        glViewport(0, 0, current_width, current_height);
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
        if (!egl_success(eglSwapBuffers(egl_display, surface), "eglSwapBuffers")) {
            running = 0;
        }

        angle += 0.7F;
        if (angle >= 360.0F) {
            angle -= 360.0F;
        }
        nanosleep(&frame_delay, NULL);
    }

    eglMakeCurrent(
        egl_display,
        EGL_NO_SURFACE,
        EGL_NO_SURFACE,
        EGL_NO_CONTEXT);
    eglDestroyContext(egl_display, context);
    eglDestroySurface(egl_display, surface);
    eglTerminate(egl_display);
    XDestroyWindow(x_display, window);
    XCloseDisplay(x_display);
    return 0;
}
