#include <QDebug>
#include <QMutexLocker>

#include "compat.h"
#include "decoder.h"
#include "videobuffer.h"

Decoder::Decoder(std::function<void(int, int, uint8_t*, uint8_t*, uint8_t*, int, int, int)> onFrame, QObject *parent)
    : IDecoder(parent)
    , m_vb(new VideoBuffer())
    , m_onFrame(onFrame)
{
    m_vb->init();
    // NULL is tolerated: onNewFrame then degrades to consume-only.
    m_renderFrame = av_frame_alloc();
    connect(this, &Decoder::newFrame, this, &Decoder::onNewFrame, Qt::QueuedConnection);
    connect(m_vb, &VideoBuffer::updateFPS, this, &Decoder::updateFPS);
}

Decoder::~Decoder() {
    // Drop the GUI reference before the buffer owner goes away.
    if (m_renderFrame) {
        av_frame_free(&m_renderFrame);
    }
    m_vb->deInit();
    delete m_vb;
}

bool Decoder::open()
{
    QMutexLocker locker(&m_codecMutex);
    // codec
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) {
        qCritical("H.264 decoder not found");
        return false;
    }

    // codec context
    m_codecCtx = avcodec_alloc_context3(codec);
    if (!m_codecCtx) {
        qCritical("Could not allocate decoder context");
        return false;
    }
    if (avcodec_open2(m_codecCtx, codec, NULL) < 0) {
        qCritical("Could not open H.264 codec");
        avcodec_free_context(&m_codecCtx);
        return false;
    }
    m_isCodecCtxOpen = true;
    return true;
}

void Decoder::close()
{
    if (m_vb) {
        m_vb->interrupt();
    }
    // Release the pool buffer pinned by the last render so teardown does not
    // keep decoder memory alive; never touch it while observers still read it.
    if (m_renderFrame && !m_renderInFlight) {
        av_frame_unref(m_renderFrame);
    }

    QMutexLocker locker(&m_codecMutex);
    if (!m_codecCtx) {
        return;
    }
    if (m_isCodecCtxOpen) {
        avcodec_close(m_codecCtx);
    }
    avcodec_free_context(&m_codecCtx);
    m_isCodecCtxOpen = false;
}

void Decoder::onVideoSessionChanged(const QSize &size)
{
    Q_UNUSED(size);
    QMutexLocker locker(&m_codecMutex);
    if (m_codecCtx) {
        avcodec_flush_buffers(m_codecCtx);
    }
}

void Decoder::setRenderExpiredFrames(bool enabled)
{
    if (m_vb) {
        m_vb->setRenderExpiredFrames(enabled);
    }
}

bool Decoder::push(const AVPacket *packet)
{
    QMutexLocker locker(&m_codecMutex);
    if (!m_codecCtx || !m_vb) {
        return false;
    }
    AVFrame *decodingFrame = m_vb->decodingFrame();
#ifdef QTSCRCPY_LAVF_HAS_NEW_ENCODING_DECODING_API
    int ret = -1;
    if ((ret = avcodec_send_packet(m_codecCtx, packet)) < 0) {
        char errorbuf[255] = { 0 };
        av_strerror(ret, errorbuf, 254);
        qCritical("Could not send video packet: %s", errorbuf);
        return false;
    }
    if (decodingFrame) {
        ret = avcodec_receive_frame(m_codecCtx, decodingFrame);
    }
    if (!ret) {
        // a frame was received
        pushFrame();

        //emit getOneFrame(yuvDecoderFrame->data[0], yuvDecoderFrame->data[1], yuvDecoderFrame->data[2],
        //        yuvDecoderFrame->linesize[0], yuvDecoderFrame->linesize[1], yuvDecoderFrame->linesize[2]);

        /*
        // m_conver转换yuv为rgb是使用cpu转的，占用cpu太高，改用opengl渲染yuv
        // QImage的copy也非常占用内存，此方案不考虑
        if (!m_conver.isInit()) {
            qDebug() << "decoder frame format" << decodingFrame->format;
            m_conver.setSrcFrameInfo(codecCtx->width, codecCtx->height, AV_PIX_FMT_YUV420P);
            m_conver.setDstFrameInfo(codecCtx->width, codecCtx->height, AV_PIX_FMT_RGB32);
            m_conver.init();
        }
        if (!outBuffer) {
            outBuffer=new quint8[avpicture_get_size(AV_PIX_FMT_RGB32, codecCtx->width, codecCtx->height)];
            avpicture_fill((AVPicture *)rgbDecoderFrame, outBuffer, AV_PIX_FMT_RGB32, codecCtx->width, codecCtx->height);
        }
        m_conver.convert(decodingFrame, rgbDecoderFrame);
        //QImage tmpImg((uchar *)outBuffer, codecCtx->width, codecCtx->height, QImage::Format_RGB32);
        //QImage image = tmpImg.copy();
        //emit getOneImage(image);
        */
    } else if (ret != AVERROR(EAGAIN)) {
        qCritical("Could not receive video frame: %d", ret);
        return false;
    }
#else
    int gotPicture = 0;
    int len = -1;
    if (decodingFrame) {
        len = avcodec_decode_video2(m_codecCtx, decodingFrame, &gotPicture, packet);
    }
    if (len < 0) {
        qCritical("Could not decode video packet: %d", len);
        return false;
    }
    if (gotPicture) {
        pushFrame();
    }
#endif
    return true;
}

void Decoder::peekFrame(std::function<void (int, int, uint8_t *)> onFrame)
{
    if (!m_vb) {
        return;
    }
    m_vb->peekRenderedFrame(onFrame);
}

void Decoder::pushFrame()
{
    if (!m_vb) {
        return;
    }
    bool previousFrameSkipped = true;
    m_vb->offerDecodedFrame(previousFrameSkipped);
    if (previousFrameSkipped) {
        // the previous newFrame will consume this frame
        return;
    }
    emit newFrame();
}

void Decoder::onNewFrame() {
    if (!m_vb) {
        return;
    }
    if (m_renderInFlight) {
        // m_onFrame re-entered the event loop and another newFrame was delivered
        // inside it. Never overwrite m_renderFrame while the outer fan-out still
        // reads its planes; re-run once the outer call finishes.
        m_renderPending = true;
        return;
    }
    if (!m_renderFrame) {
        // Consume-only so the decoder never blocks in renderExpiredFrames mode.
        m_vb->takeRenderedFrame(Q_NULLPTR);
        return;
    }

    // Take a reference under the lock, then render *outside* it: the GL upload
    // and any resize in the observers must not stall the decoder thread.
    if (!m_vb->takeRenderedFrame(m_renderFrame)) {
        return;
    }
    if (m_onFrame) {
        m_renderInFlight = true;
        AVFrame *f = m_renderFrame;
        m_onFrame(f->width, f->height, f->data[0], f->data[1], f->data[2],
                  f->linesize[0], f->linesize[1], f->linesize[2]);
        m_renderInFlight = false;
    }
    // Observers copy the planes synchronously (texture upload), so return the
    // pool buffer right away instead of pinning a third frame permanently.
    av_frame_unref(m_renderFrame);

    if (m_renderPending) {
        m_renderPending = false;
        emit newFrame();
    }
}
