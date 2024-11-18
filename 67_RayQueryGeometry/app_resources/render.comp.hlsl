#include "common.hlsl"

#include "nbl/builtin/hlsl/spirv_intrinsics/core.hlsl"

struct SPushConstants
{
    SGeomBDA geom[OT_COUNT];
};

[[vk::push_constant]] SPushConstants pc;

[[vk::binding(0, 0)]]
RaytracingAccelerationStructure topLevelAS;

[[vk::binding(1, 0)]]
cbuffer CameraData
{
    SCameraParameters params;
};

[[vk::binding(2, 0)]] RWTexture2D<float4> outImage;

float3 unpackNormals3x10(uint32_t v)
{
    // host side changes float32_t3 to EF_A2B10G10R10_SNORM_PACK32
    // follows unpacking scheme from https://github.com/KhronosGroup/SPIRV-Cross/blob/main/reference/shaders-hlsl/frag/unorm-snorm-packing.frag
    int signedValue = int(v);
    int3 pn = int3(signedValue << 22, signedValue << 12, signedValue << 2) >> 22;
    return clamp(float3(pn) / 511.0, -1.0, 1.0);
}

// How the normals are packed
// cube                     uint32_t - 8, 8, 8, 8
// sphere, cylinder, arrow  uint32_t - 10, 10, 10, 2
// rectangle, disk          float3
// cone                     uint32_t - 10, 10, 10, 2
// icosphere                float3
static const uint indexTypes[OT_COUNT]      = { 0, 1, 0, 0, 2, 0, 0, 1 };
static const uint vertexStrides[OT_COUNT]   = { 24, 28, 28, 32, 32, 28, 20, 32 };
static const uint byteOffsets[OT_COUNT]     = { 18, 24, 24, 20, 20, 24, 16, 12 };

[numthreads(WorkgroupSize, WorkgroupSize, 1)]
void main(uint32_t3 threadID : SV_DispatchThreadID)
{
    uint2 coords = threadID.xy;
    uint2 resolution;
    outImage.GetDimensions(resolution.x, resolution.y);
    float2 texCoords = float2(float(coords.x) / resolution.x, 1.0 - float(coords.y) / resolution.y);

    if (any(coords >= resolution))
        return;

    float4 color = float4(0, 0, 0, 1);

    float4 NDC = float4(texCoords * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    float3 targetPos;
    {
        float4 tmp = mul(params.invMVP, NDC);
        targetPos = tmp.xyz / tmp.w;
        NDC.z = 1.0;
    }

    RayDesc ray;
    ray.TMin = 0.01;
    ray.TMax = 1000.0;
    ray.Origin = params.camPos;
    ray.Direction = normalize(targetPos - params.camPos);

    RayQuery<RAY_FLAG_FORCE_OPAQUE> query;
    query.TraceRayInline(topLevelAS, 0, 0xFF, ray);

    while (query.Proceed()) {}

    if (query.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
    {
        const int instID = query.CommittedInstanceIndex();
        const int primID = query.CommittedPrimitiveIndex();

        uint idxOffset = primID * 3;

        const uint indexType = indexTypes[instID];      // matches obj.indexType
        const uint vertexStride = vertexStrides[instID];
        const uint byteOffset = byteOffsets[instID];

        uint i0, i1, i2;
        switch (indexType)
        {
            case 0: // EIT_16BIT
            {
                i0 = uint32_t(vk::RawBufferLoad<uint16_t>(pc.geom[instID].indexBufferAddress + (idxOffset + 0) * sizeof(uint16_t), 2u));
                i1 = uint32_t(vk::RawBufferLoad<uint16_t>(pc.geom[instID].indexBufferAddress + (idxOffset + 1) * sizeof(uint16_t), 2u));
                i2 = uint32_t(vk::RawBufferLoad<uint16_t>(pc.geom[instID].indexBufferAddress + (idxOffset + 2) * sizeof(uint16_t), 2u));
            }
            break;
            case 1: // EIT_32BIT
            {
                i0 = vk::RawBufferLoad<uint32_t>(pc.geom[instID].indexBufferAddress + (idxOffset + 0) * sizeof(uint32_t));
                i1 = vk::RawBufferLoad<uint32_t>(pc.geom[instID].indexBufferAddress + (idxOffset + 1) * sizeof(uint32_t));
                i2 = vk::RawBufferLoad<uint32_t>(pc.geom[instID].indexBufferAddress + (idxOffset + 2) * sizeof(uint32_t));
            }
            break;
            default:    // EIT_NONE
            {
                i0 = idxOffset;
                i1 = idxOffset + 1;
                i2 = idxOffset + 2;
            }
        }

        float3 n0, n1, n2;
        switch (instID)
        {
            case OT_CUBE:
            {
                uint32_t v0 = vk::RawBufferLoad<uint32_t>(pc.geom[instID].vertexBufferAddress + i0 * vertexStride + byteOffset, 2u);
                uint32_t v1 = vk::RawBufferLoad<uint32_t>(pc.geom[instID].vertexBufferAddress + i1 * vertexStride + byteOffset, 2u);
                uint32_t v2 = vk::RawBufferLoad<uint32_t>(pc.geom[instID].vertexBufferAddress + i2 * vertexStride + byteOffset, 2u);

                n0 = normalize(nbl::hlsl::spirv::unpackSnorm4x8(v0).xyz);
                n1 = normalize(nbl::hlsl::spirv::unpackSnorm4x8(v1).xyz);
                n2 = normalize(nbl::hlsl::spirv::unpackSnorm4x8(v2).xyz);
            }
            break;
            case OT_SPHERE:
            case OT_CYLINDER:
            case OT_ARROW:
            case OT_CONE:
            {
                uint32_t v0 = vk::RawBufferLoad<uint32_t>(pc.geom[instID].vertexBufferAddress + i0 * vertexStride + byteOffset);
                uint32_t v1 = vk::RawBufferLoad<uint32_t>(pc.geom[instID].vertexBufferAddress + i1 * vertexStride + byteOffset);
                uint32_t v2 = vk::RawBufferLoad<uint32_t>(pc.geom[instID].vertexBufferAddress + i2 * vertexStride + byteOffset);

                n0 = normalize(unpackNormals3x10(v0));
                n1 = normalize(unpackNormals3x10(v1));
                n2 = normalize(unpackNormals3x10(v2));
            }
            break;
            case OT_RECTANGLE:
            case OT_DISK:
            case OT_ICOSPHERE:
            default:
            {
                n0 = normalize(vk::RawBufferLoad<float3>(pc.geom[instID].vertexBufferAddress + i0 * vertexStride + byteOffset));
                n1 = normalize(vk::RawBufferLoad<float3>(pc.geom[instID].vertexBufferAddress + i1 * vertexStride + byteOffset));
                n2 = normalize(vk::RawBufferLoad<float3>(pc.geom[instID].vertexBufferAddress + i2 * vertexStride + byteOffset));
            }
        }

        float3 barycentrics = float3(0.0, query.CommittedTriangleBarycentrics());
        barycentrics.x = 1.0 - barycentrics.y - barycentrics.z;

        float3 normalInterp = barycentrics.x * n0 + barycentrics.y * n1 + barycentrics.z * n2;
        normalInterp = normalize(normalInterp) * 0.5 + 0.5;
        color = float4(normalInterp, 1.0);
    }

    outImage[coords] = color;
}
