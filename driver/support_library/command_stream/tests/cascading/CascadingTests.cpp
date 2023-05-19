//
// Copyright © 2021,2023 Arm Limited.
// SPDX-License-Identifier: Apache-2.0
//

#include "../../include/ethosn_command_stream/CommandStreamBuffer.hpp"

#include <catch.hpp>

using namespace ethosn::command_stream;

TEST_CASE("Cascading/CommandStream")
{
    CommandStreamBuffer csbuffer;

    AddCascade(csbuffer,
               {
                   cascading::Agent{ cascading::IfmS{} },
                   cascading::Agent{ cascading::WgtS{} },
                   cascading::Agent{ cascading::MceS{} },
                   cascading::Agent{ cascading::PleS{} },
                   cascading::Agent{ cascading::OfmS{} },
               },
               {}, {}, {}, {});

    CommandStream cstream(&*csbuffer.begin(), &*csbuffer.end());
    REQUIRE(cstream.IsValid());

    auto it = cstream.begin();
    {
        const auto command = it->GetCommand<Opcode::CASCADE>();

        REQUIRE(command != nullptr);

        const auto cascadeBegin = static_cast<const cascading::Agent*>(static_cast<const void*>(command + 1));

        std::span<const cascading::Agent> agents{ cascadeBegin, 5 };

        CHECK(agents[0].type == cascading::AgentType::IFM_STREAMER);
        CHECK(agents[1].type == cascading::AgentType::WGT_STREAMER);
        CHECK(agents[2].type == cascading::AgentType::MCE_SCHEDULER);
        CHECK(agents[3].type == cascading::AgentType::PLE_SCHEDULER);
        CHECK(agents[4].type == cascading::AgentType::OFM_STREAMER);
    }
}
