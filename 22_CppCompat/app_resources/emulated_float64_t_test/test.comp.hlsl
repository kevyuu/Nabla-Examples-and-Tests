//// Copyright (C) 2023-2024 - DevSH Graphics Programming Sp. z O.O.
//// This file is part of the "Nabla Engine".
//// For conditions of distribution and use, see copyright notice in nabla.h
#pragma shader_stage(compute)

#include "app_resources/emulated_float64_t_test/common.hlsl"
#include <nbl/builtin/hlsl/bit.hlsl>

[[vk::binding(0, 0)]] RWStructuredBuffer<TestValues> testValuesOutput;

[numthreads(WORKGROUP_SIZE, 1, 1)]
void main(uint3 invocationID : SV_DispatchThreadID)
{
    //nbl::hlsl::bit_cast<uint64_t, uint32_t>(2u);

    const nbl::hlsl::emulated_float64_t a = nbl::hlsl::emulated_float64_t::create(float64_t(20.0));
    const nbl::hlsl::emulated_float64_t b = nbl::hlsl::emulated_float64_t::create(float64_t(10.0));

    // "constructors"
    //testValuesOutput[0].intCreateVal = nbl::hlsl::emulated::emulated_float64_t::create(24);
    //testValuesOutput[0].uintCreateVal = nbl::hlsl::emulated::emulated_float64_t::create(24u);
    //testValuesOutput[0].uint64CreateVal = nbl::hlsl::emulated::emulated_float64_t::create(24ull);
    //testValuesOutput[0].floatCreateVal = nbl::hlsl::emulated::emulated_float64_t::create(1.2f);
    //testValuesOutput[0].doubleCreateVal = nbl::hlsl::emulated::emulated_float64_t::create(1.2);
    //nbl::hlsl::emulated_float64_t::create(min16int(2));

    // arithmetic operators
    testValuesOutput[0].additionVal = (a+b).data;
    testValuesOutput[0].substractionVal = (a-b).data;
    testValuesOutput[0].multiplicationVal = (a*b).data;
    testValuesOutput[0].divisionVal = (a/b).data;

    // relational operators
    testValuesOutput[0].lessOrEqualVal = (a<=b);
    testValuesOutput[0].greaterOrEqualVal = (a>=b);
    testValuesOutput[0].equalVal = (a==b);
    testValuesOutput[0].notEqualVal = (a!=b);
    testValuesOutput[0].lessVal = (a<b);
    testValuesOutput[0].greaterVal = (a>b);
}
