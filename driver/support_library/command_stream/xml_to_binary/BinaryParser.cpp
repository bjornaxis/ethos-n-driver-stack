//
// Copyright © 2018-2023 Arm Limited.
// SPDX-License-Identifier: Apache-2.0
//
#include "BinaryParser.hpp"
#include <ethosn_command_stream/CommandStream.hpp>
#include <ethosn_command_stream/cascading/CommandStream.hpp>

#include <cassert>
#include <iomanip>
#include <string>
#include <type_traits>
#include <vector>

using namespace ethosn::command_stream;

namespace
{

std::vector<uint8_t> ReadBinaryData(std::istream& input)
{
    return std::vector<uint8_t>(std::istreambuf_iterator<char>{ input }, {});
}

void Parse(std::stringstream& parent, const char* const value, const int& tabs, const bool& newline)
{
    for (int i = 0; i < tabs; ++i)
    {
        parent << "    ";
    }

    parent << value;

    if (newline)
    {
        parent << "\n";
    }
}

void Parse(std::stringstream& parent, const std::string& value, const int tabs, const bool newline)
{
    Parse(parent, value.c_str(), tabs, newline);
}

std::string IntegersToString(std::ostringstream& oss)
{
    return oss.str();
}

template <typename I, typename... Is>
std::string IntegersToString(std::ostringstream& oss, const I value, const Is... more)
{
    using PrintType = std::conditional_t<std::is_unsigned<I>::value, unsigned int, int>;

    oss << static_cast<PrintType>(value);

    if (sizeof...(more) > 0)
    {
        oss << " ";
    }

    return IntegersToString(oss, more...);
}

template <int Base = 10, typename... Is>
std::string IntegersToString(const Is... ints)
{
    std::ostringstream oss;
    oss << std::setbase(Base);
    return IntegersToString(oss, ints...);
}

template <typename IntType>
std::enable_if_t<std::is_integral<IntType>::value> Parse(std::stringstream& parent, const IntType value)
{
    Parse(parent, IntegersToString(value));
}

void ParseAsHex(std::stringstream& parent, const uint32_t value)
{
    Parse(parent, "0x" + IntegersToString<16>(value), 0, false);
}

void ParseAsNum(std::stringstream& parent, const int32_t value)
{
    Parse(parent, IntegersToString<10>(value), 0, false);
}

void Parse(std::stringstream& parent, const Filename& value)
{
    char output[128];
    for (int i = 0; i < 128; ++i)
    {
        output[i] = value[i];
    }
    Parse(parent, output, 0, false);
}

void Parse(std::stringstream& parent, const DumpDram& value)
{
    Parse(parent, "<DUMP_DRAM>", 1, true);

    Parse(parent, "<DRAM_BUFFER_ID>", 2, false);
    ParseAsNum(parent, value.m_DramBufferId());
    Parse(parent, "</DRAM_BUFFER_ID>", 0, true);

    Parse(parent, "<FILENAME>", 2, false);
    Parse(parent, value.m_Filename());
    Parse(parent, "</FILENAME>", 0, true);

    Parse(parent, "</DUMP_DRAM>", 1, true);
}

void Parse(std::stringstream& parent, const DumpSram& value)
{
    Parse(parent, "<DUMP_SRAM>", 1, true);

    Parse(parent, "<PREFIX>", 2, false);
    Parse(parent, value.m_Filename());
    Parse(parent, "</PREFIX>", 0, true);

    Parse(parent, "</DUMP_SRAM>", 1, true);
}

void Parse(std::stringstream& parent, const cascading::IfmS& ifms)
{
    Parse(parent, "<IFM_STREAMER>", 3, true);

    Parse(parent, "<BUFFER_ID>", 4, false);
    ParseAsNum(parent, ifms.bufferId);
    Parse(parent, "</BUFFER_ID>", 0, true);

    Parse(parent, "<DMA_COMP_CONFIG0>", 4, false);
    ParseAsHex(parent, ifms.DMA_COMP_CONFIG0);
    Parse(parent, "</DMA_COMP_CONFIG0>", 0, true);

    Parse(parent, "<DMA_STRIDE1>", 4, false);
    ParseAsHex(parent, ifms.DMA_STRIDE1);
    Parse(parent, "</DMA_STRIDE1>", 0, true);

    Parse(parent, "<DMA_STRIDE2>", 4, false);
    ParseAsHex(parent, ifms.DMA_STRIDE2);
    Parse(parent, "</DMA_STRIDE2>", 0, true);

    Parse(parent, "</IFM_STREAMER>", 3, true);
}

void Parse(std::stringstream& parent, const cascading::OfmS& ofms)
{
    Parse(parent, "<OFM_STREAMER>", 3, true);

    Parse(parent, "<BUFFER_ID>", 4, false);
    ParseAsNum(parent, ofms.bufferId);
    Parse(parent, "</BUFFER_ID>", 0, true);

    Parse(parent, "<DMA_COMP_CONFIG0>", 4, false);
    ParseAsHex(parent, ofms.DMA_COMP_CONFIG0);
    Parse(parent, "</DMA_COMP_CONFIG0>", 0, true);

    Parse(parent, "<DMA_STRIDE1>", 4, false);
    ParseAsHex(parent, ofms.DMA_STRIDE1);
    Parse(parent, "</DMA_STRIDE1>", 0, true);

    Parse(parent, "<DMA_STRIDE2>", 4, false);
    ParseAsHex(parent, ofms.DMA_STRIDE2);
    Parse(parent, "</DMA_STRIDE2>", 0, true);

    Parse(parent, "</OFM_STREAMER>", 3, true);
}

void Parse(std::stringstream& parent, const cascading::WgtS& wgts)
{
    Parse(parent, "<WGT_STREAMER>", 3, true);

    Parse(parent, "<BUFFER_ID>", 4, false);
    ParseAsNum(parent, wgts.bufferId);
    Parse(parent, "</BUFFER_ID>", 0, true);

    Parse(parent, "</WGT_STREAMER>", 3, true);
}

void Parse(std::stringstream& parent, const cascading::MceOperation value)
{
    switch (value)
    {
        case cascading::MceOperation::CONVOLUTION:
        {
            Parse(parent, "CONVOLUTION", 0, false);
            break;
        }
        case cascading::MceOperation::DEPTHWISE_CONVOLUTION:
        {
            Parse(parent, "DEPTHWISE_CONVOLUTION", 0, false);
            break;
        }
        case cascading::MceOperation::FULLY_CONNECTED:
        {
            Parse(parent, "FULLY_CONNECTED", 0, false);
            break;
        }
        default:
        {
            // Bad binary
            throw ParseException("Invalid MceOperation in binary input: " +
                                 std::to_string(static_cast<uint32_t>(value)));
        }
    }
}

void Parse(std::stringstream& parent, const cascading::MceS& mces)
{
    Parse(parent, "<MCE_SCHEDULER>", 3, true);

    Parse(parent, "<MCE_OP_MODE>", 4, false);
    Parse(parent, mces.mceOpMode);
    Parse(parent, "</MCE_OP_MODE>", 0, true);

    Parse(parent, "<PLE_KERNEL_ID>", 4, false);
    Parse(parent, cascading::PleKernelId2String(mces.pleKernelId), 0, false);
    Parse(parent, "</PLE_KERNEL_ID>", 0, true);

    Parse(parent, "<ACTIVATION_CONFIG>", 4, false);
    ParseAsHex(parent, mces.ACTIVATION_CONFIG);
    Parse(parent, "</ACTIVATION_CONFIG>", 0, true);

    Parse(parent, "<WIDE_KERNEL_CONTROL>", 4, false);
    ParseAsHex(parent, mces.WIDE_KERNEL_CONTROL);
    Parse(parent, "</WIDE_KERNEL_CONTROL>", 0, true);

    Parse(parent, "<FILTER>", 4, false);
    ParseAsHex(parent, mces.FILTER);
    Parse(parent, "</FILTER>", 0, true);

    Parse(parent, "<IFM_ZERO_POINT>", 4, false);
    ParseAsHex(parent, mces.IFM_ZERO_POINT);
    Parse(parent, "</IFM_ZERO_POINT>", 0, true);

    Parse(parent, "<IFM_DEFAULT_SLOT_SIZE>", 4, false);
    ParseAsHex(parent, mces.IFM_DEFAULT_SLOT_SIZE);
    Parse(parent, "</IFM_DEFAULT_SLOT_SIZE>", 0, true);

    Parse(parent, "<IFM_SLOT_STRIDE>", 4, false);
    ParseAsHex(parent, mces.IFM_SLOT_STRIDE);
    Parse(parent, "</IFM_SLOT_STRIDE>", 0, true);

    Parse(parent, "<STRIPE_BLOCK_CONFIG>", 4, false);
    ParseAsHex(parent, mces.STRIPE_BLOCK_CONFIG);
    Parse(parent, "</STRIPE_BLOCK_CONFIG>", 0, true);

    Parse(parent, "<DEPTHWISE_CONTROL>", 4, false);
    ParseAsHex(parent, mces.DEPTHWISE_CONTROL);
    Parse(parent, "</DEPTHWISE_CONTROL>", 0, true);

    Parse(parent, "<IFM_SLOT_BASE_ADDRESS>", 4, false);
    ParseAsHex(parent, mces.IFM_SLOT_BASE_ADDRESS);
    Parse(parent, "</IFM_SLOT_BASE_ADDRESS>", 0, true);

    Parse(parent, "<PLE_MCEIF_CONFIG>", 4, false);
    ParseAsHex(parent, mces.PLE_MCEIF_CONFIG);
    Parse(parent, "</PLE_MCEIF_CONFIG>", 0, true);

    Parse(parent, "</MCE_SCHEDULER>", 3, true);
}

void Parse(std::stringstream& parent, const cascading::PleL& plel)
{
    Parse(parent, "<PLE_LOADER>", 3, true);

    Parse(parent, "<PLE_KERNEL_ID>", 4, false);
    Parse(parent, cascading::PleKernelId2String(plel.pleKernelId), 0, false);
    Parse(parent, "</PLE_KERNEL_ID>", 0, true);

    Parse(parent, "</PLE_LOADER>", 3, true);
}

void Parse(std::stringstream& parent, const cascading::PleInputMode value)
{
    switch (value)
    {
        case cascading::PleInputMode::MCE_ALL_OGS:
        {
            Parse(parent, "MCE_ALL_OGS", 0, false);
            break;
        }
        case cascading::PleInputMode::MCE_ONE_OG:
        {
            Parse(parent, "MCE_ONE_OG", 0, false);
            break;
        }
        case cascading::PleInputMode::SRAM_ONE_INPUT:
        {
            Parse(parent, "SRAM_ONE_INPUT", 0, false);
            break;
        }
        case cascading::PleInputMode::SRAM_TWO_INPUTS:
        {
            Parse(parent, "SRAM_TWO_INPUTS", 0, false);
            break;
        }
        default:
        {
            // Bad binary
            throw ParseException("Invalid PleInputMode in binary input: " +
                                 std::to_string(static_cast<uint32_t>(value)));
        }
    }
}

void Parse(std::stringstream& parent, const cascading::PleS& ples)
{
    Parse(parent, "<PLE_SCHEDULER>", 3, true);

    Parse(parent, "<INPUT_MODE>", 4, false);
    Parse(parent, ples.inputMode);
    Parse(parent, "</INPUT_MODE>", 0, true);

    Parse(parent, "<PLE_KERNEL_ID>", 4, false);
    Parse(parent, cascading::PleKernelId2String(ples.pleKernelId), 0, false);
    Parse(parent, "</PLE_KERNEL_ID>", 0, true);

    Parse(parent, "<PLE_KERNEL_SRAM_ADDR>", 4, false);
    ParseAsNum(parent, ples.pleKernelSramAddr);
    Parse(parent, "</PLE_KERNEL_SRAM_ADDR>", 0, true);

    Parse(parent, "</PLE_SCHEDULER>", 3, true);
}

void Parse(std::stringstream& parent, const cascading::Agent& data)
{
    switch (data.type)
    {
        case cascading::AgentType::IFM_STREAMER:
            Parse(parent, data.ifm);
            break;
        case cascading::AgentType::WGT_STREAMER:
            Parse(parent, data.wgt);
            break;
        case cascading::AgentType::MCE_SCHEDULER:
            Parse(parent, data.mce);
            break;
        case cascading::AgentType::PLE_LOADER:
            Parse(parent, data.pleL);
            break;
        case cascading::AgentType::PLE_SCHEDULER:
            Parse(parent, data.pleS);
            break;
        case cascading::AgentType::OFM_STREAMER:
            Parse(parent, data.ofm);
            break;
        default:
        {
            // Bad binary
            throw ParseException("Invalid cascading agent type: " + std::to_string(static_cast<uint32_t>(data.type)));
        }
    };
}

const char* CounterNameToString(cascading::CounterName c)
{
    switch (c)
    {
        case cascading::CounterName::DmaRd:
            return "DmaRd";
        case cascading::CounterName::DmaWr:
            return "DmaWr";
        case cascading::CounterName::Mceif:
            return "Mceif";
        case cascading::CounterName::MceStripe:
            return "MceStripe";
        case cascading::CounterName::PleCodeLoadedIntoPleSram:
            return "PleCodeLoadedIntoPleSram";
        case cascading::CounterName::PleStripe:
            return "PleStripe";
        default:
            throw ParseException("Invalid counter name: " + std::to_string(static_cast<uint32_t>(c)));
    }
}

void Parse(std::stringstream& parent, const cascading::WaitForCounterCommand& waitCommand)
{
    using namespace ethosn::command_stream::cascading;

    Parse(parent, "<WAIT_FOR_COUNTER_COMMAND>", 3, true);

    Parse(parent, "<COUNTER_NAME>", 4, false);
    Parse(parent, CounterNameToString(waitCommand.counterName), 0, false);
    Parse(parent, "</COUNTER_NAME>", 0, true);

    Parse(parent, "<COUNTER_VALUE>", 4, false);
    ParseAsNum(parent, waitCommand.counterValue);
    Parse(parent, "</COUNTER_VALUE>", 0, true);

    Parse(parent, "</WAIT_FOR_COUNTER_COMMAND>", 3, true);
}

const char* CommandTypeToString(cascading::CommandType t)
{
    using namespace ethosn::command_stream::cascading;
    switch (t)
    {
        case CommandType::WaitForCounter:
            return "WaitForCounter";
        case CommandType::LoadIfmStripe:
            return "LoadIfmStripe";
        case CommandType::LoadWgtStripe:
            return "LoadWgtStripe";
        case CommandType::ProgramMceStripe:
            return "ProgramMceStripe";
        case CommandType::ConfigMceif:
            return "ConfigMceif";
        case CommandType::StartMceStripe:
            return "StartMceStripe";
        case CommandType::LoadPleCodeIntoSram:
            return "LoadPleCodeIntoSram";
        case CommandType::LoadPleCodeIntoPleSram:
            return "LoadPleCodeIntoPleSram";
        case CommandType::StartPleStripe:
            return "StartPleStripe";
        case CommandType::StoreOfmStripe:
            return "StoreOfmStripe";
        default:
            throw std::runtime_error("Invalid cascading command type: " + std::to_string(static_cast<uint32_t>(t)));
    }
}

void Parse(std::stringstream& parent, const cascading::DmaCommand& dmaCommand)
{
    using namespace ethosn::command_stream::cascading;

    // Add helpful comment to indicate the command type (DmaCommands are used as the storage for several different kinds of command)
    Parse(parent, ("<!-- Command type is " + std::string(CommandTypeToString(dmaCommand.type)) + " -->").c_str(), 3,
          true);
    Parse(parent, "<DMA_COMMAND>", 3, true);

    Parse(parent, "<AGENT_ID>", 4, false);
    ParseAsNum(parent, dmaCommand.agentId);
    Parse(parent, "</AGENT_ID>", 0, true);

    Parse(parent, "<DRAM_OFFSET>", 4, false);
    ParseAsHex(parent, dmaCommand.m_DramOffset);
    Parse(parent, "</DRAM_OFFSET>", 0, true);

    Parse(parent, "<SRAM_ADDR>", 4, false);
    ParseAsHex(parent, dmaCommand.SRAM_ADDR);
    Parse(parent, "</SRAM_ADDR>", 0, true);

    Parse(parent, "<DMA_SRAM_STRIDE>", 4, false);
    ParseAsHex(parent, dmaCommand.DMA_SRAM_STRIDE);
    Parse(parent, "</DMA_SRAM_STRIDE>", 0, true);

    Parse(parent, "<DMA_STRIDE0>", 4, false);
    ParseAsHex(parent, dmaCommand.DMA_STRIDE0);
    Parse(parent, "</DMA_STRIDE0>", 0, true);

    Parse(parent, "<DMA_STRIDE3>", 4, false);
    ParseAsHex(parent, dmaCommand.DMA_STRIDE3);
    Parse(parent, "</DMA_STRIDE3>", 0, true);

    Parse(parent, "<DMA_CHANNELS>", 4, false);
    ParseAsHex(parent, dmaCommand.DMA_CHANNELS);
    Parse(parent, "</DMA_CHANNELS>", 0, true);

    Parse(parent, "<DMA_EMCS>", 4, false);
    ParseAsHex(parent, dmaCommand.DMA_EMCS);
    Parse(parent, "</DMA_EMCS>", 0, true);

    Parse(parent, "<DMA_TOTAL_BYTES>", 4, false);
    ParseAsHex(parent, dmaCommand.DMA_TOTAL_BYTES);
    Parse(parent, "</DMA_TOTAL_BYTES>", 0, true);

    Parse(parent, "<DMA_CMD>", 4, false);
    ParseAsHex(parent, dmaCommand.DMA_CMD);
    Parse(parent, "</DMA_CMD>", 0, true);

    Parse(parent, "</DMA_COMMAND>", 3, true);
}

void Parse(std::stringstream& parent, const cascading::ProgramMceStripeCommand& programMceCommand)
{
    using namespace ethosn::command_stream::cascading;

    Parse(parent, "<PROGRAM_MCE_STRIPE_COMMAND>", 3, true);

    Parse(parent, "<AGENT_ID>", 4, false);
    ParseAsNum(parent, programMceCommand.agentId);
    Parse(parent, "</AGENT_ID>", 0, true);

    for (size_t ce = 0; ce < programMceCommand.MUL_ENABLE.size(); ++ce)
    {
        std::string beginElementName = std::string("<MUL_ENABLE_CE") + std::to_string(ce) + ">";
        Parse(parent, beginElementName.c_str(), 4, true);

        for (size_t og = 0; og < programMceCommand.MUL_ENABLE[ce].size(); ++og)
        {
            std::string beginElementName = std::string("<OG") + std::to_string(og) + ">";
            Parse(parent, beginElementName.c_str(), 5, false);

            ParseAsHex(parent, programMceCommand.MUL_ENABLE[ce][og]);

            std::string endElementName = std::string("</OG") + std::to_string(og) + ">";
            Parse(parent, endElementName.c_str(), 0, true);
        }

        std::string endElementName = std::string("</MUL_ENABLE_CE") + std::to_string(ce) + ">";
        Parse(parent, endElementName.c_str(), 4, true);
    }

    Parse(parent, "<IFM_ROW_STRIDE>", 4, false);
    ParseAsHex(parent, programMceCommand.IFM_ROW_STRIDE);
    Parse(parent, "</IFM_ROW_STRIDE>", 0, true);

    Parse(parent, "<IFM_CONFIG1>", 4, false);
    ParseAsHex(parent, programMceCommand.IFM_CONFIG1);
    Parse(parent, "</IFM_CONFIG1>", 0, true);

    for (size_t num = 0; num < programMceCommand.IFM_PAD.size(); ++num)
    {
        std::string beginElementName = std::string("<IFM_PAD_NUM") + std::to_string(num) + ">";
        Parse(parent, beginElementName.c_str(), 4, true);

        for (size_t ig = 0; ig < programMceCommand.IFM_PAD[num].size(); ++ig)
        {
            std::string beginElementName = std::string("<IG") + std::to_string(ig) + ">";
            Parse(parent, beginElementName.c_str(), 5, false);

            ParseAsHex(parent, programMceCommand.IFM_PAD[num][ig]);

            std::string endElementName = std::string("</IG") + std::to_string(ig) + ">";
            Parse(parent, endElementName.c_str(), 0, true);
        }

        std::string endElementName = std::string("</IFM_PAD_NUM") + std::to_string(num) + ">";
        Parse(parent, endElementName.c_str(), 4, true);
    }

    Parse(parent, "<WIDE_KERNEL_OFFSET>", 4, false);
    ParseAsHex(parent, programMceCommand.WIDE_KERNEL_OFFSET);
    Parse(parent, "</WIDE_KERNEL_OFFSET>", 0, true);

    Parse(parent, "<IFM_TOP_SLOTS>", 4, false);
    ParseAsHex(parent, programMceCommand.IFM_TOP_SLOTS);
    Parse(parent, "</IFM_TOP_SLOTS>", 0, true);

    Parse(parent, "<IFM_MID_SLOTS>", 4, false);
    ParseAsHex(parent, programMceCommand.IFM_MID_SLOTS);
    Parse(parent, "</IFM_MID_SLOTS>", 0, true);

    Parse(parent, "<IFM_BOTTOM_SLOTS>", 4, false);
    ParseAsHex(parent, programMceCommand.IFM_BOTTOM_SLOTS);
    Parse(parent, "</IFM_BOTTOM_SLOTS>", 0, true);

    Parse(parent, "<IFM_SLOT_PAD_CONFIG>", 4, false);
    ParseAsHex(parent, programMceCommand.IFM_SLOT_PAD_CONFIG);
    Parse(parent, "</IFM_SLOT_PAD_CONFIG>", 0, true);

    Parse(parent, "<OFM_STRIPE_SIZE>", 4, false);
    ParseAsHex(parent, programMceCommand.OFM_STRIPE_SIZE);
    Parse(parent, "</OFM_STRIPE_SIZE>", 0, true);

    Parse(parent, "<OFM_CONFIG>", 4, false);
    ParseAsHex(parent, programMceCommand.OFM_CONFIG);
    Parse(parent, "</OFM_CONFIG>", 0, true);

    for (size_t og = 0; og < programMceCommand.WEIGHT_BASE_ADDR.size(); ++og)
    {
        std::string beginElementName = std::string("<WEIGHT_BASE_ADDR_OG") + std::to_string(og) + ">";
        Parse(parent, beginElementName.c_str(), 4, false);
        ParseAsHex(parent, programMceCommand.WEIGHT_BASE_ADDR[og]);
        std::string endElementName = std::string("</WEIGHT_BASE_ADDR_OG") + std::to_string(og) + ">";
        Parse(parent, endElementName.c_str(), 0, true);
    }

    for (size_t ce = 0; ce < programMceCommand.IFM_CONFIG2.size(); ++ce)
    {
        std::string beginElementName = std::string("<IFM_CONFIG2_CE") + std::to_string(ce) + ">";
        Parse(parent, beginElementName.c_str(), 4, true);

        for (size_t ig = 0; ig < programMceCommand.IFM_CONFIG2[ce].size(); ++ig)
        {
            std::string beginElementName = std::string("<IG") + std::to_string(ig) + ">";
            Parse(parent, beginElementName.c_str(), 5, false);

            ParseAsHex(parent, programMceCommand.IFM_CONFIG2[ce][ig]);

            std::string endElementName = std::string("</IG") + std::to_string(ig) + ">";
            Parse(parent, endElementName.c_str(), 0, true);
        }

        std::string endElementName = std::string("</IFM_CONFIG2_CE") + std::to_string(ce) + ">";
        Parse(parent, endElementName.c_str(), 4, true);
    }

    Parse(parent, "<NUM_BLOCKS_PROGRAMMED_FOR_MCE>", 4, false);
    ParseAsHex(parent, programMceCommand.m_NumBlocksProgrammedForMce);
    Parse(parent, "</NUM_BLOCKS_PROGRAMMED_FOR_MCE>", 0, true);

    Parse(parent, "</PROGRAM_MCE_STRIPE_COMMAND>", 3, true);
}

void Parse(std::stringstream& parent, const cascading::ConfigMceifCommand& configMceifCommand)
{
    using namespace ethosn::command_stream::cascading;

    Parse(parent, "<CONFIG_MCEIF_COMMAND>", 3, true);

    Parse(parent, "<AGENT_ID>", 4, false);
    ParseAsNum(parent, configMceifCommand.agentId);
    Parse(parent, "</AGENT_ID>", 0, true);

    Parse(parent, "</CONFIG_MCEIF_COMMAND>", 3, true);
}

void Parse(std::stringstream& parent, const cascading::StartMceStripeCommand& startMceStripeCommand)
{
    using namespace ethosn::command_stream::cascading;

    Parse(parent, "<START_MCE_STRIPE_COMMAND>", 3, true);

    Parse(parent, "<AGENT_ID>", 4, false);
    ParseAsNum(parent, startMceStripeCommand.agentId);
    Parse(parent, "</AGENT_ID>", 0, true);

    Parse(parent, "<CE_ENABLES>", 4, false);
    ParseAsNum(parent, startMceStripeCommand.CE_ENABLES);
    Parse(parent, "</CE_ENABLES>", 0, true);

    Parse(parent, "</START_MCE_STRIPE_COMMAND>", 3, true);
}

void Parse(std::stringstream& parent, const cascading::LoadPleCodeIntoPleSramCommand& c)
{
    using namespace ethosn::command_stream::cascading;

    Parse(parent, "<LOAD_PLE_CODE_INTO_PLE_SRAM_COMMAND>", 3, true);

    Parse(parent, "<AGENT_ID>", 4, false);
    ParseAsNum(parent, c.agentId);
    Parse(parent, "</AGENT_ID>", 0, true);

    Parse(parent, "</LOAD_PLE_CODE_INTO_PLE_SRAM_COMMAND>", 3, true);
}

void Parse(std::stringstream& parent, const cascading::StartPleStripeCommand& startPleStripeCommand)
{
    using namespace ethosn::command_stream::cascading;

    Parse(parent, "<START_PLE_STRIPE_COMMAND>", 3, true);

    Parse(parent, "<AGENT_ID>", 4, false);
    ParseAsNum(parent, startPleStripeCommand.agentId);
    Parse(parent, "</AGENT_ID>", 0, true);

    for (size_t i = 0; i < startPleStripeCommand.SCRATCH.size(); ++i)
    {
        std::string beginElementName = std::string("<SCRATCH") + std::to_string(i) + ">";
        Parse(parent, beginElementName.c_str(), 4, false);
        ParseAsHex(parent, startPleStripeCommand.SCRATCH[i]);
        std::string endElementName = std::string("</SCRATCH") + std::to_string(i) + ">";
        Parse(parent, endElementName.c_str(), 0, true);
    }

    Parse(parent, "</START_PLE_STRIPE_COMMAND>", 3, true);
}

void Parse(std::stringstream& parent, const cascading::Command& cmd)
{
    using namespace ethosn::command_stream::cascading;

    switch (cmd.type)
    {
        case CommandType::WaitForCounter:
            Parse(parent, static_cast<const WaitForCounterCommand&>(cmd));
            break;
        case CommandType::LoadIfmStripe:
            Parse(parent, static_cast<const DmaCommand&>(cmd));
            break;
        case CommandType::LoadWgtStripe:
            Parse(parent, static_cast<const DmaCommand&>(cmd));
            break;
        case CommandType::ProgramMceStripe:
            Parse(parent, static_cast<const ProgramMceStripeCommand&>(cmd));
            break;
        case CommandType::ConfigMceif:
            Parse(parent, static_cast<const ConfigMceifCommand&>(cmd));
            break;
        case CommandType::StartMceStripe:
            Parse(parent, static_cast<const StartMceStripeCommand&>(cmd));
            break;
        case CommandType::LoadPleCodeIntoSram:
            Parse(parent, static_cast<const DmaCommand&>(cmd));
            break;
        case CommandType::LoadPleCodeIntoPleSram:
            Parse(parent, static_cast<const LoadPleCodeIntoPleSramCommand&>(cmd));
            break;
        case CommandType::StartPleStripe:
            Parse(parent, static_cast<const StartPleStripeCommand&>(cmd));
            break;
        case CommandType::StoreOfmStripe:
            Parse(parent, static_cast<const DmaCommand&>(cmd));
            break;
        default:
            throw ParseException("Invalid cascading command type: " + std::to_string(static_cast<uint32_t>(cmd.type)));
    }
}

void Parse(std::stringstream& parent, const Cascade& value)
{
    using namespace cascading;
    using Command = cascading::Command;

    Parse(parent, "<CASCADE>", 1, true);

    // Calculate pointers to the agent array and command lists
    const Agent* agentsArray          = value.GetAgentsArray();
    const Command* dmaRdCommandsBegin = value.GetDmaRdCommandsBegin();
    const Command* dmaWrCommandsBegin = value.GetDmaWrCommandsBegin();
    const Command* mceCommandsBegin   = value.GetMceCommandsBegin();
    const Command* pleCommandsBegin   = value.GetPleCommandsBegin();

    // Moves command pointer to next Command (each Command has different length)
    auto getNextCommand = [](const Command* c) {
        return reinterpret_cast<const Command*>(reinterpret_cast<const char*>(c) + c->GetSize());
    };

    Parse(parent, "<AGENTS>", 2, true);
    for (uint32_t agentId = 0; agentId < value.NumAgents; ++agentId)
    {
        // Add helpful comment to indicate the agent ID (very useful for long command streams)
        Parse(parent, "<!-- Agent " + std::to_string(agentId) + " -->", 3, true);
        Parse(parent, agentsArray[agentId]);
    }
    Parse(parent, "</AGENTS>", 2, true);

    Parse(parent, "<DMA_RD_COMMANDS>", 2, true);
    for (uint32_t commandIdx = 0; commandIdx < value.NumDmaRdCommands; ++commandIdx)
    {
        // Add helpful comment to indicate the command idx (very useful for long command streams)
        Parse(parent, "<!-- DmaRd Command " + std::to_string(commandIdx) + " -->", 3, true);
        Parse(parent, *dmaRdCommandsBegin);
        dmaRdCommandsBegin = getNextCommand(dmaRdCommandsBegin);
    }
    Parse(parent, "</DMA_RD_COMMANDS>", 2, true);

    Parse(parent, "<DMA_WR_COMMANDS>", 2, true);
    for (uint32_t commandIdx = 0; commandIdx < value.NumDmaWrCommands; ++commandIdx)
    {
        // Add helpful comment to indicate the command idx (very useful for long command streams)
        Parse(parent, "<!-- DmaWr Command " + std::to_string(commandIdx) + " -->", 3, true);
        Parse(parent, *dmaWrCommandsBegin);
        dmaWrCommandsBegin = getNextCommand(dmaWrCommandsBegin);
    }
    Parse(parent, "</DMA_WR_COMMANDS>", 2, true);

    Parse(parent, "<MCE_COMMANDS>", 2, true);
    for (uint32_t commandIdx = 0; commandIdx < value.NumMceCommands; ++commandIdx)
    {
        // Add helpful comment to indicate the command idx (very useful for long command streams)
        Parse(parent, "<!-- Mce Command " + std::to_string(commandIdx) + " -->", 3, true);
        Parse(parent, *mceCommandsBegin);
        mceCommandsBegin = getNextCommand(mceCommandsBegin);
    }
    Parse(parent, "</MCE_COMMANDS>", 2, true);

    Parse(parent, "<PLE_COMMANDS>", 2, true);
    for (uint32_t commandIdx = 0; commandIdx < value.NumPleCommands; ++commandIdx)
    {
        // Add helpful comment to indicate the command idx (very useful for long command streams)
        Parse(parent, "<!-- Ple Command " + std::to_string(commandIdx) + " -->", 3, true);
        Parse(parent, *pleCommandsBegin);
        pleCommandsBegin = getNextCommand(pleCommandsBegin);
    }
    Parse(parent, "</PLE_COMMANDS>", 2, true);

    Parse(parent, "</CASCADE>", 1, true);
}
}    // namespace

void ParseBinary(CommandStream& cstream, std::stringstream& output)
{
    output << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n";
    output << "<STREAM VERSION_MAJOR="
           << "\"" << std::to_string(cstream.GetVersionMajor()).c_str() << "\"";
    output << " VERSION_MINOR="
           << "\"" << std::to_string(cstream.GetVersionMinor()).c_str() << "\"";
    output << " VERSION_PATCH="
           << "\"" << std::to_string(cstream.GetVersionPatch()).c_str() << "\">\n";

    uint32_t commandCounter = 0;
    for (const CommandHeader& header : cstream)
    {
        Opcode command = header.m_Opcode();
        output << ("    <!-- Command " + std::to_string(commandCounter) + " -->\n").c_str();
        switch (command)
        {
            case Opcode::DUMP_DRAM:
            {
                Parse(output, header.GetCommand<Opcode::DUMP_DRAM>()->m_Data());
                break;
            }
            case Opcode::DUMP_SRAM:
            {
                Parse(output, header.GetCommand<Opcode::DUMP_SRAM>()->m_Data());
                break;
            }
            case Opcode::CASCADE:
            {
                Parse(output, header.GetCommand<Opcode::CASCADE>()->m_Data());
                break;
            }
            default:
            {
                // Bad binary
                throw ParseException("Invalid Opcode in binary input: " +
                                     std::to_string(static_cast<uint32_t>(command)));
            }
        }
        ++commandCounter;
    }
    output << "</STREAM>\n";
}

BinaryParser::BinaryParser(std::istream& input)
{
    std::vector<uint8_t> data = ReadBinaryData(input);

    CommandStream cstream(data.data(), data.data() + data.size());
    ParseBinary(cstream, out);
}

BinaryParser::BinaryParser(const std::vector<uint32_t>& data)
{
    CommandStream cstream(data.data(), data.data() + data.size());
    ParseBinary(cstream, out);
}

void BinaryParser::WriteXml(std::ostream& output)
{
    std::string temp = out.str();
    output.write(temp.c_str(), temp.size());
}
