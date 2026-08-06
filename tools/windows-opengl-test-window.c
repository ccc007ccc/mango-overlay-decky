#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <GL/gl.h>

#include <stdlib.h>

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

static const char* window_title(void)
{
    const char* title = getenv("MANGO_OVERLAY_WINDOWS_TEST_TITLE");
    if (title == NULL || *title == '\0') {
        return "Mango Overlay Proton OpenGL Test";
    }
    return title;
}

static LRESULT CALLBACK window_procedure(
    HWND window,
    UINT message,
    WPARAM wparam,
    LPARAM lparam)
{
    (void)wparam;
    (void)lparam;
    if (message == WM_CLOSE) {
        DestroyWindow(window);
        return 0;
    }
    if (message == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(window, message, wparam, lparam);
}

int WINAPI WinMain(
    HINSTANCE instance,
    HINSTANCE previous_instance,
    LPSTR command_line,
    int show_command)
{
    (void)previous_instance;
    (void)command_line;

    const char* class_name = "MangoOverlayProtonOpenGL";
    WNDCLASSA window_class = { 0 };
    window_class.style = CS_OWNDC;
    window_class.lpfnWndProc = window_procedure;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorA(NULL, IDC_ARROW);
    window_class.lpszClassName = class_name;
    if (RegisterClassA(&window_class) == 0) {
        return 1;
    }

    RECT rectangle = {
        .left = 0,
        .top = 0,
        .right = environment_dimension("MANGO_OVERLAY_DESKTOP_WIDTH", 960),
        .bottom = environment_dimension("MANGO_OVERLAY_DESKTOP_HEIGHT", 600),
    };
    AdjustWindowRect(&rectangle, WS_OVERLAPPEDWINDOW, FALSE);
    HWND window = CreateWindowExA(
        0,
        class_name,
        window_title(),
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        80,
        80,
        rectangle.right - rectangle.left,
        rectangle.bottom - rectangle.top,
        NULL,
        NULL,
        instance,
        NULL);
    if (window == NULL) {
        return 1;
    }

    HDC device_context = GetDC(window);
    PIXELFORMATDESCRIPTOR pixel_format = {
        .nSize = sizeof(pixel_format),
        .nVersion = 1,
        .dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER,
        .iPixelType = PFD_TYPE_RGBA,
        .cColorBits = 24,
        .cDepthBits = 24,
        .iLayerType = PFD_MAIN_PLANE,
    };
    const int format = ChoosePixelFormat(device_context, &pixel_format);
    if (format == 0 || !SetPixelFormat(device_context, format, &pixel_format)) {
        ReleaseDC(window, device_context);
        DestroyWindow(window);
        return 1;
    }

    HGLRC rendering_context = wglCreateContext(device_context);
    if (rendering_context == NULL
        || !wglMakeCurrent(device_context, rendering_context)) {
        if (rendering_context != NULL) {
            wglDeleteContext(rendering_context);
        }
        ReleaseDC(window, device_context);
        DestroyWindow(window);
        return 1;
    }

    ShowWindow(window, show_command);
    UpdateWindow(window);
    int running = 1;
    float angle = 0.0F;
    while (running) {
        MSG message;
        while (PeekMessageA(&message, NULL, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                running = 0;
                break;
            }
            TranslateMessage(&message);
            DispatchMessageA(&message);
        }
        if (!running) {
            break;
        }

        RECT client_rectangle;
        GetClientRect(window, &client_rectangle);
        const int width = client_rectangle.right - client_rectangle.left;
        const int height = client_rectangle.bottom - client_rectangle.top;
        if (width > 0 && height > 0) {
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
            SwapBuffers(device_context);
        }

        angle += 0.7F;
        if (angle >= 360.0F) {
            angle -= 360.0F;
        }
        Sleep(16);
    }

    wglMakeCurrent(NULL, NULL);
    wglDeleteContext(rendering_context);
    ReleaseDC(window, device_context);
    if (IsWindow(window)) {
        DestroyWindow(window);
    }
    UnregisterClassA(class_name, instance);
    return 0;
}
