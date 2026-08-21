#include "smart_web_conventions.h"

#include <cmath>
#include <gtest/gtest.h>
#include <limits>

namespace
{
    struct TSensorDataSample
    {
        double Value;
        int16_t Raw;
    };
}

TEST(TSensorDataTest, FromDouble)
{
    const TSensorDataSample samples[] = {
        {0, 0},
        {23.5, 235},
        {23.45, 235}, // rounded, not truncated
        {-23.45, -235},
        {3276.7, 32767},   // the largest representable value
        {-3276.5, -32765}, // the smallest representable value
    };

    for (const auto& sample: samples) {
        EXPECT_EQ(sample.Raw, SmartWeb::SensorData::FromDouble(sample.Value)) << sample.Value;
    }
}

TEST(TSensorDataTest, FromDoubleOutOfRange)
{
    const double values[] = {3276.75,
                             3276.8,
                             3276.9,
                             3277,
                             5000,
                             32000,
                             1e12,
                             -3276.6,
                             -3276.8,
                             -5000,
                             -1e12,
                             std::numeric_limits<double>::quiet_NaN()};

    for (auto value: values) {
        EXPECT_EQ(SmartWeb::SENSOR_UNDEFINED, SmartWeb::SensorData::FromDouble(value)) << value;
    }
}

TEST(TSensorDataTest, ToDouble)
{
    EXPECT_TRUE(std::isnan(SmartWeb::SensorData::ToDouble(SmartWeb::SENSOR_UNDEFINED)));
    EXPECT_DOUBLE_EQ(23.5, SmartWeb::SensorData::ToDouble(235));
    EXPECT_DOUBLE_EQ(-3276.5, SmartWeb::SensorData::ToDouble(SmartWeb::SENSOR_VALUE_MIN));
    EXPECT_DOUBLE_EQ(3276.7, SmartWeb::SensorData::ToDouble(SmartWeb::SENSOR_VALUE_MAX));
}
