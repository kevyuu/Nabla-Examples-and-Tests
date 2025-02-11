#include "common.hlsl"
#include "random.hlsl"

[[vk::push_constant]] SPushConstants pc;

[[vk::binding(0, 0)]] RaytracingAccelerationStructure topLevelAS;

[shader("anyhit")]
void main(inout HitPayload payload, in BuiltInTriangleIntersectionAttributes attribs)
{
    const int instID = InstanceID();
    const STriangleGeomInfo geom = vk::RawBufferLoad < STriangleGeomInfo > (pc.triangleGeomInfoBuffer + instID * sizeof(STriangleGeomInfo));
    const Material material = unpackMaterial(geom.material);
    
    uint32_t seed = payload.seed;
    if (material.dissolve == 0.0)
    {
        IgnoreHit();
    }
    else if (rnd(seed) > material.dissolve)
    {
        IgnoreHit();
    }
}
