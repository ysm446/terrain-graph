#include "renderer/Ephemeris.h"
#include <cmath>

namespace tg::renderer {
namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 2*kPi;
constexpr double kArcSeconds = 206264.8062;          // 1 rad の秒角。
constexpr double kObliquity = 23.43929111*kPi/180;   // 黄道傾斜角（J2000）。
double Fraction(double x) { return x-std::floor(x); }
// 黄経・黄緯（rad）から赤道座標。
EquatorialPosition FromEcliptic(double longitude, double latitude) {
    const double x = std::cos(longitude)*std::cos(latitude);
    const double y = std::sin(longitude)*std::cos(latitude);
    const double z = std::sin(latitude);
    const double ye = y*std::cos(kObliquity)-z*std::sin(kObliquity);
    const double ze = y*std::sin(kObliquity)+z*std::cos(kObliquity);
    EquatorialPosition result;
    result.rightAscension = static_cast<float>(std::atan2(ye, x));
    if (result.rightAscension < 0) result.rightAscension += static_cast<float>(kTwoPi);
    result.declination = static_cast<float>(std::asin(ze));
    return result;
}
// 太陽の黄経（rad）。Montenbruck & Pfleger "MiniSun"。
double SunLongitude(double T) {
    const double M = kTwoPi*Fraction(0.993133+99.997361*T);
    return kTwoPi*Fraction(0.7859453+M/kTwoPi+(6893*std::sin(M)+72*std::sin(2*M)+6191.2*T)/1296000);
}
// 月の黄経・黄緯（rad）。Montenbruck & Pfleger "MiniMoon"。
void MoonEcliptic(double T, double& longitude, double& latitude) {
    const double L0 = Fraction(0.606433+1336.855225*T);
    const double l = kTwoPi*Fraction(0.374897+1325.552410*T);
    const double ls = kTwoPi*Fraction(0.993133+99.997361*T);
    const double D = kTwoPi*Fraction(0.827361+1236.853086*T);
    const double F = kTwoPi*Fraction(0.259086+1342.227825*T);
    const double dL = 22640*std::sin(l)-4586*std::sin(l-2*D)+2370*std::sin(2*D)+769*std::sin(2*l)
        -668*std::sin(ls)-412*std::sin(2*F)-212*std::sin(2*l-2*D)-206*std::sin(l+ls-2*D)
        +192*std::sin(l+2*D)-165*std::sin(ls-2*D)-125*std::sin(D)-110*std::sin(l+ls)
        +148*std::sin(l-ls)-55*std::sin(2*F-2*D);
    const double S = F+(dL+412*std::sin(2*F)+541*std::sin(ls))/kArcSeconds;
    const double h = F-2*D;
    const double N = -526*std::sin(h)+44*std::sin(l+h)-31*std::sin(-l+h)-23*std::sin(ls+h)
        +11*std::sin(-ls+h)-25*std::sin(-2*l+F)+21*std::sin(-l+F);
    longitude = kTwoPi*Fraction(L0+dL/1296000);
    latitude = (18520*std::sin(S)+N)/kArcSeconds;
}
double Centuries(double julianDay) { return (julianDay-2451545.0)/36525.0; }
}

double JulianDay(int year, int month, int day, double utcHours) {
    if (month <= 2) { year -= 1; month += 12; }
    const int a = year/100;
    const int b = 2-a+a/4;
    return std::floor(365.25*(year+4716))+std::floor(30.6001*(month+1))+day+b-1524.5+utcHours/24.0;
}

float GreenwichSiderealTime(double julianDay) {
    const double hours = 18.697374558+24.06570982441908*(julianDay-2451545.0);
    return static_cast<float>(Fraction(hours/24.0)*kTwoPi);
}

float LocalSiderealTime(double julianDay, float longitude) {
    double value = GreenwichSiderealTime(julianDay)+longitude;
    value = Fraction(value/kTwoPi)*kTwoPi;
    return static_cast<float>(value);
}

EquatorialPosition SunEquatorial(double julianDay) {
    return FromEcliptic(SunLongitude(Centuries(julianDay)), 0);
}

EquatorialPosition MoonEquatorial(double julianDay) {
    double longitude = 0, latitude = 0;
    MoonEcliptic(Centuries(julianDay), longitude, latitude);
    return FromEcliptic(longitude, latitude);
}

float MoonIlluminatedFraction(double julianDay) {
    const double T = Centuries(julianDay);
    double longitude = 0, latitude = 0;
    MoonEcliptic(T, longitude, latitude);
    // 太陽からの離角 psi: cos psi = cos b cos(l - ls)。照らされた割合は (1 - cos psi)/2。
    const double cosine = std::cos(latitude)*std::cos(longitude-SunLongitude(T));
    return static_cast<float>((1-cosine)*0.5);
}

HorizontalPosition ToHorizontal(EquatorialPosition position, float latitude, float localSiderealTime) {
    const double H = localSiderealTime-position.rightAscension;
    const double sinLat = std::sin(latitude), cosLat = std::cos(latitude);
    const double sinDec = std::sin(position.declination), cosDec = std::cos(position.declination);
    HorizontalPosition result;
    result.elevation = static_cast<float>(std::asin(sinLat*sinDec+cosLat*cosDec*std::cos(H)));
    // 北向き・東向き成分から方位を取る。tan(dec) を使う式は天の極で不安定になる。
    const double north = -std::cos(H)*cosDec*sinLat+sinDec*cosLat;
    const double east = -std::sin(H)*cosDec;
    result.azimuth = static_cast<float>(std::atan2(east, north));
    return result;
}

CelestialState ComputeCelestialState(const CelestialSettings& settings, float latitude) {
    const double julianDay = JulianDay(settings.year, settings.month, settings.day, settings.hour-settings.utcOffset);
    CelestialState state;
    state.siderealTime = LocalSiderealTime(julianDay, settings.longitude);
    state.sun = ToHorizontal(SunEquatorial(julianDay), latitude, state.siderealTime);
    state.moon = ToHorizontal(MoonEquatorial(julianDay), latitude, state.siderealTime);
    // 地心視差。平均の地平視差 0.95 度ぶん、地平線に近いほど低く見える。
    state.moon.elevation -= static_cast<float>(0.95*kPi/180*std::cos(state.moon.elevation));
    state.moonPhase = MoonIlluminatedFraction(julianDay);
    return state;
}
}
