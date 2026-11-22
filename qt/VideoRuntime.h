#ifndef VIDEORUNTIME_H
#define VIDEORUNTIME_H

#include <QImage>
#include <QMutex>
#include <QSize>

struct VideoFrameSnapshot
{
    QImage image;
    quint64 sequence = 0;
    qint64 ptsMs = 0;
    qint64 producedAtMs = 0;
};

class VideoFrameMailbox
{
public:
    quint64 publish(const QImage &image, qint64 ptsMs, qint64 producedAtMs);
    bool takeLatest(quint64 lastSequence, VideoFrameSnapshot *frame);
    void clear();
    quint64 overwrittenFrames() const;

private:
    mutable QMutex m_mutex;
    VideoFrameSnapshot m_latest;
    quint64 m_lastTakenSequence = 0;
    quint64 m_overwrittenFrames = 0;
};

class VideoRateSampler
{
public:
    double sample(qint64 nowMs, quint64 total);
    void reset();

private:
    qint64 m_lastMs = -1;
    quint64 m_lastTotal = 0;
};

QSize fitVideoInside(const QSize &source, const QSize &bounds);
qint64 frameDurationMs(int rateNumerator, int rateDenominator);

#endif // VIDEORUNTIME_H
