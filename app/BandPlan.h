#ifndef BAND_PLAN_H
#define BAND_PLAN_H

#include <vector>

/// @brief AM broadcast bands, ITU region 1 (Europe). Frequencies in kHz
struct Band
{
    const char *name;
    double startKHz;
    double stopKHz;
    double gridKHz;
};

inline const std::vector<Band> &bandPlan()
{
    static const std::vector<Band> bands = {
        {"LW (ДХ)",       153,   279, 9},
        {"MW (СХ)",       531,  1602, 9},
        {"SW 120 m",     2300,  2495, 5},
        {"SW 90 m",      3200,  3400, 5},
        {"SW 75 m",      3900,  4000, 5},
        {"SW 60 m",      4750,  5060, 5},
        {"SW 49 m",      5900,  6200, 5},
        {"SW 41 m",      7200,  7600, 5},
        {"SW 31 m",      9400,  9900, 5},
        {"SW 25 m",     11600, 12100, 5},
        {"SW 22 m",     13570, 13870, 5},
        {"SW 19 m",     15100, 15830, 5},
        {"SW 16 m",     17480, 17900, 5},
        {"SW 15 m",     18900, 19020, 5},
        {"SW 13 m",     21450, 21850, 5},
        {"SW 11 m",     25670, 26100, 5},
    };
    return bands;
}

#endif
