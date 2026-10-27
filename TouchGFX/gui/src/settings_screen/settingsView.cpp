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
    bool automatic, sensorAvailable, sourceA;
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
bool sourceAwaiting = false, requestedAutomatic = false;
bool observedAutomatic = false;

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
        snapshot.automatic = temperature.automatic;
        snapshot.sensorAvailable = temperature.sensor_available;
        snapshot.sourceA = temperature.source_a;
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
    if (sourceAwaiting && !snapshot.pending) {
        const bool selected = snapshot.automatic == requestedAutomatic &&
            (!requestedAutomatic || snapshot.sourceA == (snapshot.board == 'A'));
        if (selected && snapshot.synchronized) sourceAwaiting = false;
        else {
            sourceAwaiting = false;
            editor.rejected = true;
        }
    }
    if (snapshot.automatic || observedAutomatic != snapshot.automatic) {
        editor.draft = snapshot.temperature;
        editor.initialized = true;
        editor.dirty = editor.awaiting = editor.rejected = false;
    }
    observedAutomatic = snapshot.automatic;
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

template <typename Button>
void styleSourceButton(Button& button, bool selected, bool enabled)
{
    button.setTouchable(enabled);
    button.setBoxWithBorderColors(
        selected ? touchgfx::Color::getColorFromRGB(22, 77, 122) :
        enabled ? touchgfx::Color::getColorFromRGB(255, 255, 255) : touchgfx::Color::getColorFromRGB(211, 220, 231),
        touchgfx::Color::getColorFromRGB(0, 153, 204),
        touchgfx::Color::getColorFromRGB(211, 220, 231),
        touchgfx::Color::getColorFromRGB(51, 102, 153));
    button.setTextColors(
        selected ? touchgfx::Color::getColorFromRGB(255, 255, 255) :
        enabled ? touchgfx::Color::getColorFromRGB(10, 10, 10) : touchgfx::Color::getColorFromRGB(138, 128, 128),
        touchgfx::Color::getColorFromRGB(10, 10, 10));
    button.invalidate();
}
}

settingsView::settingsView() : refreshTicks(0), applyEnabled(true), sourceButtonState(0xFFFFFFFFU),
    temperatureCallback(this, &settingsView::temperatureClicked),
    unitCallback(this, &settingsView::unitClicked)
{
    unitMeterButton.setAction(unitCallback);
    unitCmButton.setAction(unitCallback);
    temperatureMinusButton.setAction(temperatureCallback);
    temperaturePlusButton.setAction(temperatureCallback);
    applySettingsButton.setAction(temperatureCallback);
    tempAutoButton.setAction(temperatureCallback);
    tempManualButton.setAction(temperatureCallback);
    tempAutoButton.setTouchable(false);
    tempManualButton.setTouchable(false);
    // Replace the Designer's static "no sensor" hint in the derived view, so
    // regenerating the layout never overwrites the live availability message.
    tempSourceHintText.setVisible(false);
    sourceHintText.setPosition(tempSourceHintText.getX(), tempSourceHintText.getY(),
                               tempSourceHintText.getWidth(), tempSourceHintText.getHeight());
    sourceHintText.setTypedText(configSyncText.getTypedText());
    sourceHintText.setColor(touchgfx::Color::getColorFromRGB(78, 96, 117));
    sourceHintBuffer[0] = 0;
    sourceHintText.setWildcard(sourceHintBuffer);
    add(sourceHintText);
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
    refreshUnitButtons();
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

void settingsView::unitClicked(const touchgfx::AbstractButtonContainer& source)
{
    if (&source == &unitMeterButton) presenter->setDistanceUnit(DistanceUnit::Meters);
    else if (&source == &unitCmButton) presenter->setDistanceUnit(DistanceUnit::Centimeters);
    else return;
    refreshUnitButtons();
}

void settingsView::refreshUnitButtons()
{
    const DistanceUnit unit = presenter->getDistanceUnit();
    styleSourceButton(unitMeterButton, unit == DistanceUnit::Meters, true);
    styleSourceButton(unitCmButton, unit == DistanceUnit::Centimeters, true);
}

void settingsView::temperatureClicked(const touchgfx::AbstractButtonContainer& source)
{
    const TemperatureSnapshot snapshot = temperatureSnapshot();
    followSnapshot(snapshot);
    if (&source == &tempAutoButton || &source == &tempManualButton) {
        const bool automatic = &source == &tempAutoButton;
        const bool canChange = snapshot.available && snapshot.locked && snapshot.synchronized &&
                               !snapshot.pending && !sourceAwaiting &&
                               (!automatic || snapshot.sensorAvailable);
        bool accepted = false;
#ifndef SIMULATOR
        if (canChange) accepted = Ranging_SetTemperatureSource(automatic);
#else
        (void)canChange; // No physical sensor is advertised by the simulator.
#endif
        if (accepted) {
            sourceAwaiting = true;
            requestedAutomatic = automatic;
            editor.draft = snapshot.temperature;
            editor.dirty = editor.awaiting = editor.rejected = false;
        } else if (canChange) editor.rejected = true;
    } else if (&source == &temperatureMinusButton || &source == &temperaturePlusButton) {
        if (snapshot.automatic || sourceAwaiting) return;
        // Snap toward the pressed direction; normalize the remainder below zero.
        const int remainder = (editor.draft % 5 + 5) % 5;
        const int next = editor.draft + (&source == &temperaturePlusButton ?
            5 - remainder : -(remainder ? remainder : 5));
        if (next >= minimumTemperature && next <= maximumTemperature) {
            editor.draft = static_cast<int16_t>(next);
            editor.initialized = true;
            editor.dirty = !snapshot.available || editor.awaiting || editor.draft != snapshot.temperature;
            editor.rejected = false;
        }
    } else if (&source == &applySettingsButton && !snapshot.automatic && !sourceAwaiting && editor.dirty && !editor.awaiting &&
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
    const bool syncing = snapshot.pending || editor.awaiting || sourceAwaiting;
    const bool autoSelected = sourceAwaiting ? requestedAutomatic : snapshot.automatic;
    const bool canSelect = snapshot.available && snapshot.locked && snapshot.synchronized && !syncing;
    const bool manualEnabled = canSelect && snapshot.automatic;
    const bool autoEnabled = canSelect && snapshot.sensorAvailable &&
        (!snapshot.automatic || snapshot.sourceA != (snapshot.board == 'A'));
    const bool stepsEnabled = !autoSelected && !sourceAwaiting && !snapshot.automatic;
    const unsigned buttons = (autoSelected ? 1U : 0U) | (manualEnabled ? 2U : 0U) |
        (autoEnabled ? 4U : 0U) | (stepsEnabled ? 8U : 0U);
    if (buttons != sourceButtonState) {
        sourceButtonState = buttons;
        styleSourceButton(tempManualButton, !autoSelected, manualEnabled);
        styleSourceButton(tempAutoButton, autoSelected, autoEnabled);
        styleSourceButton(temperatureMinusButton, false, stepsEnabled);
        styleSourceButton(temperaturePlusButton, false, stepsEnabled);
    }
    if (sourceAwaiting) std::snprintf(text, sizeof(text), "SOURCE SYNCING");
    else if (snapshot.automatic) std::snprintf(text, sizeof(text), "AUTO FROM %c; 1 HZ", snapshot.sourceA ? 'A' : 'B');
    else if (snapshot.sensorAvailable) std::snprintf(text, sizeof(text), "SENSOR READY");
    else std::snprintf(text, sizeof(text), "NO SENSOR; PROBE 5S");
    setText(sourceHintText, sourceHintBuffer, text);
    if (!snapshot.available) std::snprintf(text, sizeof(text), "CONFIG NA; NOT APPLIED");
    else if (syncing) std::snprintf(text, sizeof(text), "SYNCING; DRAFT %s C", draft);
    else if (!snapshot.locked) std::snprintf(text, sizeof(text), editor.dirty ? "NOT APPLIED; WAIT SYNC" : "WAIT SYNC");
    else if (!snapshot.synchronized) std::snprintf(text, sizeof(text), editor.dirty ? "NOT APPLIED; CONFIG SYNC" : "CONFIG SYNC");
    else if (editor.rejected) std::snprintf(text, sizeof(text), "NOT APPLIED; RETRY APPLY");
    else if (editor.dirty) {
        temperatureText(active, sizeof(active), snapshot.temperature);
        std::snprintf(text, sizeof(text), "NOT APPLIED; ACTIVE %s C", active);
    } else if (snapshot.automatic) std::snprintf(text, sizeof(text), "AUTO %c; APPLIED R%lu",
                  snapshot.sourceA ? 'A' : 'B', (unsigned long)snapshot.revision);
    else std::snprintf(text, sizeof(text), "APPLIED R%lu", (unsigned long)snapshot.revision);
    setText(configSyncText, configSyncTextBuffer, text);
    enableApply(!snapshot.automatic && !sourceAwaiting && snapshot.available && snapshot.locked && snapshot.synchronized && !syncing && editor.dirty);
}

#ifdef SIMULATOR
int16_t settingsView::demoTemperature() { return appliedDemoTemperature; }
uint32_t settingsView::demoSoundSpeed() { return previewSpeed(appliedDemoTemperature); }
#endif
