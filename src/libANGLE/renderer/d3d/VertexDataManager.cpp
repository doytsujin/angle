//
// Copyright (c) 2002-2012 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//

// VertexDataManager.h: Defines the VertexDataManager, a class that
// runs the Buffer translation process.

#include "libANGLE/renderer/d3d/VertexDataManager.h"

#include "libANGLE/Buffer.h"
#include "libANGLE/Program.h"
#include "libANGLE/State.h"
#include "libANGLE/VertexAttribute.h"
#include "libANGLE/VertexArray.h"
#include "libANGLE/renderer/d3d/BufferD3D.h"
#include "libANGLE/renderer/d3d/VertexBuffer.h"

namespace
{
    enum { INITIAL_STREAM_BUFFER_SIZE = 1024*1024 };
    // This has to be at least 4k or else it fails on ATI cards.
    enum { CONSTANT_VERTEX_BUFFER_SIZE = 4096 };
}

namespace rx
{

static int ElementsInBuffer(const gl::VertexAttribute &attrib, unsigned int size)
{
    // Size cannot be larger than a GLsizei
    if (size > static_cast<unsigned int>(std::numeric_limits<int>::max()))
    {
        size = static_cast<unsigned int>(std::numeric_limits<int>::max());
    }

    GLsizei stride = ComputeVertexAttributeStride(attrib);
    return (size - attrib.offset % stride + (stride - ComputeVertexAttributeTypeSize(attrib))) / stride;
}

static int StreamingBufferElementCount(const gl::VertexAttribute &attrib, int vertexDrawCount, int instanceDrawCount)
{
    // For instanced rendering, we draw "instanceDrawCount" sets of "vertexDrawCount" vertices.
    //
    // A vertex attribute with a positive divisor loads one instanced vertex for every set of
    // non-instanced vertices, and the instanced vertex index advances once every "mDivisor" instances.
    if (instanceDrawCount > 0 && attrib.divisor > 0)
    {
        // When instanceDrawCount is not a multiple attrib.divisor, the division must round up.
        // For instance, with 5 non-instanced vertices and a divisor equal to 3, we need 2 instanced vertices.
        return (instanceDrawCount + attrib.divisor - 1) / attrib.divisor;
    }

    return vertexDrawCount;
}

VertexDataManager::CurrentValueState::CurrentValueState()
    : buffer(nullptr),
      offset(0)
{
    data.FloatValues[0] = std::numeric_limits<float>::quiet_NaN();
    data.FloatValues[1] = std::numeric_limits<float>::quiet_NaN();
    data.FloatValues[2] = std::numeric_limits<float>::quiet_NaN();
    data.FloatValues[3] = std::numeric_limits<float>::quiet_NaN();
    data.Type = GL_FLOAT;
}

VertexDataManager::CurrentValueState::~CurrentValueState()
{
    SafeDelete(buffer);
}

VertexDataManager::VertexDataManager(BufferFactoryD3D *factory)
    : mFactory(factory),
      mStreamingBuffer(nullptr),
      // TODO(jmadill): use context caps
      mCurrentValueCache(gl::MAX_VERTEX_ATTRIBS)
{
    mStreamingBuffer = new StreamingVertexBufferInterface(factory, INITIAL_STREAM_BUFFER_SIZE);

    if (!mStreamingBuffer)
    {
        ERR("Failed to allocate the streaming vertex buffer.");
    }
}

VertexDataManager::~VertexDataManager()
{
    SafeDelete(mStreamingBuffer);
}

void VertexDataManager::hintUnmapAllResources(const std::vector<gl::VertexAttribute> &vertexAttributes)
{
    mStreamingBuffer->getVertexBuffer()->hintUnmapResource();

    for (size_t i = 0; i < vertexAttributes.size(); i++)
    {
        const gl::VertexAttribute &attrib = vertexAttributes[i];
        if (attrib.enabled)
        {
            gl::Buffer *buffer = attrib.buffer.get();
            BufferD3D *storage = buffer ? GetImplAs<BufferD3D>(buffer) : NULL;
            StaticVertexBufferInterface *staticBuffer = storage ? storage->getStaticVertexBuffer() : NULL;

            if (staticBuffer)
            {
                staticBuffer->getVertexBuffer()->hintUnmapResource();
            }
        }
    }

    for (auto &currentValue : mCurrentValueCache)
    {
        if (currentValue.buffer != nullptr)
        {
            currentValue.buffer->getVertexBuffer()->hintUnmapResource();
        }
    }
}

gl::Error VertexDataManager::prepareVertexData(const gl::State &state, GLint start, GLsizei count,
                                               TranslatedAttribute *translated, GLsizei instances)
{
    if (!mStreamingBuffer)
    {
        return gl::Error(GL_OUT_OF_MEMORY, "Internal streaming vertex buffer is unexpectedly NULL.");
    }

    const gl::VertexArray *vertexArray = state.getVertexArray();
    const std::vector<gl::VertexAttribute> &vertexAttributes = vertexArray->getVertexAttributes();

    for (size_t attribIndex = 0; attribIndex < vertexAttributes.size(); ++attribIndex)
    {
        translated[attribIndex].active = (state.getProgram()->getSemanticIndex(attribIndex) != -1);
        if (translated[attribIndex].active)
        {
            // Record the attribute now
            translated[attribIndex].attribute = &vertexAttributes[attribIndex];

            if (vertexAttributes[attribIndex].enabled)
            {
                // Also invalidate static buffers that don't contain matching attributes
                invalidateMatchingStaticData(vertexAttributes[attribIndex],
                                             state.getVertexAttribCurrentValue(attribIndex));
            }
        }
    }

    // Reserve the required space in the buffers
    for (int i = 0; i < gl::MAX_VERTEX_ATTRIBS; i++)
    {
        if (translated[i].active && translated[i].attribute->enabled)
        {
            gl::Error error = reserveSpaceForAttrib(*translated[i].attribute, state.getVertexAttribCurrentValue(i), count, instances);
            if (error.isError())
            {
                return error;
            }
        }
    }

    // Perform the vertex data translations
    for (int i = 0; i < gl::MAX_VERTEX_ATTRIBS; i++)
    {
        if (translated[i].active)
        {
            if (translated[i].attribute->enabled)
            {
                gl::Error error = storeAttribute(state.getVertexAttribCurrentValue(i),
                                                 &translated[i],
                                                 start,
                                                 count,
                                                 instances);

                if (error.isError())
                {
                    hintUnmapAllResources(vertexAttributes);
                    return error;
                }
            }
            else
            {
                if (mCurrentValueCache[i].buffer == nullptr)
                {
                    mCurrentValueCache[i].buffer = new StreamingVertexBufferInterface(mFactory, CONSTANT_VERTEX_BUFFER_SIZE);
                }

                gl::Error error = storeCurrentValue(state.getVertexAttribCurrentValue(i),
                                                    &translated[i],
                                                    &mCurrentValueCache[i]);
                if (error.isError())
                {
                    hintUnmapAllResources(vertexAttributes);
                    return error;
                }
            }
        }
    }

    // Hint to unmap all the resources
    hintUnmapAllResources(vertexAttributes);

    for (int i = 0; i < gl::MAX_VERTEX_ATTRIBS; i++)
    {
        const gl::VertexAttribute &curAttrib = vertexAttributes[i];
        if (translated[i].active && curAttrib.enabled)
        {
            gl::Buffer *buffer = curAttrib.buffer.get();

            if (buffer)
            {
                BufferD3D *bufferImpl = GetImplAs<BufferD3D>(buffer);
                bufferImpl->promoteStaticUsage(count * ComputeVertexAttributeTypeSize(curAttrib));
            }
        }
    }

    return gl::Error(GL_NO_ERROR);
}

void VertexDataManager::invalidateMatchingStaticData(const gl::VertexAttribute &attrib,
                                                     const gl::VertexAttribCurrentValueData &currentValue) const
{
    gl::Buffer *buffer = attrib.buffer.get();

    if (buffer)
    {
        BufferD3D *bufferImpl = GetImplAs<BufferD3D>(buffer);
        StaticVertexBufferInterface *staticBuffer = bufferImpl->getStaticVertexBuffer();

        if (staticBuffer &&
            staticBuffer->getBufferSize() > 0 &&
            !staticBuffer->lookupAttribute(attrib, NULL) &&
            !staticBuffer->directStoragePossible(attrib, currentValue))
        {
            bufferImpl->invalidateStaticData();
        }
    }
}

gl::Error VertexDataManager::reserveSpaceForAttrib(const gl::VertexAttribute &attrib,
                                                   const gl::VertexAttribCurrentValueData &currentValue,
                                                   GLsizei count,
                                                   GLsizei instances) const
{
    gl::Buffer *buffer = attrib.buffer.get();
    BufferD3D *bufferImpl = buffer ? GetImplAs<BufferD3D>(buffer) : NULL;
    StaticVertexBufferInterface *staticBuffer = bufferImpl ? bufferImpl->getStaticVertexBuffer() : NULL;
    VertexBufferInterface *vertexBuffer = staticBuffer ? staticBuffer : static_cast<VertexBufferInterface*>(mStreamingBuffer);

    if (!vertexBuffer->directStoragePossible(attrib, currentValue))
    {
        if (staticBuffer)
        {
            if (staticBuffer->getBufferSize() == 0)
            {
                int totalCount = ElementsInBuffer(attrib, bufferImpl->getSize());
                gl::Error error = staticBuffer->reserveVertexSpace(attrib, totalCount, 0);
                if (error.isError())
                {
                    return error;
                }
            }
        }
        else
        {
            int totalCount = StreamingBufferElementCount(attrib, count, instances);
            ASSERT(!bufferImpl || ElementsInBuffer(attrib, bufferImpl->getSize()) >= totalCount);

            gl::Error error = mStreamingBuffer->reserveVertexSpace(attrib, totalCount, instances);
            if (error.isError())
            {
                return error;
            }
        }
    }

    return gl::Error(GL_NO_ERROR);
}

gl::Error VertexDataManager::storeAttribute(const gl::VertexAttribCurrentValueData &currentValue,
                                            TranslatedAttribute *translated,
                                            GLint start,
                                            GLsizei count,
                                            GLsizei instances)
{
    const gl::VertexAttribute &attrib = *translated->attribute;

    gl::Buffer *buffer = attrib.buffer.get();
    ASSERT(buffer || attrib.pointer);

    BufferD3D *storage = buffer ? GetImplAs<BufferD3D>(buffer) : NULL;
    StaticVertexBufferInterface *staticBuffer = storage ? storage->getStaticVertexBuffer() : NULL;
    VertexBufferInterface *vertexBuffer = staticBuffer ? staticBuffer : static_cast<VertexBufferInterface*>(mStreamingBuffer);
    bool directStorage = vertexBuffer->directStoragePossible(attrib, currentValue);

    unsigned int streamOffset = 0;
    unsigned int outputElementSize = 0;

    // Instanced vertices do not apply the 'start' offset
    GLint firstVertexIndex = (instances > 0 && attrib.divisor > 0 ? 0 : start);

    if (directStorage)
    {
        outputElementSize = ComputeVertexAttributeStride(attrib);
        streamOffset = static_cast<unsigned int>(attrib.offset + outputElementSize * firstVertexIndex);
    }
    else if (staticBuffer)
    {
        gl::Error error = staticBuffer->getVertexBuffer()->getSpaceRequired(attrib, 1, 0, &outputElementSize);
        if (error.isError())
        {
            return error;
        }

        if (!staticBuffer->lookupAttribute(attrib, &streamOffset))
        {
            // Convert the entire buffer
            int totalCount = ElementsInBuffer(attrib, storage->getSize());
            int startIndex = attrib.offset / ComputeVertexAttributeStride(attrib);

            error = staticBuffer->storeVertexAttributes(attrib, currentValue, -startIndex, totalCount,
                                                        0, &streamOffset);
            if (error.isError())
            {
                return error;
            }
        }

        unsigned int firstElementOffset = (attrib.offset / ComputeVertexAttributeStride(attrib)) * outputElementSize;
        unsigned int startOffset = (instances == 0 || attrib.divisor == 0) ? firstVertexIndex * outputElementSize : 0;
        if (streamOffset + firstElementOffset + startOffset < streamOffset)
        {
            return gl::Error(GL_OUT_OF_MEMORY);
        }

        streamOffset += firstElementOffset + startOffset;
    }
    else
    {
        int totalCount = StreamingBufferElementCount(attrib, count, instances);
        gl::Error error = mStreamingBuffer->getVertexBuffer()->getSpaceRequired(attrib, 1, 0, &outputElementSize);
        if (error.isError())
        {
            return error;
        }

        error = mStreamingBuffer->storeVertexAttributes(attrib, currentValue, firstVertexIndex,
                                                        totalCount, instances, &streamOffset);
        if (error.isError())
        {
            return error;
        }
    }

    translated->storage = directStorage ? storage : NULL;
    translated->vertexBuffer = vertexBuffer->getVertexBuffer();
    translated->serial = directStorage ? storage->getSerial() : vertexBuffer->getSerial();
    translated->divisor = attrib.divisor;

    translated->currentValueType = currentValue.Type;
    translated->stride = outputElementSize;
    translated->offset = streamOffset;

    return gl::Error(GL_NO_ERROR);
}

gl::Error VertexDataManager::storeCurrentValue(const gl::VertexAttribCurrentValueData &currentValue,
                                               TranslatedAttribute *translated,
                                               CurrentValueState *cachedState)
{
    if (cachedState->data != currentValue)
    {
        const gl::VertexAttribute &attrib = *translated->attribute;
        gl::Error error = cachedState->buffer->reserveVertexSpace(attrib, 1, 0);
        if (error.isError())
        {
            return error;
        }

        unsigned int streamOffset;
        error = cachedState->buffer->storeVertexAttributes(attrib, currentValue, 0, 1, 0, &streamOffset);
        if (error.isError())
        {
            return error;
        }

        cachedState->data = currentValue;
        cachedState->offset = streamOffset;
    }

    translated->storage = NULL;
    translated->vertexBuffer = cachedState->buffer->getVertexBuffer();
    translated->serial = cachedState->buffer->getSerial();
    translated->divisor = 0;

    translated->currentValueType = currentValue.Type;
    translated->stride = 0;
    translated->offset = cachedState->offset;

    return gl::Error(GL_NO_ERROR);
}

}
