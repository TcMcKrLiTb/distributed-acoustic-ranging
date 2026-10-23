#ifndef RANGE_DISPLAY_HPP
#define RANGE_DISPLAY_HPP
#include <touchgfx/widgets/Widget.hpp>
#include <touchgfx/hal/HAL.hpp>
#include <touchgfx/lcd/LCD.hpp>
#include <touchgfx/Color.hpp>
#include <cstring>

/* A small code-resident ASCII font. The original generated font contains only
   '?'. This display therefore needs no regenerated font/QSPI resources. */
class RangeDisplay : public touchgfx::Widget {
public:
    RangeDisplay() { std::memset(lines, 0, sizeof(lines)); setPosition(0, 0, 480, 228); }
    void setLine(unsigned row, const char* text) {
        if (row >= 6 || std::strncmp(lines[row], text, sizeof(lines[row]) - 1) == 0) return;
        std::strncpy(lines[row], text, sizeof(lines[row]) - 1);
        lines[row][sizeof(lines[row]) - 1] = 0;
        touchgfx::Rect dirty(0, y(row), 480, 7 * scale(row) + 2);
        invalidateRect(dirty);
    }
    virtual touchgfx::Rect getSolidRect() const { return touchgfx::Rect(0, 0, getWidth(), getHeight()); }
    virtual void draw(const touchgfx::Rect& invalidatedArea) const {
        fill(invalidatedArea, touchgfx::Color::getColorFromRGB(248, 250, 252));
        for (unsigned row = 0; row < 6; ++row) {
            const int s = scale(row);
            touchgfx::colortype color = row == 2 ? touchgfx::Color::getColorFromRGB(0, 96, 128) :
                                                   touchgfx::Color::getColorFromRGB(24, 40, 56);
            int x = 14;
            for (const char* p = lines[row]; *p && x + 5 * s < getWidth(); ++p, x += 6 * s) {
                const uint8_t* g = glyph(*p);
                for (int gy = 0; gy < 7; ++gy) for (int gx = 0; gx < 5; ++gx) {
                    if (g[gy] & (1U << (4 - gx))) {
                        touchgfx::Rect pixel(x + gx * s, y(row) + gy * s, s, s);
                        pixel &= invalidatedArea;
                        if (!pixel.isEmpty()) fill(pixel, color);
                    }
                }
            }
        }
    }
private:
    char lines[6][40];
    static int y(unsigned r) { const int positions[] = {8, 37, 85, 144, 180, 210}; return positions[r]; }
    static int scale(unsigned r) { const int sizes[] = {2, 3, 6, 3, 2, 2}; return sizes[r]; }
    void fill(touchgfx::Rect rect, touchgfx::colortype color) const {
        translateRectToAbsolute(rect);
        touchgfx::HAL::lcd().fillRect(rect, color, 255);
    }
    static const uint8_t* glyph(char c) {
        struct Glyph { char c; uint8_t row[7]; };
        static const Glyph table[] = {
            {'0',{14,17,19,21,25,17,14}}, {'1',{4,12,4,4,4,4,14}},
            {'2',{14,17,1,2,4,8,31}}, {'3',{30,1,1,14,1,1,30}},
            {'4',{2,6,10,18,31,2,2}}, {'5',{31,16,16,30,1,1,30}},
            {'6',{14,16,16,30,17,17,14}}, {'7',{31,1,2,4,8,8,8}},
            {'8',{14,17,17,14,17,17,14}}, {'9',{14,17,17,15,1,1,14}},
            {'A',{14,17,17,31,17,17,17}}, {'B',{30,17,17,30,17,17,30}},
            {'C',{14,17,16,16,16,17,14}}, {'D',{30,17,17,17,17,17,30}},
            {'E',{31,16,16,30,16,16,31}}, {'F',{31,16,16,30,16,16,16}},
            {'G',{14,17,16,23,17,17,15}}, {'H',{17,17,17,31,17,17,17}},
            {'I',{14,4,4,4,4,4,14}}, {'J',{7,2,2,2,18,18,12}},
            {'K',{17,18,20,24,20,18,17}}, {'L',{16,16,16,16,16,16,31}},
            {'M',{17,27,21,21,17,17,17}}, {'N',{17,25,21,19,17,17,17}},
            {'O',{14,17,17,17,17,17,14}}, {'P',{30,17,17,30,16,16,16}},
            {'Q',{14,17,17,17,21,18,13}}, {'R',{30,17,17,30,20,18,17}},
            {'S',{15,16,16,14,1,1,30}}, {'T',{31,4,4,4,4,4,4}},
            {'U',{17,17,17,17,17,17,14}}, {'V',{17,17,17,17,17,10,4}},
            {'W',{17,17,17,21,21,21,10}}, {'X',{17,17,10,4,10,17,17}},
            {'Y',{17,17,10,4,4,4,4}}, {'Z',{31,1,2,4,8,16,31}},
            {'.',{0,0,0,0,0,6,6}}, {':',{0,6,6,0,6,6,0}},
            {'-',{0,0,0,31,0,0,0}}, {'/',{1,2,2,4,8,8,16}},
            {'%',{17,2,4,4,8,16,17}}, {' ',{0,0,0,0,0,0,0}}
        };
        for (unsigned i = 0; i < sizeof(table)/sizeof(table[0]); ++i) if (table[i].c == c) return table[i].row;
        return table[sizeof(table)/sizeof(table[0])-1].row;
    }
};
#endif
