#ifndef DIAGNOSTICSVIEW_HPP
#define DIAGNOSTICSVIEW_HPP

#include <gui_generated/diagnostics_screen/diagnosticsViewBase.hpp>
#include <gui/diagnostics_screen/diagnosticsPresenter.hpp>

class diagnosticsView : public diagnosticsViewBase
{
public:
    diagnosticsView();
    virtual ~diagnosticsView() {}
    virtual void setupScreen();
    virtual void tearDownScreen();
    virtual void handleTickEvent();
protected:
    enum Tab { SYNC, AUDIO, RESULTS };
    Tab selectedTab;
    unsigned refreshTicks;
    touchgfx::Callback<diagnosticsView, const touchgfx::AbstractButtonContainer&> tabCallback;
    void tabSelected(const touchgfx::AbstractButtonContainer& source);
    void updateTabButtons();
    void refreshDiagnostics();
};

#endif // DIAGNOSTICSVIEW_HPP
