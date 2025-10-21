#ifndef TRINITY_ABINFLECTIONPOINTSETTINGS_H
#define TRINITY_ABINFLECTIONPOINTSETTINGS_H

#include "DataMap.h"

struct AutoBalanceInflectionPointSettings : public DataMap::Base
{
    AutoBalanceInflectionPointSettings() = default;
    AutoBalanceInflectionPointSettings(float value, float curveFloor, float curveCeiling)
        : value(value), curveFloor(curveFloor), curveCeiling(curveCeiling) { }

    float value = 0.5f;
    float curveFloor = 0.0f;
    float curveCeiling = 1.0f;
};

#endif // TRINITY_ABINFLECTIONPOINTSETTINGS_H
