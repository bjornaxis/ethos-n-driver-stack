//
// Copyright © 2022-2023 Arm Limited.
// SPDX-License-Identifier: Apache-2.0
//

#include "CascadingCommandStreamGenerator.hpp"

#include "CascadingCommandStreamGeneratorUtils.hpp"
#include "Compiler.hpp"
#include "DmaRegisters.hpp"
#include "MceRegisters.hpp"
#include "PleRegisters.hpp"
#include "Scheduler.hpp"
#include "Visualisation.hpp"

#include <iomanip>
#include <memory>

using namespace ethosn::command_stream::cascading;

namespace ethosn
{
namespace support_library
{
namespace cascading_compiler
{
CascadingCommandStreamGenerator::CascadingCommandStreamGenerator(const OpGraph& mergedOpGraph,
                                                                 const std::set<uint32_t>& operationIds,
                                                                 const HardwareCapabilities& capabilities,
                                                                 const CompilationOptions& compilationOptions,
                                                                 const DebuggingContext& debuggingContext)
    : m_MergedOpGraph{ mergedOpGraph }
    , m_OperationIds{ operationIds }
    , m_Capabilities{ capabilities }
    , m_CompilationOptions{ compilationOptions }
    , m_DebuggingContext(debuggingContext)
    , m_FenceOpForIfmS(nullptr)
    , m_FenceOpForPleL(nullptr)
    , m_FenceOpForWgtS(nullptr)
{

    m_CommandStreamAgents.reserve(m_MergedOpGraph.GetOps().size());
}

CascadingCommandStreamGenerator::~CascadingCommandStreamGenerator()
{}

// Compile a given network and return the compiled network
CompiledOpGraph CascadingCommandStreamGenerator::Generate()
{
    assert(m_MergedOpGraph.GetOps().size() != 0 && m_CommandStreamAgents.size() == 0);

    try
    {
        for (auto currentOp : m_MergedOpGraph.GetOps())
        {
            if (IsObjectOfType<DmaOp>(currentOp))
            {
                ProcessDmaOp(static_cast<DmaOp*>(currentOp));
            }
            else if (IsObjectOfType<MceOp>(currentOp))
            {
                ProcessMceOp(currentOp);
            }
            else if (IsObjectOfType<PleOp>(currentOp))
            {
                ProcessPleOp(currentOp);
            }
            else
            {
                throw NotSupportedException("Op is not currently supported by the Cascading Compiler");
            }

            Buffer* producedBuffer = m_MergedOpGraph.GetOutput(currentOp);
            if (producedBuffer != nullptr && producedBuffer->IsFullTensor() &&
                !(IsObjectOfType<DmaOp>(currentOp) && producedBuffer->m_Location == Location::Sram))
            {
                m_FenceOpForIfmS = currentOp;
                m_FenceOpForPleL = currentOp;
                m_FenceOpForWgtS = currentOp;
            }
        }
    }
    catch (const NotSupportedException& e)
    {
        g_Logger.Error("Error: %s", e.what());
        return {};
    }

    // Add the lifetime information of the intermediate DRAM buffers so the memory required to store these
    // buffers is reduced
    AddLifetimeInfoForIntermediateDramBuffers();

    // Use the dependencies to generate the lists of commands
    Scheduler scheduler(m_CommandStreamAgents);
    scheduler.Schedule();

    // Generate register values for each command, and store them in the extra data
    std::vector<DmaExtraData> dmaExtraData;
    std::vector<ProgramMceExtraData> programMceExtraData;
    std::vector<StartMceExtraData> startMceExtraData;
    std::vector<StartPleExtraData> startPleExtraData;
    uint32_t nextRdDmaCmdId = 0;
    uint32_t nextWrDmaCmdId = 4;

    std::map<std::pair<uint32_t, uint32_t>, uint32_t> agentAndStripeToChunkRd;
    for (const Command& c : scheduler.GetDmaRdCommands())
    {
        if (c.type == CommandType::LoadIfmStripe)
        {
            uint32_t& chunkId = agentAndStripeToChunkRd[std::make_pair(c.agentId, c.stripeId)];
            dmaExtraData.push_back(GenerateDmaExtraDataForLoadIfmStripe(
                m_CommandStreamAgents[c.agentId].agent.ifm, c.stripeId, chunkId, m_Capabilities, nextRdDmaCmdId));

            nextRdDmaCmdId = (nextRdDmaCmdId + 1) % 4;
            ++chunkId;
        }
        else if (c.type == CommandType::LoadWgtStripe)
        {
            dmaExtraData.push_back(GenerateDmaExtraDataForLoadWgtStripe(m_CommandStreamAgents[c.agentId].agent.wgt,
                                                                        c.stripeId, m_Capabilities, nextRdDmaCmdId));

            nextRdDmaCmdId = (nextRdDmaCmdId + 1) % 4;
        }
        else if (c.type == CommandType::LoadPleCode)
        {
            dmaExtraData.push_back(GenerateDmaExtraDataForLoadPleCode(m_CommandStreamAgents[c.agentId].agent.pleL,
                                                                      m_Capabilities, nextRdDmaCmdId));

            nextRdDmaCmdId = (nextRdDmaCmdId + 1) % 4;
        }
    }
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> agentAndStripeToChunkWr;
    for (const Command& c : scheduler.GetDmaWrCommands())
    {
        if (c.type == CommandType::StoreOfmStripe)
        {
            uint32_t& chunkId = agentAndStripeToChunkWr[std::make_pair(c.agentId, c.stripeId)];
            dmaExtraData.push_back(GenerateDmaExtraDataForStoreOfmStripe(
                m_CommandStreamAgents[c.agentId].agent.ofm, c.stripeId, chunkId, m_Capabilities, nextWrDmaCmdId));
            nextWrDmaCmdId = 4 + ((nextWrDmaCmdId + 1) % 4);
            ++chunkId;
        }
    }
    for (const Command& c : scheduler.GetMceCommands())
    {
        if (c.type == CommandType::ProgramMceStripe)
        {
            programMceExtraData.push_back(
                GenerateProgramMceExtraData(m_CommandStreamAgents[c.agentId].agent.mce, c.stripeId, m_Capabilities));
        }
        else if (c.type == CommandType::StartMceStripe)
        {
            startMceExtraData.push_back(
                GenerateStartMceExtraData(m_CommandStreamAgents[c.agentId].agent.mce, c.stripeId, m_Capabilities));
        }
    }
    for (const Command& c : scheduler.GetPleCommands())
    {
        if (c.type == CommandType::StartPleStripe)
        {
            startPleExtraData.push_back(
                GenerateStartPleExtraData(m_CommandStreamAgents[c.agentId].agent.pleS, c.stripeId));
        }
    }

    // Convert the AgentDesc structs into Agents for the command stream
    std::vector<Agent> agents;
    for (const AgentDescAndDeps& agentAndDeps : m_CommandStreamAgents)
    {
        switch (agentAndDeps.agent.type)
        {
            case AgentType::IFM_STREAMER:
            {
                IfmS ifmS = CreateIfmS(agentAndDeps.agent.ifm);
                agents.push_back({ agentAndDeps.agent.numStripesTotal, ifmS });
                break;
            }
            case AgentType::WGT_STREAMER:
            {
                WgtS wgtS     = {};
                wgtS.bufferId = agentAndDeps.agent.wgt.bufferId;
                agents.push_back({ agentAndDeps.agent.numStripesTotal, wgtS });
                break;
            }
            case AgentType::MCE_SCHEDULER:
            {
                MceS mceS = CreateMceS(agentAndDeps.agent.mce);
                agents.push_back({ agentAndDeps.agent.numStripesTotal, mceS });
                break;
            }
            case AgentType::PLE_LOADER:
            {
                PleL pleL        = {};
                pleL.pleKernelId = agentAndDeps.agent.pleL.pleKernelId;
                agents.push_back({ agentAndDeps.agent.numStripesTotal, pleL });
                break;
            }
            case AgentType::PLE_SCHEDULER:
            {
                PleS pleS              = {};
                pleS.inputMode         = agentAndDeps.agent.pleS.inputMode;
                pleS.pleKernelId       = agentAndDeps.agent.pleS.pleKernelId;
                pleS.pleKernelSramAddr = agentAndDeps.agent.pleS.pleKernelSramAddr;
                agents.push_back({ agentAndDeps.agent.numStripesTotal, pleS });
                break;
            }
            case AgentType::OFM_STREAMER:
            {
                OfmS ofmS = CreateOfmS(agentAndDeps.agent.ofm);
                agents.push_back({ agentAndDeps.agent.numStripesTotal, ofmS });
                break;
            }
            default:
                assert(false);
        }
    }

    // Store the arrays of agents, commands and extra data into the command for the command stream
    command_stream::AddCascade(m_CommandStream, agents, scheduler.GetDmaRdCommands(), scheduler.GetDmaWrCommands(),
                               scheduler.GetMceCommands(), scheduler.GetPleCommands(), dmaExtraData,
                               programMceExtraData, startMceExtraData, startPleExtraData);

    // Add DUMP_DRAM commands to the command stream, if requested.
    if (m_DebuggingContext.m_DebugInfo.m_DumpRam)
    {
        for (std::pair<DramBuffer*, uint32_t> b : m_DramBufToBufIdMapping)
        {
            if (b.first->m_BufferType == BufferType::Intermediate)
            {
                ethosn::command_stream::DumpDram cmdStrDumpDram = utils::GetDumpDramCommand(
                    b.first->m_TensorShape, b.second, b.first->m_DataType, b.first->m_QuantizationInfo.GetZeroPoint(),
                    ToString(b.first->m_Format).c_str());
                m_CommandStream.EmplaceBack(cmdStrDumpDram);
            }
        }
    }

    // Add the generated command stream to the buffer manager
    m_BufferManager.AddCommandStream(m_CommandStream);

    m_BufferManager.Allocate(m_DebuggingContext);

    CompiledOpGraph result;
    result.m_EstimatedOpGraph = EstimateOpGraph(m_MergedOpGraph, m_Capabilities, EstimationOptions());

    // Create the compiled network using the updated BufferManager instance
    result.m_CompiledNetwork    = std::make_unique<CompiledNetworkImpl>(m_BufferManager.GetConstantDmaData(),
                                                                     m_BufferManager.GetConstantControlUnitData(),
                                                                     m_BufferManager.GetBuffers(), m_OperationIds);
    result.m_OpToAgentIdMapping = m_OpToAgentIdMapping;
    result.m_BufferIds          = m_DramBufToBufIdMapping;

    return result;
}

// Functions used to retrieve private members
const BufferManager& CascadingCommandStreamGenerator::GetBufferManager() const
{
    return m_BufferManager;
}

const OpGraph& CascadingCommandStreamGenerator::GetMergedOpGraph() const
{
    return m_MergedOpGraph;
}

const std::unordered_map<DramBuffer*, uint32_t>& CascadingCommandStreamGenerator::GetDramBufToBufIdMapping() const
{
    return m_DramBufToBufIdMapping;
}

uint16_t CascadingCommandStreamGenerator::AddDramBufferAndCacheId(DramBuffer* inputBuffer, Op* const)
{
    uint16_t inputBufferId = std::numeric_limits<uint16_t>::max();
    if (m_DramBufToBufIdMapping.find(inputBuffer) != m_DramBufToBufIdMapping.end())
    {
        inputBufferId = ethosn::utils::NumericCast<uint16_t>(m_DramBufToBufIdMapping[inputBuffer]);
    }
    else
    {
        if (inputBuffer->m_BufferType.value() == BufferType::Input)
        {
            assert(inputBuffer->m_OperationId.has_value());
            inputBufferId = ethosn::utils::NumericCast<uint16_t>(
                m_BufferManager.AddDramInput(inputBuffer->m_SizeInBytes, inputBuffer->m_OperationId.value()));
        }
        else if (inputBuffer->m_BufferType.value() == BufferType::Intermediate)
        {
            inputBufferId = ethosn::utils::NumericCast<uint16_t>(
                m_BufferManager.AddDram(inputBuffer->m_BufferType.value(), inputBuffer->m_SizeInBytes));
        }
        else if (inputBuffer->m_BufferType.value() == BufferType::ConstantDma)
        {
            assert(inputBuffer->m_ConstantData != nullptr);
            inputBufferId = ethosn::utils::NumericCast<uint16_t>(
                m_BufferManager.AddDramConstant(inputBuffer->m_BufferType.value(), *inputBuffer->m_ConstantData));
        }
        else
        {
            assert(false);
        }
        m_DramBufToBufIdMapping[inputBuffer] = inputBufferId;
    }
    return inputBufferId;
}

// Private functions for processing OpGraph Ops
void CascadingCommandStreamGenerator::ProcessDmaOp(DmaOp* const ptrDmaOp)
{
    // Get the input buffer to the Dma Op
    OpGraph::BufferList inputBuffers = m_MergedOpGraph.GetInputs(ptrDmaOp);
    Buffer* inputBuffer              = inputBuffers[g_DmaInputBufferIndex];
    assert(inputBuffers.size() == 1);

    // Get the output buffer from the Dma Op
    Buffer* outputBuffer = m_MergedOpGraph.GetOutput(ptrDmaOp);
    assert(outputBuffer != nullptr);

    // Construct and add the required agents to the command stream
    if (inputBuffer->m_Location == Location::Dram && outputBuffer->m_Location == Location::Sram)
    {
        // cppcheck-suppress assertWithSideEffect
        assert(inputBuffer->Dram()->m_BufferType.has_value());

        if (inputBuffer->m_Format != CascadingBufferFormat::WEIGHT)
        {
            // cppcheck-suppress assertWithSideEffect
            assert(inputBuffer->Dram()->m_BufferType.value() ==
                       BufferType::Intermediate ||    // cppcheck-suppress assertWithSideEffect
                   inputBuffer->Dram()->m_BufferType.value() ==
                       BufferType::Input ||    // cppcheck-suppress assertWithSideEffect
                   inputBuffer->Dram()->m_BufferType.value() == BufferType::ConstantDma);

            DmaOp* const dmaOp = static_cast<DmaOp*>(ptrDmaOp);

            uint16_t inputBufferId = AddDramBufferAndCacheId(inputBuffer->Dram(), ptrDmaOp);

            uint32_t inputDramBufferOffset =
                utils::CalculateDramOffset(inputBuffer->m_Format, inputBuffer->m_TensorShape, ptrDmaOp->m_Offset);

            bool isExtraIfmStripeAtRightEdge  = false;
            bool isExtraIfmStripeAtBottomEdge = false;
            OpGraph::ConsumersList consumers  = m_MergedOpGraph.GetConsumers(outputBuffer);
            assert(consumers.size() == 1);
            if (IsObjectOfType<MceOp>(consumers[0].first))
            {
                MceOp* mceOp                = static_cast<MceOp*>(consumers[0].first);
                Buffer* mceInputBuffer      = outputBuffer;
                Buffer* mceOutputBuffer     = m_MergedOpGraph.GetOutput(mceOp);
                isExtraIfmStripeAtRightEdge = utils::DivRoundUp(utils::GetWidth(mceInputBuffer->m_TensorShape),
                                                                utils::GetWidth(mceOp->m_InputStripeShape)) >
                                              utils::DivRoundUp(utils::GetWidth(mceOutputBuffer->m_TensorShape),
                                                                utils::GetWidth(mceOp->m_OutputStripeShape));
                isExtraIfmStripeAtBottomEdge = utils::DivRoundUp(utils::GetHeight(mceInputBuffer->m_TensorShape),
                                                                 utils::GetHeight(mceOp->m_InputStripeShape)) >
                                               utils::DivRoundUp(utils::GetHeight(mceOutputBuffer->m_TensorShape),
                                                                 utils::GetHeight(mceOp->m_OutputStripeShape));
            }

            AgentIdType ifmStreamerAgentId = AddIfmStreamerToCommandStream(
                ptrDmaOp, inputBufferId, inputBuffer, outputBuffer->Sram(), dmaOp->m_TransferFormat,
                inputDramBufferOffset, isExtraIfmStripeAtRightEdge, isExtraIfmStripeAtBottomEdge);

            if (m_FenceOpForIfmS != nullptr)
            {
                // Note that this is an overly pessimistic approach, as corruption would only happen in practice if the SRAM
                // addresses used overlap, which we do not bother checking. A future improvement would be to check this first.
                AddReadAfterWriteDependency(
                    AgentType::IFM_STREAMER, ifmStreamerAgentId,
                    m_CommandStreamAgents[this->m_OpToAgentIdMapping.at(m_FenceOpForIfmS)].agent.type,
                    this->m_OpToAgentIdMapping.at(m_FenceOpForIfmS), m_FenceOpForIfmS);
                m_FenceOpForIfmS = nullptr;
            }
        }
        else
        {
            // Weight Streamer Agent
            AgentIdType weightStreamerAgentId = AddWeightStreamerToCommandStream(static_cast<DmaOp*>(ptrDmaOp));

            if (m_FenceOpForWgtS != nullptr)
            {
                // Note that this is an overly pessimistic approach, as corruption would only happen in practice if the SRAM
                // addresses used overlap, which we do not bother checking. A future improvement would be to check this first.
                AddReadAfterWriteDependency(
                    AgentType::WGT_STREAMER, weightStreamerAgentId,
                    m_CommandStreamAgents[this->m_OpToAgentIdMapping.at(m_FenceOpForWgtS)].agent.type,
                    this->m_OpToAgentIdMapping.at(m_FenceOpForWgtS), m_FenceOpForWgtS);
                m_FenceOpForWgtS = nullptr;
            }
        }
    }
    else if (inputBuffer->m_Location == Location::Sram && outputBuffer->m_Location == Location::Dram)
    {
        // cppcheck-suppress assertWithSideEffect
        assert(inputBuffer->Sram()->m_Offset.has_value());
        // cppcheck-suppress assertWithSideEffect
        assert(outputBuffer->Dram()->m_BufferType.has_value());

        // Get the producer of the input buffer and the producing agent type
        Op* producerOp = m_MergedOpGraph.GetSingleProducer(inputBuffer);
        assert(IsObjectOfType<PleOp>(producerOp) || IsObjectOfType<DmaOp>(producerOp));

        AgentType producerAgentType;
        if (IsObjectOfType<PleOp>(producerOp))
        {
            producerAgentType = AgentType::PLE_SCHEDULER;
        }
        else
        {
            producerAgentType = AgentType::IFM_STREAMER;
        }

        uint16_t outputBufferId = std::numeric_limits<uint16_t>::max();
        // Don't add buffers multiple times if they are used more than once
        if (m_DramBufToBufIdMapping.find(outputBuffer->Dram()) == m_DramBufToBufIdMapping.end())
        {
            outputBufferId = static_cast<uint16_t>(
                m_BufferManager.AddDram(outputBuffer->Dram()->m_BufferType.value(), outputBuffer->m_SizeInBytes));
            m_DramBufToBufIdMapping[outputBuffer->Dram()] = outputBufferId;

            if (outputBuffer->Dram()->m_BufferType.value() == BufferType::Output)
            {
                // cppcheck-suppress assertWithSideEffect
                assert(outputBuffer->Dram()->m_OperationId.has_value());
                // cppcheck-suppress assertWithSideEffect
                assert(outputBuffer->Dram()->m_ProducerOutputIndx);
                m_BufferManager.ChangeToOutput(outputBufferId, outputBuffer->Dram()->m_OperationId.value(),
                                               outputBuffer->Dram()->m_ProducerOutputIndx.value());
            }
        }
        else
        {
            outputBufferId = static_cast<uint16_t>(m_DramBufToBufIdMapping[outputBuffer->Dram()]);
        }

        uint32_t outputDramBufferOffset =
            utils::CalculateDramOffset(outputBuffer->m_Format, outputBuffer->m_TensorShape, ptrDmaOp->m_Offset);

        // Ofm Streamer Agent
        AgentIdType ofmStreamerAgentId = AddOfmStreamerToCommandStream(ptrDmaOp, inputBuffer->Sram(), outputBufferId,
                                                                       outputBuffer, outputDramBufferOffset);

        // Add 'Read After Write' dependency information to the IfmStreamer and PleScheduler agents
        // Read After Write Dependency for [OfmStreamer][IfmStreamer] or
        // Read After Write Dependency for [OfmStreamer][PleScheduler]
        AddReadAfterWriteDependency(AgentType::OFM_STREAMER, ofmStreamerAgentId, producerAgentType,
                                    m_OpToAgentIdMapping[producerOp], producerOp);

        // Add 'Write After Read' dependency information to the IfmStreamer and PleScheduler agents
        // Write After Read Dependency for [IfmStreamer][OfmStreamer] or
        // Write After Read Dependency for [PleScheduler][OfmStreamer]
        AddWriteAfterReadDependency(AgentType::OFM_STREAMER, ofmStreamerAgentId, producerAgentType,
                                    m_OpToAgentIdMapping[producerOp], producerOp);

        // Add 'Schedule Time' dependency information to the IfmStreamer and PleScheduler agents
        // Schedule Time Dependency for [IfmStreamer][OfmStreamer] or
        // Schedule Time Dependency for [PleScheduler][OfmStreamer]
        AddScheduleTimeDependency(AgentType::OFM_STREAMER, ofmStreamerAgentId, producerAgentType,
                                  m_OpToAgentIdMapping[producerOp], producerOp);
    }
    else
    {
        assert(false);
    }
}

void CascadingCommandStreamGenerator::ProcessMceOp(Op* const ptrMceOp)
{
    // Get the input buffers to the Mce Op
    OpGraph::BufferList inputBuffers = m_MergedOpGraph.GetInputs(ptrMceOp);
    assert(inputBuffers.size() == 2 && inputBuffers[g_MceIfmBufferIndex]->Sram()->m_Offset.has_value() &&
           inputBuffers[g_MceWeightBufferIndex]->Sram()->m_Offset.has_value());

    // Get the output buffer from the Mce Op
    Buffer* outputBuffer = m_MergedOpGraph.GetOutput(ptrMceOp);
    assert(outputBuffer != nullptr);

    auto producerOp = m_MergedOpGraph.GetSingleProducer(inputBuffers[g_MceIfmBufferIndex]);
    AgentType producerAgentType;
    if (IsObjectOfType<PleOp>(producerOp))
    {
        // MceOp takes input 0 from pleS agent
        producerAgentType = AgentType::PLE_SCHEDULER;
    }
    else
    {
        // MceOp takes input 0 from ifmS agent
        producerAgentType = AgentType::IFM_STREAMER;
    }

    // Construct and add the required agents to the command stream
    // Ple Loader Agent
    auto mceOpConsumer = m_MergedOpGraph.GetConsumer(outputBuffer, 0);
    assert(mceOpConsumer.first != nullptr && IsObjectOfType<PleOp>(mceOpConsumer.first));

    AgentIdType pleLoaderAgentId = 0;
    PleOp* ptrPleOp              = static_cast<PleOp*>(mceOpConsumer.first);

    if (ptrPleOp->m_LoadKernel)
    {
        pleLoaderAgentId = AddPleLoaderToCommandStream(ptrPleOp);

        if (m_FenceOpForPleL != nullptr)
        {
            // Note that this is an overly pessimistic approach, as corruption would only happen in practice if the SRAM
            // addresses used overlap, which we do not bother checking. A future improvement would be to check this first.
            AddReadAfterWriteDependency(
                AgentType::PLE_LOADER, pleLoaderAgentId,
                m_CommandStreamAgents[this->m_OpToAgentIdMapping.at(m_FenceOpForPleL)].agent.type,
                this->m_OpToAgentIdMapping.at(m_FenceOpForPleL), m_FenceOpForPleL);
            m_FenceOpForPleL = nullptr;
        }
    }

    // MCE Scheduler Agent
    AgentIdType mceSchedulerAgentId =
        AddMceSchedulerToCommandStream(static_cast<MceOp*>(ptrMceOp), ptrPleOp->m_PleKernelId);

    // Add 'Read After Write' dependency to the MceScheduler agent
    // Read After Write Dependency for [MceScheduler][IfmStreamer] or
    // Read After Write Dependency for [MceScheduler][PleScheduler]
    AddReadAfterWriteDependency(AgentType::MCE_SCHEDULER, mceSchedulerAgentId, producerAgentType,
                                m_OpToAgentIdMapping[producerOp], producerOp);
    // Read After Write Dependency for [MceScheduler][WeightStreamer]
    Op* wgtDmaOp = m_MergedOpGraph.GetSingleProducer(inputBuffers[g_MceWeightBufferIndex]);
    AddReadAfterWriteDependency(AgentType::MCE_SCHEDULER, mceSchedulerAgentId, AgentType::WGT_STREAMER,
                                m_OpToAgentIdMapping[wgtDmaOp], wgtDmaOp);

    // Add 'Write After Read' dependency information to the IfmStreamer and WeightStreamer agents
    // Write After Read Dependency for [IfmStreamer][MceScheduler] or
    // Write After Read Dependency for [PleScheduler][MceScheduler]
    AddWriteAfterReadDependency(AgentType::MCE_SCHEDULER, mceSchedulerAgentId, producerAgentType,
                                m_OpToAgentIdMapping[producerOp], producerOp);
    // Write After Read Dependency for [WeightStreamer][MceScheduler]
    AddWriteAfterReadDependency(AgentType::MCE_SCHEDULER, mceSchedulerAgentId, AgentType::WGT_STREAMER,
                                m_OpToAgentIdMapping[wgtDmaOp], wgtDmaOp);

    // Add 'Schedule Time' dependency information to the IfmStreamer and WeightStreamer agents
    // Schedule Time Dependency for [IfmStreamer][MceScheduler] or
    // Schedule Time Dependency for [PleScheduler][MceScheduler]
    AddScheduleTimeDependency(AgentType::MCE_SCHEDULER, mceSchedulerAgentId, producerAgentType,
                              m_OpToAgentIdMapping[producerOp], producerOp);
    // Schedule Time Dependency for [WeightStreamer][MceScheduler]

    AddScheduleTimeDependency(AgentType::MCE_SCHEDULER, mceSchedulerAgentId, AgentType::WGT_STREAMER,
                              m_OpToAgentIdMapping[wgtDmaOp], wgtDmaOp);
    // Add 'Schedule Time' dependency information to the PLE Loader agent
    // Schedule Time Dependency for [PLE Loader][MceScheduler]
    if (ptrPleOp->m_LoadKernel)
    {
        AddScheduleTimeDependency(AgentType::MCE_SCHEDULER, mceSchedulerAgentId, AgentType::PLE_LOADER,
                                  pleLoaderAgentId, nullptr);
    }
}

void CascadingCommandStreamGenerator::ProcessPleOp(Op* const ptrPleOp)
{
    // Get the input buffers to the Ple Op
    OpGraph::BufferList inputBuffers = m_MergedOpGraph.GetInputs(ptrPleOp);
    assert(inputBuffers.size() == 1 || inputBuffers.size() == 2);

    for (auto inputBuffer : inputBuffers)
    {
        if (inputBuffer->m_Location == Location::Sram)
        {
            // cppcheck-suppress assertWithSideEffect
            assert(inputBuffer->Sram()->m_Offset.has_value());
        }
        ETHOSN_UNUSED(inputBuffer);
    }

    // Get the output buffer from the Ple Op
    Buffer* outputBuffer = m_MergedOpGraph.GetOutput(ptrPleOp);
    // cppcheck-suppress assertWithSideEffect
    assert(outputBuffer->Sram()->m_Offset.has_value());

    // Determine whether ple op is standalone or fused
    bool isStandAlonePle = false;
    if (inputBuffers[g_PleInputBuffer0Index]->m_Location == Location::PleInputSram)
    {
        isStandAlonePle = false;
    }
    else if (inputBuffers[g_PleInputBuffer0Index]->m_Location == Location::Sram)
    {
        isStandAlonePle = true;
    }
    else
    {
        assert(false);
    }

    Op* input0Producer = m_MergedOpGraph.GetSingleProducer(inputBuffers[g_PleInputBuffer0Index]);
    Op* input1Producer = nullptr;
    if (inputBuffers.size() == 2)
    {
        input1Producer = m_MergedOpGraph.GetSingleProducer(inputBuffers[g_PleInputBuffer1Index]);
    }

    bool loadKernel = static_cast<PleOp*>(ptrPleOp)->m_LoadKernel;
    if (isStandAlonePle)
    {
        AgentIdType pleLoaderAgentId = {};

        if (loadKernel)
        {
            pleLoaderAgentId = AddPleLoaderToCommandStream(static_cast<PleOp*>(ptrPleOp));
        }

        AgentIdType pleSchedulerAgentId = AddPleSchedulerToCommandStream(static_cast<PleOp*>(ptrPleOp));

        AgentType input0AgentType = m_CommandStreamAgents[m_OpToAgentIdMapping[input0Producer]].agent.type;

        // Read After Write Dependency for [PleScheduler][IfmStreamer] or [PleScheduler][PleScheduler]
        AddReadAfterWriteDependency(AgentType::PLE_SCHEDULER, pleSchedulerAgentId, input0AgentType,
                                    m_OpToAgentIdMapping[input0Producer], input0Producer);
        if (input1Producer != nullptr)
        {
            AgentType input1AgentType = m_CommandStreamAgents[m_OpToAgentIdMapping[input1Producer]].agent.type;
            // Read After Write Dependency for [PleScheduler][IfmStreamer] or [PleScheduler][PleScheduler]
            AddReadAfterWriteDependency(AgentType::PLE_SCHEDULER, pleSchedulerAgentId, input1AgentType,
                                        m_OpToAgentIdMapping[input1Producer], input1Producer);
        }

        if (loadKernel)
        {
            // Read After Write Dependency for [PleScheduler][PleLoader]
            AddReadAfterWriteDependency(
                AgentType::PLE_SCHEDULER, pleSchedulerAgentId, AgentType::PLE_LOADER,
                m_PleKernelToPleLoaderAgentIdMapping[static_cast<PleOp*>(ptrPleOp)->m_PleKernelId], nullptr);

            if (m_FenceOpForPleL != nullptr)
            {
                // Note that this is an overly pessimistic approach, as corruption would only happen in practice if the SRAM
                // addresses used overlap, which we do not bother checking. A future improvement would be to check this first.
                AddReadAfterWriteDependency(
                    AgentType::PLE_LOADER, pleLoaderAgentId,
                    m_CommandStreamAgents[this->m_OpToAgentIdMapping.at(m_FenceOpForPleL)].agent.type,
                    this->m_OpToAgentIdMapping.at(m_FenceOpForPleL), m_FenceOpForPleL);
                m_FenceOpForPleL = nullptr;
            }
        }

        // Write After Read Dependency for [IfmStreamer][PleScheduler] or [PleScheduler][PleScheduler]
        AddWriteAfterReadDependency(AgentType::PLE_SCHEDULER, pleSchedulerAgentId, input0AgentType,
                                    m_OpToAgentIdMapping[input0Producer], input0Producer);

        // Schedule Time Dependency for [IfmStreamer][PleScheduler] or [PleScheduler][PleScheduler]
        AddScheduleTimeDependency(AgentType::PLE_SCHEDULER, pleSchedulerAgentId, input0AgentType,
                                  m_OpToAgentIdMapping[input0Producer], input0Producer);

        if (input1Producer != nullptr)
        {
            AgentType input1AgentType = m_CommandStreamAgents[m_OpToAgentIdMapping[input1Producer]].agent.type;

            // Write After Read Dependency for [IfmStreamer][PleScheduler] or [PleScheduler][PleScheduler]
            AddWriteAfterReadDependency(AgentType::PLE_SCHEDULER, pleSchedulerAgentId, input1AgentType,
                                        m_OpToAgentIdMapping[input1Producer], input1Producer);

            // Schedule Time Dependency for [IfmStreamer][PleScheduler] or [PleScheduler][PleScheduler]
            AddScheduleTimeDependency(AgentType::PLE_SCHEDULER, pleSchedulerAgentId, input1AgentType,
                                      m_OpToAgentIdMapping[input1Producer], input1Producer);
        }

        if (loadKernel)
        {
            // Schedule Time Dependency for [PleLoader][PleScheduler]
            AddScheduleTimeDependency(AgentType::PLE_SCHEDULER, pleSchedulerAgentId, AgentType::PLE_LOADER,
                                      pleLoaderAgentId, nullptr);
        }
    }
    else
    {
        AgentIdType pleSchedulerAgentId = AddPleSchedulerToCommandStream(static_cast<PleOp*>(ptrPleOp));

        // Read After Write Dependency for [PleScheduler][MceScheduler]
        AddReadAfterWriteDependency(AgentType::PLE_SCHEDULER, pleSchedulerAgentId, AgentType::MCE_SCHEDULER,
                                    m_OpToAgentIdMapping[input0Producer], input0Producer);
        if (loadKernel)
        {
            // Read After Write Dependency for [PleScheduler][PleLoader]
            AddReadAfterWriteDependency(
                AgentType::PLE_SCHEDULER, pleSchedulerAgentId, AgentType::PLE_LOADER,
                m_PleKernelToPleLoaderAgentIdMapping[static_cast<PleOp*>(ptrPleOp)->m_PleKernelId], nullptr);
        }

        // Schedule Time Dependency for [MceScheduler][PleScheduler]
        AddScheduleTimeDependency(AgentType::PLE_SCHEDULER, pleSchedulerAgentId, AgentType::MCE_SCHEDULER,
                                  m_OpToAgentIdMapping[input0Producer], input0Producer);
    }
    ETHOSN_UNUSED(outputBuffer);
}

void CascadingCommandStreamGenerator::ProcessSpaceToDepthOp(Op* const ptrSpaceToDepthOp)
{
    ETHOSN_UNUSED(ptrSpaceToDepthOp);
}

void CascadingCommandStreamGenerator::ProcessTransposeOp(Op* const ptrTransposeOp)
{
    ETHOSN_UNUSED(ptrTransposeOp);
}

// Private function to add IFM_STREAMER to the command stream
AgentIdType CascadingCommandStreamGenerator::AddIfmStreamerToCommandStream(DmaOp* const ptrOp,
                                                                           const uint16_t inputDramBufferId,
                                                                           const Buffer* const inputDramBuffer,
                                                                           const SramBuffer* const inputSramBuffer,
                                                                           const CascadingBufferFormat transferFormat,
                                                                           const uint32_t inputDramBufferOffset,
                                                                           bool isExtraIfmStripeAtRightEdge,
                                                                           bool isExtraIfmStripeAtBottomEdge)
{
    assert(inputSramBuffer->m_Format == CascadingBufferFormat::NHWCB);

    IfmSDesc ifmStreamerData = {};

    ifmStreamerData.fmData.dramOffset = inputDramBufferOffset;

    ifmStreamerData.fmData.bufferId = inputDramBufferId;

    StreamersUtils::SetBufferDataType(ifmStreamerData.fmData, transferFormat);
    ifmStreamerData.fmData.fcafInfo.signedActivation = (inputDramBuffer->m_DataType == DataType::INT8_QUANTIZED);
    ifmStreamerData.fmData.fcafInfo.zeroPoint =
        ethosn::utils::NumericCast<int16_t>(inputDramBuffer->m_QuantizationInfo.GetZeroPoint());

    CommonUtils::SetTileInfoForBuffer(m_Capabilities, ifmStreamerData.fmData.tile, inputSramBuffer);

    // The supertensor size is taken from either the SRAM buffer or the DRAM buffer, because these might be
    // different if there was a reshape. In the case of reshape then we use the SRAM shape so that is consistent
    // with the stripe shape which always comes from the SRAM buffer. If this is a concat/split though
    // then we need to use the DRAM shape because it will be a supertensor.
    TensorShape supertensorShape;
    if (utils::GetNumElements(inputSramBuffer->m_TensorShape) == utils::GetNumElements(inputDramBuffer->m_TensorShape))
    {
        supertensorShape = inputSramBuffer->m_TensorShape;
    }
    else
    {
        supertensorShape = inputDramBuffer->m_TensorShape;
    }

    StreamersUtils::SetStripeHeightInfo(ifmStreamerData.fmData, inputSramBuffer->m_TensorShape,
                                        inputSramBuffer->m_StripeShape);
    StreamersUtils::SetStripeWidthInfo(ifmStreamerData.fmData, inputSramBuffer->m_TensorShape,
                                       inputSramBuffer->m_StripeShape);
    StreamersUtils::SetStripeChannelsInfo(ifmStreamerData.fmData, inputSramBuffer->m_TensorShape,
                                          inputSramBuffer->m_StripeShape, ptrOp->m_Offset, supertensorShape);

    if (isExtraIfmStripeAtRightEdge && inputSramBuffer->m_PackedBoundaryThickness.right > 0)
    {
        ifmStreamerData.fmData.numStripes.width = static_cast<uint16_t>(ifmStreamerData.fmData.numStripes.width - 1);
        ifmStreamerData.isExtraPackedBoundaryDataOnRightEdge = true;
    }
    if (isExtraIfmStripeAtBottomEdge && inputSramBuffer->m_PackedBoundaryThickness.bottom > 0)
    {
        ifmStreamerData.fmData.numStripes.height = static_cast<uint16_t>(ifmStreamerData.fmData.numStripes.height - 1);
        ifmStreamerData.isExtraPackedBoundaryDataOnBottomEdge = true;
    }

    StreamersUtils::SetSuperTensorSizeInCells(ifmStreamerData.fmData, supertensorShape, transferFormat);

    StreamersUtils::SetStripeIdStrides(ifmStreamerData.fmData, inputSramBuffer->m_Order);
    ifmStreamerData.packedBoundaryThickness = inputSramBuffer->m_PackedBoundaryThickness;

    AgentDependencyInfo dependencyInfo = {};

    uint16_t numStripesTotal = ethosn::utils::NumericCast<uint16_t>(
        ifmStreamerData.fmData.numStripes.width * ifmStreamerData.fmData.numStripes.height *
        ifmStreamerData.fmData.numStripes.channels * inputSramBuffer->m_NumLoads);

    AgentDesc ifmStreamerAgent(numStripesTotal, ifmStreamerData);

    // Push the Ifm Streamer agent to the command stream
    AgentIdType agentId         = m_CommandStreamAgents.size();
    m_OpToAgentIdMapping[ptrOp] = agentId;
    m_CommandStreamAgents.push_back(AgentDescAndDeps{ ifmStreamerAgent, dependencyInfo });

    return agentId;
}

// Private function to add WGT_STREAMER to the command stream
AgentIdType CascadingCommandStreamGenerator::AddWeightStreamerToCommandStream(DmaOp* const ptrDmaOp)
{
    // Get the input buffer to the Dma Op
    OpGraph::BufferList inputBuffers = m_MergedOpGraph.GetInputs(ptrDmaOp);
    DramBuffer* weightsDramBuffer    = inputBuffers[g_DmaInputBufferIndex]->Dram();
    SramBuffer* weightsSramBuffer    = m_MergedOpGraph.GetOutput(ptrDmaOp)->Sram();

    // Get the Mce consumer of the weights buffer
    auto weightBufferConsumer = m_MergedOpGraph.GetConsumer(weightsSramBuffer, 0);
    assert(weightBufferConsumer.first != nullptr && IsObjectOfType<MceOp>(weightBufferConsumer.first));

    SramBuffer* ifmBuffer         = m_MergedOpGraph.GetInputs(weightBufferConsumer.first)[0]->Sram();
    PleInputSramBuffer* ofmBuffer = m_MergedOpGraph.GetOutput(weightBufferConsumer.first)->PleInputSram();

    EncodedWeights* encodedWeights = weightsDramBuffer->m_EncodedWeights.get();

    WgtSDesc weightStreamerData = {};

    std::vector<uint8_t>& compressedWeights = encodedWeights->m_Data;
    std::vector<uint8_t> metadataBytes;
    metadataBytes.assign(
        reinterpret_cast<const uint8_t*>(encodedWeights->m_Metadata.data()),
        reinterpret_cast<const uint8_t*>(encodedWeights->m_Metadata.data() + encodedWeights->m_Metadata.size()));
    weightStreamerData.bufferId = ethosn::utils::NumericCast<uint16_t>(
        m_BufferManager.AddDramConstant(BufferType::ConstantDma, compressedWeights));
    weightStreamerData.metadata = &encodedWeights->m_Metadata;
    CommonUtils::SetTileInfoForBuffer(m_Capabilities, weightStreamerData.tile, weightsSramBuffer);

    weightStreamerData.numStripes.ifmChannels =
        ethosn::utils::NumericCast<uint16_t>(utils::GetNumStripesC(ifmBuffer->m_TensorShape, ifmBuffer->m_StripeShape));
    weightStreamerData.numStripes.ofmChannels =
        ethosn::utils::NumericCast<uint16_t>(utils::GetNumStripesC(ofmBuffer->m_TensorShape, ofmBuffer->m_StripeShape));
    weightStreamerData.stripeIdStrides.ifmChannels = 1;
    weightStreamerData.stripeIdStrides.ofmChannels =
        ethosn::utils::NumericCast<uint16_t>(weightStreamerData.numStripes.ifmChannels * weightsSramBuffer->m_NumLoads);

    AgentDependencyInfo dependencyInfo = {};

    uint16_t numStripesTotal = ethosn::utils::NumericCast<uint16_t>(
        utils::GetNumStripesTotal(weightsSramBuffer->m_TensorShape, weightsSramBuffer->m_StripeShape) *
        weightsSramBuffer->m_NumLoads);

    AgentDesc weightStreamerAgent(numStripesTotal, weightStreamerData);

    // Push the Weight Streamer agent to the command stream
    AgentIdType agentId            = m_CommandStreamAgents.size();
    m_OpToAgentIdMapping[ptrDmaOp] = agentId;
    m_CommandStreamAgents.push_back(AgentDescAndDeps{ weightStreamerAgent, dependencyInfo });

    return agentId;
}

// Private function to add MCE_SCHEDULER to the command stream
AgentIdType CascadingCommandStreamGenerator::AddMceSchedulerToCommandStream(MceOp* const ptrMceOp,
                                                                            const PleKernelId pleKernelId)
{
    // Get the input buffers to the Mce Op
    OpGraph::BufferList inputBuffers = m_MergedOpGraph.GetInputs(ptrMceOp);
    SramBuffer* inputBuffer          = inputBuffers[g_MceIfmBufferIndex]->Sram();
    SramBuffer* weightSramBuffer     = inputBuffers[g_MceWeightBufferIndex]->Sram();
    DramBuffer* weightDramBuffer =
        m_MergedOpGraph.GetInputs(m_MergedOpGraph.GetSingleProducer(weightSramBuffer))[0]->Dram();

    // Get the output buffer from the Mce Op
    PleInputSramBuffer* outputBuffer = m_MergedOpGraph.GetOutput(ptrMceOp)->PleInputSram();

    MceSDesc mceSchedulerData = {};

    CommonUtils::SetTileInfoForBuffer(m_Capabilities, mceSchedulerData.ifmTile, inputBuffer);

    CommonUtils::SetTileInfoForBuffer(m_Capabilities, mceSchedulerData.wgtTile, weightSramBuffer);

    mceSchedulerData.blockSize.width  = ethosn::utils::NumericCast<uint8_t>(ptrMceOp->m_BlockConfig.m_BlockWidth());
    mceSchedulerData.blockSize.height = ethosn::utils::NumericCast<uint8_t>(ptrMceOp->m_BlockConfig.m_BlockHeight());

    MceSUtils::setMcesOpMode(mceSchedulerData, ptrMceOp->m_Op);

    MceSUtils::SetMcesOfmHeightStripeInfo(mceSchedulerData, outputBuffer->m_TensorShape, ptrMceOp->m_OutputStripeShape);
    MceSUtils::SetMcesOfmWidthStripeInfo(mceSchedulerData, outputBuffer->m_TensorShape, ptrMceOp->m_OutputStripeShape);
    if (ptrMceOp->m_Op == command_stream::MceOperation::FULLY_CONNECTED)
    {
        // Fully connected stripe shapes are always 8x8xC (for both default and edge stripes).
        // This is due to the reinterpretation that the hardware requires.
        const uint16_t w = ethosn::utils::NumericCast<uint16_t>(utils::GetWidth(g_BrickGroupShape));
        const uint16_t h = ethosn::utils::NumericCast<uint16_t>(utils::GetHeight(g_BrickGroupShape));
        mceSchedulerData.edgeStripeSize.ofmWidth     = w;
        mceSchedulerData.edgeStripeSize.ofmHeight    = h;
        mceSchedulerData.defaultStripeSize.ofmWidth  = w;
        mceSchedulerData.defaultStripeSize.ofmHeight = h;
    }

    MceSUtils::SetMcesOfmChannelsStripeInfo(mceSchedulerData, outputBuffer->m_TensorShape,
                                            ptrMceOp->m_OutputStripeShape);
    MceSUtils::SetMcesIfmChannelsStripeInfo(mceSchedulerData, inputBuffer->m_TensorShape, inputBuffer->m_StripeShape);

    MceSUtils::SetStripeIdStrides(mceSchedulerData, ptrMceOp->m_Order);

    mceSchedulerData.convStrideXy.x = ethosn::utils::NumericCast<uint8_t>(ptrMceOp->m_Stride.m_X);
    mceSchedulerData.convStrideXy.y = ethosn::utils::NumericCast<uint8_t>(ptrMceOp->m_Stride.m_Y);
    mceSchedulerData.ifmZeroPoint = ethosn::utils::NumericCast<int16_t>(inputBuffer->m_QuantizationInfo.GetZeroPoint());
    mceSchedulerData.isIfmSigned  = static_cast<uint8_t>(inputBuffer->m_DataType == DataType::INT8_QUANTIZED);
    mceSchedulerData.isOfmSigned  = static_cast<uint8_t>(outputBuffer->m_DataType == DataType::INT8_QUANTIZED);

    MceSUtils::setMcesAlgorithm(mceSchedulerData, ptrMceOp->m_Algo);

    mceSchedulerData.upsampleType = ptrMceOp->m_UpsampleType;

    const uint32_t outputBufferWidth  = utils::GetWidth(outputBuffer->m_TensorShape);
    const uint32_t outputBufferHeight = utils::GetHeight(outputBuffer->m_TensorShape);

    const bool isUpsample = mceSchedulerData.upsampleType != MceUpsampleType::OFF;
    if (isUpsample)
    {
        // As only 2x resize is supported, drop mode is only possible for odd output width/height.
        mceSchedulerData.upsampleEdgeMode.col =
            (outputBufferWidth & 1) ? MceUpsampleEdgeMode::DROP : MceUpsampleEdgeMode::GENERATE;
        mceSchedulerData.upsampleEdgeMode.row =
            (outputBufferHeight & 1) ? MceUpsampleEdgeMode::DROP : MceUpsampleEdgeMode::GENERATE;
    }

    MceSUtils::SetMcesConvolutionData(mceSchedulerData, m_MergedOpGraph, ptrMceOp,
                                      weightDramBuffer->m_EncodedWeights->m_IsWideFilter);

    mceSchedulerData.ifmStripeShapeDefault.height =
        static_cast<uint16_t>(inputBuffer->m_StripeShape[1] + inputBuffer->m_PackedBoundaryThickness.top +
                              inputBuffer->m_PackedBoundaryThickness.bottom);
    mceSchedulerData.ifmStripeShapeDefault.width =
        static_cast<uint16_t>(inputBuffer->m_StripeShape[2] + inputBuffer->m_PackedBoundaryThickness.left +
                              inputBuffer->m_PackedBoundaryThickness.right);
    // Note that the IFM edge stripe shape is not used when packed boundary data is used, so we don't need to account
    // for that here.
    mceSchedulerData.ifmStripeShapeEdge.height = CommonUtils::CalculateEdgeSize(
        utils::GetHeight(inputBuffer->m_TensorShape), utils::GetHeight(inputBuffer->m_StripeShape));
    mceSchedulerData.ifmStripeShapeEdge.width = CommonUtils::CalculateEdgeSize(
        utils::GetWidth(inputBuffer->m_TensorShape), utils::GetWidth(inputBuffer->m_StripeShape));

    mceSchedulerData.reluActiv.min = ptrMceOp->m_LowerBound;
    mceSchedulerData.reluActiv.max = ptrMceOp->m_UpperBound;
    mceSchedulerData.pleKernelId   = pleKernelId;

    mceSchedulerData.isPackedBoundaryX =
        (inputBuffer->m_PackedBoundaryThickness.left + inputBuffer->m_PackedBoundaryThickness.right) > 0;
    mceSchedulerData.isPackedBoundaryY =
        (inputBuffer->m_PackedBoundaryThickness.top + inputBuffer->m_PackedBoundaryThickness.bottom) > 0;

    AgentDependencyInfo dependencyInfo = {};
    uint16_t numStripesTotal           = ethosn::utils::NumericCast<uint16_t>(
        mceSchedulerData.numStripes.ifmChannels * mceSchedulerData.numStripes.ofmChannels *
        mceSchedulerData.numStripes.ofmWidth * mceSchedulerData.numStripes.ofmHeight);

    AgentDesc mceSchedulerAgent(numStripesTotal, mceSchedulerData);

    // Push the Mce Scheduler agent to the command stream
    AgentIdType agentId            = m_CommandStreamAgents.size();
    m_OpToAgentIdMapping[ptrMceOp] = agentId;
    m_CommandStreamAgents.push_back(AgentDescAndDeps{ mceSchedulerAgent, dependencyInfo });

    return agentId;
}

// Private function to add PLE_LOADER to the command stream
AgentIdType CascadingCommandStreamGenerator::AddPleLoaderToCommandStream(PleOp* const ptrPleOp)
{
    // Create a new Ple Loader agent
    PleLDesc pleLoaderData    = {};
    pleLoaderData.pleKernelId = ptrPleOp->m_PleKernelId;
    pleLoaderData.sramAddr    = ethosn::utils::NumericCast<uint32_t>(ptrPleOp->m_Offset.value());

    AgentDependencyInfo dependencyInfo = {};
    uint16_t numStripesTotal           = 1U;

    AgentDesc pleLoaderAgent(numStripesTotal, pleLoaderData);

    // Push the Ple Loader agent to the command stream
    AgentIdType agentId                                           = m_CommandStreamAgents.size();
    m_PleKernelToPleLoaderAgentIdMapping[ptrPleOp->m_PleKernelId] = agentId;
    m_CommandStreamAgents.push_back(AgentDescAndDeps{ pleLoaderAgent, dependencyInfo });

    return agentId;
}

// Private function to add PLE_SCHEDULER to the command stream
AgentIdType CascadingCommandStreamGenerator::AddPleSchedulerToCommandStream(PleOp* const ptrPleOp)
{
    // Get the input buffers to the Ple Op
    OpGraph::BufferList inputBuffers = m_MergedOpGraph.GetInputs(ptrPleOp);
    assert(inputBuffers.size() == 1 || inputBuffers.size() == 2);

    Buffer* inputBuffer0 = inputBuffers[g_PleInputBuffer0Index];

    // Get the output buffer from the Ple Op
    SramBuffer* outputBuffer = m_MergedOpGraph.GetOutput(ptrPleOp)->Sram();

    PleSDesc pleS = {};

    pleS.ofmZeroPoint = ethosn::utils::NumericCast<int16_t>(outputBuffer->m_QuantizationInfo.GetZeroPoint());

    PleSUtils::SetPlesHeightStripeInfo(pleS, outputBuffer->m_TensorShape, ptrPleOp->m_OutputStripeShape);
    PleSUtils::SetPlesWidthStripeInfo(pleS, outputBuffer->m_TensorShape, ptrPleOp->m_OutputStripeShape);
    PleSUtils::SetPlesChannelsStripeInfo(pleS, outputBuffer->m_TensorShape, ptrPleOp->m_OutputStripeShape);

    PleSUtils::SetStripeIdStrides(pleS, outputBuffer);

    // Can't use CommonUtils::SetTileInfoForBuffer because PLE OFM tile might be different to OfmS tile
    // (strategies where OfmS does the full height but PLE does partial height)
    PleSUtils::SetPlesTileInfo(m_Capabilities, pleS, outputBuffer);

    // Calculate input mode of Ple OP dependent on input buffer producer.
    auto pleOpProducer = m_MergedOpGraph.GetSingleProducer(inputBuffer0);
    if (inputBuffer0->m_Location == Location::Sram)
    {
        if (inputBuffers.size() == 1)
        {
            pleS.inputMode = PleInputMode::SRAM_ONE_INPUT;
        }
        else
        {
            pleS.inputMode = PleInputMode::SRAM_TWO_INPUTS;
        }
    }
    else if (inputBuffer0->m_Location == Location::PleInputSram)
    {
        PleSUtils::SetFusedPleSInputMode(pleS, static_cast<MceOp*>(pleOpProducer));
    }
    else
    {
        assert(false);
    }

    pleS.pleKernelSramAddr = ethosn::utils::NumericCast<uint32_t>(ptrPleOp->m_Offset.value());

    pleS.pleKernelId = ptrPleOp->m_PleKernelId;

    if (pleS.inputMode == PleInputMode::SRAM_ONE_INPUT || pleS.inputMode == PleInputMode::SRAM_TWO_INPUTS)
    {
        CommonUtils::SetTileInfoForBuffer(m_Capabilities, pleS.ifmTile0, inputBuffer0->Sram());
    }

    pleS.ifmInfo0.zeroPoint  = ethosn::utils::NumericCast<int16_t>(inputBuffer0->m_QuantizationInfo.GetZeroPoint());
    pleS.ifmInfo0.multiplier = ptrPleOp->m_Input0Multiplier;
    pleS.ifmInfo0.shift      = ptrPleOp->m_Input0Shift;

    // Note these are set even if there is only 1 input, because some PLE kernels (e.g. LeakyRelu)
    // use these to pass extra information
    pleS.ifmInfo1.multiplier = ptrPleOp->m_Input1Multiplier;
    pleS.ifmInfo1.shift      = ptrPleOp->m_Input1Shift;

    if (inputBuffers.size() == 2)
    {
        SramBuffer* inputBuffer1 = inputBuffers[g_PleInputBuffer1Index]->Sram();
        CommonUtils::SetTileInfoForBuffer(m_Capabilities, pleS.ifmTile1, inputBuffer1);

        pleS.ifmInfo1.zeroPoint = ethosn::utils::NumericCast<int16_t>(inputBuffer1->m_QuantizationInfo.GetZeroPoint());
    }

    AgentDependencyInfo info = {};
    uint16_t numStripesTotal = ethosn::utils::NumericCast<uint16_t>(
        utils::GetNumStripesTotal(outputBuffer->m_TensorShape, ptrPleOp->m_OutputStripeShape));

    AgentDesc pleSchedulerAgent(numStripesTotal, pleS);

    // Push the Ple Scheduler agent to the command stream
    AgentIdType agentId            = m_CommandStreamAgents.size();
    m_OpToAgentIdMapping[ptrPleOp] = agentId;
    m_CommandStreamAgents.push_back(AgentDescAndDeps{ pleSchedulerAgent, info });

    return agentId;
}

// Private function to add OFM_STREAMER to the command stream
AgentIdType CascadingCommandStreamGenerator::AddOfmStreamerToCommandStream(DmaOp* const ptrOp,
                                                                           const SramBuffer* const outputSramBuffer,
                                                                           const uint16_t outputDramBufferId,
                                                                           const Buffer* const outputDramBuffer,
                                                                           const uint32_t outputDramBufferOffset)
{
    assert(outputSramBuffer->m_Format == CascadingBufferFormat::NHWCB);

    OfmSDesc ofmStreamerData = {};

    ofmStreamerData.fmData.dramOffset = outputDramBufferOffset;

    ofmStreamerData.fmData.bufferId = outputDramBufferId;

    StreamersUtils::SetBufferDataType(ofmStreamerData.fmData, outputDramBuffer->m_Format);

    ofmStreamerData.fmData.fcafInfo.signedActivation = (outputDramBuffer->m_DataType == DataType::INT8_QUANTIZED);
    ofmStreamerData.fmData.fcafInfo.zeroPoint =
        ethosn::utils::NumericCast<int16_t>(outputDramBuffer->m_QuantizationInfo.GetZeroPoint());

    CommonUtils::SetTileInfoForBuffer(m_Capabilities, ofmStreamerData.fmData.tile, outputSramBuffer);

    // The supertensor size is taken from either the SRAM buffer or the DRAM buffer, because these might be
    // different if there was a reshape. In the case of reshape then we use the SRAM shape so that is consistent
    // with the stripe shape which always comes from the SRAM buffer. If this is a concat/split though
    // then we need to use the DRAM shape because it will be a supertensor.
    TensorShape supertensorShape;
    if (utils::GetNumElements(outputSramBuffer->m_TensorShape) ==
        utils::GetNumElements(outputDramBuffer->m_TensorShape))
    {
        supertensorShape = outputSramBuffer->m_TensorShape;
    }
    else
    {
        supertensorShape = outputDramBuffer->m_TensorShape;
    }

    StreamersUtils::SetStripeHeightInfo(ofmStreamerData.fmData, outputSramBuffer->m_TensorShape,
                                        outputSramBuffer->m_StripeShape);
    StreamersUtils::SetStripeWidthInfo(ofmStreamerData.fmData, outputSramBuffer->m_TensorShape,
                                       outputSramBuffer->m_StripeShape);
    StreamersUtils::SetStripeChannelsInfo(ofmStreamerData.fmData, outputSramBuffer->m_TensorShape,
                                          outputSramBuffer->m_StripeShape, ptrOp->m_Offset, supertensorShape);

    StreamersUtils::SetSuperTensorSizeInCells(ofmStreamerData.fmData, supertensorShape, outputDramBuffer->m_Format);

    StreamersUtils::SetStripeIdStrides(ofmStreamerData.fmData, outputSramBuffer->m_Order);

    AgentDependencyInfo dependencyInfo = {};
    uint16_t numStripesTotal           = ethosn::utils::NumericCast<uint16_t>(
        utils::GetNumStripesTotal(outputSramBuffer->m_TensorShape, outputSramBuffer->m_StripeShape));

    AgentDesc ofmStreamerAgent(numStripesTotal, ofmStreamerData);

    // Push the Ofm Streamer agent to the command stream
    AgentIdType agentId         = m_CommandStreamAgents.size();
    m_OpToAgentIdMapping[ptrOp] = agentId;
    m_CommandStreamAgents.push_back(AgentDescAndDeps{ ofmStreamerAgent, dependencyInfo });

    return agentId;
}

// Private function to add ReadAfterWrite Dependency
// Consumer agent creates and own the dependency
inline void CascadingCommandStreamGenerator::AddReadAfterWriteDependency(const AgentType consumerAgentType,
                                                                         const AgentIdType consumerAgentId,
                                                                         const AgentType producerAgentType,
                                                                         const AgentIdType producerAgentId,
                                                                         const Op* producerOp)
{
    AgentIdType relativeAgentId = consumerAgentId - producerAgentId;
    assert(relativeAgentId <= g_MaxRelativeAgentPosition);

    Dependency newDependency      = {};
    newDependency.relativeAgentId = static_cast<RelativeAgentIdType>(relativeAgentId);
    FillConsumerAgentDependency(newDependency, consumerAgentType, consumerAgentId, producerAgentType, producerAgentId,
                                producerOp);
    m_CommandStreamAgents[consumerAgentId].deps.readDependencies.push_back(newDependency);
}

// Private function to add SRAM Overlap Dependency
// Consumer agent creates and own the dependency
inline void CascadingCommandStreamGenerator::AddSramOverlapDependency(
    const command_stream::cascading::AgentType consumerAgentType,
    const AgentIdType consumerAgentId,
    const command_stream::cascading::AgentType producerAgentType,
    const AgentIdType producerAgentId,
    const Op* producerOp)
{
    AgentIdType relativeAgentId = consumerAgentId - producerAgentId;
    assert(relativeAgentId <= g_MaxRelativeAgentPosition);

    Dependency newDependency      = {};
    newDependency.relativeAgentId = static_cast<RelativeAgentIdType>(relativeAgentId);
    FillConsumerAgentDependency(newDependency, consumerAgentType, consumerAgentId, producerAgentType, producerAgentId,
                                producerOp);
    if (newDependency.relativeAgentId != 0)
    {
        m_CommandStreamAgents[consumerAgentId].deps.readDependencies.push_back(newDependency);
    }
}

// Private function to add WriteAfterRead Dependency
// Last consumer agent creates the dependency and assign it to the producer agent
inline void CascadingCommandStreamGenerator::AddWriteAfterReadDependency(const AgentType consumerAgentType,
                                                                         const AgentIdType consumerAgentId,
                                                                         const AgentType producerAgentType,
                                                                         const AgentIdType producerAgentId,
                                                                         const Op* producerOp)
{
    AgentIdType relativeAgentId = consumerAgentId - producerAgentId;
    assert(relativeAgentId <= g_MaxRelativeAgentPosition);

    Dependency newDependency      = {};
    newDependency.relativeAgentId = static_cast<RelativeAgentIdType>(relativeAgentId);
    FillProducerAgentDependency(newDependency, consumerAgentType, consumerAgentId, producerAgentType, producerAgentId,
                                producerOp, DependencyType::Write);
    if (newDependency.relativeAgentId != 0)
    {
        m_CommandStreamAgents[producerAgentId].deps.writeDependencies.push_back(newDependency);
    }
}

// Private function to add ScheduleTime Dependency
// First consumer agent creates the dependency and assign it to the producer agent
inline void CascadingCommandStreamGenerator::AddScheduleTimeDependency(const AgentType consumerAgentType,
                                                                       const AgentIdType consumerAgentId,
                                                                       const AgentType producerAgentType,
                                                                       const AgentIdType producerAgentId,
                                                                       const Op* producerOp)
{
    AgentIdType relativeAgentId = consumerAgentId - producerAgentId;
    assert(relativeAgentId <= g_MaxRelativeAgentPosition);

    Dependency newDependency      = {};
    newDependency.relativeAgentId = static_cast<RelativeAgentIdType>(relativeAgentId);
    FillProducerAgentDependency(newDependency, consumerAgentType, consumerAgentId, producerAgentType, producerAgentId,
                                producerOp, DependencyType::Schedule);
    if (newDependency.relativeAgentId != 0)
    {
        m_CommandStreamAgents[producerAgentId].deps.scheduleDependencies.push_back(newDependency);
    }
}

// Private function to fill the dependency data for Read After Write or SRAM Overlap dependencies
void CascadingCommandStreamGenerator::FillConsumerAgentDependency(
    Dependency& consumerAgentDependency,
    const command_stream::cascading::AgentType consumerAgentType,
    const AgentIdType consumerAgentId,
    const command_stream::cascading::AgentType producerAgentType,
    const AgentIdType producerAgentId,
    const Op* producerOp) const
{
    const AgentDescAndDeps& consumerAgentAndDeps = m_CommandStreamAgents[consumerAgentId];
    const AgentDesc& consumerAgentData           = consumerAgentAndDeps.agent;
    const uint16_t consumerAgentNumStripes       = consumerAgentAndDeps.agent.numStripesTotal;
    const AgentDescAndDeps& producerAgentAndDeps = m_CommandStreamAgents[producerAgentId];
    const AgentDesc& producerAgentData           = producerAgentAndDeps.agent;
    const uint16_t producerAgentNumStripes       = producerAgentAndDeps.agent.numStripesTotal;

    // Add a new 'Read After Write' dependency
    switch (consumerAgentType)
    {
        case AgentType::IFM_STREAMER:
        {
            // Read After Write Dependency for [IfmStreamer][OfmStreamer]
            if (producerAgentType == AgentType::OFM_STREAMER)
            {
                // The IfmS should wait until the OfmS has completely finished.
                consumerAgentDependency.outerRatio.other = producerAgentNumStripes;
                consumerAgentDependency.outerRatio.self  = consumerAgentNumStripes;

                consumerAgentDependency.innerRatio.other = producerAgentNumStripes;
                consumerAgentDependency.innerRatio.self  = 1;

                consumerAgentDependency.boundary = 0;
            }
            break;
        }

        case AgentType::WGT_STREAMER:
        {
            // Sram Overlap Dependency for [WeightStreamer][OfmStreamer]
            if (producerAgentType == AgentType::OFM_STREAMER)
            {
                // The WgtS should wait until the OfmS has completely finished.
                consumerAgentDependency.outerRatio.other = producerAgentNumStripes;
                consumerAgentDependency.outerRatio.self  = consumerAgentNumStripes;

                consumerAgentDependency.innerRatio.other = producerAgentNumStripes;
                consumerAgentDependency.innerRatio.self  = 1;

                consumerAgentDependency.boundary = 0;
            }
            // Sram Overlap Dependency for [WeightStreamer][PleScheduler]
            else if (producerAgentType == AgentType::PLE_SCHEDULER)
            {
                // The WgtS needs to wait for the prior PleS in the same section, for example in a strategy 1 cascade,
                // because these weights shouldn't be loaded until the weights from the previous layer are finished with.
                // The WgtS should wait until the PleS has completely finished.
                consumerAgentDependency.outerRatio.other = producerAgentNumStripes;
                consumerAgentDependency.outerRatio.self  = consumerAgentNumStripes;

                consumerAgentDependency.innerRatio.other = producerAgentNumStripes;
                consumerAgentDependency.innerRatio.self  = 1;

                consumerAgentDependency.boundary = 0;
            }
            break;
        }

        case AgentType::MCE_SCHEDULER:
        {
            // Read After Write Dependency for [MceScheduler][IfmStreamer]
            if (producerAgentType == AgentType::IFM_STREAMER)
            {
                DependencyUtils::CalculateIfmSMceSOuterRatio(consumerAgentData, producerAgentData,
                                                             consumerAgentDependency.outerRatio.self,
                                                             consumerAgentDependency.outerRatio.other);

                // The MceS can process more data than is loaded by the IfmS (e.g. two stripes at a time)
                uint16_t widthRatio  = ethosn::utils::NumericCast<uint16_t>(utils::DivRoundUp(
                    consumerAgentData.mce.numStripes.ofmWidth, producerAgentData.ifm.fmData.numStripes.width));
                uint16_t heightRatio = ethosn::utils::NumericCast<uint16_t>(utils::DivRoundUp(
                    consumerAgentData.mce.numStripes.ofmHeight, producerAgentData.ifm.fmData.numStripes.height));

                if (consumerAgentData.mce.mceOpMode == MceOperation::DEPTHWISE_CONVOLUTION)
                {
                    assert(consumerAgentData.mce.numStripes.ifmChannels == 1);
                }
                else
                {
                    assert(consumerAgentData.mce.numStripes.ifmChannels ==
                           producerAgentData.ifm.fmData.numStripes.channels);
                }

                consumerAgentDependency.innerRatio.other =
                    ethosn::utils::NumericCast<uint16_t>(widthRatio * heightRatio);
                consumerAgentDependency.innerRatio.self = 1;

                consumerAgentDependency.boundary = DependencyUtils::CalculateMceSBoundary(consumerAgentData.mce);
            }
            // Read After Write Dependency for [MceScheduler][WeightStreamer]
            else if (producerAgentType == AgentType::WGT_STREAMER)
            {
                // MCE always traverses in IXYO order. Each MCE stripe needs a new weight stripe, unless a weight stripe
                // can be re-used which can only happen if we are not IFM splitting and we are moving in XY.

                // Outer ratio is not needed (set to max)
                consumerAgentDependency.outerRatio.other = producerAgentNumStripes;
                consumerAgentDependency.outerRatio.self  = consumerAgentNumStripes;

                if (consumerAgentData.mce.numStripes.ifmChannels == 1)
                {
                    // Weight stripes can be re-used as we move in XY
                    consumerAgentDependency.innerRatio.self = ethosn::utils::NumericCast<uint16_t>(
                        consumerAgentData.mce.numStripes.ofmHeight * consumerAgentData.mce.numStripes.ofmWidth);
                    consumerAgentDependency.innerRatio.other = 1;
                }
                else
                {
                    // No re-use, 1:1 dependency
                    consumerAgentDependency.innerRatio.self  = 1;
                    consumerAgentDependency.innerRatio.other = 1;
                }

                consumerAgentDependency.boundary = 0;
            }
            // Read After Write Dependency for [MceScheduler][PleScheduler]
            else if (producerAgentType == AgentType::PLE_SCHEDULER)
            {
                // Calculate outer ratios using number of stripes
                consumerAgentDependency.outerRatio.other = ethosn::utils::NumericCast<uint16_t>(
                    producerAgentData.pleS.numStripes.height * producerAgentData.pleS.numStripes.width *
                    producerAgentData.pleS.numStripes.channels);
                consumerAgentDependency.outerRatio.self = ethosn::utils::NumericCast<uint16_t>(
                    consumerAgentData.mce.numStripes.ofmHeight * consumerAgentData.mce.numStripes.ofmWidth *
                    consumerAgentData.mce.numStripes.ofmChannels);

                // Calculate inner ratios using ratio of stripe size
                uint16_t widthRatio   = ethosn::utils::NumericCast<uint16_t>(utils::DivRoundUp(
                    producerAgentData.pleS.numStripes.width, consumerAgentData.mce.numStripes.ofmWidth));
                uint16_t heightRatio  = ethosn::utils::NumericCast<uint16_t>(utils::DivRoundUp(
                    producerAgentData.pleS.numStripes.height, consumerAgentData.mce.numStripes.ofmHeight));
                uint16_t channelRatio = ethosn::utils::NumericCast<uint16_t>(utils::DivRoundUp(
                    producerAgentData.pleS.numStripes.channels, consumerAgentData.mce.numStripes.ofmChannels));

                consumerAgentDependency.innerRatio.other =
                    ethosn::utils::NumericCast<uint16_t>(widthRatio * heightRatio * channelRatio);
                consumerAgentDependency.innerRatio.self = 1;

                consumerAgentDependency.boundary = DependencyUtils::CalculateMceSBoundary(consumerAgentData.mce);
            }
            else
            {
                assert(false);
            }
            break;
        }

        case AgentType::PLE_LOADER:
        {
            // Sram Overlap Dependency for [PleLoader][PleScheduler]
            if (producerAgentType == AgentType::PLE_SCHEDULER)
            {
                consumerAgentDependency.outerRatio.other = producerAgentNumStripes;
                consumerAgentDependency.outerRatio.self  = consumerAgentNumStripes;

                consumerAgentDependency.innerRatio.other = producerAgentNumStripes;
                consumerAgentDependency.innerRatio.self  = 1;

                consumerAgentDependency.boundary = 0;
            }
            // Sram Overlap Dependency for [PleLoader][OfmStreamer]
            else if (producerAgentType == AgentType::OFM_STREAMER)
            {
                consumerAgentDependency.outerRatio.other = producerAgentNumStripes;
                consumerAgentDependency.outerRatio.self  = consumerAgentNumStripes;

                consumerAgentDependency.innerRatio.other = producerAgentNumStripes;
                consumerAgentDependency.innerRatio.self  = 1;

                consumerAgentDependency.boundary = 0;
            }
            else
            {
                assert(false);
            }
            break;
        }

        case AgentType::PLE_SCHEDULER:
        {
            // Read After Write Dependency for [PleScheduler][IfmStreamer]
            if (producerAgentType == AgentType::IFM_STREAMER)
            {
                // Calculate outer ratios using number of stripes
                consumerAgentDependency.outerRatio.other = ethosn::utils::NumericCast<uint16_t>(
                    producerAgentData.ifm.fmData.numStripes.width * producerAgentData.ifm.fmData.numStripes.height *
                    producerAgentData.ifm.fmData.numStripes.channels);
                consumerAgentDependency.outerRatio.self = ethosn::utils::NumericCast<uint16_t>(
                    consumerAgentData.pleS.numStripes.height * consumerAgentData.pleS.numStripes.width *
                    consumerAgentData.pleS.numStripes.channels);
            }
            // Read After Write Dependency for [PleScheduler][MceScheduler]
            else if (producerAgentType == AgentType::MCE_SCHEDULER)
            {
                // Outer ratio not used (set to max)
                consumerAgentDependency.outerRatio.other = producerAgentNumStripes;
                consumerAgentDependency.outerRatio.self  = consumerAgentNumStripes;

                // Calculate inner ratios using ratio of stripe channels
                uint16_t channelRatio = ethosn::utils::NumericCast<uint16_t>(utils::DivRoundUp(
                    producerAgentData.mce.numStripes.ofmChannels, consumerAgentData.pleS.numStripes.channels));

                consumerAgentDependency.innerRatio.other =
                    ethosn::utils::NumericCast<uint16_t>(channelRatio * producerAgentData.mce.numStripes.ifmChannels);
                consumerAgentDependency.innerRatio.self = 1;

                // Set boundary to 1 if producer stripe count is not a factor of consumer stripe count
                uint16_t numberOfIfmStripesInXYDimProducer = ethosn::utils::NumericCast<uint16_t>(
                    producerAgentData.mce.numStripes.ofmWidth * producerAgentData.mce.numStripes.ofmHeight);
                uint16_t numberOfIfmStripesInXYDimConsumer = ethosn::utils::NumericCast<uint16_t>(
                    consumerAgentData.pleS.numStripes.width * consumerAgentData.pleS.numStripes.height);

                uint16_t ifmStripeRemainder = ethosn::utils::NumericCast<uint16_t>(numberOfIfmStripesInXYDimConsumer %
                                                                                   numberOfIfmStripesInXYDimProducer);
                if (ifmStripeRemainder == 0)
                {
                    consumerAgentDependency.boundary = 0;
                }
                else
                {
                    consumerAgentDependency.boundary = 1;
                }
            }
            // Read After Write Dependency for [PleScheduler][PleLoader]
            else if (producerAgentType == AgentType::PLE_LOADER)
            {
                consumerAgentDependency.outerRatio.other = 1U;
                consumerAgentDependency.outerRatio.self  = ethosn::utils::NumericCast<uint16_t>(
                    consumerAgentData.pleS.numStripes.height * consumerAgentData.pleS.numStripes.width *
                    consumerAgentData.pleS.numStripes.channels);
            }
            // Read After Write Dependency for [PleScheduler][PleScheduler]
            else if (producerAgentType == AgentType::PLE_SCHEDULER)
            {
                // We only support strategy 3 (full tensor) cascading for Ple -> Ple
                consumerAgentDependency.outerRatio.other = 1U;
                consumerAgentDependency.outerRatio.self  = 1U;
            }
            else
            {
                assert(false);
            }
            break;
        }

        case AgentType::OFM_STREAMER:
        {
            // Read After Write Dependency for [OfmStreamer][IfmStreamer]
            if (producerAgentType == AgentType::IFM_STREAMER)
            {
                // Simple 1:1 dependency
                consumerAgentDependency.outerRatio.other = 1;
                consumerAgentDependency.outerRatio.self  = 1;

                consumerAgentDependency.innerRatio.other = 1;
                consumerAgentDependency.innerRatio.self  = 1;

                consumerAgentDependency.boundary = 0;
            }
            // Read After Write Dependency for [OfmStreamer][PleScheduler]
            else if (producerAgentType == AgentType::PLE_SCHEDULER)
            {
                // Normally this is a simple 1:1 dependency, but in some cases the PLE can have multiple stripes
                // for each OFM stripe (strategies where OfmS does the full height but PLE does partial height)

                // Outer ratio is not used (set to max)
                consumerAgentDependency.outerRatio.other = producerAgentNumStripes;
                consumerAgentDependency.outerRatio.self  = consumerAgentNumStripes;

                // Inner ratio based on the stripe heights
                consumerAgentDependency.innerRatio.other =
                    ethosn::utils::NumericCast<uint16_t>(consumerAgentData.ofm.fmData.defaultStripeSize.height /
                                                         producerAgentData.pleS.defaultStripeSize.height);
                consumerAgentDependency.innerRatio.self = 1;

                command_stream::PleOperation pleOperation = static_cast<const PleOp*>(producerOp)->m_Op;
                if ((pleOperation == command_stream::PleOperation::MAXPOOL_3X3_2_2_EVEN ||
                     pleOperation == command_stream::PleOperation::MAXPOOL_3X3_2_2_ODD) &&
                    producerAgentData.pleS.numStripes.height > 1)
                {
                    consumerAgentDependency.boundary = 1;
                }
                else
                {
                    consumerAgentDependency.boundary = 0;
                }
            }
            else
            {
                assert(false);
            }
            break;
        }

        default:
        {
            assert(false);
            break;
        }
    }

    // Calculate remaining agent dependencies
    if (consumerAgentDependency.relativeAgentId != 0)
    {
        ethosn::support_library::cascading_compiler::DependencyUtils::CalculateInnerRatio(consumerAgentDependency);

        ethosn::support_library::cascading_compiler::DependencyUtils::CalculateRemainingAgentDependencies(
            consumerAgentDependency);
    }
}

// Private function to fill the dependency data for Write After Read or Schedule Time dependencies
void CascadingCommandStreamGenerator::FillProducerAgentDependency(
    Dependency& producerAgentDependency,
    const command_stream::cascading::AgentType consumerAgentType,
    const AgentIdType consumerAgentId,
    const command_stream::cascading::AgentType producerAgentType,
    const AgentIdType producerAgentId,
    const Op* producerOp,
    DependencyType dependencyType) const
{
    const AgentDescAndDeps& consumerAgentAndDeps = m_CommandStreamAgents[consumerAgentId];
    const AgentDesc& consumerAgentData           = consumerAgentAndDeps.agent;
    const uint16_t consumerAgentNumStripes       = consumerAgentAndDeps.agent.numStripesTotal;
    const AgentDescAndDeps& producerAgentAndDeps = m_CommandStreamAgents[producerAgentId];
    const AgentDesc& producerAgentData           = producerAgentAndDeps.agent;
    const uint16_t producerAgentNumStripes       = producerAgentAndDeps.agent.numStripesTotal;

    // Add a new 'Write After Read' dependency or
    // Add a new 'Schedule Time' dependency
    switch (consumerAgentType)
    {
        case AgentType::IFM_STREAMER:
        {
            // Write After Read Dependency for [OfmStreamer][IfmStreamer] or
            // Schedule Time Dependency for [OfmStreamer][IfmStreamer]
            if (producerAgentType == AgentType::OFM_STREAMER)
            {
                // The last OFM stripe is needed by the first IFM stripe
                producerAgentDependency.outerRatio.other = consumerAgentNumStripes;
                producerAgentDependency.outerRatio.self  = producerAgentNumStripes;

                producerAgentDependency.innerRatio.other = 1;
                producerAgentDependency.innerRatio.self  = producerAgentNumStripes;

                producerAgentDependency.boundary = 0;
            }
            break;
        }

        case AgentType::WGT_STREAMER:
        {
            assert(false);
            break;
        }

        case AgentType::MCE_SCHEDULER:
        {
            // Write After Read Dependency for [IfmStreamer][MceScheduler] or
            // Schedule Time Dependency for [IfmStreamer][MceScheduler]
            if (producerAgentType == AgentType::IFM_STREAMER)
            {
                DependencyUtils::CalculateIfmSMceSOuterRatio(consumerAgentAndDeps.agent, producerAgentAndDeps.agent,
                                                             producerAgentDependency.outerRatio.other,
                                                             producerAgentDependency.outerRatio.self);

                // The MceS can process more data than is loaded by the IfmS (e.g. two stripes at a time)
                uint16_t widthRatio  = ethosn::utils::NumericCast<uint16_t>(utils::DivRoundUp(
                    consumerAgentData.mce.numStripes.ofmWidth, producerAgentData.ifm.fmData.numStripes.width));
                uint16_t heightRatio = ethosn::utils::NumericCast<uint16_t>(utils::DivRoundUp(
                    consumerAgentData.mce.numStripes.ofmHeight, producerAgentData.ifm.fmData.numStripes.height));

                if (consumerAgentData.mce.mceOpMode == MceOperation::DEPTHWISE_CONVOLUTION)
                {
                    assert(consumerAgentData.mce.numStripes.ifmChannels == 1);
                }
                else
                {
                    assert(producerAgentData.ifm.fmData.numStripes.channels ==
                           consumerAgentData.mce.numStripes.ifmChannels);
                }

                producerAgentDependency.innerRatio.other = 1;
                producerAgentDependency.innerRatio.self =
                    ethosn::utils::NumericCast<uint16_t>(widthRatio * heightRatio);

                producerAgentDependency.boundary = DependencyUtils::CalculateMceSBoundary(consumerAgentData.mce);
            }
            // Write After Read Dependency for [WeightStreamer][MceScheduler] or
            // Schedule Time Dependency for [WeightStreamer][MceScheduler]
            else if (producerAgentType == AgentType::WGT_STREAMER)
            {
                // MCE always traverses in IXYO order. Each MCE stripe needs a new weight stripe, unless a weight stripe
                // can be re-used which can only happen if we are not IFM splitting and we are moving in XY.

                // Outer ratio is not needed (set to max)
                producerAgentDependency.outerRatio.other = consumerAgentNumStripes;
                producerAgentDependency.outerRatio.self  = producerAgentNumStripes;

                if (consumerAgentData.mce.numStripes.ifmChannels == 1)
                {
                    // Weight stripes can be re-used as we move in XY
                    producerAgentDependency.innerRatio.other = ethosn::utils::NumericCast<uint16_t>(
                        consumerAgentData.mce.numStripes.ofmHeight * consumerAgentData.mce.numStripes.ofmWidth);
                    producerAgentDependency.innerRatio.self = 1;
                }
                else
                {
                    // No re-use, 1:1 dependency
                    producerAgentDependency.innerRatio.other = 1;
                    producerAgentDependency.innerRatio.other = 1;
                }

                producerAgentDependency.boundary = 0;
            }
            // Schedule Time Dependency for [PleLoader][MceScheduler]
            else if (producerAgentType == AgentType::PLE_LOADER)
            {
                producerAgentDependency.outerRatio.other = ethosn::utils::NumericCast<uint16_t>(
                    consumerAgentData.mce.numStripes.ofmHeight * consumerAgentData.mce.numStripes.ofmWidth *
                    consumerAgentData.mce.numStripes.ifmChannels);
                producerAgentDependency.outerRatio.self = 1;

                producerAgentDependency.innerRatio.other = ethosn::utils::NumericCast<uint16_t>(
                    consumerAgentData.mce.numStripes.ofmHeight * consumerAgentData.mce.numStripes.ofmWidth *
                    consumerAgentData.mce.numStripes.ifmChannels);
                producerAgentDependency.innerRatio.self = 1;

                producerAgentDependency.boundary = 0;
            }
            // Schedule Time Dependency for [PleScheduler][MceScheduler]
            else if (producerAgentType == AgentType::PLE_SCHEDULER)
            {
                if (dependencyType == DependencyType::Write && consumerAgentNumStripes == 1)
                {
                    // For the case where we have the PLE stripes split in height but being written into an output buffer
                    // which is the full tensor, we have only one stripe in the following MceS. We don't want a write dependency
                    // from the PleS onto this MceS, otherwise it will stall.
                    producerAgentDependency.relativeAgentId = 0;
                    break;
                }

                // Calculate outer ratios using number of stripes
                producerAgentDependency.outerRatio.other = ethosn::utils::NumericCast<uint16_t>(
                    consumerAgentData.mce.numStripes.ofmHeight * consumerAgentData.mce.numStripes.ofmWidth *
                    consumerAgentData.mce.numStripes.ofmChannels);
                producerAgentDependency.outerRatio.self = ethosn::utils::NumericCast<uint16_t>(
                    producerAgentData.pleS.numStripes.height * producerAgentData.pleS.numStripes.width *
                    producerAgentData.pleS.numStripes.channels);

                // Calculate inner ratios using ratio of stripe size
                uint16_t widthRatio   = ethosn::utils::NumericCast<uint16_t>(utils::DivRoundUp(
                    producerAgentData.pleS.numStripes.width, consumerAgentData.mce.numStripes.ofmWidth));
                uint16_t heightRatio  = ethosn::utils::NumericCast<uint16_t>(utils::DivRoundUp(
                    producerAgentData.pleS.numStripes.height, consumerAgentData.mce.numStripes.ofmHeight));
                uint16_t channelRatio = ethosn::utils::NumericCast<uint16_t>(utils::DivRoundUp(
                    producerAgentData.pleS.numStripes.channels, consumerAgentData.mce.numStripes.ofmChannels));

                producerAgentDependency.innerRatio.self =
                    ethosn::utils::NumericCast<uint16_t>(widthRatio * heightRatio * channelRatio);
                producerAgentDependency.innerRatio.other = 1;

                producerAgentDependency.boundary = DependencyUtils::CalculateMceSBoundary(consumerAgentData.mce);
            }
            else
            {
                assert(false);
            }

            break;
        }

        case AgentType::PLE_LOADER:
        {
            assert(false);
            break;
        }

        case AgentType::PLE_SCHEDULER:
        {
            // Write After Read Dependency for [IfmStreamer][PleScheduler] or
            // Schedule Time Dependency for [IfmStreamer][PleScheduler]
            if (producerAgentType == AgentType::IFM_STREAMER)
            {
                // Calculate outer ratios using number of stripes.
                producerAgentDependency.outerRatio.other = ethosn::utils::NumericCast<uint16_t>(
                    consumerAgentData.pleS.numStripes.height * consumerAgentData.pleS.numStripes.width *
                    consumerAgentData.pleS.numStripes.channels);
                producerAgentDependency.outerRatio.self = ethosn::utils::NumericCast<uint16_t>(
                    producerAgentData.ifm.fmData.numStripes.width * producerAgentData.ifm.fmData.numStripes.height *
                    producerAgentData.ifm.fmData.numStripes.channels);
            }
            // Schedule Time Dependency for [MceScheduler][PleScheduler]
            else if (producerAgentType == AgentType::MCE_SCHEDULER)
            {
                // Outer ratio not used (set to max)
                producerAgentDependency.outerRatio.other = consumerAgentNumStripes;
                producerAgentDependency.outerRatio.self  = producerAgentNumStripes;

                // Calculate inner ratios using ratio of stripe channels
                uint16_t channelRatio = ethosn::utils::NumericCast<uint16_t>(utils::DivRoundUp(
                    consumerAgentData.pleS.numStripes.channels, producerAgentData.mce.numStripes.ofmChannels));

                producerAgentDependency.innerRatio.other = 1;
                producerAgentDependency.innerRatio.self =
                    ethosn::utils::NumericCast<uint16_t>(channelRatio * producerAgentData.mce.numStripes.ifmChannels);

                // Set boundary to 1 if producer stripe count is not a factor of consumer stripe count
                uint16_t numberOfIfmStripesInXYDimProducer = ethosn::utils::NumericCast<uint16_t>(
                    producerAgentData.mce.numStripes.ofmWidth * producerAgentData.mce.numStripes.ofmHeight *
                    producerAgentData.mce.numStripes.ofmChannels);
                uint16_t numberOfIfmStripesInXYDimConsumer = ethosn::utils::NumericCast<uint16_t>(
                    consumerAgentData.pleS.numStripes.width * consumerAgentData.pleS.numStripes.height *
                    consumerAgentData.pleS.numStripes.channels);

                uint16_t ifmStripeRemainder = ethosn::utils::NumericCast<uint16_t>(numberOfIfmStripesInXYDimConsumer %
                                                                                   numberOfIfmStripesInXYDimProducer);

                if (ifmStripeRemainder == 0)
                {
                    producerAgentDependency.boundary = 0;
                }
                else
                {
                    producerAgentDependency.boundary = 1;
                }
            }
            // Schedule Time Dependency for [PleLoader][PleScheduler]
            else if (producerAgentType == AgentType::PLE_LOADER)
            {
                producerAgentDependency.outerRatio.other = ethosn::utils::NumericCast<uint16_t>(
                    consumerAgentData.pleS.numStripes.height * consumerAgentData.pleS.numStripes.width *
                    consumerAgentData.pleS.numStripes.channels);
                producerAgentDependency.outerRatio.self = 1U;
            }
            // Schedule Time Dependency for [PleScheduler][PleScheduler]
            else if (producerAgentType == AgentType::PLE_SCHEDULER)
            {
                // We only support strategy 3 (full tensor) cascading for Ple -> Ple
                producerAgentDependency.outerRatio.other = 1U;
                producerAgentDependency.outerRatio.self  = 1U;
            }
            else
            {
                assert(false);
            }
            break;
        }

        case AgentType::OFM_STREAMER:
        {
            // Write After Read Dependency for [IfmStreamer][OfmStreamer] or
            // Schedule Time Dependency for [IfmStreamer][OfmStreamer]
            if (producerAgentType == AgentType::IFM_STREAMER)
            {
                // Simple 1:1 dependency
                producerAgentDependency.outerRatio.other = 1;
                producerAgentDependency.outerRatio.self  = 1;

                producerAgentDependency.innerRatio.other = 1;
                producerAgentDependency.innerRatio.self  = 1;

                producerAgentDependency.boundary = 0;
            }
            // Write After Read Dependency for [PleScheduler][OfmStreamer] or
            // Schedule Time Dependency for [PleScheduler][OfmStreamer]
            else if (producerAgentType == AgentType::PLE_SCHEDULER)
            {
                // Normally this is a simple 1:1 dependency, but in some cases the PLE can have multiple stripes
                // for each OFM stripe (strategies where OfmS does the full height but PLE does partial height)
                producerAgentDependency.outerRatio.other = consumerAgentNumStripes;
                producerAgentDependency.outerRatio.self  = producerAgentNumStripes;

                producerAgentDependency.innerRatio.other =
                    ethosn::utils::NumericCast<uint16_t>(producerAgentData.pleS.defaultStripeSize.height /
                                                         consumerAgentData.ofm.fmData.defaultStripeSize.height);
                producerAgentDependency.innerRatio.self = 1;

                command_stream::PleOperation pleOperation = static_cast<const PleOp*>(producerOp)->m_Op;
                if (dependencyType == DependencyType::Schedule &&
                    (pleOperation == command_stream::PleOperation::MAXPOOL_3X3_2_2_EVEN ||
                     pleOperation == command_stream::PleOperation::MAXPOOL_3X3_2_2_ODD) &&
                    producerAgentData.pleS.numStripes.height > 1)
                {
                    producerAgentDependency.boundary = 1;
                }
                else
                {
                    producerAgentDependency.boundary = 0;
                }
            }
            else
            {
                assert(false);
            }
            break;
        }

        default:
        {
            assert(false);
            break;
        }
    }

    // Calculate remaining agent dependencies
    if (producerAgentDependency.relativeAgentId != 0)
    {
        ethosn::support_library::cascading_compiler::DependencyUtils::CalculateInnerRatio(producerAgentDependency);

        ethosn::support_library::cascading_compiler::DependencyUtils::CalculateRemainingAgentDependencies(
            producerAgentDependency);
    }
}

namespace
{

/// Returns the index of the Op (in execution order) of the earliest Op
/// which could write to the given buffer.
size_t WalkGraphUp(const OpGraph& graph, Buffer* b)
{
    size_t result = std::numeric_limits<size_t>::max();

    for (Op* producer : graph.GetProducers(b))
    {
        assert(producer != nullptr);
        size_t earliestOpIdxThisProducer = std::numeric_limits<size_t>::max();
        for (Buffer* input : graph.GetInputs(producer))
        {
            if (input->m_Location != Location::Dram)
            {
                earliestOpIdxThisProducer = std::min(earliestOpIdxThisProducer, WalkGraphUp(graph, input));
            }
        }

        if (earliestOpIdxThisProducer == std::numeric_limits<size_t>::max())
        {
            // This producer has all inputs in DRAM, so is the earliest along this branch
            earliestOpIdxThisProducer = utils::FindIndex(graph.GetOps(), producer).second;
        }

        result = std::min(result, earliestOpIdxThisProducer);
    }

    return result;
}

/// Returns the index of the Op (in execution order) of the latest Op
/// which could read from the given buffer.
size_t WalkGraphDown(const OpGraph& graph, Buffer* b)
{
    size_t result = 0;
    for (std::pair<Op*, uint32_t> consumer : graph.GetConsumers(b))
    {
        Buffer* output = graph.GetOutput(consumer.first);
        assert(output != nullptr);

        size_t latestOpIdxThisConsumer;
        if (output->m_Location == Location::Dram)
        {
            latestOpIdxThisConsumer = utils::FindIndex(graph.GetOps(), consumer.first).second;
        }
        else
        {
            latestOpIdxThisConsumer = WalkGraphDown(graph, output);
        }
        result = std::max(result, latestOpIdxThisConsumer);
    }

    return result;
}

}    // namespace

/// Private function to add the lifetime information of the intermediate DRAM buffers
/// Determines the start and end of the lifetime of the given (intermediate DRAM) buffer.
/// The approach is to walk the graph backwards from the buffer to find the previous time
/// there was a DRAM buffer, at which point we know the target buffer would not be needed,
/// and we do the same walking forwards, to know the point at which the target buffer
/// will be finished with. When there are branches, we go along each to find the
/// furthest away usage. This can be thought of as a "flood fill" to find the set of Ops
/// in the section before/after the target buffer, and then finding the min/max agent ID
/// of those Ops.
/// This is somewhat conservative because in a strategy 1 or 3 cascade, we could
/// shorten the lifetime, but we don't account for that here to keep it simple.
void CascadingCommandStreamGenerator::AddLifetimeInfoForIntermediateDramBuffers()
{
    for (Buffer* buffer : m_MergedOpGraph.GetBuffers())
    {
        if (buffer->m_Location == Location::Dram)
        {
            // cppcheck-suppress assertWithSideEffect
            assert(buffer->Dram()->m_BufferType.has_value());
            if (buffer->Dram()->m_BufferType.value() == BufferType::Intermediate)
            {
                AgentIdType lifetimeStart = WalkGraphUp(m_MergedOpGraph, buffer);
                AgentIdType lifetimeEnd   = WalkGraphDown(m_MergedOpGraph, buffer);
                m_BufferManager.MarkBufferUsedAtTime(m_DramBufToBufIdMapping.at(buffer->Dram()),
                                                     static_cast<uint32_t>(lifetimeStart),
                                                     static_cast<uint32_t>(lifetimeEnd + 1));
            }
        }
    }
}
}    // namespace cascading_compiler
}    // namespace support_library
}    // namespace ethosn
