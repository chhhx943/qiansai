#include "VideoRuntime.h"

#include <QMutexLocker>

quint64 VideoFrameMailbox::publish(const QImage &image,
                                   qint64 ptsMs,
                                   qint64 producedAtMs)
{
    QMutexLocker locker(&m_mutex);
    if(!m_latest.image.isNull() && m_latest.sequence != m_lastTakenSequence)
        ++m_overwrittenFrames;

    ++m_latest.sequence;
    m_latest.image = image;
    m_latest.ptsMs = ptsMs;
    m_latest.producedAtMs = producedAtMs;
    return m_latest.sequence;
}

bool VideoFrameMailbox::takeLatest(quint64 lastSequence,
                                   VideoFrameSnapshot *frame)
{
    if(!frame)
        return false;

    QMutexLocker locker(&m_mutex);
    if(m_latest.image.isNull() || m_latest.sequence == lastSequence)
        return false;

    *frame = m_latest;
    m_lastTakenSequence = m_latest.sequence;
    return true;
}

void VideoFrameMailbox::clear()
{
    QMutexLocker locker(&m_mutex);
    m_latest = VideoFrameSnapshot{};
    m_lastTakenSequence = 0;
    m_overwrittenFrames = 0;
}

quint64 VideoFrameMailbox::overwrittenFrames() const
{
    QMutexLocker locker(&m_mutex);
    return m_overwrittenFrames;
}

double VideoRateSampler::sample(qint64 nowMs, quint64 total)
{
    if(m_lastMs < 0 || nowMs <= m_lastMs || total < m_lastTotal) {
        m_lastMs = nowMs;
        m_lastTotal = total;
        return 0.0;
    }

    const qint64 elapsedMs = nowMs - m_lastMs;
    const quint64 delta = total - m_lastTotal;
    m_lastMs = nowMs;
    m_lastTotal = total;
    return static_cast<double>(delta) * 1000.0 / static_cast<double>(elapsedMs);
}

void VideoRateSampler::reset()
{
    m_lastMs = -1;
    m_lastTotal = 0;
}

QSize fitVideoInside(const QSize &source, const QSize &bounds)
{
    if(!source.isValid() || !bounds.isValid()
       || source.width() <= 0 || source.height() <= 0
       || bounds.width() <= 0 || bounds.height() <= 0) {
        return QSize();
    }

    const qint64 widthLimitedHeight =
        (static_cast<qint64>(source.height()) * bounds.width()
         + source.width() / 2) / source.width();
    if(widthLimitedHeight <= bounds.height())
        return QSize(bounds.width(), static_cast<int>(widthLimitedHeight));

    const qint64 heightLimitedWidth =
        (static_cast<qint64>(source.width()) * bounds.height()
         + source.height() / 2) / source.height();
    return QSize(static_cast<int>(heightLimitedWidth), bounds.height());
}

qint64 frameDurationMs(int rateNumerator, int rateDenominator)
{
    if(rateNumerator <= 0 || rateDenominator <= 0)
        return 17;

    const double duration = 1000.0 * rateDenominator / rateNumerator;
    return qMax<qint64>(1, qRound64(duration));
}
