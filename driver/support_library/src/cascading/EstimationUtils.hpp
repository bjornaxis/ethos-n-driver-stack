//
// Copyright © 2020-2021 Arm Limited.
// SPDX-License-Identifier: Apache-2.0
//
#pragma once

#include "../GraphNodes.hpp"
#include "../WeightEncoder.hpp"
#include "Plan.hpp"

#include <vector>

namespace ethosn
{
namespace support_library
{

class Buffer;
class HardwareCapabilities;

struct ConversionData
{
    ConversionData() = default;
    TensorShape tensorShape;
    TensorShape stripeShape;
    bool isNhwc;
};

PassStats GetConversionStats(const ConversionData& input, const ConversionData& output, bool isDramToDram);

PleStats GetPleStats(const HardwareCapabilities& caps,
                     const std::vector<TensorShape>& inputShapes,
                     const command_stream::PleOperation& pleoperation);

InputStats GetInputStats(const HardwareCapabilities& caps,
                         const TensorShape& shape,
                         const TensorShape& stripeShape,
                         const Location location,
                         const uint32_t tileSize,
                         const TensorInfo& weights =
                             {
                                 { { 1, 1, 1, 1 } },
                                 DataType::UINT8_QUANTIZED,
                                 DataFormat::HWIM,
                                 { 0, 0.1f },
                             },
                         const uint32_t numOutStripesC = 1);

InputStats GetInputStats(const TensorShape& shape,
                         const TensorShape& stripeShape,
                         CascadingBufferFormat format,
                         const Location location);

OutputStats GetOutputStats(const TensorShape& shape, const TensorShape& stripeShape, const Location location);

InputStats AccountForActivationCompression(InputStats stats, float spaceSavingRatio);

uint64_t GetPerformanceTotalDataMetric(const NetworkPerformanceData& netPerfData);
uint64_t GetPerformanceParallelDataMetric(const NetworkPerformanceData& netPerfData);
uint64_t GetPerformanceNonParallelDataMetric(const NetworkPerformanceData& netPerfData);
uint64_t GetPerformanceMceCycleCountMetric(const NetworkPerformanceData& netPerfData);
std::vector<uint64_t> GetPerformanceMetrics(const NetworkPerformanceData& netPerfData);

enum class PerformanceComparisonResult
{
    Equal,
    LeftBetter,
    RightBetter
};
PerformanceComparisonResult ComparePerformanceData(const NetworkPerformanceData& left,
                                                   const NetworkPerformanceData& right);

}    //namespace support_library
}    //namespace ethosn
