#ifndef UI_COLOR_MAP_H
#define UI_COLOR_MAP_H

#include <QColor>
#include <QRgb>

#include <algorithm>
#include <array>

/// @brief 256-entry waterfall palette: black -> blue -> cyan -> yellow -> red -> white
class ColorMap
{
public:
    ColorMap()
    {
        struct Stop
        {
            double pos;
            QColor color;
        };
        const Stop stops[] = {
            {0.00, QColor(0, 0, 0)},       {0.20, QColor(0, 0, 140)},   {0.40, QColor(0, 120, 220)},
            {0.55, QColor(0, 220, 200)},   {0.70, QColor(240, 230, 0)}, {0.85, QColor(255, 60, 0)},
            {1.00, QColor(255, 255, 255)},
        };
        for (size_t i = 0; i < table_.size(); ++i)
        {
            double t = static_cast<double>(i) / (table_.size() - 1);
            size_t s = 0;
            while (s + 2 < std::size(stops) && t > stops[s + 1].pos)
            {
                ++s;
            }
            const Stop &a = stops[s];
            const Stop &b = stops[s + 1];
            double f = std::clamp((t - a.pos) / (b.pos - a.pos), 0.0, 1.0);
            table_[i] = qRgb(static_cast<int>(a.color.red() + f * (b.color.red() - a.color.red())),
                             static_cast<int>(a.color.green() + f * (b.color.green() - a.color.green())),
                             static_cast<int>(a.color.blue() + f * (b.color.blue() - a.color.blue())));
        }
    }

    /// @param t 0..1
    QRgb at(double t) const
    {
        int i = static_cast<int>(std::clamp(t, 0.0, 1.0) * (table_.size() - 1) + 0.5);
        return table_[static_cast<size_t>(i)];
    }

private:
    std::array<QRgb, 256> table_{};
};

#endif
