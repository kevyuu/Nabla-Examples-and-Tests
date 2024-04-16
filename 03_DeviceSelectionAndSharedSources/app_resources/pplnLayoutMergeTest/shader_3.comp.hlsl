#pragma wave shader_stage(compute)
#include "common.hlsl"

[[vk::binding(0,0)]] RWStructuredBuffer<SomeType> asdf;
[[vk::binding(1,0)]] RWByteAddressBuffer output[3];

[numthreads(WorkgroupSize, 1, 1)]
void main(uint32_t3 ID : SV_DispatchThreadID)
{
    const uint32_t index = ID.x;
	const uint32_t byteOffset = sizeof(uint32_t)*index;

    output[0].Store<uint32_t>(byteOffset, asdf[index].a);
}