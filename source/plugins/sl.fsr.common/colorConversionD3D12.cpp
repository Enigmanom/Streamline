/*
* Copyright (c) 2026
*
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to deal
* in the Software without restriction, including without limitation the rights
* to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
* copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in all
* copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
* OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
* SOFTWARE.
*/

#include "colorConversionD3D12.h"

#include "_artifacts/shaders/fsr_color_conversion_cs.h"

#include <iterator>

namespace sl::fsr
{

namespace
{

bool succeeded(chi::ComputeStatus status)
{
    return status == chi::ComputeStatus::eOk;
}

}

bool ColorConversionD3D12::initialize(chi::ICompute* compute)
{
    if (!compute) return false;
    m_compute = compute;
    return succeeded(m_compute->createKernel(
        const_cast<unsigned char*>(fsr_color_conversion_cs),
        fsr_color_conversion_cs_len,
        "fsr_color_conversion.cs",
        "main",
        m_kernel));
}

void ColorConversionD3D12::shutdown()
{
    if (m_compute && m_kernel)
    {
        m_compute->destroyKernel(m_kernel);
    }
    m_kernel = {};
    m_compute = {};
}

bool ColorConversionD3D12::ensureLinearTexture(
    LinearColorTexture& texture,
    uint32_t width,
    uint32_t height,
    const char* debugName)
{
    if (!m_compute || !width || !height) return false;
    if (texture.resource &&
        texture.width == width &&
        texture.height == height)
    {
        return true;
    }
    release(texture);
    // Frame Gen runs this on a compute queue, which can't use pixel-shader resource states.
    chi::ResourceDescription description(
        width,
        height,
        chi::eFormatRGBA16F,
        chi::HeapType::eHeapTypeDefault,
        chi::ResourceState::eStorageRead,
        chi::ResourceFlags::eShaderResourceStorage);
    if (!succeeded(m_compute->createTexture2D(
            description,
            texture.resource,
            debugName)))
    {
        texture = {};
        return false;
    }
    texture.width = width;
    texture.height = height;
    return true;
}

void ColorConversionD3D12::release(
    LinearColorTexture& texture,
    uint32_t frameDelay)
{
    if (m_compute && texture.resource)
    {
        m_compute->destroyResource(texture.resource, frameDelay);
    }
    texture = {};
}

bool ColorConversionD3D12::decodeGamma22(
    chi::CommandList commandList,
    ID3D12Resource* source,
    D3D12_RESOURCE_STATES sourceState,
    LinearColorTexture& destination,
    uint32_t width,
    uint32_t height)
{
    if (!source || !destination.resource) return false;
    Resource sourceResource(ResourceType::eTex2d, source, sourceState);
    return dispatch(
        commandList,
        &sourceResource,
        destination.resource,
        width,
        height,
        Direction::eGamma22ToLinear);
}

bool ColorConversionD3D12::encodeGamma22(
    chi::CommandList commandList,
    const LinearColorTexture& source,
    ID3D12Resource* destination,
    D3D12_RESOURCE_STATES destinationState,
    uint32_t width,
    uint32_t height)
{
    if (!source.resource || !destination) return false;
    Resource destinationResource(
        ResourceType::eTex2d,
        destination,
        destinationState);
    return dispatch(
        commandList,
        source.resource,
        &destinationResource,
        width,
        height,
        Direction::eLinearToGamma22);
}

bool ColorConversionD3D12::dispatch(
    chi::CommandList commandList,
    chi::Resource source,
    chi::Resource destination,
    uint32_t width,
    uint32_t height,
    Direction direction)
{
    if (!m_compute || !m_kernel || !commandList || !source || !destination ||
        !width || !height)
    {
        return false;
    }

    chi::ResourceState sourceState{};
    chi::ResourceState destinationState{};
    if (!succeeded(m_compute->getResourceState(source->state, sourceState)) ||
        !succeeded(m_compute->getResourceState(
            destination->state,
            destinationState)))
    {
        return false;
    }

    extra::ScopedTasks reverseTransitions;
    const chi::ResourceTransition transitions[]{
        { source, chi::ResourceState::eStorageRead, sourceState },
        { destination, chi::ResourceState::eStorageRW, destinationState }
    };
    if (!succeeded(m_compute->transitionResources(
            commandList,
            transitions,
            static_cast<uint32_t>(std::size(transitions)),
            &reverseTransitions)) ||
        !succeeded(m_compute->bindSharedState(commandList)) ||
        !succeeded(m_compute->bindKernel(m_kernel)) ||
        !succeeded(m_compute->bindTexture(0, 0, source)) ||
        !succeeded(m_compute->bindRWTexture(1, 0, destination)))
    {
        return false;
    }

    struct Constants
    {
        uint32_t width;
        uint32_t height;
        uint32_t direction;
        uint32_t unused;
    };
    const Constants constants{
        width,
        height,
        static_cast<uint32_t>(direction),
        0
    };
    return succeeded(m_compute->bindConsts(
               2,
               0,
               const_cast<Constants*>(&constants),
               sizeof(constants),
               64)) &&
        succeeded(m_compute->dispatch(
            (width + 15) / 16,
            (height + 15) / 16,
            1));
}

}
