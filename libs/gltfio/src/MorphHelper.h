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

#include "FFilamentAsset.h"
#include "FFilamentInstance.h"

#include <math/vec4.h>

#include <utils/Hash.h>

#include <tsl/robin_map.h>

#include <vector>

struct cgltf_node;
struct cgltf_primitive;

namespace gltfio {

/**
 * Internal class that partitions lists of morph weights and maintains a cache of VertexBuffer
 * objects for each partition.
 *
 * MorphHelper allows Filament to fully support meshes with many morph targets, as long as no more
 * than 4 are ever used simultaneously. If more than 4 are used simultaneously, MorphHelpers falls
 * back to a reasonable compromise by picking the 4 most influential weight values.
 *
 * Animator has ownership over a single instance of MorphHelper, thus it is 1:1 with FilamentAsset.
 */
class MorphHelper {
public:
    using Entity = utils::Entity;

    MorphHelper(FFilamentAsset* asset, FFilamentInstance* inst);
    ~MorphHelper();
    void applyWeights(Entity targetEntity, float const* weights, size_t count);

private:
    // At any point in time over the course of a particular animation, we consider the indices of
    // the four most influential weights, then proclaim that they the "primary indices", and stash
    // them in a ubyte4. Technically this should be an unordered set rather than a tuple. However
    // the indices are naturally sorted so it works out that this a good set representation. We
    // form a cache of VertexBuffer objects by bundling a particular permutation of primary indices
    // with a reference to its intended target entity; this is our cache key.
    struct MorphKey {
        Entity targetEntity;
        filament::math::ubyte4 primaryIndices;
    };

    // The important values in the cache are the VertexBuffer objects that the helper creates,
    // but we also stash the index buffer and primitive type, since they are not queryable in the
    // Filament API.
    struct Primitive {
        filament::VertexBuffer* vertices;
        filament::IndexBuffer* indices;
        filament::RenderableManager::PrimitiveType type;
    };

    using MorphValue = std::vector<Primitive>;
    using MorphKeyHashFn = utils::hash::MurmurHashFn<MorphKey>;

    struct MorphKeyEqualFn {
        bool operator()(const MorphKey& k1, const MorphKey& k2) const {
            return k1.targetEntity == k2.targetEntity &&
                    k1.primaryIndices == k2.primaryIndices;
        }
    };

    MorphValue createMorphTableEntry(Entity targetEntity, filament::math::ubyte4 primaryIndices);
    filament::VertexBuffer* createVertexBuffer(const cgltf_primitive& prim, const UvMap& uvmap,
            filament::math::ubyte4 primaryIndices);

    std::vector<float> mPartiallySortedWeights;
    tsl::robin_map<MorphKey, MorphValue, MorphKeyHashFn, MorphKeyEqualFn> mMorphTable;
    tsl::robin_map<Entity, const cgltf_node*> mNodeMap;
    const FFilamentAsset* mAsset;
    const FFilamentInstance* mInstance;
};

} // namespace gltfio
