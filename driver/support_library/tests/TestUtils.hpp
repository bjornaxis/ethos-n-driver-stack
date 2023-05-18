//
// Copyright © 2018-2023 Arm Limited.
// SPDX-License-Identifier: Apache-2.0
//

#pragma once

#include "../include/ethosn_support_library/Support.hpp"
#include "../src/Utils.hpp"
#include "../src/cascading/CombinerDFS.hpp"
#include "../src/cascading/Part.hpp"
#include "../src/cascading/Plan.hpp"

#include <ethosn_command_stream/CommandStreamBuffer.hpp>

namespace ethosn
{
namespace support_library
{

HardwareCapabilities GetEthosN78HwCapabilities();
HardwareCapabilities GetEthosN78HwCapabilities(EthosNVariant variant, uint32_t sramSizeOverride = 0);

std::vector<char> GetRawDefaultCapabilities();
std::vector<char> GetRawDefaultEthosN78Capabilities();
std::vector<char> GetRawEthosN78Capabilities(EthosNVariant variant, uint32_t sramSizeOverride = 0);

bool Contains(const char* string, const char* substring);

std::vector<uint8_t> GetCommandStreamData(const ethosn::command_stream::CommandStreamBuffer& cmdStream);

std::vector<uint8_t> GetCommandStreamData(const CompiledNetwork* compiledNetwork);

ethosn::command_stream::CommandStream GetCommandStream(const CompiledNetwork* compiledNetwork);

class MockPart : public BasePart
{
public:
    MockPart(PartId id, bool hasInput = true, bool hasOutput = true)
        : BasePart(id, "MockPart", estOpt, compOpt, GetEthosN78HwCapabilities())
        , m_CanMergeWithChannelSelectorBefore(false)
        , m_CanMergeWithChannelSelectorAfter(false)
        , m_HasInput(hasInput)
        , m_HasOutput(hasOutput)
    {}
    virtual Plans GetPlans(CascadeType, ethosn::command_stream::BlockConfig, Buffer*, uint32_t) const override;

    virtual utils::Optional<ethosn::command_stream::MceOperation> GetMceOperation() const override
    {
        return {};
    }

    utils::Optional<utils::ConstTensorData> GetChannelSelectorWeights() const override
    {
        return m_ChannelSelectorWeights;
    }
    bool MergeWithChannelSelectorBefore(const utils::ConstTensorData&) override
    {
        return m_CanMergeWithChannelSelectorBefore;
    }
    bool MergeWithChannelSelectorAfter(const utils::ConstTensorData&) override
    {
        return m_CanMergeWithChannelSelectorAfter;
    }

    std::vector<BoundaryRequirements> GetInputBoundaryRequirements() const override
    {
        return { BoundaryRequirements{} };
    }

    std::vector<bool> CanInputsTakePleInputSram() const override
    {
        return { false };
    }

    utils::Optional<utils::ConstTensorData> m_ChannelSelectorWeights;
    bool m_CanMergeWithChannelSelectorBefore;
    bool m_CanMergeWithChannelSelectorAfter;

protected:
    bool m_HasInput;
    bool m_HasOutput;

private:
    const EstimationOptions estOpt   = EstimationOptions();
    const CompilationOptions compOpt = CompilationOptions();
};

/// Simple Node type for tests.
/// Includes a friendly name and ignores shape, quantisation info etc. so that tests
/// can focus on graph topology.
class NameOnlyNode : public Node
{
public:
    // cppcheck-suppress passedByValue
    NameOnlyNode(NodeId id, std::string name)
        : Node(id,
               TensorShape(),
               DataType::UINT8_QUANTIZED,
               QuantizationInfo(),
               CompilerDataFormat::NONE,
               std::set<uint32_t>{ 0 })
        , m_Name(std::move(name))
    {}

    DotAttributes GetDotAttributes() override
    {
        return DotAttributes(std::to_string(m_Id), m_Name, "");
    }

    bool IsPrepared() override
    {
        return false;
    }

    NodeType GetNodeType() override
    {
        return NodeType::NameOnlyNode;
    }

    std::string m_Name;
};

bool IsEstimateOnlyOp(const Op* const op);
bool IsMceOp(const Op* const op);
bool IsPleOp(const Op* const op);

class CombinerTest : public Combiner
{
public:
    using Combiner::Combiner;

    bool IsPartSi(const BasePart& part) const
    {
        return Combiner::IsPartSi(part);
    }
    bool IsPartSo(const BasePart& part) const
    {
        return Combiner::IsPartSo(part);
    }
    bool IsPartSiso(const BasePart& part) const
    {
        return Combiner::IsPartSiso(part);
    }

    bool IsPlanAllocated(SectionContext& context,
                         const Plan& plan,
                         const Buffer* const outBufOfPrevPlanInSection,
                         bool inputBufferNeedAllocation) const
    {
        return Combiner::IsPlanAllocated(context, plan, outBufOfPrevPlanInSection, inputBufferNeedAllocation);
    }

    void DeallocateUnusedBuffers(const Buffer& prevPlanBuffer, SectionContext& context)
    {
        return Combiner::DeallocateUnusedBuffers(prevPlanBuffer, context);
    }

    Combination GluePartToCombinationSrcToDests(const BasePart& sPart, const Combination& comb, uint32_t outputSlotIdx)
    {
        return Combiner::GluePartToCombinationSrcToDests(sPart, comb, outputSlotIdx);
    }
};

}    // namespace support_library
}    // namespace ethosn
