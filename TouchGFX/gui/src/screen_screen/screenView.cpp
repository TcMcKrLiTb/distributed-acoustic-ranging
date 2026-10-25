#include <gui/screen_screen/screenView.hpp>
#include <gui/common/DebugText.hpp>
#include <cstdio>
#ifndef SIMULATOR
#include "Ranging/ranging.h"
#include "Ranging/range_config.h"
#include "AudioArrival/audio_arrival.h"
#include "TimeSync/time_sync.h"
#endif

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
#ifndef SIMULATOR
    RangingStatus s = {};
    Ranging_GetStatus(&s);
    TimeSyncStatus sync = {};
    const bool haveSync = TimeSync_GetStatus(&sync);
    AudioArrivalEvent event = {};
    const bool haveEvent = AudioArrival_GetLatest(&event) && s.locked;
    RangingDiagnostics diagnostics = {};
    const bool haveDiagnostics = Ranging_GetDiagnostics(&diagnostics) && diagnostics.valid && s.locked;
    char text[64];
    const char board = !haveSync || sync.role == TIME_SYNC_DISCOVERY ? '?' :
                       sync.role == TIME_SYNC_MASTER ? 'A' : 'B';
    std::snprintf(text, sizeof(text), "BOARD %c  %s", board, s.locked ? "SYNC LOCKED" : "WAIT SYNC");
    set(boardStatusText, boardStatusTextBuffer, text);
    if (s.valid && s.locked) {
        std::snprintf(text, sizeof(text), "%lu.%03lu M", (unsigned long)(s.distance_mm / 1000U), (unsigned long)(s.distance_mm % 1000U));
        set(distanceText, distanceTextBuffer, text);
        std::snprintf(text, sizeof(text), "SOURCE %c SIDE", s.source_a ? 'A' : 'B');
        set(directionText, directionTextBuffer, text);
    } else {
        set(distanceText, distanceTextBuffer, "-.--- M");
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

    if (board == 'B') {
        set(correlationText, correlationTextBuffer, "Q DSP ON A");
        set(peakMarginText, peakMarginTextBuffer, "M DSP ON A");
        set(measurementStatusText, measurementStatusTextBuffer, "DSP ON A; FRONT IS DIAGNOSTIC ONLY");
    } else if (haveDiagnostics) {
        const RangeEstimate& l = diagnostics.long_window;
        const RangeEstimate& f = diagnostics.front_window;
        char lq[12] = "NA", fq[12] = "NA", lm[12] = "NA", fm[12] = "NA";
        if (l.correlation_valid) std::snprintf(lq, sizeof(lq), "%lu", (unsigned long)l.quality_permille);
        if (f.correlation_valid) std::snprintf(fq, sizeof(fq), "%lu", (unsigned long)f.quality_permille);
        if (l.competitor_valid) std::snprintf(lm, sizeof(lm), "%ld", (long)l.peak_margin_permille);
        if (f.competitor_valid) std::snprintf(fm, sizeof(fm), "%ld", (long)f.peak_margin_permille);
        std::snprintf(text, sizeof(text), "Q L%s F%s", lq, fq);
        set(correlationText, correlationTextBuffer, text);
        std::snprintf(text, sizeof(text), "M L%s F%s", lm, fm);
        set(peakMarginText, peakMarginTextBuffer, text);
        std::snprintf(text, sizeof(text), "LAST LONG %s; FRONT %s DIAG",
                      DebugText::error(l.error), DebugText::error(f.error));
        set(measurementStatusText, measurementStatusTextBuffer, text);
    } else {
        set(correlationText, correlationTextBuffer, "Q LNA FNA");
        set(peakMarginText, peakMarginTextBuffer, "M LNA FNA");
        set(measurementStatusText, measurementStatusTextBuffer,
            s.locked ? "WAIT FOR PAIRED AUDIO" : "WAIT FOR SYNC");
    }
    set(temperatureText, temperatureTextBuffer, "TEMP NA");
    std::snprintf(text, sizeof(text), "c %lu.%02lu m/s", (unsigned long)(RANGE_SOUND_SPEED_MM_S / 1000U),
                  (unsigned long)((RANGE_SOUND_SPEED_MM_S % 1000U) / 10U));
    set(soundSpeedText, soundSpeedTextBuffer, text);
    set(calibrationStatusText, calibrationStatusTextBuffer, "CAL NOT SET");
#else
    set(boardStatusText, boardStatusTextBuffer, "BOARD A  DEMO");
    set(distanceText, distanceTextBuffer, "0.700 M");
    set(directionText, directionTextBuffer, "SOURCE B SIDE");
    set(rangeStateText, rangeStateTextBuffer, "WAIT SOUND ID 1");
    set(statisticsText, statisticsTextBuffer, "LAST Q 99.0% OK 1 REJ 1");
    set(syncOffsetText, syncOffsetTextBuffer, "O 120 ns");
    set(eventInfoText, eventInfoTextBuffer, "P258 N4 T128");
    set(correlationText, correlationTextBuffer, "Q L437 F962");
    set(peakMarginText, peakMarginTextBuffer, "M L18 F34");
    set(temperatureText, temperatureTextBuffer, "TEMP NA");
    set(soundSpeedText, soundSpeedTextBuffer, "c 343.42 m/s");
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
