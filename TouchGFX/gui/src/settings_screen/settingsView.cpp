#include <gui/settings_screen/settingsView.hpp>
#include <touchgfx/Color.hpp>
#include <cstddef>
#include <cstdio>
#ifdef SIMULATOR
#include <cmath>
#endif
#ifndef SIMULATOR
#include "Ranging/ranging.h"
#include "Ranging/range_temperature.h"
#include "TimeSync/time_sync.h"
#endif

namespace
{
#ifdef SIMULATOR
const int16_t minimumTemperature = -200;
const int16_t maximumTemperature = 500;
const int16_t defaultTemperature = 200;
int16_t appliedDemoTemperature = defaultTemperature;
uint32_t demoRevision = 1;
#else
const int16_t minimumTemperature = RANGE_TEMPERATURE_MIN_X10;
const int16_t maximumTemperature = RANGE_TEMPERATURE_MAX_X10;
const int16_t defaultTemperature = RANGE_TEMPERATURE_DEFAULT_X10;
#endif

struct TemperatureSnapshot
{
    int16_t temperature;
    uint32_t speed, revision;
    bool available, locked, synchronized, pending;
    char board;
};

// The editor is owned only by the GUI task and survives leaving/re-entering the
// page. Backend snapshots never overwrite a dirty or submitted local draft.
struct TemperatureEditor
{
    int16_t draft, submittedTemperature;
    uint32_t submittedRevision;
    bool initialized, dirty, awaiting, rejected;
};
TemperatureEditor editor = {defaultTemperature, defaultTemperature, 0, false, false, false, false};

uint32_t previewSpeed(int16_t temperature)
{
#ifdef SIMULATOR
    if (temperature < minimumTemperature || temperature > maximumTemperature) return 0;
    // Same 20 C reference model as the firmware; no App symbols are linked.
    return static_cast<uint32_t>(std::floor(343420.0 *
        std::sqrt((273.15 + temperature / 10.0) / 293.15) + 0.5));
#else
    return Range_TemperatureSoundSpeed(temperature);
#endif
}

TemperatureSnapshot temperatureSnapshot()
{
    TemperatureSnapshot snapshot = {};
#ifdef SIMULATOR
    snapshot.temperature = appliedDemoTemperature;
    snapshot.speed = previewSpeed(appliedDemoTemperature);
    snapshot.revision = demoRevision;
    snapshot.available = snapshot.locked = snapshot.synchronized = true;
    snapshot.board = 'A';
#else
    RangingTemperatureStatus temperature = {};
    snapshot.available = Ranging_GetTemperature(&temperature);
    if (snapshot.available) {
        snapshot.temperature = temperature.temperature_c_x10;
        snapshot.speed = temperature.sound_speed_mm_s;
        snapshot.revision = temperature.revision;
        snapshot.synchronized = temperature.synchronized;
        snapshot.pending = temperature.pending;
    }
    TimeSyncStatus sync = {};
    if (TimeSync_GetStatus(&sync)) {
        snapshot.locked = sync.locked;
        snapshot.board = sync.role == TIME_SYNC_MASTER ? 'A' : sync.role == TIME_SYNC_SLAVE ? 'B' : '?';
    } else snapshot.board = '?';
#endif
    return snapshot;
}

void followSnapshot(const TemperatureSnapshot& snapshot)
{
    if (!snapshot.available) return;
    if (!editor.initialized) {
        editor.draft = snapshot.temperature;
        editor.initialized = true;
    }
    if (editor.awaiting) {
        if (!snapshot.pending && snapshot.synchronized && snapshot.temperature == editor.submittedTemperature) {
            editor.awaiting = false;
            editor.dirty = editor.draft != snapshot.temperature;
            editor.rejected = false;
        } else if (!snapshot.pending && (!snapshot.locked ||
                   (snapshot.synchronized && snapshot.revision != editor.submittedRevision))) {
            editor.awaiting = false;
            editor.dirty = true;
            editor.rejected = true;
        }
    }
    if (!editor.dirty && !editor.awaiting) editor.draft = snapshot.temperature;
}

void temperatureText(char* out, std::size_t size, int16_t value)
{
    const int magnitude = value < 0 ? -static_cast<int>(value) : value;
    std::snprintf(out, size, "%s%d.%d", value < 0 ? "-" : "", magnitude / 10, magnitude % 10);
}

template <std::size_t N>
void setText(touchgfx::TextAreaWithOneWildcard& widget,
             touchgfx::Unicode::UnicodeChar (&buffer)[N], const char* text)
{
    std::size_t i = 0;
    while (i < N - 1 && text[i] && buffer[i] == static_cast<touchgfx::Unicode::UnicodeChar>(text[i])) ++i;
    if (!text[i] && buffer[i] == 0) return;
    touchgfx::Unicode::strncpy(buffer, text, N);
    buffer[N - 1] = 0;
    widget.invalidate();
}
}

settingsView::settingsView() : refreshTicks(0), applyEnabled(true),
    temperatureCallback(this, &settingsView::temperatureClicked)
{
    temperatureMinusButton.setAction(temperatureCallback);
    temperaturePlusButton.setAction(temperatureCallback);
    applySettingsButton.setAction(temperatureCallback);
    // Manual input is the implemented source; the board has no ambient sensor.
    tempAutoButton.setTouchable(false);
    tempManualButton.setTouchable(false);
    enableApply(false);
}

void settingsView::setupScreen()
{
    settingsViewBase::setupScreen();
#ifndef SIMULATOR
    demoText.setVisible(false);
#else
    demoText.setVisible(true);
#endif
    refreshTicks = 0;
    refreshTemperature();
}

void settingsView::tearDownScreen()
{
    settingsViewBase::tearDownScreen();
}

void settingsView::handleTickEvent()
{
    settingsViewBase::handleTickEvent();
    if (++refreshTicks < 6U) return;
    refreshTicks = 0;
    refreshTemperature();
}

void settingsView::enableApply(bool enabled)
{
    if (enabled == applyEnabled) return;
    applyEnabled = enabled;
    applySettingsButton.setTouchable(enabled);
    applySettingsButton.setBoxWithBorderColors(
        enabled ? touchgfx::Color::getColorFromRGB(22, 77, 122) : touchgfx::Color::getColorFromRGB(211, 220, 231),
        touchgfx::Color::getColorFromRGB(0, 153, 204),
        touchgfx::Color::getColorFromRGB(211, 220, 231),
        touchgfx::Color::getColorFromRGB(51, 102, 153));
    applySettingsButton.setTextColors(
        enabled ? touchgfx::Color::getColorFromRGB(255, 255, 255) : touchgfx::Color::getColorFromRGB(78, 96, 117),
        touchgfx::Color::getColorFromRGB(10, 10, 10));
    applySettingsButton.invalidate();
}

void settingsView::temperatureClicked(const touchgfx::AbstractButtonContainer& source)
{
    const TemperatureSnapshot snapshot = temperatureSnapshot();
    followSnapshot(snapshot);
    if (&source == &temperatureMinusButton || &source == &temperaturePlusButton) {
        const int next = editor.draft + (&source == &temperaturePlusButton ? 5 : -5);
        if (next >= minimumTemperature && next <= maximumTemperature) {
            editor.draft = static_cast<int16_t>(next);
            editor.initialized = true;
            editor.dirty = !snapshot.available || editor.awaiting || editor.draft != snapshot.temperature;
            editor.rejected = false;
        }
    } else if (&source == &applySettingsButton && editor.dirty && !editor.awaiting &&
               snapshot.available && snapshot.locked && snapshot.synchronized && !snapshot.pending) {
        bool accepted;
#ifdef SIMULATOR
        appliedDemoTemperature = editor.draft;
        ++demoRevision;
        accepted = true;
#else
        accepted = Ranging_SetTemperature(editor.draft);
#endif
        if (accepted) {
            editor.submittedTemperature = editor.draft;
            editor.submittedRevision = snapshot.revision;
            editor.awaiting = true;
            editor.rejected = false;
        } else editor.rejected = true;
    }
    refreshTicks = 0;
    refreshTemperature();
}

void settingsView::refreshTemperature()
{
    const TemperatureSnapshot snapshot = temperatureSnapshot();
    followSnapshot(snapshot);
    char text[64], draft[16], active[16];
    temperatureText(draft, sizeof(draft), editor.draft);
    setText(temperatureValueText, temperatureValueTextBuffer, draft);
    const uint32_t speed = previewSpeed(editor.draft);
    if (speed) {
        std::snprintf(text, sizeof(text), "c %lu.%03lu m/s", (unsigned long)(speed / 1000U),
                      (unsigned long)(speed % 1000U));
        setText(soundSpeedText, soundSpeedTextBuffer, text);
    } else setText(soundSpeedText, soundSpeedTextBuffer, "c NA");
#ifdef SIMULATOR
    std::snprintf(text, sizeof(text), "DEMO; LOCAL CONFIG");
#else
    std::snprintf(text, sizeof(text), "BOARD %c; %s", snapshot.board, snapshot.locked ? "SYNC LOCKED" : "WAIT SYNC");
#endif
    setText(settingsSyncText, settingsSyncTextBuffer, text);
    const bool syncing = snapshot.pending || editor.awaiting;
    if (!snapshot.available) std::snprintf(text, sizeof(text), "CONFIG NA; NOT APPLIED");
    else if (syncing) std::snprintf(text, sizeof(text), "SYNCING; DRAFT %s C", draft);
    else if (!snapshot.locked) std::snprintf(text, sizeof(text), editor.dirty ? "NOT APPLIED; WAIT SYNC" : "WAIT SYNC");
    else if (!snapshot.synchronized) std::snprintf(text, sizeof(text), editor.dirty ? "NOT APPLIED; CONFIG SYNC" : "CONFIG SYNC");
    else if (editor.rejected) std::snprintf(text, sizeof(text), "NOT APPLIED; RETRY APPLY");
    else if (editor.dirty) {
        temperatureText(active, sizeof(active), snapshot.temperature);
        std::snprintf(text, sizeof(text), "NOT APPLIED; ACTIVE %s C", active);
    } else std::snprintf(text, sizeof(text), "APPLIED R%lu", (unsigned long)snapshot.revision);
    setText(configSyncText, configSyncTextBuffer, text);
    enableApply(snapshot.available && snapshot.locked && snapshot.synchronized && !syncing && editor.dirty);
}

#ifdef SIMULATOR
int16_t settingsView::demoTemperature() { return appliedDemoTemperature; }
uint32_t settingsView::demoSoundSpeed() { return previewSpeed(appliedDemoTemperature); }
#endif
