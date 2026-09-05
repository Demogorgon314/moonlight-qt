#include "backend/apple/applemetaltexturereferences_p.h"

#include <QDebug>
#include <QScopeGuard>

bool testAppleMetalTexturesSurviveUntilCompletion()
{ @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    id<MTLCommandQueue> queue = [device newCommandQueue];
    CVMetalTextureCacheRef cache = nullptr;
    auto cleanup = qScopeGuard([&] {
        if (cache != nullptr) CFRelease(cache);
        [queue release];
        [device release];
    });
    if (queue == nil || CVMetalTextureCacheCreate(
                nullptr, nullptr, device, nullptr, &cache) != kCVReturnSuccess) {
        return false;
    }

    for (bool submit : {false, true}) {
        for (int tileCount : {1, 2}) {
            CVPixelBufferPoolRef pool = nullptr;
            NSDictionary* attributes = @{
                (id)kCVPixelBufferPixelFormatTypeKey: @(0x34343466), // NV24 '444f'
                (id)kCVPixelBufferWidthKey: @8,
                (id)kCVPixelBufferHeightKey: @4,
                (id)kCVPixelBufferIOSurfacePropertiesKey: @{},
            };
            if (CVPixelBufferPoolCreate(nullptr, nullptr,
                        (CFDictionaryRef)attributes, &pool) != kCVReturnSuccess) return false;
            auto releasePool = qScopeGuard([&] { CFRelease(pool); });
            NSDictionary* limit = @{
                (id)kCVPixelBufferPoolAllocationThresholdKey: @(tileCount),
            };
            id<MTLSharedEvent> gate = [device newSharedEvent];
            id<MTLCommandBuffer> command = [queue commandBuffer];
            bool committed = false;
            auto releaseGate = qScopeGuard([&] {
                gate.signaledValue = 1;
                if (committed) [command waitUntilCompleted];
                [gate release];
            });
            if (gate == nil || command == nil) return false;
            [command encodeWaitForEvent:gate value:1];
            @autoreleasepool {
                AppleMetalTextureReferences images;
                for (int tile = 0; tile < tileCount; ++tile) {
                    CVPixelBufferRef pixel = nullptr;
                    if (CVPixelBufferPoolCreatePixelBufferWithAuxAttributes(
                                nullptr, pool, (CFDictionaryRef)limit, &pixel) != kCVReturnSuccess) {
                        return false;
                    }
                    auto releasePixel = qScopeGuard([&] { CFRelease(pixel); });
                    for (int plane = 0; plane < 2; ++plane) {
                        CVMetalTextureRef image = nullptr;
                        const CVReturn status = CVMetalTextureCacheCreateTextureFromImage(
                                nullptr, cache, pixel, nullptr,
                                plane == 0 ? MTLPixelFormatR8Unorm : MTLPixelFormatRG8Unorm,
                                8, 4, plane, &image);
                        images.adopt(image);
                        if (status != kCVReturnSuccess || image == nullptr) return false;
                    }
                }
                if (submit) images.retainUntilCompleted(command);
            }

            const auto allocationStatus = [&] {
                CVMetalTextureCacheFlush(cache, 0);
                CVPixelBufferRef recycled = nullptr;
                const CVReturn status = CVPixelBufferPoolCreatePixelBufferWithAuxAttributes(
                        nullptr, pool, (CFDictionaryRef)limit, &recycled);
                if (recycled != nullptr) CFRelease(recycled);
                return status;
            };
            if (!submit) {
                if (allocationStatus() != kCVReturnSuccess) return false;
                continue;
            }
            if (allocationStatus() != kCVReturnWouldExceedAllocationThreshold) {
                qWarning() << "Metal images recycled before command submission";
                return false;
            }
            [command commit];
            committed = true;
            if (allocationStatus() != kCVReturnWouldExceedAllocationThreshold) {
                qWarning() << "Metal images recycled while the GPU event blocks completion";
                return false;
            }
            gate.signaledValue = 1;
            [command waitUntilCompleted];
            if (command.status != MTLCommandBufferStatusCompleted ||
                    allocationStatus() != kCVReturnSuccess) {
                qWarning() << "Metal images not reclaimed after GPU completion";
                return false;
            }
        }
    }
    return true;
}}
