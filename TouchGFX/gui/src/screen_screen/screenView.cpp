#include <gui/screen_screen/screenView.hpp>
#include <cstddef>
#include <cstdio>
#ifndef SIMULATOR
#include "Ranging/ranging.h"
#else
#include <gui/settings_screen/settingsView.hpp>
#endif

namespace
{
// The Designer owns widget geometry; repaint only when its text changes.
template <std::size_t N>
void setText(touchgfx::TextAreaWithOneWildcard& widget,
             touchgfx::Unicode::UnicodeChar (&buffer)[N], const char* text)
{
    std::size_t i = 0;
    while (i < N - 1 && text[i] &&
           buffer[i] == static_cast<touchgfx::Unicode::UnicodeChar>(text[i]))
    {
        ++i;
    }
    if (!text[i] && buffer[i] == 0)
    {
        return;
    }
    touchgfx::Unicode::strncpy(buffer, text, N);
    buffer[N - 1] = 0;
    widget.invalidate();
}
}

screenView::screenView()
{

}

void screenView::setupScreen()
{
    screenViewBase::setupScreen();
    handleTickEvent();
}

void screenView::handleTickEvent()
{
    screenViewBase::handleTickEvent();
#ifndef SIMULATOR
    RangingStatus s = {};
    Ranging_GetStatus(&s);
    char text[64];
    std::snprintf(text, sizeof(text), "BOARD %c  %s", s.master ? 'A' : 'B', s.locked ? "SYNC LOCKED" : "WAIT SYNC");
    setText(boardStatusText, boardStatusTextBuffer, text);
    if (s.valid) {
        std::snprintf(text, sizeof(text), "%lu.%03lu M", (unsigned long)(s.distance_mm / 1000U), (unsigned long)(s.distance_mm % 1000U));
        setText(distanceText, distanceTextBuffer, text);
        std::snprintf(text, sizeof(text), "SOURCE %c SIDE", s.source_a ? 'A' : 'B');
        setText(directionText, directionTextBuffer, text);
    } else {
        setText(distanceText, distanceTextBuffer, "-.--- M");
        setText(directionText, directionTextBuffer, "SOURCE -");
    }
    const char* states[] = {"WAIT SYNC", "WAIT SOUND", "TRANSFER", "CORRELATING", "WAIT DISPLAY"};
    std::snprintf(text, sizeof(text), "%s  ID %lu", states[(unsigned)s.state < 5U ? s.state : 0], (unsigned long)s.sequence);
    setText(rangeStateText, rangeStateTextBuffer, text);
    std::snprintf(text, sizeof(text), "Q %lu.%lu%%  OK %lu  REJECT %lu", (unsigned long)(s.quality_permille / 10U),
                  (unsigned long)(s.quality_permille % 10U), (unsigned long)s.accepted, (unsigned long)s.rejected);
    setText(statisticsText, statisticsTextBuffer, text);

    RangingTemperatureStatus temperature = {};
    if (Ranging_GetTemperature(&temperature)) {
        const int value = temperature.temperature_c_x10;
        const int magnitude = value < 0 ? -value : value;
        std::snprintf(text, sizeof(text), "T %s%d.%d C", value < 0 ? "-" : "", magnitude / 10, magnitude % 10);
        setText(temperatureText, temperatureTextBuffer, text);
        std::snprintf(text, sizeof(text), "c %lu.%03lu m/s", (unsigned long)(temperature.sound_speed_mm_s / 1000U),
                      (unsigned long)(temperature.sound_speed_mm_s % 1000U));
        setText(soundSpeedText, soundSpeedTextBuffer, text);
        const char* state = temperature.pending ? "TEMP CONFIG SYNCING" :
                            !s.locked ? "TEMP CONFIG WAIT SYNC" :
                            !temperature.synchronized ? "TEMP CONFIG NOT SYNCED" :
                            temperature.automatic ? (temperature.source_a ? "AUTO A TEMP APPLIED" : "AUTO B TEMP APPLIED") :
                            "MANUAL TEMP APPLIED";
        setText(measurementStatusText, measurementStatusTextBuffer, state);
    } else {
        setText(temperatureText, temperatureTextBuffer, "TEMP NA");
        setText(soundSpeedText, soundSpeedTextBuffer, "c NA");
        setText(measurementStatusText, measurementStatusTextBuffer, "TEMP CONFIG NA");
    }
#else
    setText(boardStatusText, boardStatusTextBuffer, "BOARD A  SYNC LOCKED");
    setText(distanceText, distanceTextBuffer, "1.234 M");
    setText(directionText, directionTextBuffer, "SOURCE A SIDE");
    setText(rangeStateText, rangeStateTextBuffer, "WAIT SOUND  ID 1");
    setText(statisticsText, statisticsTextBuffer, "Q 99.0%  OK 1  REJECT 0");
    const int temperature = settingsView::demoTemperature();
    const int magnitude = temperature < 0 ? -temperature : temperature;
    const uint32_t speed = settingsView::demoSoundSpeed();
    char text[64];
    std::snprintf(text, sizeof(text), "T %s%d.%d C", temperature < 0 ? "-" : "", magnitude / 10, magnitude % 10);
    setText(temperatureText, temperatureTextBuffer, text);
    std::snprintf(text, sizeof(text), "c %lu.%03lu m/s", (unsigned long)(speed / 1000U), (unsigned long)(speed % 1000U));
    setText(soundSpeedText, soundSpeedTextBuffer, text);
    setText(measurementStatusText, measurementStatusTextBuffer, "DEMO MANUAL TEMPERATURE");
#endif
}

void screenView::tearDownScreen()
{
    screenViewBase::tearDownScreen();
}

void screenView::button1Clicked()
{
    presenter->toggleGpio();
}
