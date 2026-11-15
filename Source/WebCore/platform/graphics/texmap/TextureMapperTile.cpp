/*
 Copyright (C) 2013 Nokia Corporation and/or its subsidiary(-ies)

 This library is free software; you can redistribute it and/or
 modify it under the terms of the GNU Library General Public
 License as published by the Free Software Foundation; either
 version 2 of the License, or (at your option) any later version.

 This library is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 Library General Public License for more details.

 You should have received a copy of the GNU Library General Public License
 along with this library; see the file COPYING.LIB.  If not, write to
 the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 Boston, MA 02110-1301, USA.
 */
#include "config.h"
#include "TextureMapperTile.h"

#include "BitmapTexture.h"
#include "NativeImage.h"
#include "TextureMapper.h"
#include "TextureMapperGLHeaders.h"
#include <wtf/TZoneMallocInlines.h>

namespace WebCore {

WTF_MAKE_TZONE_ALLOCATED_IMPL(TextureMapperTile);

class GraphicsLayer;

TextureMapperTile::TextureMapperTile(const FloatRect& rect)
    : m_rect(rect)
{
}

TextureMapperTile::~TextureMapperTile() = default;

RefPtr<BitmapTexture> TextureMapperTile::texture() const
{
    return m_texture;
}

void TextureMapperTile::setTexture(BitmapTexture* texture)
{
    m_texture = texture;
}

void TextureMapperTile::updateContents(NativeImage& image, const IntRect& dirtyRect)
{
    IntRect targetRect = enclosingIntRect(m_rect);
    targetRect.intersect(dirtyRect);
    if (targetRect.isEmpty())
        return;
    IntPoint sourceOffset = targetRect.location();

    // Normalize sourceRect to the buffer's coordinates.
    sourceOffset.move(-dirtyRect.x(), -dirtyRect.y());

    // Normalize targetRect to the texture's coordinates.
    targetRect.move(-m_rect.x(), -m_rect.y());
    if (!m_texture) {
        OptionSet<BitmapTexture::Flags> flags;
        if (image.hasAlpha())
            flags.add(BitmapTexture::Flags::SupportsAlpha);
        m_texture = BitmapTexture::create(targetRect.size(), flags);
    }

    m_texture->updateContents(&image, targetRect, sourceOffset);
}

static void clearTexture(BitmapTexture& texture)
{
    GLint boundFramebuffer = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &boundFramebuffer);
    GLboolean scissorEnabled = glIsEnabled(GL_SCISSOR_TEST);
    GLfloat clearColor[4];
    glGetFloatv(GL_COLOR_CLEAR_VALUE, clearColor);

    GLuint framebuffer = 0;
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture.id(), 0);
    if (scissorEnabled)
        glDisable(GL_SCISSOR_TEST);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);

    glClearColor(clearColor[0], clearColor[1], clearColor[2], clearColor[3]);
    if (scissorEnabled)
        glEnable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, boundFramebuffer);
    glDeleteFramebuffers(1, &framebuffer);
}

bool TextureMapperTile::updateContentsFromTexture(unsigned sourceTextureID, const IntRect& dirtyRect)
{
    IntRect tileRect = enclosingIntRect(m_rect);
    IntRect targetRect = intersection(tileRect, dirtyRect);
    if (targetRect.isEmpty())
        return true;
    IntPoint sourceOffset = targetRect.location();
    sourceOffset.move(-dirtyRect.x(), -dirtyRect.y());
    targetRect.move(-m_rect.x(), -m_rect.y());
    // The texture covers the whole tile, so updates of any part of it fit. A new texture's memory is undefined (it can
    // hold another texture's old pixels), so one the update doesn't cover entirely starts transparent.
    if (!m_texture || m_texture->size() != tileRect.size()) {
        m_texture = BitmapTexture::create(tileRect.size(), BitmapTexture::Flags::SupportsAlpha);
        if (targetRect != IntRect { { }, tileRect.size() })
            clearTexture(*m_texture);
    }
    // copyFromExternalTexture's release assertion bounds the source offset by this texture's size; never reach it.
    if (sourceOffset.x() + targetRect.width() > m_texture->size().width() || sourceOffset.y() + targetRect.height() > m_texture->size().height())
        return false;
    m_texture->copyFromExternalTexture(sourceTextureID, targetRect, toIntSize(sourceOffset));
    return true;
}

void TextureMapperTile::updateContents(GraphicsLayer* sourceLayer, const IntRect& dirtyRect, float scale)
{
    IntRect targetRect = enclosingIntRect(m_rect);
    targetRect.intersect(dirtyRect);
    if (targetRect.isEmpty())
        return;
    IntPoint sourceOffset = targetRect.location();

    // Normalize targetRect to the texture's coordinates.
    targetRect.move(-m_rect.x(), -m_rect.y());

    if (!m_texture)
        m_texture = BitmapTexture::create(targetRect.size(), { BitmapTexture::Flags::SupportsAlpha });

    m_texture->updateContents(sourceLayer, targetRect, sourceOffset, scale);
}

void TextureMapperTile::paint(TextureMapper& textureMapper, const TransformationMatrix& transform, float opacity, bool allEdgesExposed)
{
    if (texture().get())
        textureMapper.drawTexture(*texture().get(), rect(), transform, opacity, allEdgesExposed ? TextureMapper::AllEdgesExposed::Yes : TextureMapper::AllEdgesExposed::No);
}

} // namespace WebCore
