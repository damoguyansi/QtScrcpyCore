#ifndef VIDEO_BUFFER_H
#define VIDEO_BUFFER_H

#include <QMutex>
#include <QWaitCondition>
#include <QObject>

#include <functional>
#include "fpscounter.h"

// forward declarations
typedef struct AVFrame AVFrame;

class VideoBuffer : public QObject
{
    Q_OBJECT
public:
    VideoBuffer(QObject *parent = Q_NULLPTR);
    virtual ~VideoBuffer();

    bool init();
    void deInit();
    void lock();
    void unLock();
    void setRenderExpiredFrames(bool renderExpiredFrames);

    AVFrame *decodingFrame();
    // set the decoder frame as ready for rendering
    // this function locks m_mutex during its execution
    // returns true if the previous frame had been consumed
    void offerDecodedFrame(bool &previousFrameSkipped);

    // mark the rendering frame as consumed and return it
    // MUST be called with m_mutex locked!!!
    // the caller is expected to render the returned frame to some texture before
    // unlocking m_mutex (or use takeRenderedFrame, which handles this for you)
    const AVFrame *consumeRenderedFrame();

    // Consume the rendering frame and move a *reference* to it into dst
    // (av_frame_ref: O(1) for refcounted decoder frames, no pixel copy).
    // Locks m_mutex internally and releases it before returning, so the caller
    // can render dst without stalling the decoder thread. The caller owns the
    // reference and must av_frame_unref(dst) when done.
    // Returns false when there is no unconsumed/valid frame or av_frame_ref failed
    // (dst is left unref'd in that case). dst may be null to consume without taking.
    bool takeRenderedFrame(AVFrame *dst);

    void peekRenderedFrame(std::function<void(int width, int height, uint8_t* dataRGB32)> onFrame);

    // wake up and avoid any blocking call
    void interrupt();

signals:
    void updateFPS(quint32 fps);

private:
    void swap();

private:
    AVFrame *m_decodingFrame = Q_NULLPTR;
    AVFrame *m_renderingframe = Q_NULLPTR;
    QMutex m_mutex;
    bool m_renderingFrameConsumed = true;
    FpsCounter m_fpsCounter;

    bool m_renderExpiredFrames = false;
    QWaitCondition m_renderingFrameConsumedCond;

    // interrupted is not used if expired frames are not rendered
    // since offering a frame will never block
    bool m_interrupted = false;
};

#endif // VIDEO_BUFFER_H
