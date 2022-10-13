//
// Copyright © 2022 Arm Limited.
// SPDX-License-Identifier: Apache-2.0
//

#include "../include/ethosn_driver_library/ProcMemAllocator.hpp"

#include "NetworkImpl.hpp"
#if defined(TARGET_KMOD)
#include "KmodBuffer.hpp"
#include "KmodNetwork.hpp"
#elif defined(TARGET_MODEL)
#include "ModelBuffer.hpp"
#include "ModelNetwork.hpp"
#endif

#include <ethosn_utils/Macros.hpp>
#include <uapi/ethosn.h>

#include <errno.h>
#include <fcntl.h>
#include <stdexcept>
#include <sys/ioctl.h>
#if defined(__unix__)
#include <unistd.h>
#endif

namespace ethosn
{
namespace driver_library
{

ProcMemAllocator::ProcMemAllocator(const std::string& device)
{
#ifdef TARGET_KMOD
    int ethosnFd = open(device.c_str(), O_RDONLY);
    if (ethosnFd < 0)
    {
        throw std::runtime_error(std::string("Unable to open " + device + ": ") + strerror(errno));
    }

    // Check compatibility between driver library and the kernel
    try
    {
        if (!VerifyKernel(device))
        {
            throw std::runtime_error(std::string("Wrong kernel module version\n"));
        }
    }
    catch (const std::runtime_error& error)
    {
        close(ethosnFd);
        throw;
    }

    m_AllocatorFd = ioctl(ethosnFd, ETHOSN_IOCTL_CREATE_PROC_MEM_ALLOCATOR);
    int err       = errno;
    close(ethosnFd);
    if (m_AllocatorFd < 0)
    {
        throw std::runtime_error(std::string("Failed to create process memory allocator: ") + strerror(err));
    }
#else
    ETHOSN_UNUSED(device);
    m_AllocatorFd = 0;
#endif
}

ProcMemAllocator::ProcMemAllocator()
    : ProcMemAllocator(DEVICE_NODE)
{}

ProcMemAllocator::~ProcMemAllocator()
{
#ifdef TARGET_KMOD
    close(m_AllocatorFd);
#endif
}

Buffer ProcMemAllocator::CreateBuffer(uint32_t size, DataFormat format)
{
    return Buffer(std::make_unique<Buffer::BufferImpl>(size, format, m_AllocatorFd));
}

Buffer ProcMemAllocator::CreateBuffer(const uint8_t* src, uint32_t size, DataFormat format)
{
    return Buffer(std::make_unique<Buffer::BufferImpl>(src, size, format, m_AllocatorFd));
}

Buffer ProcMemAllocator::ImportBuffer(int fd, uint32_t size)
{
    return Buffer(std::make_unique<Buffer::BufferImpl>(fd, size, m_AllocatorFd));
}

Network ProcMemAllocator::CreateNetwork(const char* compiledNetworkData, size_t compiledNetworkSize)
{
    return Network(
#if defined(TARGET_MODEL)
        std::make_unique<ModelNetworkImpl>(compiledNetworkData, compiledNetworkSize)
#elif defined(TARGET_KMOD)
        std::make_unique<KmodNetworkImpl>(compiledNetworkData, compiledNetworkSize, m_AllocatorFd)
#elif defined(TARGET_DUMPONLY)
        std::make_unique<NetworkImpl>(compiledNetworkData, compiledNetworkSize, false)
#else
#error "Unknown target backend."
#endif
    );
}

}    // namespace driver_library
}    // namespace ethosn