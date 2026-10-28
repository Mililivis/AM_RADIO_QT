# SDR: драйвер для RTL-SDR Blog V4 (Raspberry Pi 5)

Драйвер на C++17 поверх `librtlsdr` (форк [rtl-sdr-blog](https://github.com/rtlsdrblog/rtl-sdr-blog), бо стандартний пакет із apt не підтримує V4 як слід).

```
include/RtlSdrDriver.h        API драйвера (клас sdr::RtlSdrDriver)
src/RtlSdrDriver.cpp          реалізація
src/main.cpp                  sdr_test: утиліта для перевірки і запису I/Q
scripts/install_librtlsdr.sh  встановлення librtlsdr (rtl-sdr-blog) на Пі
```

## 1. Встановлення librtlsdr на Пі (один раз)

```bash
./scripts/install_librtlsdr.sh
```

Скрипт ставить залежності, видаляє стокову librtlsdr, збирає rtl-sdr-blog, ставить udev-правила
і блокує ядерний DVB-драйвер `dvb_usb_rtl28xxu`. Після цього перепідключіть донгл, перезайдіть
по SSH (щоб застосувалась група `plugdev`) і перевірте:

```bash
rtl_test -t
```

Має з'явитися `RTL-SDR Blog V4 Detected`.

## 2. Збірка

```bash
cmake -B build && cmake --build build -j4
```

## 3. Перевірка

```bash
./build/sdr_test -l                               # список пристроїв
./build/sdr_test -f 100e6 -g 30                   # прочитати 1M семплів, показати потужність/DC/кліппінг
./build/sdr_test -f 7.1e6 -s 1.024e6 -g auto      # КХ (V4 вмикає апконвертер сам, нижче 28.8 МГц)
./build/sdr_test -f 101.5e6 -n 20e6 -o fm.iq      # записати 20M семплів у файл (формат rtl_sdr, u8 I/Q)
```

## API

```cpp
#include "RtlSdrDriver.h"

sdr::RtlSdrDriver dev(0);                 // або RtlSdrDriver(RtlSdrDriver::indexBySerial("00000001"))
dev.setSampleRate(2'048'000);
dev.setCenterFrequency(100'000'000);
dev.setGain(30.0);                        // до найближчого кроку тюнера, ручний режим
// dev.setGainMode(sdr::GainMode::Auto);
// dev.setBiasTee(true);                  // 4.5 В на антенний вхід для LNA

// Синхронно
std::vector<sdr::IqSample> iq = dev.readIq(262144);   // std::complex<float> у [-1, 1]

// Асинхронно (окремий потік)
dev.startStreaming([](const sdr::IqSample *s, size_t n) {
    // обробка; не блокувати надовго, інакше librtlsdr губитиме семпли
});
// ...
dev.stopStreaming();
```

Усі помилки кидаються як `sdr::RtlSdrError` (`code()` повертає код librtlsdr/libusb).
Під час стрімінгу не можна змінювати sample rate і робити синхронне читання.
Частоту і підсилення змінювати можна.

## Типові проблеми

| Симптом | Причина |
|---|---|
| `code -3` (permission denied) | немає udev-правил або користувач не в `plugdev` |
| `code -6` (busy) | донгл тримає `dvb_usb_rtl28xxu` або інша програма (rtl_tcp, SDR++) |
| `PLL not locked`, немає сигналу на КХ | стокова librtlsdr замість rtl-sdr-blog |
| `lost at least N bytes` | Пі не встигає: зменшіть sample rate (≤ 2.4 МГц) або обробку в колбеку |
