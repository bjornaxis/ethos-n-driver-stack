//
// Copyright © 2021-2022 Arm Limited.
// SPDX-License-Identifier: Apache-2.0
//

#pragma once

#include "../../include/ethosn_support_library/Optional.hpp"
#include "../Graph.hpp"
#include "../Network.hpp"
#include "cascading/MceEstimationUtils.hpp"

#include <unordered_map>

namespace ethosn
{
namespace support_library
{

class McePart;

class NetworkToGraphOfPartsConverter : public NetworkVisitor
{
public:
    using NetworkVisitor::Visit;
    NetworkToGraphOfPartsConverter(const Network& network,
                                   const HardwareCapabilities& capabilities,
                                   const EstimationOptions& estimationOptions,
                                   const CompilationOptions& compilationOptions);
    ~NetworkToGraphOfPartsConverter();

    // Visitor functions for supported operations in the Network
    void Visit(Input& input) final;
    void Visit(Output& output) final;
    void Visit(Convolution& convolution) final;
    void Visit(DepthwiseConvolution& convolution) final;
    void Visit(FullyConnected& fullyConnected) final;
    void Visit(Pooling& pooling) final;
    void Visit(Reshape& reshape) final;
    void Visit(Concatenation& concatenation) final;
    void Visit(LeakyRelu& leakyRelu) final;
    void Visit(Sigmoid& sigmoid) final;
    void Visit(Tanh& tanh) final;
    void Visit(MeanXy& meanxy) final;
    void Visit(EstimateOnly& estimateOnly) final;
    void Visit(Addition& addition) final;
    void Visit(Resize& resize) final;
    void Visit(Relu& relu) final;
    void Visit(TransposeConvolution& transposeConvolution) final;
    void Visit(ReinterpretQuantization& reinterpretQuantization) final;
    void Visit(Softmax& softmax) final;
    void Visit(DepthToSpace& depthToSpace) final;
    void Visit(Split& split) final;
    void Visit(Transpose& split) final;
    void Visit(SpaceToDepth& split) final;

    void ConnectParts(Operation& operation, std::vector<BasePart*>& m_Part);

    std::vector<uint8_t> OverrideWeights(const std::vector<uint8_t>& userWeights, const TensorInfo& weightsInfo) const;

    // Function used to release the GraphOfParts object. Caller should store the object locally, since
    // the function performs an std::move().
    GraphOfParts ReleaseGraphOfParts();

private:
    std::unique_ptr<McePart> CreateIdentityMcePart(const TensorShape& shape,
                                                   const QuantizationInfo& inputQuantInfo,
                                                   const QuantizationInfo& outputQuantInfo,
                                                   uint32_t operationId,
                                                   command_stream::DataType dataType,
                                                   const EstimationOptions& estOpt,
                                                   const CompilationOptions& compOpt,
                                                   const HardwareCapabilities& capabilities);
    std::vector<BasePart*> CreateTransposeConv(const Stride& stride,
                                               const TensorInfo& weightsInfo,
                                               const std::vector<uint8_t>& weightsData,
                                               const TensorInfo& biasInfo,
                                               std::vector<int32_t> biasData,
                                               const Padding& padding,
                                               const TensorInfo& inputInfo,
                                               const TensorInfo& outputInfo,
                                               const std::set<uint32_t>& operationIds);

    const HardwareCapabilities& m_Capabilities;
    utils::Optional<const EstimationOptions&> m_EstimationOptions;
    const CompilationOptions& m_CompilationOptions;
    SupportQueries m_Queries;
    std::map<const Operand*, BasePart*> m_OperandToPart;
    GraphOfParts m_GraphOfParts;
};

}    // namespace support_library
}    // namespace ethosn
