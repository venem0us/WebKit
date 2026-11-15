/*
 * Copyright (C) 2024 Igalia S.L.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "FontRenderOptions.h"

#if USE(SKIA)

#if OS(WINDOWS)
#include <cstdlib>
#include <cstring>
#include <windows.h>
#endif

namespace WebCore {

#if OS(WINDOWS)
// Glass: text as Windows draws it. Nothing told Skia about Windows' settings, so text was always drawn with grayscale
// smoothing, softer than in Chrome or Edge, which use ClearType (subpixel smoothing) where Windows does. Follow
// Windows: ClearType with the screen's subpixel order, grayscale when ClearType is off, none when font smoothing is
// off. Subpixel text needs an opaque background; where there isn't one it is drawn grayscale (see
// FontCascade::drawGlyphs). GLASS_FONT_SMOOTHING=cleartype|grayscale|none overrides Windows' setting (Glass's
// Settings > Accessibility > Text smoothing sets it).
FontRenderOptions::FontRenderOptions()
{
    BOOL smoothing = TRUE;
    UINT type = FE_FONTSMOOTHINGCLEARTYPE;
    UINT orientation = FE_FONTSMOOTHINGORIENTATIONRGB;
    SystemParametersInfoW(SPI_GETFONTSMOOTHING, 0, &smoothing, 0);
    SystemParametersInfoW(SPI_GETFONTSMOOTHINGTYPE, 0, &type, 0);
    SystemParametersInfoW(SPI_GETFONTSMOOTHINGORIENTATION, 0, &orientation, 0);
    auto antialias = !smoothing ? Antialias::None : type == FE_FONTSMOOTHINGCLEARTYPE ? Antialias::Subpixel : Antialias::Normal;
    if (const char* value = std::getenv("GLASS_FONT_SMOOTHING"); value && *value) {
        if (!std::strcmp(value, "cleartype"))
            antialias = Antialias::Subpixel;
        else if (!std::strcmp(value, "grayscale"))
            antialias = Antialias::Normal;
        else if (!std::strcmp(value, "none"))
            antialias = Antialias::None;
    }
    setAntialias(antialias);
    setSubpixelOrder(orientation == FE_FONTSMOOTHINGORIENTATIONBGR ? SubpixelOrder::HorizontalBGR : SubpixelOrder::HorizontalRGB);
    setHinting(Hinting::Slight);   // as before
    setFollowSystemSettings(true);
}
#else
FontRenderOptions::FontRenderOptions() = default;
#endif

void FontRenderOptions::setHinting(std::optional<Hinting> hinting)
{
    switch (hinting.value_or(Hinting::Medium)) {
    case Hinting::None:
        m_hinting = SkFontHinting::kNone;
        break;
    case Hinting::Slight:
        m_hinting = SkFontHinting::kSlight;
        break;
    case Hinting::Medium:
        m_hinting = SkFontHinting::kNormal;
        break;
    case Hinting::Full:
        m_hinting = SkFontHinting::kFull;
        break;
    }
}

SkFontHinting FontRenderOptions::hinting() const
{
    if (m_isHintingDisabledForTesting)
        return SkFontHinting::kNone;

    if (m_followSystemSettings)
        return m_hinting;

    return SkFontHinting::kSlight;
}

void FontRenderOptions::setAntialias(std::optional<Antialias> antialias)
{
    switch (antialias.value_or(Antialias::Normal)) {
    case Antialias::None:
        m_antialias = SkFont::Edging::kAlias;
        break;
    case Antialias::Normal:
        m_antialias = SkFont::Edging::kAntiAlias;
        break;
    case Antialias::Subpixel:
        m_antialias = SkFont::Edging::kSubpixelAntiAlias;
        break;
    }
}

SkFont::Edging FontRenderOptions::antialias() const
{
    if (m_followSystemSettings)
        return m_antialias;

    return SkFont::Edging::kAntiAlias;
}

void FontRenderOptions::setSubpixelOrder(std::optional<SubpixelOrder> subpixelOrder)
{
    switch (subpixelOrder.value_or(SubpixelOrder::Unknown)) {
    case SubpixelOrder::Unknown:
        m_subpixelOrder = kUnknown_SkPixelGeometry;
        break;
    case SubpixelOrder::HorizontalRGB:
        m_subpixelOrder = kRGB_H_SkPixelGeometry;
        break;
    case SubpixelOrder::HorizontalBGR:
        m_subpixelOrder = kBGR_H_SkPixelGeometry;
        break;
    case SubpixelOrder::VerticalRGB:
        m_subpixelOrder = kRGB_V_SkPixelGeometry;
        break;
    case SubpixelOrder::VerticalBGR:
        m_subpixelOrder = kBGR_V_SkPixelGeometry;
        break;
    }
}

SkPixelGeometry FontRenderOptions::subpixelOrder() const
{
    if (m_followSystemSettings)
        return m_subpixelOrder;

    return kUnknown_SkPixelGeometry;
}

bool FontRenderOptions::useSubpixelPositioning() const
{
    // Subpixel positioning is not a system setting, it's set when device scale factor is >= 2.
    // So, we only apply it when not following system settings to force subpixel position with
    // linear metrics and no hinting.
    if (!m_followSystemSettings)
        return m_useSubpixelPositioning;

    return false;
}

void FontRenderOptions::disableHintingForTesting()
{
    m_isHintingDisabledForTesting = true;
}

} // namespace WebCore

#endif // USE(SKIA)
