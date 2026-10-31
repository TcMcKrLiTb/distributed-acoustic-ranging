#include <gui/screen_screen/screenView.hpp>
#include <gui/common/DebugText.hpp>
#include <cstddef>
#include <cstdio>
#ifndef SIMULATOR
#include "Ranging/ranging.h"
#include "AudioArrival/audio_arrival.h"
#include "TimeSync/time_sync.h"
#else
#include <gui/settings_screen/settingsView.hpp>
#endif

namespace
{
void formatDistance(char* text, std::size_t size, uint32_t millimeters, bool valid, DistanceUnit unit)
{
    if (!valid) {
        std::snprintf(text, size, "%s", unit == DistanceUnit::Centimeters ? "---.- CM" : "-.--- M");
    } else if (unit == DistanceUnit::Centimeters) {
        std::snprintf(text, size, "%lu.%lu CM", (unsigned long)(millimeters / 10U),
                      (unsigned long)(millimeters % 10U));
    } else {
        std::snprintf(text, size, "%lu.%03lu M", (unsigned long)(millimeters / 1000U),
                      (unsigned long)(millimeters % 1000U));
    }
}
}

screenView::screenView() : refreshTicks(0)
{

}

void screenView::setupScreen()
{
    screenViewBase::setupScreen();
#ifndef SIMULATOR
    demoText.setVisible(false);
#else
    demoText.setVisible(true);
#endif
    refreshTicks = 0;
    refreshDiagnostics();
}

void screenView::handleTickEvent()
{
    screenViewBase::handleTickEvent();
    if (++refreshTicks < 6U) return;
    refreshTicks = 0;
    refreshDiagnostics();
}

void screenView::refreshDiagnostics()
{
    using DebugText::set;
    char text[64];
#ifndef SIMULATOR
    RangingStatus s = {};
    Ranging_GetStatus(&s);
    TimeSyncStatus sync = {};
    const bool haveSync = TimeSync_GetStatus(&sync);
    AudioArrivalEvent event = {};
    const bool haveEvent = AudioArrival_GetLatest(&event) && s.locked;
    RangingDiagnostics diagnostics = {};
    const bool haveDiagnostics = Ranging_GetDiagnostics(&diagnostics) && diagnostics.valid && s.locked;
    const char board = !haveSync || sync.role == TIME_SYNC_DISCOVERY ? '?' :
                       sync.role == TIME_SYNC_MASTER ? 'A' : 'B';
    std::snprintf(text, sizeof(text), "BOARD %c  %s", board, s.locked ? "SYNC LOCKED" : "WAIT SYNC");
    set(boardStatusText, boardStatusTextBuffer, text);
    formatDistance(text, sizeof(text), s.distance_mm, s.valid && s.locked, presenter->getDistanceUnit());
    set(distanceText, distanceTextBuffer, text);
    if (s.valid && s.locked) {
        std::snprintf(text, sizeof(text), "SOURCE %c SIDE", s.source_a ? 'A' : 'B');
        set(directionText, directionTextBuffer, text);
    } else {
        set(directionText, directionTextBuffer, "SOURCE -");
    }
    const char* states[] = {"WAIT SYNC", "WAIT SOUND", "TRANSFER", "CORRELATING", "WAIT DISPLAY"};
    std::snprintf(text, sizeof(text), "%s  ID %lu", states[(unsigned)s.state < 5U ? s.state : 0], (unsigned long)s.sequence);
    set(rangeStateText, rangeStateTextBuffer, text);
    if (s.accepted) {
        std::snprintf(text, sizeof(text), "LAST Q %lu.%lu%%  OK %lu REJ %lu",
                      (unsigned long)(s.quality_permille / 10U), (unsigned long)(s.quality_permille % 10U),
                      (unsigned long)s.accepted, (unsigned long)s.rejected);
    } else {
        std::snprintf(text, sizeof(text), "LAST Q NA  OK %lu REJ %lu",
                      (unsigned long)s.accepted, (unsigned long)s.rejected);
    }
    set(statisticsText, statisticsTextBuffer, text);

    if (haveSync && (sync.sample_valid || sync.locked)) {
        char offset[24];
        DebugText::integer(offset, sizeof(offset), sync.offset_ns);
        std::snprintf(text, sizeof(text), "O %s ns", offset);
        set(syncOffsetText, syncOffsetTextBuffer, text);
    } else set(syncOffsetText, syncOffsetTextBuffer, "OFFSET NA");
    if (haveEvent) {
        std::snprintf(text, sizeof(text), "P%lu N%lu T%lu", (unsigned long)event.peak,
                      (unsigned long)event.noise, (unsigned long)event.threshold);
        set(eventInfoText, eventInfoTextBuffer, text);
    } else set(eventInfoText, eventInfoTextBuffer, "NO AUDIO EVENT");

    char dspState[40];
    if (board == 'B') {
        set(correlationText, correlationTextBuffer, "Q DSP ON A");
        set(peakMarginText, peakMarginTextBuffer, "M DSP ON A");
        std::snprintf(dspState, sizeof(dspState), "DSP ON A; DIRECT");
    } else if (haveDiagnostics) {
        const RangeEstimate& l = diagnostics.long_window;
        const RangeEstimate& f = diagnostics.front_window;
        const RangeEstimate& d = diagnostics.direct_window;
        char lq[12] = "NA", fq[12] = "NA", dq[12] = "NA", lm[12] = "NA", fm[12] = "NA";
        if (l.correlation_valid) std::snprintf(lq, sizeof(lq), "%lu", (unsigned long)l.quality_permille);
        if (f.correlation_valid) std::snprintf(fq, sizeof(fq), "%lu", (unsigned long)f.quality_permille);
        if (d.correlation_valid) std::snprintf(dq, sizeof(dq), "%lu", (unsigned long)d.quality_permille);
        if (l.competitor_valid) std::snprintf(lm, sizeof(lm), "%ld", (long)l.peak_margin_permille);
        if (f.competitor_valid) std::snprintf(fm, sizeof(fm), "%ld", (long)f.peak_margin_permille);
        std::snprintf(text, sizeof(text), "Q D%s L%s F%s", dq, lq, fq);
        set(correlationText, correlationTextBuffer, text);
        std::snprintf(text, sizeof(text), "M L%s F%s", lm, fm);
        set(peakMarginText, peakMarginTextBuffer, text);
        std::snprintf(dspState, sizeof(dspState), "DIRECT %s; LONG %s",
                      DebugText::error(d.error), DebugText::error(l.error));
    } else {
        set(correlationText, correlationTextBuffer, "Q LNA FNA");
        set(peakMarginText, peakMarginTextBuffer, "M LNA FNA");
        std::snprintf(dspState, sizeof(dspState), "%s", s.locked ? "WAIT FOR PAIRED AUDIO" : "WAIT FOR SYNC");
    }
    RangingTemperatureStatus temperature = {};
    const char* temperatureState = "TEMP CONFIG NA";
    if (Ranging_GetTemperature(&temperature)) {
        const int value = temperature.temperature_c_x10;
        const int magnitude = value < 0 ? -value : value;
        std::snprintf(text, sizeof(text), "T %s%d.%d C", value < 0 ? "-" : "", magnitude / 10, magnitude % 10);
        set(temperatureText, temperatureTextBuffer, text);
        std::snprintf(text, sizeof(text), "c %lu.%03lu m/s", (unsigned long)(temperature.sound_speed_mm_s / 1000U),
                      (unsigned long)(temperature.sound_speed_mm_s % 1000U));
        set(soundSpeedText, soundSpeedTextBuffer, text);
        temperatureState = temperature.pending ? "TEMP CONFIG SYNCING" :
                           !s.locked ? "TEMP CONFIG WAIT SYNC" :
                           !temperature.synchronized ? "TEMP CONFIG NOT SYNCED" :
                           temperature.automatic ? (temperature.source_a ? "AUTO A TEMP APPLIED" : "AUTO B TEMP APPLIED") :
                           "MANUAL TEMP APPLIED";
    } else {
        set(temperatureText, temperatureTextBuffer, "TEMP NA");
        set(soundSpeedText, soundSpeedTextBuffer, "c NA");
    }
    // Retain both the temperature state and the per-attempt DSP diagnosis.
    char measurement[64];
    std::snprintf(measurement, sizeof(measurement), "%s; %s", temperatureState, dspState);
    set(measurementStatusText, measurementStatusTextBuffer, measurement);
    set(calibrationStatusText, calibrationStatusTextBuffer, "CAL NOT SET");
#else
    set(boardStatusText, boardStatusTextBuffer, "BOARD A  DEMO");
    formatDistance(text, sizeof(text), 700U, true, presenter->getDistanceUnit());
    set(distanceText, distanceTextBuffer, text);
    set(directionText, directionTextBuffer, "SOURCE B SIDE");
    set(rangeStateText, rangeStateTextBuffer, "WAIT SOUND ID 1");
    set(statisticsText, statisticsTextBuffer, "LAST Q 99.0% OK 1 REJ 1");
    set(syncOffsetText, syncOffsetTextBuffer, "O 120 ns");
    set(eventInfoText, eventInfoTextBuffer, "P258 N4 T128");
    set(correlationText, correlationTextBuffer, "Q L437 F962");
    set(peakMarginText, peakMarginTextBuffer, "M L18 F34");
    const int temperature = settingsView::demoTemperature();
    const int magnitude = temperature < 0 ? -temperature : temperature;
    const uint32_t speed = settingsView::demoSoundSpeed();
    std::snprintf(text, sizeof(text), "T %s%d.%d C", temperature < 0 ? "-" : "", magnitude / 10, magnitude % 10);
    set(temperatureText, temperatureTextBuffer, text);
    std::snprintf(text, sizeof(text), "c %lu.%03lu m/s", (unsigned long)(speed / 1000U), (unsigned long)(speed % 1000U));
    set(soundSpeedText, soundSpeedTextBuffer, text);
    set(calibrationStatusText, calibrationStatusTextBuffer, "CAL NOT SET");
    set(measurementStatusText, measurementStatusTextBuffer, "DEMO LONG LOW Q; FRONT PASS DIAG");
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
