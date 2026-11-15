#include "WindowCapture.h"

#include <cstring>

#ifndef NOMINMAX
 #define NOMINMAX
#endif
#include <windows.h>

#ifndef PW_RENDERFULLCONTENT
 #define PW_RENDERFULLCONTENT 0x00000002
#endif

namespace
{
// ほとんど真っ黒なら撮れなかったとみなす（GPU で描くものの一部は PrintWindow で黒くなる）
bool looksBlank (const juce::Image& image)
{
    const juce::Image::BitmapData data (image, juce::Image::BitmapData::readOnly);
    for (int y = 0; y < data.height; y += juce::jmax (1, data.height / 24))
        for (int x = 0; x < data.width; x += juce::jmax (1, data.width / 24))
            if (const auto c = data.getPixelColour (x, y); c.getRed() > 12 || c.getGreen() > 12 || c.getBlue() > 12)
                return false;
    return true;
}
} // namespace

namespace WindowCapture
{
juce::Image captureClientArea (juce::Component& topLevelWindow)
{
    auto* peer = topLevelWindow.getPeer();
    if (peer == nullptr)
        return {};

    const auto hwnd = (HWND) peer->getNativeHandle();
    if (hwnd == nullptr || IsIconic (hwnd) || ! IsWindowVisible (hwnd))
        return {};

    RECT client {};
    if (! GetClientRect (hwnd, &client))
        return {};

    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    if (width <= 0 || height <= 0 || width > 8192 || height > 8192)
        return {};

    HDC screenDc = GetDC (nullptr);
    HDC memoryDc = CreateCompatibleDC (screenDc);

    BITMAPINFO info {};
    info.bmiHeader.biSize = sizeof (BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height; // 上から下へ
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection (memoryDc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    juce::Image image;

    if (bitmap != nullptr && bits != nullptr)
    {
        const auto previous = SelectObject (memoryDc, bitmap);

        if (PrintWindow (hwnd, memoryDc, PW_CLIENTONLY | PW_RENDERFULLCONTENT))
        {
            GdiFlush();
            // DIB の並び（B, G, R, 未使用）は JUCE の ARGB の画像（Windows ではメモリ上 B, G, R, A）と同じなので、行ごとに写して
            // 不透明にする
            image = juce::Image (juce::Image::ARGB, width, height, false, juce::SoftwareImageType());
            const juce::Image::BitmapData data (image, juce::Image::BitmapData::writeOnly);
            const auto* source = static_cast<const juce::uint8*> (bits);

            for (int y = 0; y < height; ++y)
            {
                auto* line = data.getLinePointer (y);
                std::memcpy (line, source + (size_t) y * (size_t) width * 4, (size_t) width * 4);
                for (int x = 0; x < width; ++x)
                    line[x * data.pixelStride + 3] = 0xff;
            }
        }

        SelectObject (memoryDc, previous);
    }

    if (bitmap != nullptr)
        DeleteObject (bitmap);
    DeleteDC (memoryDc);
    ReleaseDC (nullptr, screenDc);

    if (image.isValid() && looksBlank (image))
        return {};
    return image;
}
} // namespace WindowCapture
