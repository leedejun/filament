/*
 * Copyright (C) 2021 The Android Open Source Project
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

#include "MorphHelper.h"

#include <filament/RenderableManager.h>
#include <filament/VertexBuffer.h>

#include "GltfEnums.h"
#include "TangentsJob.h"

using namespace filament;
using namespace filament::math;
using namespace utils;

static const auto kFreeCallback = [](void* mem, size_t, void*) { free(mem); };
static constexpr uint8_t kUnused = 0xff;

namespace gltfio {

uint32_t computeBindingSize(const cgltf_accessor* accessor);
uint32_t computeBindingOffset(const cgltf_accessor* accessor);

MorphHelper::MorphHelper(FFilamentAsset* asset, FFilamentInstance* inst) : mAsset(asset),
        mInstance(inst) {
    // Populate an inverse mapping between cgltf nodes and Filament Entities.
    NodeMap& nodeMap = asset->isInstanced() ? asset->mInstances[0]->nodeMap : asset->mNodeMap;
    for (auto pair : nodeMap) {
        mNodeMap[pair.second] = pair.first;
    }
}

MorphHelper::~MorphHelper() {
    auto engine = mAsset->mEngine;
    for (const auto& entry : mMorphTable) {
        for (auto prim : entry.second) {
            engine->destroy(prim.vertices);
        }
    }
}

void MorphHelper::applyWeights(Entity entity, float const* weights, size_t count) {
    auto renderableManager = &mAsset->mEngine->getRenderableManager();
    auto renderable = renderableManager->getInstance(entity);

    // If there are 4 or fewer targets, we can simply re-use the original VertexBuffer.
    if (count <= 4) {
        auto renderable = renderableManager->getInstance(entity);
        float4 vec(0, 0, 0, 0);
        for (size_t i = 0; i < count; i++) vec[i] = weights[i];
        renderableManager->setMorphWeights(renderable, vec);
        return;
    }

    // We only allow for 255 weights because our set representation is a 4-tuple of bytes, with one
    // slot reserved for a sentinel value. Note that 255 is much more than the glTF min spec of 4.
    // In practice this number tends to be smallish.
    count = std::min(count, size_t(255));

    // Make a copy of the weights because we want to re-order them.
    auto& sorted = mPartiallySortedWeights;
    sorted.clear();
    sorted.insert(sorted.begin(), weights, weights + count);

    // Find the four highest weights in O(n) by doing a partial sort.
    std::nth_element(sorted.begin(), sorted.begin() + 4, sorted.end(), [](float a, float b) {
        return a > b;
    });

    // Find the "primary indices" which are the indices of the four highest weights. This is O(n).
    ubyte4 primaryIndices = {kUnused, kUnused, kUnused, kUnused};
    while (sorted.size() < 4) sorted.push_back(-1);
    const size_t primaryCount = std::min(size_t(4), count);
    for (size_t index = 0, primary = 0; index < count && primary < primaryCount; ++index) {
        const float w = weights[index];
        if (w > 0 && (w == sorted[0] || w == sorted[1] || w == sorted[2] || w == sorted[3])) {
            primaryIndices[primary++] = index;
        }
    }

    // Check if we already encountered this set (fast). If not, create a new VertexBuffer (slow).
    const MorphKey key = { entity, primaryIndices };
    auto iter = mMorphTable.find(key);
    if (iter == mMorphTable.end()) {
        MorphValue val = createMorphTableEntry(entity, primaryIndices);
        iter = mMorphTable.insert({key, val}).first;
    }

    // Swap out the vertex buffer on all affected renderables. Often this will be a no-op,
    // but this is a fairly efficient operation in Filament.
    const MorphValue& tableEntry = iter.value();
    for (size_t primIndex = 0; primIndex < tableEntry.size(); ++primIndex) {
        Primitive prim = tableEntry[primIndex];
        renderableManager->setGeometryAt(renderable, primIndex, prim.type, prim.vertices,
               prim.indices, 0, prim.indices->getIndexCount());
    }

    // Finally, set the 4-tuple uniform for the weight values by derefing the primary indices.
    // Note that we first create a "safe set" by replacing the unused sentinel with zero.
    ubyte4 safe = primaryIndices;
    for (int i = 0; i < 4; i++) if (safe[i] == kUnused) safe[i] = 0;
    float4 highest(weights[safe[0]], weights[safe[1]], weights[safe[2]], weights[safe[3]]);
    renderableManager->setMorphWeights(renderable, highest);
}

MorphHelper::MorphValue MorphHelper::createMorphTableEntry(Entity entity, ubyte4 primaryIndices) {
    MorphValue result;
    const cgltf_node* node = mNodeMap[entity];
    const cgltf_mesh* mesh = node->mesh;
    const cgltf_primitive* prims = mesh->primitives;
    const cgltf_size prim_count = mesh->primitives_count;
    for (cgltf_size pi = 0; pi < prim_count; ++pi) {
        const cgltf_primitive& prim = prims[pi];
        RenderableManager::PrimitiveType prim_type;
        getPrimitiveType(prim.type, &prim_type);
        const auto& gltfioPrim = mAsset->mMeshCache.at(mesh)[pi];
        result.push_back({
            .vertices = createVertexBuffer(prim, gltfioPrim.uvmap, primaryIndices),
            .indices = gltfioPrim.indices,
            .type = prim_type,
        });
    }
    return result;
}

// This private method creates a VertexBuffer for a given permutation of "primary indices" (i.e.
// set of 4 targets). In some ways this mimics AssetLoader::createPrimitive() but it is simpler
// and less efficient because it immediately clones and uploads buffer data.
//
// TODO: This strategy is very inefficient because it results in massive duplication of data. For
// example, an unmorphed UV0 attribute will have duplicated uploads and duplicated GPU data. We will
// soon be adding a new API to Filament to fix this.
VertexBuffer* MorphHelper::createVertexBuffer(const cgltf_primitive& prim, const UvMap& uvmap,
        ubyte4 primaryIndices) {

    // Determine the number of vertices by looking at the first usable attribute.
    const uint32_t vertexCount = [&prim]() {
        uint32_t count = 0;
        for (cgltf_size aindex = 0; aindex < prim.attributes_count; aindex++) {
            const cgltf_attribute& attribute = prim.attributes[aindex];
            const cgltf_accessor* accessor = attribute.data;
            if (accessor) {
                count = accessor->count;
                break;
            }
        }
        return count;
    }();

    // This creates a copy because we don't know when the user will free the cgltf source data.
    // For non-morphed vertex buffers, we use a sharing mechanism to prevent copies, but here
    // we just want to keep it as simple as possible.
    auto createBufferDescriptor = [](const cgltf_accessor* accessor) {
        auto bufferData = (const uint8_t*) accessor->buffer_view->buffer->data;
        const uint8_t* data = computeBindingOffset(accessor) + bufferData;
        const uint32_t size = computeBindingSize(accessor);
        uint8_t* clone = (uint8_t*) malloc(size);
        memcpy(clone, data, size);
        return VertexBuffer::BufferDescriptor(clone, size, kFreeCallback);
    };

    // This is used to populate unused attributes (e.g. UV1 or COLOR) in ubershader mode.
    // It mimics the dummy buffer in AssetLoader created for non-morphing renderables.
    auto createDummyBuffer = [vertexCount]() {
        uint32_t size = sizeof(ubyte4) * vertexCount;
        uint32_t* dummyData = (uint32_t*) malloc(size);
        memset(dummyData, 0xff, size);
        return VertexBuffer::BufferDescriptor(dummyData, size, kFreeCallback);
    };

    // TODO: The following computations should be invoked by the job system, like what we
    // already do in the non-morphing codepath.
    auto createTangentsBuffer = [&prim](int morphTarget) {
        TangentsJob::Params params = {{ &prim, morphTarget }};
        TangentsJob::run(&params);
        uint32_t size = sizeof(short4) * params.out.vertexCount;
        return VertexBuffer::BufferDescriptor(params.out.results, size, kFreeCallback);
    };

    // TODO: this magic number should be exposed by a static VertexBuffer method or constant.
    static constexpr int kMaxBufferCount = 16;
    VertexBuffer::BufferDescriptor buffers[kMaxBufferCount] = {};

    VertexBuffer::Builder vbb;
    vbb.vertexCount(vertexCount);

    bool hasUv0 = false, hasUv1 = false, hasVertexColor = false, hasNormals = false;
    int slot = 0;
    for (cgltf_size aindex = 0; aindex < prim.attributes_count; aindex++) {
        const cgltf_attribute& attribute = prim.attributes[aindex];
        const int index = attribute.index;
        const cgltf_attribute_type atype = attribute.type;
        const cgltf_accessor* accessor = attribute.data;
        if (atype == cgltf_attribute_type_tangent) {
            continue;
        }
        if (atype == cgltf_attribute_type_normal) {
            vbb.attribute(VertexAttribute::TANGENTS, slot, VertexBuffer::AttributeType::SHORT4);
            vbb.normalized(VertexAttribute::TANGENTS);
            buffers[slot++] = createTangentsBuffer(TangentsJob::kMorphTargetUnused);
            hasNormals = true;
            continue;
        }
        if (atype == cgltf_attribute_type_color) {
            hasVertexColor = true;
        }
        VertexAttribute semantic;
        getVertexAttrType(atype, &semantic);
        if (atype == cgltf_attribute_type_texcoord) {
            if (index >= UvMapSize) {
                continue;
            }
            UvSet uvset = uvmap[index];
            switch (uvset) {
                case UV0:
                    semantic = VertexAttribute::UV0;
                    hasUv0 = true;
                    break;
                case UV1:
                    semantic = VertexAttribute::UV1;
                    hasUv1 = true;
                    break;
                case UNUSED:
                    if (!hasUv0 && getNumUvSets(uvmap) == 0) {
                        semantic = VertexAttribute::UV0;
                        hasUv0 = true;
                        break;
                    }
                    continue;
            }
        }
        VertexBuffer::AttributeType fatype;
        getElementType(accessor->type, accessor->component_type, &fatype);
        vbb.attribute(semantic, slot, fatype, 0, accessor->stride);
        vbb.normalized(semantic, accessor->normalized);
        buffers[slot++] = createBufferDescriptor(accessor);
    }

    // If the model is lit but does not have normals, we'll need to generate flat normals.
    if (prim.material && !prim.material->unlit && !hasNormals) {
        vbb.attribute(VertexAttribute::TANGENTS, slot, VertexBuffer::AttributeType::SHORT4);
        vbb.normalized(VertexAttribute::TANGENTS);
        buffers[slot++] = createTangentsBuffer(TangentsJob::kMorphTargetUnused);
        cgltf_attribute_type atype = cgltf_attribute_type_normal;
    }

    constexpr int baseTangentsAttr = (int) VertexAttribute::MORPH_TANGENTS_0;
    constexpr int basePositionAttr = (int) VertexAttribute::MORPH_POSITION_0;

    int targetCount = 0;
    for (; targetCount < 4; ++targetCount) {
        cgltf_size targetIndex = primaryIndices[targetCount];
        if (targetIndex == kUnused) {
            break;
        }
        const cgltf_morph_target& morphTarget = prim.targets[targetIndex];
        for (cgltf_size aindex = 0; aindex < morphTarget.attributes_count; aindex++) {
            const cgltf_attribute& attribute = morphTarget.attributes[aindex];
            const cgltf_accessor* accessor = attribute.data;
            const cgltf_attribute_type atype = attribute.type;
            if (atype == cgltf_attribute_type_tangent) {
                continue;
            }
            if (atype == cgltf_attribute_type_normal) {
                VertexAttribute attr = (VertexAttribute) (baseTangentsAttr + targetCount);
                vbb.attribute(attr, slot, VertexBuffer::AttributeType::SHORT4);
                vbb.normalized(attr);
                buffers[slot++] = createTangentsBuffer(targetIndex);
                continue;
            }
            VertexBuffer::AttributeType fatype;
            getElementType(accessor->type, accessor->component_type, &fatype);
            VertexAttribute attr = (VertexAttribute) (basePositionAttr + targetCount);
            vbb.attribute(attr, slot, fatype, 0, accessor->stride);
            vbb.normalized(attr, accessor->normalized);
            buffers[slot++] = createBufferDescriptor(accessor);
        }
    }

    // Next determine if we need to create a dummy buffer for tex coords or vertex color. If so,
    // create a single dummy buffer and share it. This is inefficient because it assumes the worst
    // case (i.e. ubershader mode) and assigns the dummy buffer to every unused attribute. We
    // should instead remember which attributes are actually required by MaterialProvider.

    bool needsDummyData = false;
    if (!hasUv0) {
        needsDummyData = true;
        vbb.attribute(VertexAttribute::UV0, slot, VertexBuffer::AttributeType::USHORT2);
        vbb.normalized(VertexAttribute::UV0);
    }
    if (!hasUv1) {
        needsDummyData = true;
        vbb.attribute(VertexAttribute::UV1, slot, VertexBuffer::AttributeType::USHORT2);
        vbb.normalized(VertexAttribute::UV1);
    }
    if (!hasVertexColor) {
        needsDummyData = true;
        vbb.attribute(VertexAttribute::COLOR, slot, VertexBuffer::AttributeType::UBYTE4);
        vbb.normalized(VertexAttribute::COLOR);
    }
    if (needsDummyData) {
        buffers[slot++] = createDummyBuffer();
    }

    const int bufferCount = slot;

    vbb.bufferCount(bufferCount);
    VertexBuffer* vertices = vbb.build(*mAsset->mEngine);

    for (int bufferIndex = 0; bufferIndex < bufferCount; ++bufferIndex) {
        if (buffers[bufferIndex].buffer != nullptr) {
            vertices->setBufferAt(*mAsset->mEngine, bufferIndex, std::move(buffers[bufferIndex]));
        }
    }

    return vertices;
}

}  // namespace gltfio
