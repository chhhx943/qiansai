#include <QtTest>

#include "../VideoRuntime.h"

class VideoRuntimeTest : public QObject
{
    Q_OBJECT

private slots:
    void mailboxReturnsOnlyNewestFrame();
    void mailboxDoesNotRedeliverSequence();
    void rateSamplerUsesCounterDelta();
    void fitInsidePreservesSixteenByNine();
};

void VideoRuntimeTest::mailboxReturnsOnlyNewestFrame()
{
    VideoFrameMailbox mailbox;
    mailbox.publish(QImage(2, 2, QImage::Format_RGB32), 10, 100);
    mailbox.publish(QImage(4, 3, QImage::Format_RGB32), 20, 116);

    VideoFrameSnapshot frame;
    QVERIFY(mailbox.takeLatest(0, &frame));
    QCOMPARE(frame.sequence, quint64(2));
    QCOMPARE(frame.image.size(), QSize(4, 3));
    QCOMPARE(frame.ptsMs, qint64(20));
    QCOMPARE(frame.producedAtMs, qint64(116));
    QCOMPARE(mailbox.overwrittenFrames(), quint64(1));
}

void VideoRuntimeTest::mailboxDoesNotRedeliverSequence()
{
    VideoFrameMailbox mailbox;
    mailbox.publish(QImage(2, 2, QImage::Format_RGB32), 0, 0);

    VideoFrameSnapshot frame;
    QVERIFY(mailbox.takeLatest(0, &frame));
    QVERIFY(!mailbox.takeLatest(frame.sequence, &frame));
}

void VideoRuntimeTest::rateSamplerUsesCounterDelta()
{
    VideoRateSampler sampler;
    QCOMPARE(sampler.sample(1000, 60), 0.0);
    QCOMPARE(sampler.sample(2000, 120), 60.0);
    QCOMPARE(sampler.sample(2500, 150), 60.0);
}

void VideoRuntimeTest::fitInsidePreservesSixteenByNine()
{
    QCOMPARE(fitVideoInside(QSize(1920, 1080), QSize(1000, 700)), QSize(1000, 563));
    QCOMPARE(fitVideoInside(QSize(1920, 1080), QSize(700, 1000)), QSize(700, 394));
    QCOMPARE(fitVideoInside(QSize(), QSize(700, 1000)), QSize());
    QCOMPARE(fitVideoInside(QSize(1920, 1080), QSize()), QSize());
}

QTEST_MAIN(VideoRuntimeTest)
#include "tst_video_runtime.moc"
