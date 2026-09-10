#include "videobuffer.h"
#include "avframeconvert.h"
#include <QMutexLocker>
extern "C"
{
#include "libavformat/avformat.h"
#include "libavutil/avutil.h"
#include "libavutil/frame.h"
#include "libavutil/imgutils.h"
}

VideoBuffer::VideoBuffer(QObject *parent) : QObject(parent) {
    connect(&m_fpsCounter, &FpsCounter::updateFPS, this, &VideoBuffer::updateFPS);
}

VideoBuffer::~VideoBuffer() {}

bool VideoBuffer::init()
{
    m_decodingFrame = av_frame_alloc();
    if (!m_decodingFrame) {
        goto error;
    }

    m_renderingframe = av_frame_alloc();
    if (!m_renderingframe) {
        goto error;
    }

    // there is initially no rendering frame, so consider it has already been
    // consumed
    m_renderingFrameConsumed = true;

    m_fpsCounter.start();
    return true;

error:
    deInit();
    return false;
}

void VideoBuffer::deInit()
{
    if (m_decodingFrame) {
        av_frame_free(&m_decodingFrame);
        m_decodingFrame = Q_NULLPTR;
    }
    if (m_renderingframe) {
        av_frame_free(&m_renderingframe);
        m_renderingframe = Q_NULLPTR;
    }
    m_fpsCounter.stop();
}

void VideoBuffer::lock()
{
    m_mutex.lock();
}

void VideoBuffer::unLock()
{
    m_mutex.unlock();
}

void VideoBuffer::setRenderExpiredFrames(bool renderExpiredFrames)
{
    m_renderExpiredFrames = renderExpiredFrames;
}

AVFrame *VideoBuffer::decodingFrame()
{
    return m_decodingFrame;
}

void VideoBuffer::offerDecodedFrame(bool &previousFrameSkipped)
{
    m_mutex.lock();

    if (m_renderExpiredFrames) {
        // if m_renderExpiredFrames is enable, then the decoder must wait for the current
        // frame to be consumed
        while (!m_renderingFrameConsumed && !m_interrupted) {
            m_renderingFrameConsumedCond.wait(&m_mutex);
        }
    } else {
        if (m_fpsCounter.isStarted() && !m_renderingFrameConsumed) {
            m_fpsCounter.addSkippedFrame();
        }
    }

    swap();
    previousFrameSkipped = !m_renderingFrameConsumed;
    m_renderingFrameConsumed = false;
    m_mutex.unlock();
}

const AVFrame *VideoBuffer::consumeRenderedFrame()
{
    Q_ASSERT(!m_renderingFrameConsumed);
    m_renderingFrameConsumed = true;
    if (m_fpsCounter.isStarted()) {
        m_fpsCounter.addRenderedFrame();
    }
    if (m_renderExpiredFrames) {
        // if m_renderExpiredFrames is enable, then notify the decoder the current frame is
        // consumed, so that it may push a new one
        m_renderingFrameConsumedCond.wakeOne();
    }
    return m_renderingframe;
}

bool VideoBuffer::takeRenderedFrame(AVFrame *dst)
{
    QMutexLocker locker(&m_mutex);
    if (m_renderingFrameConsumed) {
        // Nothing new: a previous take already consumed it (a queued newFrame
        // can still arrive after the frame it announced was consumed).
        return false;
    }
    // Consume first so the decoder thread is always woken in renderExpiredFrames
    // mode, even if the frame turns out to be unusable or nobody wants it.
    const AVFrame *src = consumeRenderedFrame();
    if (!dst) {
        return false;
    }
    av_frame_unref(dst);
    if (!src || src->width <= 0 || src->height <= 0 || !src->data[0]) {
        return false;
    }
    if (av_frame_ref(dst, src) < 0) {
        av_frame_unref(dst);
        return false;
    }
    return true;
}

void VideoBuffer::peekRenderedFrame(std::function<void(int width, int height, uint8_t* dataRGB32)> onFrame)
{
    if (!onFrame) {
        return;
    }

    QMutexLocker locker(&m_mutex);
    auto frame = m_renderingframe;
    if (!frame || frame->width <= 0 || frame->height <= 0 || !frame->data[0]) {
        return;
    }
    int width = frame->width;
    int height = frame->height;

    // create buffer
    const int bufferSize = av_image_get_buffer_size(AV_PIX_FMT_RGB32, width, height, 4);
    if (bufferSize <= 0) {
        return;
    }
    uint8_t* rgbBuffer = new uint8_t[static_cast<size_t>(bufferSize)];
    AVFrame *rgbFrame = av_frame_alloc();
    if (!rgbFrame) {
        delete [] rgbBuffer;
        return;
    }

    // bind buffer to AVFrame
    av_image_fill_arrays(rgbFrame->data, rgbFrame->linesize, rgbBuffer, AV_PIX_FMT_RGB32, width, height, 4);

    // convert
    AVFrameConvert convert;
    convert.setSrcFrameInfo(width, height, AV_PIX_FMT_YUV420P);
    convert.setDstFrameInfo(width, height, AV_PIX_FMT_RGB32);
    bool ret = false;
    ret = convert.init();
    if (!ret) {
        delete [] rgbBuffer;
        av_frame_free(&rgbFrame);
        return;
    }
    ret = convert.convert(frame, rgbFrame);
    if (!ret) {
        delete [] rgbBuffer;
        av_frame_free(&rgbFrame);
        return;
    }
    convert.deInit();
    av_frame_free(&rgbFrame);
    locker.unlock();

    onFrame(width, height, rgbBuffer);
    delete [] rgbBuffer;
}

void VideoBuffer::interrupt()
{
    QMutexLocker locker(&m_mutex);
    m_interrupted = true;
    m_renderingFrameConsumedCond.wakeAll();
}

void VideoBuffer::swap()
{
    AVFrame *tmp = m_decodingFrame;
    m_decodingFrame = m_renderingframe;
    m_renderingframe = tmp;
}
