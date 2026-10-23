/**
 * @file
 * @brief Векторные значки меню.
 *
 * Строит значки средствами ImDrawList. Координаты преобразуются к размеру canvas; эти значки не
 * требуют отдельного файла текстуры в процессе игры.
 */

#include "sidebar_icons.h"
#include "sidebar_icon_resources.h"

#include <algorithm>
#include <cstdint>
#include <vector>

#include <Windows.h>
#include <d3d11.h>
#include <wincodec.h>

namespace sidebar_icons {
namespace {

ID3D11ShaderResourceView* g_armlet_texture = nullptr;

struct canvas {
    ImVec2 origin;
    float scale;

    ImVec2 point(float x, float y) const {
        return ImVec2(origin.x + x * scale, origin.y + y * scale);
    }
};

canvas make_canvas(ImVec2 minimum, ImVec2 maximum) {
    const float width = maximum.x - minimum.x;
    const float height = maximum.y - minimum.y;
    const float scale = std::min(width, height) / 64.0f;
    return {
        ImVec2(minimum.x + (width - 64.0f * scale) * 0.5f,
            minimum.y + (height - 64.0f * scale) * 0.5f),
        scale,
    };
}

void stroke_armlet(ImDrawList* draw, const canvas& c, ImU32 color) {
    const float thickness = std::max(1.35f, 3.2f * c.scale);
    draw->PathLineTo(c.point(18, 21));
    draw->PathBezierCubicCurveTo(c.point(22, 16), c.point(27, 14), c.point(32, 14));
    draw->PathBezierCubicCurveTo(c.point(37, 14), c.point(42, 16), c.point(46, 21));
    draw->PathLineTo(c.point(52, 29));
    draw->PathBezierCubicCurveTo(c.point(54, 32), c.point(54, 36), c.point(52, 39));
    draw->PathLineTo(c.point(46, 47));
    draw->PathBezierCubicCurveTo(c.point(42, 51), c.point(37, 53), c.point(32, 53));
    draw->PathBezierCubicCurveTo(c.point(27, 53), c.point(22, 51), c.point(18, 47));
    draw->PathLineTo(c.point(12, 39));
    draw->PathBezierCubicCurveTo(c.point(10, 36), c.point(10, 32), c.point(12, 29));
    draw->PathLineTo(c.point(18, 21));
    draw->PathStroke(color, ImDrawFlags_Closed, thickness);

    draw->PathLineTo(c.point(18, 25));
    draw->PathBezierCubicCurveTo(c.point(22, 21), c.point(27, 19), c.point(32, 19));
    draw->PathBezierCubicCurveTo(c.point(37, 19), c.point(42, 21), c.point(46, 25));
    draw->PathStroke(color, ImDrawFlags_None, thickness);
    draw->PathLineTo(c.point(18, 43));
    draw->PathBezierCubicCurveTo(c.point(22, 47), c.point(27, 49), c.point(32, 49));
    draw->PathBezierCubicCurveTo(c.point(37, 49), c.point(42, 47), c.point(46, 43));
    draw->PathStroke(color, ImDrawFlags_None, thickness);

    const auto path = [&](ImVec2 a, ImVec2 b, ImVec2 d) {
        draw->PathLineTo(a); draw->PathLineTo(b); draw->PathLineTo(d);
        draw->PathStroke(color, ImDrawFlags_None, thickness);
    };
    path(c.point(12, 31), c.point(20, 33), c.point(16, 39));
    path(c.point(52, 31), c.point(44, 33), c.point(48, 39));
    draw->AddCircle(c.point(32, 34), 7.0f * c.scale, color, 24, thickness);
    draw->AddLine(c.point(32, 27), c.point(32, 22), color, thickness);
    draw->AddLine(c.point(32, 46), c.point(32, 41), color, thickness);
    draw->AddLine(c.point(25, 34), c.point(20, 34), color, thickness);
    draw->AddLine(c.point(44, 34), c.point(39, 34), color, thickness);
}

void stroke_render(ImDrawList* draw, const canvas& c, ImU32 color) {
    const float thickness = std::max(1.35f, 3.2f * c.scale);
    draw->AddRect(c.point(7, 9), c.point(57, 47), color, 6.0f * c.scale,
        ImDrawFlags_None, thickness);
    draw->AddLine(c.point(25, 53), c.point(39, 53), color, thickness);
    draw->AddLine(c.point(28, 47), c.point(28, 53), color, thickness);
    draw->AddLine(c.point(36, 47), c.point(36, 53), color, thickness);
    draw->AddLine(c.point(22, 56), c.point(42, 56), color, thickness);

    draw->PathLineTo(c.point(32, 18));
    draw->PathLineTo(c.point(40, 23));
    draw->PathLineTo(c.point(40, 33));
    draw->PathLineTo(c.point(32, 38));
    draw->PathLineTo(c.point(24, 33));
    draw->PathLineTo(c.point(24, 23));
    draw->PathStroke(color, ImDrawFlags_Closed, thickness);
    draw->PathLineTo(c.point(24, 23));
    draw->PathLineTo(c.point(32, 28));
    draw->PathLineTo(c.point(40, 23));
    draw->PathStroke(color, ImDrawFlags_None, thickness);
    draw->AddLine(c.point(32, 28), c.point(32, 38), color, thickness);
    draw->AddLine(c.point(43, 35), c.point(49, 35), color, thickness);
    draw->AddLine(c.point(46, 32), c.point(46, 38), color, thickness);
}

void stroke_diagnostics(ImDrawList* draw, const canvas& c, ImU32 color) {
    const float thickness = std::max(1.5f, 4.0f * c.scale);
    draw->AddRect(c.point(8, 13), c.point(56, 51), color, 7.0f * c.scale,
        ImDrawFlags_None, thickness);
    draw->PathLineTo(c.point(14, 33));
    draw->PathLineTo(c.point(21, 33));
    draw->PathLineTo(c.point(26, 24));
    draw->PathLineTo(c.point(32, 42));
    draw->PathLineTo(c.point(38, 28));
    draw->PathLineTo(c.point(42, 33));
    draw->PathLineTo(c.point(50, 33));
    draw->PathStroke(color, ImDrawFlags_None, thickness);
    draw->AddCircleFilled(c.point(49, 20), 2.5f * c.scale, color, 16);
}

void stroke_comfort(ImDrawList* draw, const canvas& c, ImU32 color) {
    const float thickness = std::max(1.35f, 3.2f * c.scale);
    draw->PathLineTo(c.point(32, 8));
    draw->PathLineTo(c.point(52, 16));
    draw->PathLineTo(c.point(50, 35));
    draw->PathBezierCubicCurveTo(c.point(48, 46), c.point(39, 53), c.point(32, 57));
    draw->PathBezierCubicCurveTo(c.point(25, 53), c.point(16, 46), c.point(14, 35));
    draw->PathLineTo(c.point(12, 16));
    draw->PathStroke(color, ImDrawFlags_Closed, thickness);
    draw->AddLine(c.point(23, 32), c.point(29, 38), color, thickness);
    draw->AddLine(c.point(29, 38), c.point(42, 25), color, thickness);
}

} // namespace

bool initialize(void* module_handle, ID3D11Device* device) {
    shutdown();
    if (module_handle == nullptr || device == nullptr)
        return false;

    const HMODULE module = static_cast<HMODULE>(module_handle);
    const HRSRC resource = FindResourceW(module,
        MAKEINTRESOURCEW(IDR_ARMLET_MENU_ICON), RT_RCDATA);
    if (resource == nullptr)
        return false;
    const HGLOBAL loaded_resource = LoadResource(module, resource);
    const DWORD encoded_size = SizeofResource(module, resource);
    auto* encoded_data = static_cast<BYTE*>(LockResource(loaded_resource));
    if (loaded_resource == nullptr || encoded_data == nullptr || encoded_size == 0)
        return false;

    const HRESULT com_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitialize_com = SUCCEEDED(com_result);
    if (FAILED(com_result) && com_result != RPC_E_CHANGED_MODE)
        return false;

    IWICImagingFactory* factory = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;
    ID3D11Texture2D* texture = nullptr;

    HRESULT result = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (SUCCEEDED(result))
        result = factory->CreateStream(&stream);
    if (SUCCEEDED(result))
        result = stream->InitializeFromMemory(encoded_data, encoded_size);
    if (SUCCEEDED(result))
        result = factory->CreateDecoderFromStream(stream, nullptr,
            WICDecodeMetadataCacheOnLoad, &decoder);
    if (SUCCEEDED(result))
        result = decoder->GetFrame(0, &frame);
    if (SUCCEEDED(result))
        result = factory->CreateFormatConverter(&converter);
    if (SUCCEEDED(result))
        result = converter->Initialize(frame, GUID_WICPixelFormat32bppRGBA,
            WICBitmapDitherTypeNone, nullptr, 0.0,
            WICBitmapPaletteTypeCustom);

    UINT width = 0;
    UINT height = 0;
    if (SUCCEEDED(result))
        result = converter->GetSize(&width, &height);

    std::vector<std::uint8_t> pixels;
    if (SUCCEEDED(result) && width != 0 && height != 0 &&
        width <= 512 && height <= 512) {
        pixels.resize(static_cast<std::size_t>(width) * height * 4);
        result = converter->CopyPixels(nullptr, width * 4,
            static_cast<UINT>(pixels.size()), pixels.data());
    } else if (SUCCEEDED(result)) {
        result = E_FAIL;
    }

    if (SUCCEEDED(result)) {
        D3D11_TEXTURE2D_DESC description{};
        description.Width = width;
        description.Height = height;
        description.MipLevels = 1;
        description.ArraySize = 1;
        description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.Usage = D3D11_USAGE_IMMUTABLE;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA initial_data{};
        initial_data.pSysMem = pixels.data();
        initial_data.SysMemPitch = width * 4;
        result = device->CreateTexture2D(
            &description, &initial_data, &texture);
    }
    if (SUCCEEDED(result))
        result = device->CreateShaderResourceView(
            texture, nullptr, &g_armlet_texture);

    if (texture != nullptr)
        texture->Release();
    if (converter != nullptr)
        converter->Release();
    if (frame != nullptr)
        frame->Release();
    if (decoder != nullptr)
        decoder->Release();
    if (stream != nullptr)
        stream->Release();
    if (factory != nullptr)
        factory->Release();
    if (uninitialize_com)
        CoUninitialize();

    return SUCCEEDED(result) && g_armlet_texture != nullptr;
}

void shutdown() {
    if (g_armlet_texture != nullptr) {
        g_armlet_texture->Release();
        g_armlet_texture = nullptr;
    }
}

void draw(ImDrawList* draw_list, icon value, ImVec2 minimum, ImVec2 maximum,
    ImU32 color) {
    if (draw_list == nullptr)
        return;
    if (value == icon::armlet && g_armlet_texture != nullptr) {
        draw_list->AddImage(
            reinterpret_cast<ImTextureID>(g_armlet_texture), minimum, maximum);
        return;
    }
    const canvas c = make_canvas(minimum, maximum);
    switch (value) {
    case icon::armlet: stroke_armlet(draw_list, c, color); break;
    case icon::render: stroke_render(draw_list, c, color); break;
    case icon::comfort: stroke_comfort(draw_list, c, color); break;
    case icon::diagnostics: stroke_diagnostics(draw_list, c, color); break;
    }
}

} // namespace sidebar_icons
