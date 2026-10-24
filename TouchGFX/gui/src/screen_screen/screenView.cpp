#include <gui/screen_screen/screenView.hpp>
#include <cstddef>
#include <cstdio>
#ifndef SIMULATOR
#include "Ranging/ranging.h"
#endif

namespace
{
// The Designer owns widget geometry; repaint only when its text changes.
template <std::size_t N>
void setText(touchgfx::TextAreaWithOneWildcard& widget,
             touchgfx::Unicode::UnicodeChar (&buffer)[N], const char* text)
{
    std::size_t i = 0;
    while (i < N - 1 && text[i] &&
           buffer[i] == static_cast<touchgfx::Unicode::UnicodeChar>(text[i]))
    {
        ++i;
    }
    if (!text[i] && buffer[i] == 0)
    {
        return;
    }
    touchgfx::Unicode::strncpy(buffer, text, N);
    buffer[N - 1] = 0;
    widget.invalidate();
}
}

screenView::screenView()
{

}

void screenView::setupScreen()
{
    screenViewBase::setupScreen();
    handleTickEvent();
}

void screenView::handleTickEvent()
{
    screenViewBase::handleTickEvent();
#ifndef SIMULATOR
    RangingStatus s = {};
    Ranging_GetStatus(&s);
    char text[64];
    std::snprintf(text, sizeof(text), "BOARD %c  %s", s.master ? 'A' : 'B', s.locked ? "SYNC LOCKED" : "WAIT SYNC");
    setText(boardStatusText, boardStatusTextBuffer, text);
    if (s.valid) {
        std::snprintf(text, sizeof(text), "%lu.%03lu M", (unsigned long)(s.distance_mm / 1000U), (unsigned long)(s.distance_mm % 1000U));
        setText(distanceText, distanceTextBuffer, text);
        std::snprintf(text, sizeof(text), "SOURCE %c SIDE", s.source_a ? 'A' : 'B');
        setText(directionText, directionTextBuffer, text);
    } else {
        setText(distanceText, distanceTextBuffer, "-.--- M");
        setText(directionText, directionTextBuffer, "SOURCE -");
    }
    const char* states[] = {"WAIT SYNC", "WAIT SOUND", "TRANSFER", "CORRELATING", "WAIT DISPLAY"};
    std::snprintf(text, sizeof(text), "%s  ID %lu", states[(unsigned)s.state < 5U ? s.state : 0], (unsigned long)s.sequence);
    setText(rangeStateText, rangeStateTextBuffer, text);
    std::snprintf(text, sizeof(text), "Q %lu.%lu%%  OK %lu  REJECT %lu", (unsigned long)(s.quality_permille / 10U),
                  (unsigned long)(s.quality_permille % 10U), (unsigned long)s.accepted, (unsigned long)s.rejected);
    setText(statisticsText, statisticsTextBuffer, text);
#else
    setText(boardStatusText, boardStatusTextBuffer, "BOARD A  SYNC LOCKED");
    setText(distanceText, distanceTextBuffer, "1.234 M");
    setText(directionText, directionTextBuffer, "SOURCE A SIDE");
    setText(rangeStateText, rangeStateTextBuffer, "WAIT SOUND  ID 1");
    setText(statisticsText, statisticsTextBuffer, "Q 99.0%  OK 1  REJECT 0");
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
