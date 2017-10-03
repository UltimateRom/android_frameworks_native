/*
 * Copyright (C) 2007 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "GraphicBuffer"

#include <ui/GraphicBuffer.h>

#include <cutils/atomic.h>

#include <grallocusage/GrallocUsageConversion.h>

#include <ui/Gralloc2.h>
#include <ui/GraphicBufferAllocator.h>
#include <ui/GraphicBufferMapper.h>

namespace android {

// ===========================================================================
// Buffer and implementation of ANativeWindowBuffer
// ===========================================================================

static uint64_t getUniqueId() {
    static volatile int32_t nextId = 0;
    uint64_t id = static_cast<uint64_t>(getpid()) << 32;
    id |= static_cast<uint32_t>(android_atomic_inc(&nextId));
    return id;
}

sp<GraphicBuffer> GraphicBuffer::from(ANativeWindowBuffer* anwb) {
    return static_cast<GraphicBuffer *>(anwb);
}

GraphicBuffer::GraphicBuffer()
    : BASE(), mOwner(ownData), mBufferMapper(GraphicBufferMapper::get()),
      mInitCheck(NO_ERROR), mId(getUniqueId())
{
    width  =
    height =
    stride =
    format =
    usage  = 0;
    layerCount = 0;
    handle = NULL;
}

// deprecated
GraphicBuffer::GraphicBuffer(uint32_t inWidth, uint32_t inHeight,
        PixelFormat inFormat, uint32_t inUsage, std::string requestorName)
    : GraphicBuffer(inWidth, inHeight, inFormat, 1, static_cast<uint64_t>(inUsage),
            requestorName)
{
}

GraphicBuffer::GraphicBuffer(uint32_t inWidth, uint32_t inHeight,
        PixelFormat inFormat, uint32_t inLayerCount, uint64_t usage,
        std::string requestorName)
    : GraphicBuffer()
{
    mInitCheck = initWithSize(inWidth, inHeight, inFormat, inLayerCount,
            usage);
}

// deprecated
GraphicBuffer::GraphicBuffer(uint32_t inWidth, uint32_t inHeight,
        PixelFormat inFormat, uint32_t inLayerCount, uint32_t inUsage,
        uint32_t inStride, native_handle_t* inHandle, bool keepOwnership)
    : GraphicBuffer(inHandle, keepOwnership ? TAKE_HANDLE : WRAP_HANDLE,
            inWidth, inHeight, inFormat, inLayerCount, static_cast<uint64_t>(inUsage),
            inStride)
{
}

GraphicBuffer::GraphicBuffer(uint32_t inWidth, uint32_t inHeight,
        PixelFormat inFormat, uint32_t inUsage, uint32_t inStride,
        native_handle_t* inHandle, bool keepOwnership)
    : BASE(), mOwner(keepOwnership ? ownHandle : ownNone),
      mBufferMapper(GraphicBufferMapper::get()),
      mInitCheck(NO_ERROR), mId(getUniqueId())
{
    width  = static_cast<int>(inWidth);
    height = static_cast<int>(inHeight);
    stride = static_cast<int>(inStride);
    format = inFormat;
    usage  = static_cast<int>(inUsage);
    handle = inHandle;
}

GraphicBuffer::GraphicBuffer(ANativeWindowBuffer* buffer, bool keepOwnership)
    : BASE(), mOwner(keepOwnership ? ownHandle : ownNone),
      mBufferMapper(GraphicBufferMapper::get()),
      mInitCheck(NO_ERROR), mId(getUniqueId())
{
    width  = buffer->width;
    height = buffer->height;
    stride = buffer->stride;
    format = buffer->format;
    layerCount = buffer->layerCount;
    usage  = buffer->usage;
    handle = buffer->handle;
}

GraphicBuffer::GraphicBuffer(const native_handle_t* handle,
        HandleWrapMethod method, uint32_t width, uint32_t height,
        PixelFormat format, uint32_t layerCount,
        uint64_t usage,
        uint32_t stride)
    : GraphicBuffer()
{
    mInitCheck = initWithHandle(handle, method, width, height, format,
            layerCount, usage, stride);
}

GraphicBuffer::~GraphicBuffer()
{
    if (handle) {
        free_handle();
    }
}

void GraphicBuffer::free_handle()
{
     if (mOwner == ownHandle) {
         mBufferMapper.unregisterBuffer(handle);
         native_handle_close(handle);
         native_handle_delete(const_cast<native_handle*>(handle));
     } else if (mOwner == ownData) {
         GraphicBufferAllocator& allocator(GraphicBufferAllocator::get());
         allocator.free(handle);
     }

#ifndef EGL_NEEDS_HANDLE
    handle = NULL;
#endif
}

status_t GraphicBuffer::initCheck() const {
    return static_cast<status_t>(mInitCheck);
}

void GraphicBuffer::dumpAllocationsToSystemLog()
{
    GraphicBufferAllocator::dumpToSystemLog();
}

ANativeWindowBuffer* GraphicBuffer::getNativeBuffer() const
{
    LOG_ALWAYS_FATAL_IF(this == NULL, "getNativeBuffer() called on NULL GraphicBuffer");
    return static_cast<ANativeWindowBuffer*>(
            const_cast<GraphicBuffer*>(this));
}

status_t GraphicBuffer::reallocate(uint32_t inWidth, uint32_t inHeight,
        PixelFormat inFormat, uint32_t inLayerCount, uint64_t inUsage)
{
    if (mOwner != ownData)
        return INVALID_OPERATION;

    if (handle &&
            static_cast<int>(inWidth) == width &&
            static_cast<int>(inHeight) == height &&
            inFormat == format &&
            inLayerCount == layerCount &&
            static_cast<int>(inUsage) == usage)
        return NO_ERROR;

    if (handle) {
        GraphicBufferAllocator& allocator(GraphicBufferAllocator::get());
        allocator.free(handle);
        handle = 0;
    }
    return initWithSize(inWidth, inHeight, inFormat, inLayerCount,
            inUsage);
}

bool GraphicBuffer::needsReallocation(uint32_t inWidth, uint32_t inHeight,
        PixelFormat inFormat, uint32_t inLayerCount, uint64_t inUsage)
{
    if (static_cast<int>(inWidth) != width) return true;
    if (static_cast<int>(inHeight) != height) return true;
    if (inFormat != format) return true;
    if (inLayerCount != layerCount) return true;
    if ((static_cast<uint32_t>(usage) & inUsage) != inUsage) return true;
    return false;
}

status_t GraphicBuffer::initWithSize(uint32_t inWidth, uint32_t inHeight,
        PixelFormat inFormat, uint32_t inLayerCount, uint32_t inUsage)
{
    GraphicBufferAllocator& allocator = GraphicBufferAllocator::get();
    uint32_t outStride = 0;
    status_t err = allocator.alloc(inWidth, inHeight, inFormat, inUsage, &handle, &outStride);
    if (err == NO_ERROR) {
        width = static_cast<int>(inWidth);
        height = static_cast<int>(inHeight);
        format = inFormat;
        layerCount = inLayerCount;
        usage = static_cast<int>(inUsage);
        stride = static_cast<int>(outStride);
    }
    return err;
}

status_t GraphicBuffer::initWithHandle(const native_handle_t* handle,
        HandleWrapMethod method, uint32_t width, uint32_t height,
        PixelFormat format, uint32_t layerCount, uint64_t usage,
        uint32_t stride)
{
    native_handle_t* clone = nullptr;

    if (method == CLONE_HANDLE) {
        clone = native_handle_clone(handle);
        if (!clone) {
            return NO_MEMORY;
        }

        handle = clone;
        method = TAKE_UNREGISTERED_HANDLE;
    }

    ANativeWindowBuffer::width  = static_cast<int>(width);
    ANativeWindowBuffer::height = static_cast<int>(height);
    ANativeWindowBuffer::stride = static_cast<int>(stride);
    ANativeWindowBuffer::format = format;
    ANativeWindowBuffer::usage  = static_cast<int>(usage);

    ANativeWindowBuffer::layerCount = layerCount;
    ANativeWindowBuffer::handle = handle;

    mOwner = (method == WRAP_HANDLE) ? ownNone : ownHandle;

    if (method == TAKE_UNREGISTERED_HANDLE) {
        status_t err = mBufferMapper.importBuffer(handle);
        if (err != NO_ERROR) {
            // clean up cloned handle
            if (clone) {
                native_handle_close(clone);
                native_handle_delete(clone);
            }

            initWithHandle(nullptr, WRAP_HANDLE, 0, 0, 0, 0, 0, 0);

            return err;
        }
    }

    return NO_ERROR;
}

status_t GraphicBuffer::lock(uint32_t inUsage, void** vaddr)
{
    const Rect lockBounds(width, height);
    status_t res = lock(inUsage, lockBounds, vaddr);
    return res;
}

status_t GraphicBuffer::lock(uint32_t inUsage, const Rect& rect, void** vaddr)
{
    if (rect.left < 0 || rect.right  > width ||
        rect.top  < 0 || rect.bottom > height) {
        ALOGE("locking pixels (%d,%d,%d,%d) outside of buffer (w=%d, h=%d)",
                rect.left, rect.top, rect.right, rect.bottom,
                width, height);
        return BAD_VALUE;
    }
    status_t res = getBufferMapper().lock(handle, inUsage, rect, vaddr);
    return res;
}

status_t GraphicBuffer::lockYCbCr(uint32_t inUsage, android_ycbcr* ycbcr)
{
    const Rect lockBounds(width, height);
    status_t res = lockYCbCr(inUsage, lockBounds, ycbcr);
    return res;
}

status_t GraphicBuffer::lockYCbCr(uint32_t inUsage, const Rect& rect,
        android_ycbcr* ycbcr)
{
    if (rect.left < 0 || rect.right  > width ||
        rect.top  < 0 || rect.bottom > height) {
        ALOGE("locking pixels (%d,%d,%d,%d) outside of buffer (w=%d, h=%d)",
                rect.left, rect.top, rect.right, rect.bottom,
                width, height);
        return BAD_VALUE;
    }
    status_t res = getBufferMapper().lockYCbCr(handle, inUsage, rect, ycbcr);
    return res;
}

status_t GraphicBuffer::unlock()
{
    status_t res = getBufferMapper().unlock(handle);
    return res;
}

status_t GraphicBuffer::lockAsync(uint32_t inUsage, void** vaddr, int fenceFd)
{
    const Rect lockBounds(width, height);
    status_t res = lockAsync(inUsage, lockBounds, vaddr, fenceFd);
    return res;
}

/*
status_t GraphicBuffer::lockAsync(uint32_t inUsage, const Rect& rect,
        void** vaddr, int fenceFd)
{
    return lockAsync(inUsage, inUsage, rect, vaddr, fenceFd);
}
*/

status_t GraphicBuffer::lockAsync(uint32_t inUsage, const Rect& rect, void** vaddr, int fenceFd)
{
    if (rect.left < 0 || rect.right  > width ||
        rect.top  < 0 || rect.bottom > height) {
        ALOGE("locking pixels (%d,%d,%d,%d) outside of buffer (w=%d, h=%d)",
                rect.left, rect.top, rect.right, rect.bottom,
                width, height);
        return BAD_VALUE;
    }
    status_t res = getBufferMapper().lockAsync(handle, inUsage, rect, vaddr, fenceFd);
    return res;
}

status_t GraphicBuffer::lockAsyncYCbCr(uint32_t inUsage, android_ycbcr* ycbcr,
        int fenceFd)
{
    const Rect lockBounds(width, height);
    status_t res = lockAsyncYCbCr(inUsage, lockBounds, ycbcr, fenceFd);
    return res;
}

status_t GraphicBuffer::lockAsyncYCbCr(uint32_t inUsage, const Rect& rect,
        android_ycbcr* ycbcr, int fenceFd)
{
    if (rect.left < 0 || rect.right  > width ||
        rect.top  < 0 || rect.bottom > height) {
        ALOGE("locking pixels (%d,%d,%d,%d) outside of buffer (w=%d, h=%d)",
                rect.left, rect.top, rect.right, rect.bottom,
                width, height);
        return BAD_VALUE;
    }
    status_t res = getBufferMapper().lockAsyncYCbCr(handle, inUsage, rect,
            ycbcr, fenceFd);
    return res;
}

status_t GraphicBuffer::unlockAsync(int *fenceFd)
{
    status_t res = getBufferMapper().unlockAsync(handle, fenceFd);
    return res;
}

size_t GraphicBuffer::getFlattenedSize() const {
    return static_cast<size_t>(12 + (handle ? handle->numInts : 0)) * sizeof(int);
}

size_t GraphicBuffer::getFdCount() const {
    return static_cast<size_t>(handle ? handle->numFds : 0);
}

status_t GraphicBuffer::flatten(void*& buffer, size_t& size, int*& fds, size_t& count) const {
    size_t sizeNeeded = GraphicBuffer::getFlattenedSize();
    if (size < sizeNeeded) return NO_MEMORY;

    size_t fdCountNeeded = GraphicBuffer::getFdCount();
    if (count < fdCountNeeded) return NO_MEMORY;

    int32_t* buf = static_cast<int32_t*>(buffer);
    buf[0] = 'GBFR';
    buf[1] = width;
    buf[2] = height;
    buf[3] = stride;
    buf[4] = format;
    buf[5] = static_cast<int32_t>(layerCount);
    buf[6] = usage;
    buf[7] = static_cast<int32_t>(mId >> 32);
    buf[8] = static_cast<int32_t>(mId & 0xFFFFFFFFull);
    buf[9] = static_cast<int32_t>(mGenerationNumber);
    buf[10] = 0;
    buf[11] = 0;

    if (handle) {
        buf[10] = handle->numFds;
        buf[11] = handle->numInts;
        memcpy(fds, handle->data,
                static_cast<size_t>(handle->numFds) * sizeof(int));
        memcpy(&buf[12], handle->data + handle->numFds,
                static_cast<size_t>(handle->numInts) * sizeof(int));
    }

    buffer = static_cast<void*>(static_cast<uint8_t*>(buffer) + sizeNeeded);
    size -= sizeNeeded;
    if (handle) {
        fds += handle->numFds;
        count -= static_cast<size_t>(handle->numFds);
    }

    return NO_ERROR;
}

status_t GraphicBuffer::unflatten(
        void const*& buffer, size_t& size, int const*& fds, size_t& count) {
    if (size < 12 * sizeof(int)) return NO_MEMORY;

    int const* buf = static_cast<int const*>(buffer);
    if (buf[0] != 'GBFR') return BAD_TYPE;

    const size_t numFds  = static_cast<size_t>(buf[10]);
    const size_t numInts = static_cast<size_t>(buf[11]);

    // Limit the maxNumber to be relatively small. The number of fds or ints
    // should not come close to this number, and the number itself was simply
    // chosen to be high enough to not cause issues and low enough to prevent
    // overflow problems.
    const size_t maxNumber = 4096;
    if (numFds >= maxNumber || numInts >= (maxNumber - 12)) {
        width = height = stride = format = layerCount = usage = 0;
        handle = NULL;
        ALOGE("unflatten: numFds or numInts is too large: %zd, %zd",
                numFds, numInts);
        return BAD_VALUE;
    }

    const size_t sizeNeeded = (12 + numInts) * sizeof(int);
    if (size < sizeNeeded) return NO_MEMORY;

    size_t fdCountNeeded = numFds;
    if (count < fdCountNeeded) return NO_MEMORY;

    if (handle) {
        // free previous handle if any
        free_handle();
    }

    if (numFds || numInts) {
        width  = buf[1];
        height = buf[2];
        stride = buf[3];
        format = buf[4];
        layerCount = static_cast<uintptr_t>(buf[5]);
        usage  = buf[6];
        native_handle* h = native_handle_create(
                static_cast<int>(numFds), static_cast<int>(numInts));
        if (!h) {
            width = height = stride = format = layerCount = usage = 0;
            handle = NULL;
            ALOGE("unflatten: native_handle_create failed");
            return NO_MEMORY;
        }
        memcpy(h->data, fds, numFds * sizeof(int));
        memcpy(h->data + numFds, &buf[12], numInts * sizeof(int));
        handle = h;
    } else {
        width = height = stride = format = layerCount = usage = 0;
        handle = NULL;
    }

    mId = static_cast<uint64_t>(buf[7]) << 32;
    mId |= static_cast<uint32_t>(buf[8]);

    mGenerationNumber = static_cast<uint32_t>(buf[9]);

    mOwner = ownHandle;

    if (handle != 0) {
        status_t err = mBufferMapper.importBuffer(handle);
        if (err != NO_ERROR) {
            width = height = stride = format = layerCount = usage = 0;
            handle = NULL;
            ALOGE("unflatten: registerBuffer failed: %s (%d)",
                    strerror(-err), err);
            return err;
        }
    }

    buffer = static_cast<void const*>(static_cast<uint8_t const*>(buffer) + sizeNeeded);
    size -= sizeNeeded;
    fds += numFds;
    count -= numFds;

    return NO_ERROR;
}

// ---------------------------------------------------------------------------

}; // namespace android
