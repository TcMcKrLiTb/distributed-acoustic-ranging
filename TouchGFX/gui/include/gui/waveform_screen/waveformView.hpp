#ifndef WAVEFORMVIEW_HPP
#define WAVEFORMVIEW_HPP

#include <gui_generated/waveform_screen/waveformViewBase.hpp>
#include <gui/waveform_screen/waveformPresenter.hpp>

class waveformView : public waveformViewBase
{
public:
    waveformView();
    virtual ~waveformView() {}
    virtual void setupScreen();
    virtual void tearDownScreen();
protected:
};

#endif // WAVEFORMVIEW_HPP
