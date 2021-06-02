//
// Copyright © 2020-2021 Arm Ltd.
// SPDX-License-Identifier: Apache-2.0
//
#include "EthosNReplaceUnsupported.hpp"

#include "EthosNConfig.hpp"

#include <armnn/INetwork.hpp>
#include <backendsCommon/test/CommonTestUtils.hpp>
#include <test/GraphUtils.hpp>

#include <boost/test/unit_test.hpp>

#include <numeric>

using namespace armnn;
using namespace armnn::ethosnbackend;

// By default, specific unsupported layer patterns are substituted for patterns
// that can be optimized on the backend.
BOOST_AUTO_TEST_SUITE(EthosNReplaceUnsupported)

// Multiplication operations that take as input a Constant tensor in the shape
// { 1, 1, 1, C } can be substituted for DepthwiseConvolution2d.
//
// Original pattern:
// Input    ->
//              Multiplication -> Output
// Constant ->
//
// Expected modified pattern:
// Input -> DepthwiseConvolution2d -> Output
BOOST_AUTO_TEST_CASE(ConstMulToDepthwiseReplacement)
{
    auto net = std::make_unique<NetworkImpl>();

    TensorInfo inputInfo({ 1, 8, 8, 16 }, DataType::QAsymmU8, 1.0f, 0);
    TensorInfo constInfo({ 1, 1, 1, 16 }, DataType::QAsymmU8, 0.9f, 0);
    TensorInfo outputInfo({ 1, 8, 8, 16 }, DataType::QAsymmU8, 1.0f, 0);

    std::vector<uint8_t> constData(constInfo.GetNumElements(), 0);
    std::iota(constData.begin(), constData.end(), 0);
    ConstTensor constTensor(constInfo, constData);

    // Add the original pattern
    IConnectableLayer* const input    = net->AddInputLayer(0, "input");
    IConnectableLayer* const constant = net->AddConstantLayer(constTensor, "const");
    IConnectableLayer* const mul      = net->AddMultiplicationLayer("mul");
    IConnectableLayer* const output   = net->AddOutputLayer(0, "output");

    // Create connections between layers
    input->GetOutputSlot(0).SetTensorInfo(inputInfo);
    constant->GetOutputSlot(0).SetTensorInfo(constInfo);
    mul->GetOutputSlot(0).SetTensorInfo(outputInfo);

    input->GetOutputSlot(0).Connect(mul->GetInputSlot(0));
    constant->GetOutputSlot(0).Connect(mul->GetInputSlot(1));
    mul->GetOutputSlot(0).Connect(output->GetInputSlot(0));

    // Substitute the subgraph and check for expected pattern and connections
    Graph pattern = net->GetGraph();
    ethosnbackend::ReplaceUnsupportedLayers(pattern, EthosNConfig(), EthosNMappings(),
                                            EthosNConfig().QueryCapabilities());

    BOOST_CHECK(pattern.GetNumLayers() == 3);

    const std::vector<Layer*> vecPattern(pattern.begin(), pattern.end());

    Layer* inputLayer     = vecPattern[0];
    Layer* depthwiseLayer = vecPattern[1];
    Layer* outputLayer    = vecPattern[2];

    BOOST_CHECK(inputLayer->GetType() == LayerType::Input);
    BOOST_CHECK(depthwiseLayer->GetType() == LayerType::DepthwiseConvolution2d);
    BOOST_CHECK(outputLayer->GetType() == LayerType::Output);

    Layer* depthwiseInput  = &depthwiseLayer->GetInputSlots()[0].GetConnectedOutputSlot()->GetOwningLayer();
    Layer* depthwiseOutput = &depthwiseLayer->GetOutputSlots()[0].GetConnections()[0]->GetOwningLayer();
    BOOST_CHECK(depthwiseInput == inputLayer);
    BOOST_CHECK(depthwiseOutput == outputLayer);

    Layer* inputNextLayer  = &inputLayer->GetOutputSlots()[0].GetConnections()[0]->GetOwningLayer();
    Layer* outputPrevLayer = &outputLayer->GetInputSlots()[0].GetConnectedOutputSlot()->GetOwningLayer();
    BOOST_CHECK(inputNextLayer == depthwiseLayer);
    BOOST_CHECK(outputPrevLayer == depthwiseLayer);

    // Depthwise weights should be exact with the Constant data
    const uint8_t* dwWeightData =
        PolymorphicPointerDowncast<DepthwiseConvolution2dLayer>(depthwiseLayer)->m_Weight->GetConstTensor<uint8_t>();
    std::vector<uint8_t> depthwiseWeights(dwWeightData, dwWeightData + constData.size());
    BOOST_CHECK(depthwiseWeights == constData);
}

BOOST_AUTO_TEST_CASE(CalcConstantAddToDepthwiseReplacementConfigTest)
{
    auto ExpectFail = [](const TensorInfo& inputInfo, const TensorInfo& constantInfo, const TensorInfo& outputInfo,
                         const char* expectedFailureReason) {
        std::string failureReason;
        Optional<ConstantAddToDepthwiseReplacementConfig> result =
            CalcConstantAddToDepthwiseReplacementConfig(inputInfo, constantInfo, outputInfo, failureReason);
        BOOST_CHECK(!result.has_value() && failureReason == expectedFailureReason);
    };

    // Valid inputs
    TensorInfo validInput(TensorShape{ 1, 16, 16, 3 }, DataType::QAsymmU8, 1.0f, 0);
    TensorInfo validConstant(TensorShape{ 1, 1, 1, 3 }, DataType::QAsymmU8, 2.0f, 0);
    TensorInfo validOutput(TensorShape{ 1, 16, 16, 3 }, DataType::QAsymmU8, 4.0f, 0);

    // Error case - input has unsupported datatype
    {
        TensorInfo invalidInput = validInput;
        invalidInput.SetDataType(DataType::Float32);
        ExpectFail(invalidInput, validConstant, validOutput, "Unsupported datatype");
    }
    // Error case - constant has unsupported datatype
    {
        TensorInfo invalidConstant = validConstant;
        invalidConstant.SetDataType(DataType::Float32);
        ExpectFail(validInput, invalidConstant, validOutput, "Unsupported datatype");
    }
    // Error case - output has unsupported datatype
    {
        TensorInfo invalidOutput = validOutput;
        invalidOutput.SetDataType(DataType::Float32);
        ExpectFail(validInput, validConstant, invalidOutput, "Unsupported datatype");
    }

    // Error case - input has wrong number of dims
    {
        TensorInfo invalidInput = validInput;
        invalidInput.SetShape(TensorShape{ 1, 16, 16, 3, 16 });
        ExpectFail(invalidInput, validConstant, validOutput, "Shapes not compatible");
    }
    // Error case - constant has wrong number of dims
    {
        TensorInfo invalidConstant = validConstant;
        invalidConstant.SetShape(TensorShape{ 3, 5 });
        ExpectFail(validInput, invalidConstant, validOutput, "Shapes not compatible");
    }
    // Error case - constant has wrong shape
    {
        TensorInfo invalidConstant = validConstant;
        invalidConstant.SetShape(TensorShape{ 1, 1, 1, 4 });
        ExpectFail(validInput, invalidConstant, validOutput, "Shapes not compatible");
    }

    // Error case where no valid weight scale is possible
    {
        TensorInfo invalidInput = validInput;
        invalidInput.SetQuantizationScale(100000);
        ExpectFail(invalidInput, validConstant, validOutput, "Couldn't find valid weight scale");
    }

    // Valid case
    {
        std::string failureReason;
        Optional<ConstantAddToDepthwiseReplacementConfig> result =
            CalcConstantAddToDepthwiseReplacementConfig(validInput, validConstant, validOutput, failureReason);
        BOOST_CHECK(result.has_value() && failureReason.empty());
        ConstantAddToDepthwiseReplacementConfig config = result.value();
        BOOST_CHECK(config.m_Desc.m_BiasEnabled == true);
        BOOST_CHECK(config.m_Desc.m_DataLayout == DataLayout::NHWC);
        BOOST_CHECK(config.m_WeightsInfo == TensorInfo(TensorShape{ 1, 3, 1, 1 }, DataType::QAsymmU8, 0.5f, 0));
        BOOST_CHECK(config.m_WeightsQuantizedValue == 2);
        BOOST_CHECK(config.m_BiasInfo == TensorInfo(TensorShape{ 1, 1, 1, 3 }, DataType::Signed32, 0.5f, 0));
    }
}

namespace
{

/// Creates a graph comprising an Addition of two other layers, which are either Inputs or Constants, depending
/// on the flags provided. For any layers which are Constants, dummy constant data is generated.
Graph CreateAdditionGraph(const TensorInfo& input0Info,
                          bool isInput0Constant,
                          const TensorInfo& input1Info,
                          bool isInput1Constant,
                          const TensorInfo& outputInfo)
{
    auto net = std::make_unique<NetworkImpl>();

    auto AddConstLayer = [&net](const TensorInfo& info, const char* name) -> IConnectableLayer* {
        switch (info.GetDataType())
        {
            case DataType::QAsymmU8:
            {
                std::vector<uint8_t> data(info.GetNumElements(), 0);
                std::iota(data.begin(), data.end(), 0);
                ConstTensor tensor(info, data);
                return net->AddConstantLayer(tensor, name);
            }
            case DataType::QAsymmS8:    // Deliberate fallthrough
            case DataType::QSymmS8:
            {
                std::vector<int8_t> data(info.GetNumElements(), 0);
                std::iota(data.begin(), data.end(), -3);    // Include negative numbers for better test coverage
                ConstTensor tensor(info, data);
                return net->AddConstantLayer(tensor, name);
            }
            default:
            {
                ARMNN_ASSERT(!"Not implemented");
                return nullptr;
            }
        }
    };

    IConnectableLayer* const input0 =
        isInput0Constant ? AddConstLayer(input0Info, "input0") : net->AddInputLayer(0, "input0");
    IConnectableLayer* const input1 =
        isInput1Constant ? AddConstLayer(input1Info, "input1") : net->AddInputLayer(1, "input1");
    IConnectableLayer* const add    = net->AddAdditionLayer("add");
    IConnectableLayer* const output = net->AddOutputLayer(0, "output");

    input0->GetOutputSlot(0).SetTensorInfo(input0Info);
    input1->GetOutputSlot(0).SetTensorInfo(input1Info);
    add->GetOutputSlot(0).SetTensorInfo(outputInfo);

    input0->GetOutputSlot(0).Connect(add->GetInputSlot(0));
    input1->GetOutputSlot(0).Connect(add->GetInputSlot(1));
    add->GetOutputSlot(0).Connect(output->GetInputSlot(0));

    return net->GetGraph();
}

}    // namespace

BOOST_AUTO_TEST_CASE(ReplaceConstantAdditionWithDepthwiseTest)
{
    // Failure case - not an Addition layer
    {
        Graph g = CreateAdditionGraph(TensorInfo({ 1, 8, 8, 4 }, DataType::QAsymmU8, 1.0f, 0), false,
                                      TensorInfo({ 1, 1, 1, 4 }, DataType::QAsymmU8, 1.0f, 0), true,
                                      TensorInfo({ 1, 8, 8, 4 }, DataType::QAsymmU8, 1.0f, 0));
        BOOST_CHECK(ReplaceConstantAdditionWithDepthwise(g, *g.begin(), EthosNConfig(), EthosNMappings(),
                                                         EthosNConfig().QueryCapabilities()) == false);
    }

    // Failure case - addition that doesn't need replacing (as it is supported natively - not a broadcast)
    {
        Graph g                 = CreateAdditionGraph(TensorInfo({ 1, 8, 8, 4 }, DataType::QAsymmU8, 1.0f, 0), false,
                                      TensorInfo({ 1, 8, 8, 4 }, DataType::QAsymmU8, 1.0f, 0), true,
                                      TensorInfo({ 1, 8, 8, 4 }, DataType::QAsymmU8, 1.0f, 0));
        AdditionLayer* addLayer = PolymorphicPointerDowncast<AdditionLayer>(GetFirstLayerWithName(g, "add"));
        BOOST_CHECK(ReplaceConstantAdditionWithDepthwise(g, addLayer, EthosNConfig(), EthosNMappings(),
                                                         EthosNConfig().QueryCapabilities()) == false);
    }

    // Error case - neither input is a constant
    {
        Graph g                 = CreateAdditionGraph(TensorInfo({ 1, 8, 8, 4 }, DataType::QAsymmU8, 1.0f, 0), false,
                                      TensorInfo({ 1, 1, 1, 4 }, DataType::QAsymmU8, 1.0f, 0), false,
                                      TensorInfo({ 1, 8, 8, 4 }, DataType::QAsymmU8, 1.0f, 0));
        AdditionLayer* addLayer = PolymorphicPointerDowncast<AdditionLayer>(GetFirstLayerWithName(g, "add"));
        BOOST_CHECK(ReplaceConstantAdditionWithDepthwise(g, addLayer, EthosNConfig(), EthosNMappings(),
                                                         EthosNConfig().QueryCapabilities()) == false);
    }

    // Valid cases
    auto ValidTest = [](bool isInput0Constant, bool isInput1Constant, DataType constantDataType) {
        // Note we use non-trivial quant params for the constant to better test the requantization that takes place
        TensorInfo constantInfo({ 1, 1, 1, 4 }, constantDataType, 10.0f, 2);
        TensorInfo inputInfo({ 1, 8, 8, 4 }, DataType::QAsymmU8, 1.0f, 0);

        Graph g                 = CreateAdditionGraph(isInput0Constant ? constantInfo : inputInfo, isInput0Constant,
                                      isInput1Constant ? constantInfo : inputInfo, isInput1Constant,
                                      TensorInfo({ 1, 8, 8, 4 }, DataType::QAsymmU8, 1.0f, 0));
        AdditionLayer* addLayer = PolymorphicPointerDowncast<AdditionLayer>(GetFirstLayerWithName(g, "add"));
        BOOST_CHECK(ReplaceConstantAdditionWithDepthwise(g, addLayer, EthosNConfig(), EthosNMappings(),
                                                         EthosNConfig().QueryCapabilities()) == true);

        // Original pattern:
        // Input    ->
        //              Multiplication -> Output
        // Constant ->
        //
        // Expected modified pattern:
        // Input -> DepthwiseConvolution2d -> Output
        const std::vector<Layer*> outLayers(g.begin(), g.end());
        BOOST_CHECK(outLayers.size() == 3);

        Layer* inputLayer  = outLayers[0];
        Layer* layer1      = outLayers[1];
        Layer* outputLayer = outLayers[2];

        BOOST_CHECK(inputLayer->GetType() == LayerType::Input);
        BOOST_CHECK(layer1->GetType() == LayerType::DepthwiseConvolution2d);
        BOOST_CHECK(outputLayer->GetType() == LayerType::Output);

        const DepthwiseConvolution2dLayer* depthwiseLayer =
            PolymorphicPointerDowncast<DepthwiseConvolution2dLayer>(layer1);

        Layer* depthwiseInput  = &depthwiseLayer->GetInputSlots()[0].GetConnectedOutputSlot()->GetOwningLayer();
        Layer* depthwiseOutput = &depthwiseLayer->GetOutputSlots()[0].GetConnections()[0]->GetOwningLayer();
        BOOST_CHECK(depthwiseInput == inputLayer);
        BOOST_CHECK(depthwiseOutput == outputLayer);

        Layer* inputNextLayer  = &inputLayer->GetOutputSlots()[0].GetConnections()[0]->GetOwningLayer();
        Layer* outputPrevLayer = &outputLayer->GetInputSlots()[0].GetConnectedOutputSlot()->GetOwningLayer();
        BOOST_CHECK(inputNextLayer == depthwiseLayer);
        BOOST_CHECK(outputPrevLayer == depthwiseLayer);

        // Check weights tensor info and data
        BOOST_CHECK(depthwiseLayer->m_Weight->GetTensorInfo() ==
                    TensorInfo(TensorShape{ 1, 4, 1, 1 }, DataType::QAsymmU8, 0.5f, 0));
        const uint8_t* dwWeightData = depthwiseLayer->m_Weight->GetConstTensor<uint8_t>();
        BOOST_CHECK(std::all_of(dwWeightData, dwWeightData + depthwiseLayer->m_Weight->GetShape().GetNumElements(),
                                [](auto x) { return x == 2; }));

        // Check bias tensor info and data
        BOOST_CHECK(depthwiseLayer->m_Bias->GetTensorInfo() ==
                    TensorInfo(TensorShape{ 1, 1, 1, 4 }, DataType::Signed32, 0.5f, 0));
        const int32_t* dwBiasData = depthwiseLayer->m_Bias->GetConstTensor<int32_t>();
        std::vector<int32_t> expectedBiasData;
        switch (constantDataType)
        {
            case DataType::QAsymmU8:
                expectedBiasData = { -40, -20, 0, 20 };
                break;
            case DataType::QAsymmS8:
                expectedBiasData = { -100, -80, -60, -40 };
                break;
            case DataType::QSymmS8:
                expectedBiasData = { -60, -40, -20, 0 };
                break;
            default:
                ARMNN_ASSERT(!"Not implemented");
        }
        BOOST_CHECK(
            (std::vector<int32_t>(dwBiasData, dwBiasData + depthwiseLayer->m_Weight->GetShape().GetNumElements()) ==
             expectedBiasData));
    };
    // Try both combinations of input/const as first/second input. The resulting graph should be identical
    // no matter the order of the inputs.
    ValidTest(true, false, DataType::QAsymmU8);
    ValidTest(false, true, DataType::QAsymmU8);
    // Test signed data types for the constant input
    ValidTest(true, false, DataType::QAsymmS8);
    ValidTest(true, false, DataType::QSymmS8);
}

BOOST_AUTO_TEST_SUITE_END()