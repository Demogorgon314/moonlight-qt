#pragma once

#import <CoreVideo/CoreVideo.h>
#import <Metal/Metal.h>

#include <memory>
#include <type_traits>
#include <vector>

// MTLTexture retention alone does not keep its Core Video image in use.
// Adopt the Create-rule references while encoding, then transfer their lifetime
// to the command buffer. Abandoned encodes release them with this container.
class AppleMetalTextureReferences
{
public:
    void adopt(CVMetalTextureRef image)
    {
        if (image != nullptr) {
            m_Images.emplace_back(image, [](CVMetalTextureRef value) { CFRelease(value); });
        }
    }

    void retainUntilCompleted(id<MTLCommandBuffer> commandBuffer)
    {
        auto images = std::make_shared<decltype(m_Images)>(std::move(m_Images));
        [commandBuffer addCompletedHandler:^(id<MTLCommandBuffer>) {
            images->clear();
        }];
    }

private:
    std::vector<std::shared_ptr<std::remove_pointer_t<CVMetalTextureRef>>> m_Images;
};
