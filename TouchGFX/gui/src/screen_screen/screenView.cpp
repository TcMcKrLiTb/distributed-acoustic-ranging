#include <gui/screen_screen/screenView.hpp>
#include <cstdio>
#ifndef SIMULATOR
#include "Ranging/ranging.h"
#endif

screenView::screenView()
{

}

void screenView::setupScreen()
{
    screenViewBase::setupScreen();
    button1.setXY(370, 232);
    add(rangeDisplay);
    rangeDisplay.setLine(0, "DISTRIBUTED ACOUSTIC RANGING");
    handleTickEvent();
}

void screenView::handleTickEvent()
{
#ifndef SIMULATOR
    RangingStatus s = {};
    Ranging_GetStatus(&s);
    char text[40];
    std::snprintf(text, sizeof(text), "BOARD %c  %s", s.master ? 'A' : 'B', s.locked ? "SYNC LOCKED" : "WAIT SYNC");
    rangeDisplay.setLine(1, text);
    if (s.valid) {
        std::snprintf(text, sizeof(text), "%lu.%03lu M", (unsigned long)(s.distance_mm / 1000U), (unsigned long)(s.distance_mm % 1000U));
        rangeDisplay.setLine(2, text);
        std::snprintf(text, sizeof(text), "SOURCE %c SIDE", s.source_a ? 'A' : 'B');
        rangeDisplay.setLine(3, text);
    } else {
        rangeDisplay.setLine(2, "-.--- M");
        rangeDisplay.setLine(3, "SOURCE -");
    }
    const char* states[] = {"WAIT SYNC", "WAIT SOUND", "TRANSFER", "CORRELATING", "WAIT DISPLAY"};
    std::snprintf(text, sizeof(text), "%s  ID %lu", states[(unsigned)s.state < 5U ? s.state : 0], (unsigned long)s.sequence);
    rangeDisplay.setLine(4, text);
    std::snprintf(text, sizeof(text), "Q %lu.%lu%%  OK %lu  REJECT %lu", (unsigned long)(s.quality_permille / 10U),
                  (unsigned long)(s.quality_permille % 10U), (unsigned long)s.accepted, (unsigned long)s.rejected);
    rangeDisplay.setLine(5, text);
#else
    rangeDisplay.setLine(1, "BOARD A  SYNC LOCKED");
    rangeDisplay.setLine(2, "1.234 M");
    rangeDisplay.setLine(3, "SOURCE A SIDE");
    rangeDisplay.setLine(4, "WAIT SOUND  ID 1");
    rangeDisplay.setLine(5, "Q 99.0%  OK 1  REJECT 0");
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
