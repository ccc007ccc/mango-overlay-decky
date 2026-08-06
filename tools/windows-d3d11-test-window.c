#define COBJMACROS
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Vertex {
    float position[2];
    float color[3];
} Vertex;

typedef struct FrameConstants {
    float angle;
    float padding[3];
} FrameConstants;

typedef struct SceneResources {
    ID3D11VertexShader* vertex_shader;
    ID3D11PixelShader* pixel_shader;
    ID3D11InputLayout* input_layout;
    ID3D11Buffer* vertex_buffer;
    ID3D11Buffer* constant_buffer;
} SceneResources;

static void release_scene(SceneResources* scene)
{
    if (scene->constant_buffer != NULL) {
        ID3D11Buffer_Release(scene->constant_buffer);
    }
    if (scene->vertex_buffer != NULL) {
        ID3D11Buffer_Release(scene->vertex_buffer);
    }
    if (scene->input_layout != NULL) {
        ID3D11InputLayout_Release(scene->input_layout);
    }
    if (scene->pixel_shader != NULL) {
        ID3D11PixelShader_Release(scene->pixel_shader);
    }
    if (scene->vertex_shader != NULL) {
        ID3D11VertexShader_Release(scene->vertex_shader);
    }
    memset(scene, 0, sizeof(*scene));
}

static HRESULT compile_shader(
    const char* source,
    const char* entry_point,
    const char* target,
    ID3DBlob** shader)
{
    ID3DBlob* errors = NULL;
    const HRESULT result = D3DCompile(
        source,
        strlen(source),
        NULL,
        NULL,
        NULL,
        entry_point,
        target,
        D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
        0,
        shader,
        &errors);
    if (FAILED(result) && errors != NULL) {
        fprintf(
            stderr,
            "D3D shader compilation failed: %.*s\n",
            (int)ID3D10Blob_GetBufferSize(errors),
            (const char*)ID3D10Blob_GetBufferPointer(errors));
    }
    if (errors != NULL) {
        ID3D10Blob_Release(errors);
    }
    return result;
}

static HRESULT create_scene(
    ID3D11Device* device,
    SceneResources* scene)
{
    static const char shader_source[] =
        "cbuffer Frame : register(b0) { float angle; float3 padding; };"
        "struct VertexInput { float2 position : POSITION; float3 color : COLOR; };"
        "struct PixelInput { float4 position : SV_POSITION; float3 color : COLOR; };"
        "PixelInput vertex_main(VertexInput input) {"
        "  PixelInput output;"
        "  float cosine = cos(angle);"
        "  float sine = sin(angle);"
        "  float2 position = float2("
        "    input.position.x * cosine - input.position.y * sine,"
        "    input.position.x * sine + input.position.y * cosine);"
        "  output.position = float4(position, 0.0, 1.0);"
        "  output.color = input.color;"
        "  return output;"
        "}"
        "float4 pixel_main(PixelInput input) : SV_TARGET {"
        "  return float4(input.color, 1.0);"
        "}";
    static const Vertex vertices[] = {
        { { 0.0F, 0.65F }, { 0.45F, 0.55F, 0.80F } },
        { { 0.6F, -0.45F }, { 0.35F, 0.70F, 0.65F } },
        { { -0.6F, -0.45F }, { 0.75F, 0.48F, 0.25F } },
    };
    ID3DBlob* vertex_blob = NULL;
    ID3DBlob* pixel_blob = NULL;
    HRESULT result = compile_shader(
        shader_source,
        "vertex_main",
        "vs_4_0",
        &vertex_blob);
    if (SUCCEEDED(result)) {
        result = compile_shader(
            shader_source,
            "pixel_main",
            "ps_4_0",
            &pixel_blob);
    }
    if (SUCCEEDED(result)) {
        result = ID3D11Device_CreateVertexShader(
            device,
            ID3D10Blob_GetBufferPointer(vertex_blob),
            ID3D10Blob_GetBufferSize(vertex_blob),
            NULL,
            &scene->vertex_shader);
    }
    if (SUCCEEDED(result)) {
        result = ID3D11Device_CreatePixelShader(
            device,
            ID3D10Blob_GetBufferPointer(pixel_blob),
            ID3D10Blob_GetBufferSize(pixel_blob),
            NULL,
            &scene->pixel_shader);
    }
    if (SUCCEEDED(result)) {
        const D3D11_INPUT_ELEMENT_DESC input_elements[] = {
            {
                "POSITION",
                0,
                DXGI_FORMAT_R32G32_FLOAT,
                0,
                0,
                D3D11_INPUT_PER_VERTEX_DATA,
                0,
            },
            {
                "COLOR",
                0,
                DXGI_FORMAT_R32G32B32_FLOAT,
                0,
                2U * sizeof(float),
                D3D11_INPUT_PER_VERTEX_DATA,
                0,
            },
        };
        result = ID3D11Device_CreateInputLayout(
            device,
            input_elements,
            sizeof(input_elements) / sizeof(input_elements[0]),
            ID3D10Blob_GetBufferPointer(vertex_blob),
            ID3D10Blob_GetBufferSize(vertex_blob),
            &scene->input_layout);
    }
    if (SUCCEEDED(result)) {
        const D3D11_BUFFER_DESC vertex_description = {
            .ByteWidth = sizeof(vertices),
            .Usage = D3D11_USAGE_IMMUTABLE,
            .BindFlags = D3D11_BIND_VERTEX_BUFFER,
        };
        const D3D11_SUBRESOURCE_DATA vertex_data = {
            .pSysMem = vertices,
        };
        result = ID3D11Device_CreateBuffer(
            device,
            &vertex_description,
            &vertex_data,
            &scene->vertex_buffer);
    }
    if (SUCCEEDED(result)) {
        const D3D11_BUFFER_DESC constant_description = {
            .ByteWidth = sizeof(FrameConstants),
            .Usage = D3D11_USAGE_DEFAULT,
            .BindFlags = D3D11_BIND_CONSTANT_BUFFER,
        };
        result = ID3D11Device_CreateBuffer(
            device,
            &constant_description,
            NULL,
            &scene->constant_buffer);
    }
    if (pixel_blob != NULL) {
        ID3D10Blob_Release(pixel_blob);
    }
    if (vertex_blob != NULL) {
        ID3D10Blob_Release(vertex_blob);
    }
    if (FAILED(result)) {
        release_scene(scene);
    }
    return result;
}

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
        return "Mango Overlay Proton D3D11 Test";
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

static HRESULT create_render_target(
    ID3D11Device* device,
    IDXGISwapChain* swap_chain,
    ID3D11RenderTargetView** render_target)
{
    ID3D11Texture2D* back_buffer = NULL;
    HRESULT result = IDXGISwapChain_GetBuffer(
        swap_chain,
        0,
        &IID_ID3D11Texture2D,
        (void**)&back_buffer);
    if (FAILED(result)) {
        return result;
    }
    result = ID3D11Device_CreateRenderTargetView(
        device,
        (ID3D11Resource*)back_buffer,
        NULL,
        render_target);
    ID3D11Texture2D_Release(back_buffer);
    return result;
}

int WINAPI WinMain(
    HINSTANCE instance,
    HINSTANCE previous_instance,
    LPSTR command_line,
    int show_command)
{
    (void)previous_instance;
    (void)command_line;

    const char* class_name = "MangoOverlayProtonD3D11";
    WNDCLASSA window_class = { 0 };
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

    DXGI_SWAP_CHAIN_DESC swap_chain_description = { 0 };
    swap_chain_description.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swap_chain_description.SampleDesc.Count = 1;
    swap_chain_description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swap_chain_description.BufferCount = 2;
    swap_chain_description.OutputWindow = window;
    swap_chain_description.Windowed = TRUE;
    swap_chain_description.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL requested_levels[] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };
    IDXGISwapChain* swap_chain = NULL;
    ID3D11Device* device = NULL;
    ID3D11DeviceContext* context = NULL;
    D3D_FEATURE_LEVEL selected_level = D3D_FEATURE_LEVEL_10_0;
    HRESULT result = D3D11CreateDeviceAndSwapChain(
        NULL,
        D3D_DRIVER_TYPE_HARDWARE,
        NULL,
        0,
        requested_levels,
        sizeof(requested_levels) / sizeof(requested_levels[0]),
        D3D11_SDK_VERSION,
        &swap_chain_description,
        &swap_chain,
        &device,
        &selected_level,
        &context);
    (void)selected_level;
    if (FAILED(result)) {
        DestroyWindow(window);
        return 1;
    }

    ID3D11RenderTargetView* render_target = NULL;
    result = create_render_target(device, swap_chain, &render_target);
    if (FAILED(result)) {
        ID3D11DeviceContext_Release(context);
        ID3D11Device_Release(device);
        IDXGISwapChain_Release(swap_chain);
        DestroyWindow(window);
        return 1;
    }
    SceneResources scene = { 0 };
    result = create_scene(device, &scene);
    if (FAILED(result)) {
        ID3D11RenderTargetView_Release(render_target);
        ID3D11DeviceContext_Release(context);
        ID3D11Device_Release(device);
        IDXGISwapChain_Release(swap_chain);
        DestroyWindow(window);
        return 1;
    }

    ShowWindow(window, show_command);
    UpdateWindow(window);
    int running = 1;
    unsigned int current_width = 0;
    unsigned int current_height = 0;
    unsigned int frame = 0;
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
        const unsigned int width =
            (unsigned int)(client_rectangle.right - client_rectangle.left);
        const unsigned int height =
            (unsigned int)(client_rectangle.bottom - client_rectangle.top);
        if (width == 0 || height == 0) {
            Sleep(16);
            continue;
        }
        if (width != current_width || height != current_height) {
            ID3D11DeviceContext_OMSetRenderTargets(context, 0, NULL, NULL);
            ID3D11RenderTargetView_Release(render_target);
            render_target = NULL;
            result = IDXGISwapChain_ResizeBuffers(
                swap_chain,
                0,
                width,
                height,
                DXGI_FORMAT_UNKNOWN,
                0);
            if (SUCCEEDED(result)) {
                result = create_render_target(
                    device,
                    swap_chain,
                    &render_target);
            }
            if (FAILED(result)) {
                running = 0;
                break;
            }
            current_width = width;
            current_height = height;
        }

        ID3D11DeviceContext_OMSetRenderTargets(
            context,
            1,
            &render_target,
            NULL);
        const D3D11_VIEWPORT viewport = {
            .TopLeftX = 0.0F,
            .TopLeftY = 0.0F,
            .Width = (float)width,
            .Height = (float)height,
            .MinDepth = 0.0F,
            .MaxDepth = 1.0F,
        };
        ID3D11DeviceContext_RSSetViewports(context, 1, &viewport);
        const float clear_color[4] = {
            0.035F,
            0.045F,
            0.075F,
            1.0F,
        };
        ID3D11DeviceContext_ClearRenderTargetView(
            context,
            render_target,
            clear_color);
        const FrameConstants constants = {
            .angle = (float)(frame % 720U) * 0.0122173048F,
        };
        ID3D11DeviceContext_UpdateSubresource(
            context,
            (ID3D11Resource*)scene.constant_buffer,
            0,
            NULL,
            &constants,
            0,
            0);
        const UINT stride = sizeof(Vertex);
        const UINT offset = 0;
        ID3D11DeviceContext_IASetInputLayout(context, scene.input_layout);
        ID3D11DeviceContext_IASetVertexBuffers(
            context,
            0,
            1,
            &scene.vertex_buffer,
            &stride,
            &offset);
        ID3D11DeviceContext_IASetPrimitiveTopology(
            context,
            D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11DeviceContext_VSSetShader(
            context,
            scene.vertex_shader,
            NULL,
            0);
        ID3D11DeviceContext_VSSetConstantBuffers(
            context,
            0,
            1,
            &scene.constant_buffer);
        ID3D11DeviceContext_PSSetShader(
            context,
            scene.pixel_shader,
            NULL,
            0);
        ID3D11DeviceContext_Draw(context, 3, 0);
        result = IDXGISwapChain_Present(swap_chain, 1, 0);
        if (FAILED(result)) {
            running = 0;
        }
        ++frame;
    }

    if (render_target != NULL) {
        ID3D11RenderTargetView_Release(render_target);
    }
    release_scene(&scene);
    ID3D11DeviceContext_Release(context);
    ID3D11Device_Release(device);
    IDXGISwapChain_Release(swap_chain);
    if (IsWindow(window)) {
        DestroyWindow(window);
    }
    UnregisterClassA(class_name, instance);
    return result == S_OK ? 0 : 1;
}
