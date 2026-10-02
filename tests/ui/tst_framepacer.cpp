#include "ui/FramePacer.h"

#include <QtTest>

#include <random>

using namespace scvp::ui;

class TstFramePacer : public QObject {
    Q_OBJECT

private slots:
    void blockingSwapsAdvanceOnePeriodEach()
    {
        const double p = 1.0 / 60.0;
        VsyncClock c(p);
        std::mt19937 rng(1);
        std::uniform_real_distribution<double> jitter(-0.004, 0.004);  // < half a period
        double prev = c.update(10.0);
        QCOMPARE(prev, p);  // the first frame one period after start
        for (int i = 1; i <= 600; ++i) {
            const double m = c.update(10.0 + i * p + jitter(rng));
            QVERIFY2(std::abs(m - prev - p) < 1e-9, qPrintable(QString::number(i)));
            prev = m;
        }
    }

    void missedVsyncsCountTwice()
    {
        const double p = 1.0 / 60.0;
        VsyncClock c(p);
        c.update(0);
        const double a = c.update(p);
        const double b = c.update(3 * p);  // one swap missed a vsync
        QVERIFY(std::abs(b - a - 2 * p) < 1e-9);
    }

    void nonBlockingSwapsFollowTheWallClock()
    {
        // Swaps every 9 ms on a 60 Hz period (Xvfb, vsync disabled): never faster than real time.
        const double p = 1.0 / 60.0;
        VsyncClock c(p);
        double wall = 5.0;
        c.update(wall);
        for (int i = 0; i < 1000; ++i)
            c.update(wall += 0.009);
        const double elapsed = 1000 * 0.009;
        QVERIFY2(std::abs(c.media() - (elapsed + p)) <= p / 2 + 1e-9, qPrintable(QString::number(c.media())));
    }

    void restartKeepsMediaTimeContinuous()
    {
        const double p = 1.0 / 60.0;
        VsyncClock c(p);
        c.update(0);
        c.update(p);
        const double before = c.media();
        c.restart();
        const double after = c.update(100.0);  // long pause: no jump
        QVERIFY(std::abs(after - before - p) < 1e-9);
        QVERIFY(c.update(100.0) == after);  // never goes back or repeats ahead
    }
};

QTEST_APPLESS_MAIN(TstFramePacer)
#include "tst_framepacer.moc"
