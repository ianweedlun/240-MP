#include <QtTest/QtTest>
#include <QJsonArray>
#include "../WeatherCodes.h"

using namespace wxcodes;

class WeatherCodesTest : public QObject {
    Q_OBJECT

private slots:
    void conditionForCodeMapsAllWmoCodes();
    void conditionShortDistinguishesDayAndNight();
    void iconForCodeUsesDayNightSuffixOnlyForSky();
    void codeForSkyWorstLayerWins();
    void codeForPresentWeatherHighestPriorityWins();
    void fogMistRequiresLowVisibility();
    void cardinalIsSixteenPoint();
    void skyRankOrdersByCoverage();
};

void WeatherCodesTest::conditionForCodeMapsAllWmoCodes() {
    QCOMPARE(conditionForCode(0),  QString("CLEAR"));
    QCOMPARE(conditionForCode(1),  QString("MAINLY CLEAR"));
    QCOMPARE(conditionForCode(2),  QString("PARTLY CLOUDY"));
    QCOMPARE(conditionForCode(3),  QString("OVERCAST"));
    QCOMPARE(conditionForCode(45), QString("FOG"));
    QCOMPARE(conditionForCode(48), QString("FOG"));
    QCOMPARE(conditionForCode(51), QString("DRIZZLE"));
    QCOMPARE(conditionForCode(55), QString("DRIZZLE"));
    QCOMPARE(conditionForCode(56), QString("FREEZING DRIZZLE"));
    QCOMPARE(conditionForCode(61), QString("RAIN"));
    QCOMPARE(conditionForCode(65), QString("RAIN"));
    QCOMPARE(conditionForCode(66), QString("FREEZING RAIN"));
    QCOMPARE(conditionForCode(71), QString("SNOW"));
    QCOMPARE(conditionForCode(77), QString("SNOW GRAINS"));
    QCOMPARE(conditionForCode(79), QString("SLEET"));
    QCOMPARE(conditionForCode(80), QString("SHOWERS"));
    QCOMPARE(conditionForCode(89), QString("HAIL"));
    QCOMPARE(conditionForCode(95), QString("THUNDERSTORM"));
    QCOMPARE(conditionForCode(96), QString("THUNDERSTORM HAIL"));
    QCOMPARE(conditionForCode(kCodeMostlyCloudy), QString("MOSTLY CLOUDY"));
    QCOMPARE(conditionForCode(12345), QString("UNKNOWN"));
}

void WeatherCodesTest::conditionShortDistinguishesDayAndNight() {
    QCOMPARE(conditionShortForCode(0, true),  QString("SUNNY"));
    QCOMPARE(conditionShortForCode(0, false), QString("CLEAR"));
    QCOMPARE(conditionShortForCode(1, true),  QString("SUNNY"));
    QCOMPARE(conditionShortForCode(1, false), QString("MAINLY CLEAR"));
    QCOMPARE(conditionShortForCode(3, true),  QString("CLOUDY"));
    QCOMPARE(conditionShortForCode(3, false), QString("CLOUDY"));
    QCOMPARE(conditionShortForCode(95, true), QString("T'STORMS"));
    // Unknown codes fall through to UNKNOWN, day or night.
    QCOMPARE(conditionShortForCode(999, true),  QString("UNKNOWN"));
    QCOMPARE(conditionShortForCode(999, false), QString("UNKNOWN"));
}

void WeatherCodesTest::iconForCodeUsesDayNightSuffixOnlyForSky() {
    // Sky states get -day / -night variants.
    QCOMPARE(iconForCode(0, true),  QString("clear-day"));
    QCOMPARE(iconForCode(0, false), QString("clear-night"));
    QCOMPARE(iconForCode(2, true),  QString("partly-cloudy-day"));
    QCOMPARE(iconForCode(2, false), QString("partly-cloudy-night"));
    // Non-sky states are the same day or night.
    QCOMPARE(iconForCode(61, true),  QString("rain"));
    QCOMPARE(iconForCode(61, false), QString("rain"));
    QCOMPARE(iconForCode(45, true),  QString("fog"));
    QCOMPARE(iconForCode(45, false), QString("fog"));
    // Unmapped codes yield empty (draw no icon).
    QVERIFY(iconForCode(999, true).isEmpty());
}

void WeatherCodesTest::codeForSkyWorstLayerWins() {
    // BKN (103) outranks FEW.
    QJsonArray layers;
    QJsonObject few, bkn;
    few.insert("amount", "FEW");
    bkn.insert("amount", "BKN");
    layers << few << bkn;
    QCOMPARE(codeForSky(layers), kCodeMostlyCloudy);

    // OVC wins over everything.
    QJsonArray layers2;
    QJsonObject ovc, few2;
    ovc.insert("amount", "OVC");
    few2.insert("amount", "FEW");
    layers2 << few2 << ovc;
    QCOMPARE(codeForSky(layers2), 3);

    // Empty layers = clear.
    QCOMPARE(codeForSky(QJsonArray()), 0);
}

void WeatherCodesTest::codeForPresentWeatherHighestPriorityWins() {
    QJsonArray pw;
    QJsonObject rain, fog;
    rain.insert("weather", "rain");
    rain.insert("intensity", "light");
    fog.insert("weather", "fog");
    pw << fog << rain;
    QCOMPARE(codeForPresentWeather(pw, 9999.0, true), 61);

    // Thunderstorm outranks rain even when listed second.
    QJsonArray pw2;
    QJsonObject ts, rain2;
    ts.insert("weather", "thunderstorms");
    rain2.insert("weather", "rain");
    pw2 << rain2 << ts;
    QCOMPARE(codeForPresentWeather(pw2, 9999.0, true), 95);

    // Hail upgrades thunderstorm to 96.
    QJsonArray pw3;
    QJsonObject ts2, hail;
    ts2.insert("weather", "thunderstorms");
    hail.insert("weather", "hail");
    pw3 << ts2 << hail;
    QCOMPARE(codeForPresentWeather(pw3, 9999.0, true), 96);

    // In-vicinity weather is ignored.
    QJsonArray pw4;
    QJsonObject near;
    near.insert("weather", "thunderstorms");
    near.insert("inVicinity", true);
    pw4 << near;
    QCOMPARE(codeForPresentWeather(pw4, 9999.0, true), -1);
}

void WeatherCodesTest::fogMistRequiresLowVisibility() {
    QJsonArray pw;
    QJsonObject mist;
    mist.insert("weather", "fog_mist");
    pw << mist;
    // High visibility: no fog (just mugginess).
    QCOMPARE(codeForPresentWeather(pw, 9999.0, true), -1);
    // Low visibility: fog.
    QCOMPARE(codeForPresentWeather(pw, 500.0, true), 45);
    // No visibility data: no fog.
    QCOMPARE(codeForPresentWeather(pw, 0.0, false), -1);
}

void WeatherCodesTest::cardinalIsSixteenPoint() {
    QCOMPARE(cardinal(0),    QString("N"));
    QCOMPARE(cardinal(22.5), QString("NNE"));
    QCOMPARE(cardinal(90),   QString("E"));
    QCOMPARE(cardinal(180),  QString("S"));
    QCOMPARE(cardinal(270),  QString("W"));
    QCOMPARE(cardinal(337.5),QString("NNW"));
    QCOMPARE(cardinal(360),  QString("N"));
    // Wraps negative.
    QCOMPARE(cardinal(-90),  QString("W"));
}

void WeatherCodesTest::skyRankOrdersByCoverage() {
    // Clear < FeW < Scattered < Mostly < Overcast.
    QVERIFY(skyRank(0) < skyRank(1));
    QVERIFY(skyRank(1) < skyRank(2));
    QVERIFY(skyRank(2) < skyRank(kCodeMostlyCloudy));
    QVERIFY(skyRank(kCodeMostlyCloudy) < skyRank(3));
    // Non-sky codes are not rankable.
    QCOMPARE(skyRank(61), -1);
}

QTEST_GUILESS_MAIN(WeatherCodesTest)
#include "WeatherCodesTest.moc"
