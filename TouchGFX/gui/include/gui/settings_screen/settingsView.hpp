#ifndef SETTINGSVIEW_HPP
#define SETTINGSVIEW_HPP

#include <gui_generated/settings_screen/settingsViewBase.hpp>
#include <gui/settings_screen/settingsPresenter.hpp>
#include <stdint.h>

class settingsView : public settingsViewBase
{
public:
    settingsView();
    virtual ~settingsView() {}
    virtual void setupScreen();
    virtual void tearDownScreen();
    virtual void handleTickEvent();
#ifdef SIMULATOR
    // Share the simulator's local applied value with the main screen.
    static int16_t demoTemperature();
    static uint32_t demoSoundSpeed();
#endif
protected:
    unsigned refreshTicks;
    bool applyEnabled;
    unsigned sourceButtonState;
    touchgfx::TextAreaWithOneWildcard sourceHintText;
    touchgfx::Unicode::UnicodeChar sourceHintBuffer[64];
    touchgfx::Callback<settingsView, const touchgfx::AbstractButtonContainer&> temperatureCallback;
    void temperatureClicked(const touchgfx::AbstractButtonContainer& source);
    void refreshTemperature();
    void enableApply(bool enabled);
};

#endif // SETTINGSVIEW_HPP
