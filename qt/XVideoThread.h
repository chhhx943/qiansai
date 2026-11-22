#ifndef XVIDEOTHREAD_H
#define XVIDEOTHREAD_H

#include <QThread>
#include <QImage>
#include <QObject>
#include <QDebug>
#include <QMutex>
#include <QSize>
#include <atomic>

#include "VideoRuntime.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/time.h>
#include <libavutil/mem.h>
#include <libavutil/frame.h>
#include <libavutil/error.h>
#include <libavcodec/bsf.h>
}

/**
 * XVideoThread
 * -------------------------
 * 用于 RTMP/H264 视频流解码
 * 自动处理 codecpar 为空
 * 自动用 h264_mp4toannexb BSF 转 AnnexB
 * 输出 QImage 信号用于 Qt 显示
 */
class XVideoThread : public QThread
{
    Q_OBJECT
public:
    explicit XVideoThread(QObject *parent = nullptr);
    ~XVideoThread() override;

    void setUrl(const QString &url); // 设置流地址
    void setPlaybackRate(double rate);
    void seekToMs(qint64 ms);
    void setOutputSize(const QSize &size);
    bool takeLatestFrame(quint64 lastSequence, VideoFrameSnapshot *frame);
    quint64 decodedFrames() const;
    quint64 overwrittenFrames() const;
    QSize inputSize() const;
    double nominalFps() const;

signals:
    void sig_sendInitState(bool ok);    // 初始化成功或失败
    void sig_SendOneFrame(const QImage &img); // 发送一帧图片
    void sig_errorMessage(const QString &message); // 发送错误信息
    void sig_positionChanged(qint64 positionMs);
    void sig_durationChanged(qint64 durationMs);

protected:
    void run() override;

private:
    bool init();        // 初始化 AVFormatContext
    void unInit();      // 释放资源

    bool decodeSession();
    bool sleepInterruptibly(int milliseconds);
    QImage frameToImage(AVFrame *frame);
    QString avErrorString(int errorCode) const;
    bool isLiveUrl() const;
    double playbackRate() const;
    bool takePendingSeek(qint64 *positionMs);
    static int interruptCallback(void *opaque);

private:
    QString m_url;

    AVFormatContext  *m_fmtCtx  = nullptr;
    AVCodecContext   *m_codecCtx = nullptr;
    SwsContext       *m_swsCtx  = nullptr;
    AVBSFContext     *m_bsfCtx  = nullptr; // H264 AVCC -> AnnexB 转换
    int m_videoIndex             = -1;
    bool m_isLiveStream          = false;
    qint64 m_durationMs          = 0;
    qint64 m_streamStartMs       = 0;
    mutable QMutex m_controlMutex;
    double m_playbackRate        = 1.0;
    bool m_seekRequested         = false;
    qint64 m_pendingSeekMs       = 0;
    mutable QMutex m_geometryMutex;
    QSize m_outputSize;
    QSize m_inputSize;
    double m_nominalFps = 0.0;
    VideoFrameMailbox m_frameMailbox;
    std::atomic<quint64> m_decodedFrames{0};
};

#endif // XVIDEOTHREAD_H
