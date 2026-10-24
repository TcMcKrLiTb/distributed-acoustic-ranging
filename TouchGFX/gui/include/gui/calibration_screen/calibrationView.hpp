#ifndef CALIBRATIONVIEW_HPP
#define CALIBRATIONVIEW_HPP

#include <gui_generated/calibration_screen/calibrationViewBase.hpp>
#include <gui/calibration_screen/calibrationPresenter.hpp>

class calibrationView : public calibrationViewBase
{
public:
    calibrationView();
    virtual ~calibrationView() {}
    virtual void setupScreen();
    virtual void tearDownScreen();
protected:
};

#endif // CALIBRATIONVIEW_HPP
