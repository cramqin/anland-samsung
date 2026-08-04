/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

#include "core/outputlayer.h"
#include "opengl/eglbackend.h"

#include <array>
#include <memory>

extern "C" {
#include "display_producer.h"
#include "protocol.h"
}

namespace KWin
{
class GLFramebuffer;
class GLTexture;
class DrmDevice;
class OutputFrame;
class BackendOutput;
class SurfacePixmap;
class SurfaceTexture;
class AnlandBackend;
class AnlandEglBackend;
class AnlandOutput;

class AnlandEglLayer : public OutputLayer
{
public:
    AnlandEglLayer(AnlandOutput *output, AnlandEglBackend *backend);
    ~AnlandEglLayer() override;

    std::optional<OutputLayerBeginFrameInfo> doBeginFrame() override;
    bool doEndFrame(const Region &renderedDeviceRegion, const Region &damagedDeviceRegion, OutputFrame *frame) override;

    DrmDevice *scanoutDevice() const override;
    FormatModifierMap supportedDrmFormats() const override;

    bool importBuffers(int count);
    void releaseBuffers() override;
    std::shared_ptr<GLTexture> texture() const;

private:
    void onOutputTransformChanged();

    AnlandEglBackend *const m_backend;
    AnlandOutput *m_output;
    display_ctx *const m_display;

    int m_bufCount = 0;
    int m_currentIndex = 0;
    std::array<std::shared_ptr<GLTexture>, MAX_BUFS> m_textures;
    std::array<std::unique_ptr<GLFramebuffer>, MAX_BUFS> m_fbos;
    std::array<Region, MAX_BUFS> m_accumDamage;
};

class AnlandEglBackend : public EglBackend
{
    Q_OBJECT

public:
    AnlandEglBackend(AnlandBackend *b);
    ~AnlandEglBackend() override;

    bool init() override;
    QList<OutputLayer *> compatibleOutputLayers(BackendOutput *output) override;
    DrmDevice *drmDevice() const override;

    AnlandBackend *backend() const
    {
        return m_backend;
    }
    display_ctx *display() const;

private:
    bool initializeEgl();

    void addOutput(BackendOutput *output);

    AnlandBackend *m_backend;
};

} // namespace KWin
