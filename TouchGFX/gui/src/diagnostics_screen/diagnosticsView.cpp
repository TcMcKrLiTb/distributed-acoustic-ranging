#include <gui/diagnostics_screen/diagnosticsView.hpp>
#include <gui/common/DebugText.hpp>
#include <touchgfx/Color.hpp>
#include <cstdio>

namespace
{
template <typename Button>
void styleTab(Button& button, bool selected)
{
    // Reuse the Designer's selected/unselected palette without changing layout.
    button.setBoxWithBorderColors(
        selected ? touchgfx::Color::getColorFromRGB(22, 77, 122) : touchgfx::Color::getColorFromRGB(255, 255, 255),
        touchgfx::Color::getColorFromRGB(0, 153, 204),
        touchgfx::Color::getColorFromRGB(211, 220, 231),
        touchgfx::Color::getColorFromRGB(51, 102, 153));
    button.setTextColors(
        selected ? touchgfx::Color::getColorFromRGB(255, 255, 255) : touchgfx::Color::getColorFromRGB(10, 10, 10),
        touchgfx::Color::getColorFromRGB(10, 10, 10));
    button.invalidate();
}
}

#ifndef SIMULATOR
#include "Ranging/ranging.h"
#include "AudioArrival/audio_arrival.h"
#include "TimeSync/time_sync.h"

namespace
{
const char* syncState(TimeSyncState state)
{
    const char* names[] = {"STARTING", "ERROR", "LINK DOWN", "WAIT PEER", "ACQUIRING", "LOCKED ESTIMATE"};
    const unsigned index = static_cast<unsigned>(state);
    return index < sizeof(names) / sizeof(names[0]) ? names[index] : "UNKNOWN";
}

void scoreText(char* out, std::size_t size, const RangeEstimate& estimate)
{
    if (!estimate.correlation_valid) {
        std::snprintf(out, size, "Q NA M NA %s", DebugText::error(estimate.error));
        return;
    }
    char margin[16] = "NA";
    if (estimate.competitor_valid)
        std::snprintf(margin, sizeof(margin), "%ld", (long)estimate.peak_margin_permille);
    std::snprintf(out, size, "Q%lu M%s %s", (unsigned long)estimate.quality_permille,
                  margin, DebugText::error(estimate.error));
}

void candidateText(char* out, std::size_t size, const RangeEstimate& estimate)
{
    if (!estimate.candidate_valid) {
        std::snprintf(out, size, "NA");
        return;
    }
    char delay[24];
    DebugText::integer(delay, sizeof(delay), estimate.candidate_raw_delay_ns);
    std::snprintf(out, size, "%lu mm k%ld %s ns", (unsigned long)estimate.candidate_distance_mm,
                  (long)estimate.peak_lag_samples, delay);
}
}
#endif

diagnosticsView::diagnosticsView() : selectedTab(SYNC), refreshTicks(0),
    tabCallback(this, &diagnosticsView::tabSelected)
{
    diagSyncButton.setAction(tabCallback);
    diagAudioButton.setAction(tabCallback);
    diagResultsButton.setAction(tabCallback);
}

void diagnosticsView::setupScreen()
{
    diagnosticsViewBase::setupScreen();
#ifndef SIMULATOR
    demoText.setVisible(false);
#else
    demoText.setVisible(true);
#endif
    refreshTicks = 0;
    updateTabButtons();
    refreshDiagnostics();
}

void diagnosticsView::tearDownScreen()
{
    diagnosticsViewBase::tearDownScreen();
}

void diagnosticsView::handleTickEvent()
{
    diagnosticsViewBase::handleTickEvent();
    if (++refreshTicks < 6U) return;
    refreshTicks = 0;
    refreshDiagnostics();
}

void diagnosticsView::tabSelected(const touchgfx::AbstractButtonContainer& source)
{
    Tab nextTab = selectedTab;
    if (&source == &diagSyncButton) nextTab = SYNC;
    else if (&source == &diagAudioButton) nextTab = AUDIO;
    else if (&source == &diagResultsButton) nextTab = RESULTS;
    if (nextTab == selectedTab) return;
    selectedTab = nextTab;
    refreshTicks = 0;
    updateTabButtons();
    refreshDiagnostics();
}

void diagnosticsView::updateTabButtons()
{
    styleTab(diagSyncButton, selectedTab == SYNC);
    styleTab(diagAudioButton, selectedTab == AUDIO);
    styleTab(diagResultsButton, selectedTab == RESULTS);
}

void diagnosticsView::refreshDiagnostics()
{
    using DebugText::set;
    char labels[4][64] = {};
    char values[4][64] = {};
    char hint[64] = {};
    char header[64] = {};
    const char* tab = selectedTab == SYNC ? "SYNC" : selectedTab == AUDIO ? "AUDIO" : "RESULTS";
#ifndef SIMULATOR
    TimeSyncStatus sync = {};
    const bool haveSync = TimeSync_GetStatus(&sync);
    const char board = !haveSync || sync.role == TIME_SYNC_DISCOVERY ? '?' :
                       sync.role == TIME_SYNC_MASTER ? 'A' : 'B';
    std::snprintf(header, sizeof(header), "%c %s; %s", board,
                  haveSync && sync.locked ? "LOCKED" : "WAIT SYNC", tab);
    if (selectedTab == SYNC) {
        std::snprintf(labels[0], sizeof(labels[0]), "ROLE / STATE");
        std::snprintf(values[0], sizeof(values[0]), "%c / %s", board,
                      haveSync ? syncState(sync.state) : "NO SNAPSHOT");
        std::snprintf(labels[1], sizeof(labels[1]), "OFFSET / PATH");
        if (haveSync && (sync.sample_valid || sync.locked)) {
            char offset[24];
            DebugText::integer(offset, sizeof(offset), sync.offset_ns);
            std::snprintf(values[1], sizeof(values[1]), "%s / %lu ns", offset, (unsigned long)sync.delay_ns);
        } else std::snprintf(values[1], sizeof(values[1]), "NA");
        std::snprintf(labels[2], sizeof(labels[2]), "FREQ / SAMPLES");
        std::snprintf(values[2], sizeof(values[2]), "%ld ppb / %lu", (long)sync.frequency_ppb,
                      (unsigned long)sync.samples);
        std::snprintf(labels[3], sizeof(labels[3]), "REJ TX PPSMISS");
        std::snprintf(values[3], sizeof(values[3]), "%lu / %lu / %lu", (unsigned long)sync.rejected,
                      (unsigned long)sync.tx_failures, (unsigned long)sync.pps_missed);
        std::snprintf(hint, sizeof(hint), "OFFSET IS SYNC ESTIMATE");
    } else if (selectedTab == AUDIO) {
        AudioArrivalEvent event = {};
        const bool haveEvent = AudioArrival_GetLatest(&event) &&
                               (sync.locked || !event.synchronized);
        std::snprintf(labels[0], sizeof(labels[0]), "LOCAL EVENT");
        std::snprintf(labels[1], sizeof(labels[1]), "PEAK NOISE THR");
        std::snprintf(labels[2], sizeof(labels[2]), "ARRIVAL / S");
        std::snprintf(labels[3], sizeof(labels[3]), "INTERVAL / S");
        if (haveEvent) {
            std::snprintf(values[0], sizeof(values[0]), "%lu %s%s", (unsigned long)event.sequence,
                          event.synchronized ? "LOCKED" : "LOCAL", event.clipped ? " CLIP" : "");
            std::snprintf(values[1], sizeof(values[1]), "%lu / %lu / %lu", (unsigned long)event.peak,
                          (unsigned long)event.noise, (unsigned long)event.threshold);
            DebugText::seconds(values[2], sizeof(values[2]), event.time_ns);
            if (event.interval_valid) DebugText::seconds(values[3], sizeof(values[3]), event.interval_ns);
            else std::snprintf(values[3], sizeof(values[3]), "NA");
        } else {
            for (unsigned i = 0; i < 4; ++i) std::snprintf(values[i], sizeof(values[i]), "NA");
        }
        std::snprintf(hint, sizeof(hint), "PEAK: TRIGGER CONFIRM WINDOW");
    } else {
        RangingDiagnostics diagnostics = {};
        const bool haveDiagnostics = Ranging_GetDiagnostics(&diagnostics) && diagnostics.valid && sync.locked;
        if (board == 'B') {
            RangingStatus ranging = {};
            Ranging_GetStatus(&ranging);
            std::snprintf(labels[0], sizeof(labels[0]), "COMPUTATION");
            std::snprintf(values[0], sizeof(values[0]), "DSP ON MASTER A");
            std::snprintf(labels[1], sizeof(labels[1]), "FRONT");
            std::snprintf(values[1], sizeof(values[1]), "DIAGNOSTIC ON A");
            std::snprintf(labels[2], sizeof(labels[2]), "LAST RESULT");
            if (ranging.valid && ranging.locked)
                std::snprintf(values[2], sizeof(values[2]), "%lu mm Q%lu", (unsigned long)ranging.distance_mm,
                              (unsigned long)ranging.quality_permille);
            else std::snprintf(values[2], sizeof(values[2]), "NA");
            std::snprintf(labels[3], sizeof(labels[3]), "OK / REJECT");
            std::snprintf(values[3], sizeof(values[3]), "%lu / %lu", (unsigned long)ranging.accepted,
                          (unsigned long)ranging.rejected);
            std::snprintf(hint, sizeof(hint), "LOCAL PCM IS CAPTURED ON B");
        } else if (haveDiagnostics) {
            const RangeEstimate& l = diagnostics.long_window;
            const RangeEstimate& f = diagnostics.front_window;
            std::snprintf(labels[0], sizeof(labels[0]), "LONG %u:%u", (unsigned)l.window_begin, (unsigned)l.window_end);
            scoreText(values[0], sizeof(values[0]), l);
            std::snprintf(labels[1], sizeof(labels[1]), "LONG CANDIDATE");
            candidateText(values[1], sizeof(values[1]), l);
            std::snprintf(labels[2], sizeof(labels[2]), "FRONT %u:%u", (unsigned)f.window_begin, (unsigned)f.window_end);
            scoreText(values[2], sizeof(values[2]), f);
            std::snprintf(labels[3], sizeof(labels[3]), "FRONT CANDIDATE");
            candidateText(values[3], sizeof(values[3]), f);
            std::snprintf(hint, sizeof(hint), "A%lu B%lu; FRONT DIAG ONLY", (unsigned long)diagnostics.a_sequence,
                          (unsigned long)diagnostics.b_sequence);
        } else {
            std::snprintf(labels[0], sizeof(labels[0]), "LONG");
            std::snprintf(labels[1], sizeof(labels[1]), "LONG CANDIDATE");
            std::snprintf(labels[2], sizeof(labels[2]), "FRONT DIAG");
            std::snprintf(labels[3], sizeof(labels[3]), "FRONT CANDIDATE");
            for (unsigned i = 0; i < 4; ++i) std::snprintf(values[i], sizeof(values[i]), "NA");
            std::snprintf(hint, sizeof(hint), "WAIT FOR PAIRED AUDIO");
        }
    }
#else
    std::snprintf(header, sizeof(header), "DEMO; %s", tab);
    const char* demoLabels[3][4] = {
        {"ROLE / STATE", "OFFSET / PATH", "FREQ / SAMPLES", "REJ TX PPSMISS"},
        {"LOCAL EVENT", "PEAK NOISE THR", "ARRIVAL / S", "INTERVAL / S"},
        {"LONG 128:1920", "LONG CANDIDATE", "FRONT 128:384", "FRONT CANDIDATE"}
    };
    const char* demoValues[3][4] = {
        {"A / LOCKED ESTIMATE", "120 / 3000 ns", "-600 ppb / 100", "0 / 0 / 0"},
        {"1 LOCKED", "258 / 4 / 128", "9.095623972", "NA"},
        {"Q437 M18 LOW Q", "692 mm k12 -2016000 ns", "Q962 M34 PASS", "699 mm k11 -2036858 ns"}
    };
    for (unsigned i = 0; i < 4; ++i) {
        std::snprintf(labels[i], sizeof(labels[i]), "%s", demoLabels[selectedTab][i]);
        std::snprintf(values[i], sizeof(values[i]), "%s", demoValues[selectedTab][i]);
    }
    std::snprintf(hint, sizeof(hint), "SIMULATOR DEMO; FRONT DIAG ONLY");
#endif
    set(diagnosticsSyncText, diagnosticsSyncTextBuffer, header);
    set(diagRow1LabelText, diagRow1LabelTextBuffer, labels[0]);
    set(diagRow1ValueText, diagRow1ValueTextBuffer, values[0]);
    set(diagRow2LabelText, diagRow2LabelTextBuffer, labels[1]);
    set(diagRow2ValueText, diagRow2ValueTextBuffer, values[1]);
    set(diagRow3LabelText, diagRow3LabelTextBuffer, labels[2]);
    set(diagRow3ValueText, diagRow3ValueTextBuffer, values[2]);
    set(diagRow4LabelText, diagRow4LabelTextBuffer, labels[3]);
    set(diagRow4ValueText, diagRow4ValueTextBuffer, values[3]);
    set(diagnosticHintText, diagnosticHintTextBuffer, hint);
}
