/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "anland_egl_backend.h"
#include "anland_backend.h"
#include "anland_logging.h"
#include "anland_output.h"

#include "core/graphicsbuffer.h" // DmaBufAttributes
#include "core/drmdevice.h"
#include "core/output.h" // OutputTransform
#include "core/renderdevice.h"
#include "opengl/eglcontext.h"
#include "opengl/egldisplay.h"
#include "opengl/eglnativefence.h"
#include "opengl/glutils.h"
#include "utils/filedescriptor.h"

#include <drm_fourcc.h>
#include <unistd.h>

#ifndef EGL_PLATFORM_SURFACELESS_MESA
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
#endif

namespace KWin
{

static uint32_t protocol_format_to_drm(uint32_t fmt)
{
    switch (fmt) {
    case 1:
        return DRM_FORMAT_ABGR8888;
    default:
        return DRM_FORMAT_XRGB8888;
    }
}

AnlandEglLayer::AnlandEglLayer(AnlandOutput *output, AnlandEglBackend *backend)
    : OutputLayer(output, OutputLayerType::Primary)
    , m_backend(backend)
    , m_output(output)
    , m_display(backend->display())
{
    connect(m_output, &BackendOutput::transformChanged, this, &AnlandEglLayer::onOutputTransformChanged);
}

AnlandEglLayer::~AnlandEglLayer()
{
    releaseBuffers();
}

void AnlandEglLayer::releaseBuffers()
{
    m_backend->openglContext()->makeCurrent();

    for (int i = 0; i < MAX_BUFS; i++) {
        m_fbos[i].reset();
        m_textures[i].reset();
        m_accumDamage[i] = Region();
}
    m_bufCount = 0;
}

bool AnlandEglLayer::importBuffers(int count)
{
    m_backend->openglContext()->makeCurrent();

    releaseBuffers();

    const OutputTransform contentTransform = m_output->transform().combine(OutputTransform::FlipY);

    for (int i = 0; i < count; i++) {
        const int fd = get_dmabuf_fd_at(m_display, i);
        buf_info info;
        if (fd < 0 || get_dmabuf_info_at(m_display, i, &info) < 0) {
            qCWarning(KWIN_ANLAND) << "failed to get dmabuf info for buffer" << i;
            releaseBuffers();
            return false;
        }

        if (i == 0) {
            const QSize bufSize(info.width, info.height);
            if (bufSize != m_output->modeSize() && bufSize.isValid()) {
                qCInfo(KWIN_ANLAND) << "dmabuf size changed, resizing output to" << bufSize;
                m_output->resize(bufSize);
            }
        }
        const QSize actual(info.width, info.height);

        DmaBufAttributes attrs;
        attrs.planeCount = 1;
        attrs.width = actual.width();
        attrs.height = actual.height();
        attrs.format = protocol_format_to_drm(info.format);
        attrs.modifier = info.modifier;
        attrs.fd[0] = FileDescriptor(dup(fd));
        attrs.offset[0] = static_cast<int>(info.offset);
        attrs.pitch[0] = static_cast<int>(info.stride);

        std::shared_ptr<GLTexture> texture = m_backend->importDmaBufAsTexture(attrs);
        if (!texture) {
            qCWarning(KWIN_ANLAND) << "failed to import dmabuf" << i << "as texture";
            releaseBuffers();
            return false;
        }

        texture->setContentTransform(contentTransform);
        auto fbo = std::make_unique<GLFramebuffer>(texture.get());
        if (!fbo->valid()) {
            qCWarning(KWIN_ANLAND) << "framebuffer for dmabuf" << i << "is not complete";
            releaseBuffers();
            return false;
        }

        qCDebug(KWIN_ANLAND) << "imported buffer" << i << "fd" << fd << actual
                             << "fmt" << Qt::hex << attrs.format << "mod" << attrs.modifier;

        m_textures[i] = std::move(texture);
        m_fbos[i] = std::move(fbo);
        m_accumDamage[i] = Region::infinite();
    }

    m_bufCount = count;
    return true;
}
void AnlandEglLayer::onOutputTransformChanged()
{
    const OutputTransform contentTransform = m_output->transform().combine(OutputTransform::FlipY);
    for (int i = 0; i < m_bufCount; i++) {
        m_textures[i]->setContentTransform(contentTransform);
        m_accumDamage[i] = Region::infinite();
    }
    addDeviceRepaint(Region::infinite());
}

std::optional<OutputLayerBeginFrameInfo> AnlandEglLayer::doBeginFrame()
{
    m_backend->openglContext()->makeCurrent();

    if (m_bufCount == 0) {
        return std::nullopt;
    }

    m_currentIndex = get_selected_idx(m_display);
    if (m_currentIndex < 0 || m_currentIndex >= m_bufCount) {
        m_currentIndex = 0;
    }

    return OutputLayerBeginFrameInfo{
        .renderTarget = RenderTarget(m_fbos[m_currentIndex].get()),
        .repaint = m_accumDamage[m_currentIndex],
    };
}

bool AnlandEglLayer::doEndFrame(const Region &renderedDeviceRegion, const Region &damagedDeviceRegion, OutputFrame *frame)
{
    Q_UNUSED(renderedDeviceRegion)
    Q_UNUSED(frame)
    glFlush();
    for (int i = 0; i < m_bufCount; i++) {
        m_accumDamage[i] = m_accumDamage[i] + damagedDeviceRegion;
    }
    if (m_currentIndex < m_bufCount) {
        m_accumDamage[m_currentIndex] = Region();
    }

    for (int i = 0; i < m_bufCount; i++) {
        if (!m_accumDamage[i].isEmpty()) {
            addDeviceRepaint(Region::infinite());
            break;
        }
    }

    EGLNativeFence fence{m_backend->eglDisplayObject()};
    set_render_fence(m_display, fence.takeFileDescriptor().take());
    return true;
}

DrmDevice *AnlandEglLayer::scanoutDevice() const
{
    return m_backend->drmDevice();
}

FormatModifierMap AnlandEglLayer::supportedDrmFormats() const
{
    return {};
}

std::shared_ptr<GLTexture> AnlandEglLayer::texture() const
{
    return m_textures[m_currentIndex];
}

AnlandEglBackend::AnlandEglBackend(AnlandBackend *b)
    : m_backend(b)
{
}

AnlandEglBackend::~AnlandEglBackend()
{
    const auto outputs = m_backend->outputs();
    for (BackendOutput *output : outputs) {
        static_cast<AnlandOutput *>(output)->setEglLayer(nullptr);
    }
    cleanup();
}

display_ctx *AnlandEglBackend::display() const
{
    return m_backend->display();
}

DrmDevice *AnlandEglBackend::drmDevice() const
{
    return m_backend->drmDevice();
}

bool AnlandEglBackend::initializeEgl()
{
    if (!initClientExtensions()) {
        return false;
    }
    if (!m_backend->renderDevice()) {
        qCWarning(KWIN_ANLAND) << "backend has no render device";
        return false;
    }
    setRenderDevice(m_backend->renderDevice());
    return true;
}
bool AnlandEglBackend::init()
{
    if (!initializeEgl()) {
        qCWarning(KWIN_ANLAND) << "Could not initialize egl";
        return false;
    }
    if (!createContext()) {
        qCWarning(KWIN_ANLAND) << "Could not initialize rendering context";
        return false;
    }

    initWayland();

    const auto outputs = m_backend->outputs();
    for (BackendOutput *output : outputs) {
        addOutput(output);
    }

    connect(m_backend, &AnlandBackend::outputAdded, this, &AnlandEglBackend::addOutput);
    return true;
}

void AnlandEglBackend::addOutput(BackendOutput *output)
{
    openglContext()->makeCurrent();
    auto *anlandOutput = static_cast<AnlandOutput *>(output);
    anlandOutput->setEglLayer(std::make_unique<AnlandEglLayer>(anlandOutput, this));
}

QList<OutputLayer *> AnlandEglBackend::compatibleOutputLayers(BackendOutput *output)
{
    return {static_cast<AnlandOutput *>(output)->eglLayer()};
}

} // namespace KWin

#include "moc_anland_egl_backend.cpp"
