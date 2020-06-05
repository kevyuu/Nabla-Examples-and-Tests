#define _IRR_STATIC_LIB_
#include <iostream>
#include <cstdio>
#include <irrlicht.h>

#include "CommandLineHandler.hpp"

#include "../ext/ToneMapper/CToneMapper.h"
#include "../../ext/OptiX/Manager.h"

#include "CommonPushConstants.h"

using namespace irr;
using namespace asset;
using namespace video;

enum E_IMAGE_INPUT : uint32_t
{
	EII_COLOR,
	EII_ALBEDO,
	EII_NORMAL,
	EII_COUNT
};
struct ImageToDenoise
{
	uint32_t width = 0u, height = 0u;
	uint32_t colorTexelSize = 0u;
	E_IMAGE_INPUT denoiserType = EII_COUNT;
	core::smart_refctd_ptr<asset::ICPUImage> image[EII_COUNT] = { nullptr,nullptr,nullptr };
};
struct DenoiserToUse
{
	core::smart_refctd_ptr<ext::OptiX::IDenoiser> m_denoiser;
	size_t stateOffset = 0u;
	size_t stateSize = 0u;
	size_t scratchSize = 0u;
};

int error_code = 0;
bool check_error(bool cond, const char* message)
{
	error_code++;
	if (cond)
		os::Printer::log(message, ELL_ERROR);
	return cond;
}
/*
	if (check_error(,"!"))
		return error_code;
*/

int main(int argc, char* argv[])
{
	irr::SIrrlichtCreationParameters params;
	params.Bits = 24;
	params.ZBufferBits = 24;
	params.DriverType = video::EDT_OPENGL;
	params.WindowSize = core::dimension2d<uint32_t>(1280, 720);
	params.Fullscreen = false;
	params.Vsync = true;
	params.Doublebuffer = true;
	params.Stencilbuffer = false;
	params.StreamingDownloadBufferSize = 256*1024*1024; // change in Vulkan fo
	auto device = createDeviceEx(params);

	if (check_error(!device,"Could not create Irrlicht Device!"))
		return error_code;

	auto driver = device->getVideoDriver();
	auto smgr = device->getSceneManager();
	auto am = device->getAssetManager();

	auto compiler = am->getGLSLCompiler();
	auto filesystem = device->getFileSystem();

	auto getArgvFetchedList = [&]()
	{
		core::vector<std::string> arguments;
		arguments.reserve(PROPER_CMD_ARGUMENTS_AMOUNT);
		arguments.emplace_back(argv[0]);
		if (argc>1)
		for (auto i = 1ul; i < argc; ++i)
			arguments.emplace_back(argv[i]);
		else // use default for example
		{
			arguments.emplace_back("-batch");
			arguments.emplace_back("../exampleInputArguments.txt");
		}

		return arguments;
	};

	auto cmdHandler = CommandLineHandler(getArgvFetchedList(), am);

	if (check_error(!cmdHandler.getStatus(),"Could not parse input commands!"))
		return error_code;

	auto m_optixManager = ext::OptiX::Manager::create(driver,device->getFileSystem());
	if (check_error(!m_optixManager, "Could not initialize CUDA or OptiX!"))
		return error_code;
	auto m_cudaStream = m_optixManager->getDeviceStream(0);
	if (check_error(!m_cudaStream, "Could not obtain CUDA stream!"))
		return error_code;
	auto m_optixContext = m_optixManager->createContext(0);
	if (check_error(!m_optixContext, "Could not create Optix Context!"))
		return error_code;

	constexpr auto forcedOptiXFormat = OPTIX_PIXEL_FORMAT_HALF3; // TODO: make more denoisers with formats
	E_FORMAT irrFmtRequired = EF_UNKNOWN;
	switch (forcedOptiXFormat)
	{
		case OPTIX_PIXEL_FORMAT_UCHAR3:
			irrFmtRequired = EF_R8G8B8_SRGB;
			break;
		case OPTIX_PIXEL_FORMAT_UCHAR4:
			irrFmtRequired = EF_R8G8B8A8_SRGB;
			break;
		case OPTIX_PIXEL_FORMAT_HALF3:
			irrFmtRequired = EF_R16G16B16_SFLOAT;
			break;
		case OPTIX_PIXEL_FORMAT_HALF4:
			irrFmtRequired = EF_R16G16B16A16_SFLOAT;
			break;
		case OPTIX_PIXEL_FORMAT_FLOAT3:
			irrFmtRequired = EF_R32G32B32_SFLOAT;
			break;
		case OPTIX_PIXEL_FORMAT_FLOAT4:
			irrFmtRequired = EF_R32G32B32A32_SFLOAT;
			break;
	}
	constexpr auto forcedOptiXFormatPixelStride = 6u;
	DenoiserToUse denoisers[EII_COUNT];
	{
		OptixDenoiserOptions opts = { OPTIX_DENOISER_INPUT_RGB,forcedOptiXFormat };
		denoisers[EII_COLOR].m_denoiser = m_optixContext->createDenoiser(&opts);
		if (check_error(!denoisers[EII_COLOR].m_denoiser, "Could not create Optix Color Denoiser!"))
			return error_code;
		opts.inputKind = OPTIX_DENOISER_INPUT_RGB_ALBEDO;
		denoisers[EII_ALBEDO].m_denoiser = m_optixContext->createDenoiser(&opts);
		if (check_error(!denoisers[EII_ALBEDO].m_denoiser, "Could not create Optix Color-Albedo Denoiser!"))
			return error_code;
		opts.inputKind = OPTIX_DENOISER_INPUT_RGB_ALBEDO_NORMAL;
		denoisers[EII_NORMAL].m_denoiser = m_optixContext->createDenoiser(&opts);
		if (check_error(!denoisers[EII_NORMAL].m_denoiser, "Could not create Optix Color-Albedo-Normal Denoiser!"))
			return error_code;
	}


	constexpr uint32_t kComputeWGSize = 256u;
	

	using LumaMeterClass = ext::LumaMeter::CLumaMeter;
	using ToneMapperClass = ext::ToneMapper::CToneMapper;
	constexpr bool usingLumaMeter = true;
	constexpr auto MeterMode = LumaMeterClass::EMM_MEDIAN;
	const auto HistogramBufferSize = LumaMeterClass::getOutputBufferSize(MeterMode);
	constexpr float lowerPercentile = 0.45f;
	constexpr float upperPercentile = 0.55f;
	constexpr auto TMO = ToneMapperClass::EO_ACES;

	constexpr auto SharedDescriptorSetDescCount = 4u;
	core::smart_refctd_ptr<IGPUDescriptorSetLayout> sharedDescriptorSetLayout;
	core::smart_refctd_ptr<IGPUPipelineLayout> sharedPipelineLayout;
	core::smart_refctd_ptr<IGPUComputePipeline> deinterleavePipeline,intensityPipeline,interleavePipeline;
	{
		auto deinterleaveShader = driver->createGPUShader(core::make_smart_refctd_ptr<ICPUShader>(R"===(
#version 450 core
#extension GL_EXT_shader_16bit_storage : require
#define _IRR_GLSL_EXT_LUMA_METER_FIRST_PASS_DEFINED_
#include "../ShaderCommon.glsl"
layout(binding = 0, std430) restrict readonly buffer ImageInputBuffer
{
	f16vec4 inBuffer[];
};
layout(binding = 1, std430) restrict writeonly buffer ImageOutputBuffer
{
	float16_t outBuffer[];
};
vec3 fetchData(in uvec3 texCoord)
{
	vec3 data = vec4(inBuffer[pc.data.inImageTexelOffset[texCoord.z]+texCoord.y*pc.data.inImageTexelPitch[texCoord.z]+texCoord.x]).xyz;
	bool invalid = any(isnan(data))||any(isinf(abs(data)));
	if (texCoord.z==EII_ALBEDO)
		data = invalid ? vec3(1.0):data;
	else if (texCoord.z==EII_NORMAL)
	{
		data = invalid||length(data)<0.000000001 ? vec3(0.0,0.0,1.0):normalize(pc.data.normalMatrix*data);
	}
	return data;
}
void main()
{
	bool colorLayer = gl_GlobalInvocationID.z==EII_COLOR;
	if (colorLayer)
	{
		ivec3 tc = ivec3(gl_GlobalInvocationID);
		const vec3 lumaCoeffs = transpose(_IRR_GLSL_EXT_LUMA_METER_XYZ_CONVERSION_MATRIX_DEFINED_)[1];
		for (int i=-pc.data.medianFilterRadius; i<=pc.data.medianFilterRadius; i++)
		for (int j=-pc.data.medianFilterRadius; j<=pc.data.medianFilterRadius; j++)
		{
			vec4 data;
			data.rgb = fetchData(clampCoords(ivec3(j,i,0)+tc));
			data.a = dot(data.rgb,lumaCoeffs);
			medianWindow[MAX_MEDIAN_FILTER_DIAMETER*(i+MAX_MEDIAN_FILTER_RADIUS)+(j+MAX_MEDIAN_FILTER_RADIUS)] = data;
		}
		#define SWAP(X,Y) ltswap(medianWindow[X],medianWindow[Y])
		if (pc.data.medianFilterRadius==1)
		{ // optimized sorting network for median finding
			SWAP(6, 7);
			SWAP(11, 12);
			SWAP(16, 17);
			SWAP(7, 8);
			SWAP(12, 13);
			SWAP(17, 18);
			SWAP(6, 7);
			SWAP(11, 12);
			SWAP(16, 17);
			SWAP(6, 11);
			SWAP(11, 16);
			SWAP(7, 12);
			SWAP(12, 17);
			SWAP(7, 12);
			SWAP(8, 13);
			SWAP(13, 18);
			SWAP(8, 13);
			SWAP(8, 16);
			SWAP(12, 16);
			SWAP(8, 12);
		}
		else if (pc.data.medianFilterRadius==2)
		{
			SWAP(1, 2);
			SWAP(0, 2);
			SWAP(0, 1);
			SWAP(4, 5);
			SWAP(3, 5);
			SWAP(3, 4);
			SWAP(0, 3);
			SWAP(1, 4);
			SWAP(2, 5);
			SWAP(2, 4);
			SWAP(1, 3);
			SWAP(2, 3);
			SWAP(7, 8);
			SWAP(6, 8);
			SWAP(6, 7);
			SWAP(10, 11);
			SWAP(9, 11);
			SWAP(9, 10);
			SWAP(6, 9);
			SWAP(7, 10);
			SWAP(8, 11);
			SWAP(8, 10);
			SWAP(7, 9);
			SWAP(8, 9);
			SWAP(0, 6);
			SWAP(1, 7);
			SWAP(2, 8);
			SWAP(2, 7);
			SWAP(1, 6);
			SWAP(2, 6);
			SWAP(3, 9);
			SWAP(4, 10);
			SWAP(5, 11);
			SWAP(5, 10);
			SWAP(4, 9);
			SWAP(5, 9);
			SWAP(3, 6);
			SWAP(4, 7);
			SWAP(5, 8);
			SWAP(5, 7);
			SWAP(4, 6);
			SWAP(5, 6);
			SWAP(13, 14);
			SWAP(12, 14);
			SWAP(12, 13);
			SWAP(16, 17);
			SWAP(15, 17);
			SWAP(15, 16);
			SWAP(12, 15);
			SWAP(13, 16);
			SWAP(14, 17);
			SWAP(14, 16);
			SWAP(13, 15);
			SWAP(14, 15);
			SWAP(19, 20);
			SWAP(18, 20);
			SWAP(18, 19);
			SWAP(21, 22);
			SWAP(23, 24);
			SWAP(21, 23);
			SWAP(22, 24);
			SWAP(22, 23);
			SWAP(18, 22);
			SWAP(18, 21);
			SWAP(19, 23);
			SWAP(20, 24);
			SWAP(20, 23);
			SWAP(19, 21);
			SWAP(20, 22);
			SWAP(20, 21);
			SWAP(12, 19);
			SWAP(12, 18);
			SWAP(13, 20);
			SWAP(14, 21);
			SWAP(14, 20);
			SWAP(13, 18);
			SWAP(14, 19);
			SWAP(14, 18);
			SWAP(15, 22);
			SWAP(16, 23);
			SWAP(17, 24);
			SWAP(17, 23);
			SWAP(16, 22);
			SWAP(17, 22);
			SWAP(15, 19);
			SWAP(15, 18);
			SWAP(16, 20);
			SWAP(17, 21);
			SWAP(17, 20);
			SWAP(16, 18);
			SWAP(17, 19);
			SWAP(17, 18);
			SWAP(0, 13);
			SWAP(0, 12);
			SWAP(1, 14);
			SWAP(2, 15);
			SWAP(2, 14);
			SWAP(1, 12);
			SWAP(2, 13);
			SWAP(2, 12);
			SWAP(3, 16);
			SWAP(4, 17);
			SWAP(5, 18);
			SWAP(5, 17);
			SWAP(4, 16);
			SWAP(5, 16);
			SWAP(3, 13);
			SWAP(3, 12);
			SWAP(4, 14);
			SWAP(5, 15);
			SWAP(5, 14);
			SWAP(4, 12);
			SWAP(5, 13);
			SWAP(5, 12);
			SWAP(6, 19);
			SWAP(7, 20);
			SWAP(8, 21);
			SWAP(8, 20);
			SWAP(7, 19);
			SWAP(8, 19);
			SWAP(9, 22);
			SWAP(10, 23);
			SWAP(11, 24);
			SWAP(11, 23);
			SWAP(10, 22);
			SWAP(11, 22);
			SWAP(9, 19);
			SWAP(10, 20);
			SWAP(11, 21);
			SWAP(11, 20);
			SWAP(10, 19);
			SWAP(11, 19);
			SWAP(6, 13);
			SWAP(6, 12);
			SWAP(7, 14);
			SWAP(8, 15);
			SWAP(8, 14);
			SWAP(7, 12);
			SWAP(8, 13);
			SWAP(8, 12);
			SWAP(9, 16);
			SWAP(10, 17);
			SWAP(11, 18);
			SWAP(11, 17);
			SWAP(10, 16);
			SWAP(11, 16);
			SWAP(9, 13);
			SWAP(9, 12);
			SWAP(10, 14);
			SWAP(11, 15);
			SWAP(11, 14);
			SWAP(10, 12);
			SWAP(11, 13);
			SWAP(11, 12);
		}
		#undef SWAP
		irr_glsl_ext_LumaMeter(colorLayer && gl_GlobalInvocationID.x<pc.data.imageWidth);
		barrier(); // no barrier because we were just reading from shared not writing since the last memory barrier
		medianWindow[medianIndex].rgb *= exp2(pc.data.denoiserExposureBias);
		repackBuffer[gl_LocalInvocationIndex*SHARED_CHANNELS+0u] = floatBitsToUint(medianWindow[medianIndex].r);
		repackBuffer[gl_LocalInvocationIndex*SHARED_CHANNELS+1u] = floatBitsToUint(medianWindow[medianIndex].g);
		repackBuffer[gl_LocalInvocationIndex*SHARED_CHANNELS+2u] = floatBitsToUint(medianWindow[medianIndex].b);
	}
	else
	{
		vec3 data = fetchData(gl_GlobalInvocationID);
		repackBuffer[gl_LocalInvocationIndex*SHARED_CHANNELS+0u] = floatBitsToUint(data[0u]);
		repackBuffer[gl_LocalInvocationIndex*SHARED_CHANNELS+1u] = floatBitsToUint(data[1u]);
		repackBuffer[gl_LocalInvocationIndex*SHARED_CHANNELS+2u] = floatBitsToUint(data[2u]);
	}
	barrier();
	memoryBarrierShared();
	const uint outImagePitch = pc.data.imageWidth*SHARED_CHANNELS;
	uint rowOffset = pc.data.outImageOffset[gl_GlobalInvocationID.z]+gl_GlobalInvocationID.y*outImagePitch;
	uint lineOffset = gl_WorkGroupID.x*COMPUTE_WG_SIZE*SHARED_CHANNELS+gl_LocalInvocationIndex;
	if (lineOffset<outImagePitch)
		outBuffer[rowOffset+lineOffset] = float16_t(uintBitsToFloat(repackBuffer[gl_LocalInvocationIndex+COMPUTE_WG_SIZE*0u]));
	lineOffset += COMPUTE_WG_SIZE;
	if (lineOffset<outImagePitch)
		outBuffer[rowOffset+lineOffset] = float16_t(uintBitsToFloat(repackBuffer[gl_LocalInvocationIndex+COMPUTE_WG_SIZE*1u]));
	lineOffset += COMPUTE_WG_SIZE;
	if (lineOffset<outImagePitch)
		outBuffer[rowOffset+lineOffset] = float16_t(uintBitsToFloat(repackBuffer[gl_LocalInvocationIndex+COMPUTE_WG_SIZE*2u]));
}
		)==="));
		auto intensityShader = driver->createGPUShader(core::make_smart_refctd_ptr<ICPUShader>(R"===(
#version 450 core
#extension GL_EXT_shader_16bit_storage : require
#include "../ShaderCommon.glsl"
layout(set=_IRR_GLSL_EXT_LUMA_METER_OUTPUT_SET_DEFINED_, binding=_IRR_GLSL_EXT_LUMA_METER_OUTPUT_BINDING_DEFINED_) restrict readonly buffer LumaMeterOutputBuffer
{
	irr_glsl_ext_LumaMeter_output_t lumaParams[];
};
layout(binding = 3, std430) restrict writeonly buffer IntensityBuffer
{
	float intensity[];
};
irr_glsl_ext_LumaMeter_output_SPIRV_CROSS_is_dumb_t irr_glsl_ext_ToneMapper_getLumaMeterOutput()
{
	irr_glsl_ext_LumaMeter_output_SPIRV_CROSS_is_dumb_t retval;
	retval = lumaParams[0].packedHistogram[gl_LocalInvocationIndex];
	for (int i=1; i<_IRR_GLSL_EXT_LUMA_METER_BIN_GLOBAL_REPLICATION; i++)
		retval += lumaParams[0].packedHistogram[gl_LocalInvocationIndex+i*_IRR_GLSL_EXT_LUMA_METER_BIN_COUNT];
	return retval;
}
void main()
{
	irr_glsl_ext_LumaMeter_PassInfo_t lumaPassInfo;
	lumaPassInfo.percentileRange[0] = pc.data.percentileRange[0];
	lumaPassInfo.percentileRange[1] = pc.data.percentileRange[1];
	float measuredLumaLog2 = irr_glsl_ext_LumaMeter_getMeasuredLumaLog2(irr_glsl_ext_ToneMapper_getLumaMeterOutput(),lumaPassInfo);
	// write optix brightness
	if (all(equal(uvec3(0,0,0),gl_GlobalInvocationID)))
		intensity[pc.data.intensityBufferDWORDOffset] = irr_glsl_ext_LumaMeter_getOptiXIntensity(measuredLumaLog2+pc.data.denoiserExposureBias);
}
		)==="));
		auto interleaveShader = driver->createGPUShader(core::make_smart_refctd_ptr<ICPUShader>(R"===(
#version 450 core
#extension GL_EXT_shader_16bit_storage : require
#include "../ShaderCommon.glsl"
#include "irr/builtin/glsl/ext/ToneMapper/operators.glsl"
layout(binding = 0, std430) restrict readonly buffer ImageInputBuffer
{
	float16_t inBuffer[];
};
layout(binding = 1, std430) restrict writeonly buffer ImageOutputBuffer
{
	f16vec4 outBuffer[];
};
layout(binding = 3, std430) restrict readonly buffer IntensityBuffer
{
	float intensity[];
};
void main()
{
	uint wgOffset = pc.data.outImageOffset[EII_COLOR]+(gl_GlobalInvocationID.y*pc.data.imageWidth+gl_WorkGroupID.x*COMPUTE_WG_SIZE)*SHARED_CHANNELS;
	uint localOffset = gl_LocalInvocationIndex;
	repackBuffer[localOffset] = floatBitsToUint(float(inBuffer[wgOffset+localOffset]));
	localOffset += COMPUTE_WG_SIZE;
	repackBuffer[localOffset] = floatBitsToUint(float(inBuffer[wgOffset+localOffset]));
	localOffset += COMPUTE_WG_SIZE;
	repackBuffer[localOffset] = floatBitsToUint(float(inBuffer[wgOffset+localOffset]));
	barrier();
	memoryBarrierShared();
	bool alive = gl_GlobalInvocationID.x<pc.data.imageWidth;
	vec3 color = uintBitsToFloat(uvec3(repackBuffer[gl_LocalInvocationIndex*SHARED_CHANNELS+0u],repackBuffer[gl_LocalInvocationIndex*SHARED_CHANNELS+1u],repackBuffer[gl_LocalInvocationIndex*SHARED_CHANNELS+2u]));
	
	color = _IRR_GLSL_EXT_LUMA_METER_XYZ_CONVERSION_MATRIX_DEFINED_*color;
	color *= intensity[pc.data.intensityBufferDWORDOffset]; // *= 0.18/AvgLuma
	switch (pc.data.tonemappingOperator)
	{
		case _IRR_GLSL_EXT_TONE_MAPPER_REINHARD_OPERATOR:
		{
			irr_glsl_ext_ToneMapper_ReinhardParams_t tonemapParams;
			tonemapParams.keyAndManualLinearExposure = pc.data.tonemapperParams[0];
			tonemapParams.rcpWhite2 = pc.data.tonemapperParams[1];
			color = irr_glsl_ext_ToneMapper_Reinhard(tonemapParams,color);
			break;
		}
		case _IRR_GLSL_EXT_TONE_MAPPER_ACES_OPERATOR:
		{
			irr_glsl_ext_ToneMapper_ACESParams_t tonemapParams;
			tonemapParams.gamma = pc.data.tonemapperParams[0];
			tonemapParams.exposure = pc.data.tonemapperParams[1];
			color = irr_glsl_ext_ToneMapper_ACES(tonemapParams,color);
			break;
		}
		default:
			color *= pc.data.tonemapperParams[0];
			break;
	}
	color = irr_glsl_XYZtosRGB*color;
	// TODO: compute DFFT of the image in the X-axis
	uint dataOffset = pc.data.inImageTexelOffset[EII_COLOR]+gl_GlobalInvocationID.y*pc.data.inImageTexelPitch[EII_COLOR]+gl_GlobalInvocationID.x;
	if (alive)
		outBuffer[dataOffset] = f16vec4(vec4(color,1.0));
}
		)==="));
		struct SpecializationConstants
		{
			uint32_t workgroupSize = kComputeWGSize;
			uint32_t enumEII_COLOR = EII_COLOR;
			uint32_t enumEII_ALBEDO = EII_ALBEDO;
			uint32_t enumEII_NORMAL = EII_NORMAL;
		} specData;
		auto specConstantBuffer = core::make_smart_refctd_ptr<CCustomAllocatorCPUBuffer<core::null_allocator<uint8_t> > >(sizeof(SpecializationConstants), &specData, core::adopt_memory);
		IGPUSpecializedShader::SInfo specInfo = {	core::make_refctd_dynamic_array<core::smart_refctd_dynamic_array<IGPUSpecializedShader::SInfo::SMapEntry> >
													(
														std::initializer_list<IGPUSpecializedShader::SInfo::SMapEntry>
														{
															{0u,offsetof(SpecializationConstants,workgroupSize),sizeof(SpecializationConstants::workgroupSize)},
															{1u,offsetof(SpecializationConstants,enumEII_COLOR),sizeof(SpecializationConstants::enumEII_COLOR)},
															{2u,offsetof(SpecializationConstants,enumEII_ALBEDO),sizeof(SpecializationConstants::enumEII_ALBEDO)},
															{3u,offsetof(SpecializationConstants,enumEII_NORMAL),sizeof(SpecializationConstants::enumEII_NORMAL)}
														}
													),
													core::smart_refctd_ptr(specConstantBuffer),"main",ISpecializedShader::ESS_COMPUTE
												};
		auto deinterleaveSpecializedShader = driver->createGPUSpecializedShader(deinterleaveShader.get(),specInfo);
		auto intensitySpecializedShader = driver->createGPUSpecializedShader(intensityShader.get(),specInfo);
		auto interleaveSpecializedShader = driver->createGPUSpecializedShader(interleaveShader.get(),specInfo);
		
		IGPUDescriptorSetLayout::SBinding binding[SharedDescriptorSetDescCount] = {
			{0u,EDT_STORAGE_BUFFER,1u,IGPUSpecializedShader::ESS_COMPUTE,nullptr},
			{1u,EDT_STORAGE_BUFFER,1u,IGPUSpecializedShader::ESS_COMPUTE,nullptr},
			{2u,EDT_STORAGE_BUFFER,1u,IGPUSpecializedShader::ESS_COMPUTE,nullptr},
			{3u,EDT_STORAGE_BUFFER,1u,IGPUSpecializedShader::ESS_COMPUTE,nullptr}
		};
		sharedDescriptorSetLayout = driver->createGPUDescriptorSetLayout(binding,binding+SharedDescriptorSetDescCount);
		SPushConstantRange pcRange[1] = {IGPUSpecializedShader::ESS_COMPUTE,0u,sizeof(CommonPushConstants)};
		sharedPipelineLayout = driver->createGPUPipelineLayout(pcRange,pcRange+sizeof(pcRange)/sizeof(SPushConstantRange),core::smart_refctd_ptr(sharedDescriptorSetLayout));

		deinterleavePipeline = driver->createGPUComputePipeline(nullptr,core::smart_refctd_ptr(sharedPipelineLayout),std::move(deinterleaveSpecializedShader));
		intensityPipeline = driver->createGPUComputePipeline(nullptr,core::smart_refctd_ptr(sharedPipelineLayout),std::move(intensitySpecializedShader));
		interleavePipeline = driver->createGPUComputePipeline(nullptr,core::smart_refctd_ptr(sharedPipelineLayout),std::move(interleaveSpecializedShader));
	}

	const auto inputFilesAmount = cmdHandler.getInputFilesAmount();
	const auto& colorFileNameBundle = cmdHandler.getColorFileNameBundle();
	const auto& albedoFileNameBundle = cmdHandler.getAlbedoFileNameBundle();
	const auto& normalFileNameBundle = cmdHandler.getNormalFileNameBundle();
	const auto& colorChannelNameBundle = cmdHandler.getColorChannelNameBundle();
	const auto& albedoChannelNameBundle = cmdHandler.getAlbedoChannelNameBundle();
	const auto& normalChannelNameBundle = cmdHandler.getNormalChannelNameBundle();
	const auto& cameraTransformBundle = cmdHandler.getCameraTransformBundle();
	const auto& medianFilterRadiusBundle = cmdHandler.getMedianFilterRadiusBundle();
	const auto& denoiserExposureBiasBundle = cmdHandler.getExposureBiasBundle();
	const auto& denoiserBlendFactorBundle = cmdHandler.getDenoiserBlendFactorBundle();
	const auto& bloomFovBundle = cmdHandler.getBloomFovBundle();
	const auto& tonemapperBundle = cmdHandler.getTonemapperBundle();
	const auto& outputFileBundle = cmdHandler.getOutputFileBundle();
	const auto& psdFileBunde = cmdHandler.getBloomPsfBundle();

	auto makeImageIDString = [](uint32_t i, const core::vector<std::optional<std::string>>& fileNameBundle = {})
	{
		std::string imageIDString("Image Input #");
		imageIDString += std::to_string(i);

		if (!fileNameBundle.empty() && fileNameBundle[i].has_value())
		{
			imageIDString += " called \"";
			imageIDString += fileNameBundle[i].value();
		}

		return imageIDString;
	};

	core::vector<ImageToDenoise> images(inputFilesAmount);
	// load images
	uint32_t maxResolution[EII_COUNT][2] = { 0 };
	{
		asset::IAssetLoader::SAssetLoadParams lp(0ull,nullptr);
		for (size_t i=0; i < inputFilesAmount; i++)
		{
			const auto imageIDString = makeImageIDString(i, colorFileNameBundle);

			auto color_image_bundle = am->getAsset(colorFileNameBundle[i].value(), lp); decltype(color_image_bundle) albedo_image_bundle, normal_image_bundle;
			if (color_image_bundle.isEmpty())
			{
				os::Printer::log("ERROR (" + std::to_string(__LINE__) + " line): Could not load the image from file: " + imageIDString + "!", ELL_ERROR);
				continue;
			}

			albedo_image_bundle = albedoFileNameBundle[i].has_value() ? am->getAsset(albedoFileNameBundle[i].value(), lp) : decltype(albedo_image_bundle)();
			normal_image_bundle = normalFileNameBundle[i].has_value() ? am->getAsset(normalFileNameBundle[i].value(), lp) : decltype(normal_image_bundle)();

			auto& outParam = images[i];

			auto getImageAssetGivenChannelName = [](asset::SAssetBundle& assetBundle, const std::optional<std::string>& channelName) -> core::smart_refctd_ptr<ICPUImage>
			{
				if (assetBundle.isEmpty())
					return nullptr;

				// calculate a score for how much each channel name matches the requested
				size_t firstChannelNameOccurence = std::string::npos;
				uint32_t pickedChannel = 0u;
				auto contents = assetBundle.getContents();
				if (channelName.has_value())
				for (auto it=contents.first; it!=contents.second; it++)
				{
					auto asset = *it;
					assert(asset);

					auto metadata = asset->getMetadata();
					auto exrmeta = static_cast<COpenEXRImageMetadata*>(metadata);
					if (strcmp(metadata->getLoaderName(),COpenEXRImageMetadata::LoaderName)!=0)
						continue;
					else
					{
						const auto& assetMetaChannelName = exrmeta->getName();
						auto found = assetMetaChannelName.find(channelName.value());
						if (found>=firstChannelNameOccurence)
							continue;
						firstChannelNameOccurence = found;
						pickedChannel = std::distance(contents.first, it);
					}
				}

				return asset::IAsset::castDown<ICPUImage>(contents.first[pickedChannel]);
			};

			auto& color = getImageAssetGivenChannelName(color_image_bundle,colorChannelNameBundle[i]);
			decltype(color)& albedo = getImageAssetGivenChannelName(albedo_image_bundle,albedoChannelNameBundle[i]);
			decltype(color)& normal = getImageAssetGivenChannelName(normal_image_bundle,normalChannelNameBundle[i]);

			auto convertImageToRequiredFmt = [&](core::smart_refctd_ptr<ICPUImage> image)
			{
				using CONVERSION_FILTER = CConvertFormatImageFilter<>;

				core::smart_refctd_ptr<ICPUImage> newConvertedImage;
				{
					auto referenceImageParams = image->getCreationParameters();
					auto referenceBuffer = image->getBuffer();
					auto referenceRegions = image->getRegions();
					auto referenceRegion = referenceRegions.begin();
					const auto newTexelOrBlockByteSize = asset::getTexelOrBlockBytesize(irrFmtRequired);

					auto newImageParams = referenceImageParams;
					auto newCpuBuffer = core::make_smart_refctd_ptr<ICPUBuffer>(referenceRegion->getExtent().width * referenceRegion->getExtent().height * referenceRegion->getExtent().depth * newTexelOrBlockByteSize);
					auto newRegions = core::make_refctd_dynamic_array<core::smart_refctd_dynamic_array<ICPUImage::SBufferCopy>>(referenceRegions.size());

					*newRegions->begin() = *referenceRegion;

					newImageParams.format = irrFmtRequired;
					newConvertedImage = ICPUImage::create(std::move(newImageParams));
					newConvertedImage->setBufferAndRegions(std::move(newCpuBuffer), newRegions);

					CONVERSION_FILTER convertFilter;
					CONVERSION_FILTER::state_type state;

					state.inImage = image.get();
					state.outImage = newConvertedImage.get();
					state.inOffset = { 0, 0, 0 };
					state.inBaseLayer = 0;
					state.outOffset = { 0, 0, 0 };
					state.outBaseLayer = 0;

					auto region = newConvertedImage->getRegions().begin();

					state.extent = region->getExtent();
					state.layerCount = region->imageSubresource.layerCount;
					state.inMipLevel = region->imageSubresource.mipLevel;
					state.outMipLevel = region->imageSubresource.mipLevel;

					if (!convertFilter.execute(&state))
						os::Printer::log("WARNING (" + std::to_string(__LINE__) + " line): Something went wrong while converting the image!", ELL_WARNING);
				}
				return newConvertedImage;
			};

			auto putImageIntoImageToDenoise = [&](core::smart_refctd_ptr<ICPUImage>&& queriedImage, E_IMAGE_INPUT defaultEII, const std::optional<std::string>& actualWantedChannel)
			{
				outParam.image[defaultEII] = nullptr;
				if (!queriedImage)
				{
					switch (defaultEII)
					{
						case EII_ALBEDO:
						{
							os::Printer::log("INFO (" + std::to_string(__LINE__) + " line): Running in mode without albedo channel!", ELL_INFORMATION);
						} break;
						case EII_NORMAL:
						{
							os::Printer::log("INFO (" + std::to_string(__LINE__) + " line): Running in mode without normal channel!", ELL_INFORMATION);
						} break;
					}
					return;
				}

				/*

				I'm not sure why we should do this, so I leave it for you @devsh to uncomment eventually
				Current Compute shader relies on having vec4 input and vec3 output

				if (handledDefaultImage->getCreationParameters().format != irrFmtRequired)
					handledDefaultImage = convertImageToRequiredFmt(handledDefaultImage);

				*/

				auto metadata = queriedImage->getMetadata();
				auto exrmeta = static_cast<const COpenEXRImageMetadata*>(metadata);
				if (strcmp(metadata->getLoaderName(),COpenEXRImageMetadata::LoaderName)!=0)
					os::Printer::log("WARNING (" + std::to_string(__LINE__) + "): "+ imageIDString+" is not an EXR file, so there are no multiple layers of channels.", ELL_WARNING);
				else if (!actualWantedChannel.has_value())
					os::Printer::log("WARNING (" + std::to_string(__LINE__) + "): User did not specify channel choice for "+ imageIDString+" using the default (first).", ELL_WARNING);
				else if (exrmeta->getName()!=actualWantedChannel.value())
				{
					os::Printer::log("WARNING (" + std::to_string(__LINE__) + "): Using best fit channel \""+exrmeta->getName()+"\" for requested \""+actualWantedChannel.value()+"\" out of "+ imageIDString+"!", ELL_WARNING);
				}
				outParam.image[defaultEII] = std::move(queriedImage);
			};

			putImageIntoImageToDenoise(std::move(color), EII_COLOR, colorChannelNameBundle[i]);
			putImageIntoImageToDenoise(std::move(albedo), EII_ALBEDO, albedoChannelNameBundle[i]);
			putImageIntoImageToDenoise(std::move(normal), EII_NORMAL, normalChannelNameBundle[i]);
		}
		// check inputs and set-up
		for (size_t i=0; i<inputFilesAmount; i++)
		{
			auto imageIDString = makeImageIDString(i);

			auto& outParam = images[i];
			{
				auto* colorImage = outParam.image[EII_COLOR].get();
				if (!colorImage)
				{
					os::Printer::log(imageIDString+"Could not find the Color Channel for denoising, image will be skipped!", ELL_ERROR);
					outParam = {};
					continue;
				}

				const auto& colorCreationParams = colorImage->getCreationParameters();
				const auto& extent = colorCreationParams.extent;
				// compute storage size and check if we can successfully upload
				{
					auto regions = colorImage->getRegions();
					assert(regions.begin()+1u==regions.end());

					const auto& region = regions.begin()[0];
					assert(region.bufferRowLength);
					outParam.colorTexelSize = asset::getTexelOrBlockBytesize(colorCreationParams.format);
					uint32_t bytesize = extent.height*region.bufferRowLength*outParam.colorTexelSize;
					if (bytesize>params.StreamingDownloadBufferSize)
					{
						os::Printer::log(imageIDString + "Image too large to download from GPU in one piece!", ELL_ERROR);
						outParam = {};
						continue;
					}
				}

				outParam.denoiserType = EII_COLOR;

				outParam.width = extent.width;
				outParam.height = extent.height;
			}

			auto& albedoImage = outParam.image[EII_ALBEDO];
			if (albedoImage)
			{
				auto extent = albedoImage->getCreationParameters().extent;
				if (extent.width!=outParam.width || extent.height!=outParam.height)
				{
					os::Printer::log(imageIDString + "Image extent of the Albedo Channel does not match the Color Channel, Albedo Channel will not be used!", ELL_ERROR);
					albedoImage = nullptr;
				}
				else
					outParam.denoiserType = EII_ALBEDO;
			}

			auto& normalImage = outParam.image[EII_NORMAL];
			if (normalImage)
			{
				auto extent = normalImage->getCreationParameters().extent;
				if (extent.width != outParam.width || extent.height != outParam.height)
				{
					os::Printer::log(imageIDString + "Image extent of the Normal Channel does not match the Color Channel, Normal Channel will not be used!", ELL_ERROR);
					normalImage = nullptr;
				}
				else if (!albedoImage)
				{
					os::Printer::log(imageIDString + "Invalid Albedo Channel for denoising, Normal Channel will not be used!", ELL_ERROR);
					normalImage = nullptr;
				}
				else
					outParam.denoiserType = EII_NORMAL;
			}

			maxResolution[outParam.denoiserType][0] = core::max(maxResolution[outParam.denoiserType][0],outParam.width);
			maxResolution[outParam.denoiserType][1] = core::max(maxResolution[outParam.denoiserType][1],outParam.height);
		}
	}

#define DENOISER_BUFFER_COUNT 4u //had to change to a define cause lambda was complaining about it not being a constant expression when capturing
	// keep all CUDA links in an array (less code to map/unmap
	cuda::CCUDAHandler::GraphicsAPIObjLink<video::IGPUBuffer> bufferLinks[DENOISER_BUFFER_COUNT];
	// set-up denoisers
	constexpr size_t IntensityValuesSize = sizeof(float);
	auto& intensityBuffer = bufferLinks[0];
	auto& denoiserState = bufferLinks[0];
	auto& scratchBuffer = bufferLinks[1];
	auto& temporaryPixelBuffer = bufferLinks[2];
	auto& imagePixelBuffer = bufferLinks[3];
	size_t denoiserStateBufferSize = 0ull;
	{
		size_t scratchBufferSize = 0ull;
		size_t pixelBufferSize = 0ull;
		for (uint32_t i=0u; i<EII_COUNT; i++)
		{
			auto& denoiser = denoisers[i].m_denoiser;
			if (maxResolution[i][0]==0u || maxResolution[i][1]==0u)
			{
				denoiser = nullptr;
				continue;
			}

			OptixDenoiserSizes m_denoiserMemReqs;
			if (denoiser->computeMemoryResources(&m_denoiserMemReqs, maxResolution[i])!=OPTIX_SUCCESS)
			{
				static const char* errorMsgs[EII_COUNT] = {	"Failed to compute Color-Denoiser Memory Requirements!",
															"Failed to compute Color-Albedo-Denoiser Memory Requirements!",
															"Failed to compute Color-Albedo-Normal-Denoiser Memory Requirements!"};
				os::Printer::log(errorMsgs[i],ELL_ERROR);
				denoiser = nullptr;
				continue;
			}

			denoisers[i].stateOffset = denoiserStateBufferSize;
			denoiserStateBufferSize += denoisers[i].stateSize = m_denoiserMemReqs.stateSizeInBytes;
			scratchBufferSize = core::max(core::max(scratchBufferSize,denoisers[i].scratchSize = m_denoiserMemReqs.recommendedScratchSizeInBytes),HistogramBufferSize);
			pixelBufferSize = core::max(pixelBufferSize,core::max(asset::getTexelOrBlockBytesize(EF_R32G32B32A32_SFLOAT),(i+1u)*forcedOptiXFormatPixelStride)*maxResolution[i][0]*maxResolution[i][1]);
		}
		std::string message = "Total VRAM consumption for Denoiser algorithm: ";
		os::Printer::log(message+std::to_string(denoiserStateBufferSize+scratchBufferSize+pixelBufferSize), ELL_INFORMATION);

		if (check_error(pixelBufferSize==0ull,"No input files at all!"))
			return error_code;

		denoiserState = driver->createDeviceLocalGPUBufferOnDedMem(denoiserStateBufferSize+IntensityValuesSize);
		if (check_error(!cuda::CCUDAHandler::defaultHandleResult(cuda::CCUDAHandler::registerBuffer(&denoiserState)),"Could not register buffer for Denoiser states!"))
			return error_code;
		scratchBuffer = driver->createDeviceLocalGPUBufferOnDedMem(scratchBufferSize);
		if (check_error(!cuda::CCUDAHandler::defaultHandleResult(cuda::CCUDAHandler::registerBuffer(&scratchBuffer)),"Could not register buffer for Denoiser scratch memory!"))
			return error_code;
		temporaryPixelBuffer = driver->createDeviceLocalGPUBufferOnDedMem(pixelBufferSize);
		if (check_error(!cuda::CCUDAHandler::defaultHandleResult(cuda::CCUDAHandler::registerBuffer(&temporaryPixelBuffer)),"Could not register buffer for Denoiser scratch memory!"))
			return error_code;
	}
	const auto intensityBufferOffset = denoiserStateBufferSize;

	// do the processing
	for (size_t i=0; i<inputFilesAmount; i++)
	{
		auto& param = images[i];
		if (param.denoiserType>=EII_COUNT)
			continue;
		const auto denoiserInputCount = param.denoiserType+1u;

		// set up the constants (partially)
		CommonPushConstants shaderConstants;
		{
			for (uint32_t j=0u; j<denoiserInputCount; j++)
				shaderConstants.outImageOffset[j] = j*param.width*param.height*forcedOptiXFormatPixelStride/sizeof(uint16_t); // float 16 actually
			shaderConstants.imageWidth = param.width;
			shaderConstants.medianFilterRadius = medianFilterRadiusBundle[i].value();
			assert(intensityBufferOffset%IntensityValuesSize==0u);
			shaderConstants.intensityBufferDWORDOffset = intensityBufferOffset/IntensityValuesSize;
			shaderConstants.denoiserExposureBias = denoiserExposureBiasBundle[i].value();

			switch (tonemapperBundle[i].first)
			{
				case DTEA_TONEMAPPER_REINHARD:
					shaderConstants.tonemappingOperator = ToneMapperClass::EO_REINHARD;
					break;
				case DTEA_TONEMAPPER_ACES:
					shaderConstants.tonemappingOperator = ToneMapperClass::EO_ACES;
					break;
				case DTEA_TONEMAPPER_NONE:
					shaderConstants.tonemappingOperator = ToneMapperClass::EO_COUNT;
					break;
				default:
					assert(false, "An unexcepted error ocured while trying to specify tonemapper!");
					break;
			}

			const float optiXIntensityKeyCompensation = -log2(0.18);
			float key = tonemapperBundle[i].second[TA_KEY_VALUE];
			float extraParam = tonemapperBundle[i].second[TA_EXTRA_PARAMETER];
			switch (shaderConstants.tonemappingOperator)
			{
				case ToneMapperClass::EO_REINHARD:
				{
					auto tp = ToneMapperClass::Params_t<ToneMapperClass::EO_REINHARD>(optiXIntensityKeyCompensation,key,extraParam);
					shaderConstants.tonemapperParams[0] = tp.keyAndLinearExposure;
					shaderConstants.tonemapperParams[1] = tp.rcpWhite2;
					break;
				}
				case ToneMapperClass::EO_ACES:
				{
					auto tp = ToneMapperClass::Params_t<ToneMapperClass::EO_ACES>(optiXIntensityKeyCompensation,key,extraParam);
					shaderConstants.tonemapperParams[0] = tp.gamma;
					shaderConstants.tonemapperParams[1] = (&tp.gamma)[1];
					break;
				}
				default:
					shaderConstants.tonemapperParams[0] = key*exp2(optiXIntensityKeyCompensation);
					shaderConstants.tonemapperParams[1] = core::nan<float>();
					break;
			}
			auto totalSampleCount = param.width*param.height;
			shaderConstants.percentileRange[0] = lowerPercentile*float(totalSampleCount);
			shaderConstants.percentileRange[1] = upperPercentile*float(totalSampleCount);
			shaderConstants.normalMatrix = cameraTransformBundle[i].value();
		}

		// upload image channels and register their buffer
		{
			asset::ICPUBuffer* buffersToUpload[EII_COUNT];
			for (uint32_t j=0u; j<denoiserInputCount; j++)
				buffersToUpload[j] = param.image[j]->getBuffer();
			auto gpubuffers = driver->getGPUObjectsFromAssets(buffersToUpload,buffersToUpload+denoiserInputCount);

			imagePixelBuffer = core::smart_refctd_ptr<IGPUBuffer>(gpubuffers->operator[](0)->getBuffer());
			if (!cuda::CCUDAHandler::defaultHandleResult(cuda::CCUDAHandler::registerBuffer(&imagePixelBuffer)))
			{
				os::Printer::log(makeImageIDString(i) + "Could register the image data buffer with CUDA, skipping image!", ELL_ERROR);
				continue;
			}

			for (uint32_t j=0u; j<denoiserInputCount; j++)
			{
				auto offsetPair = gpubuffers->operator[](j);
				// it needs to be packed into the same buffer to work
				assert(offsetPair->getBuffer()==imagePixelBuffer.getObject());

				// make sure cache doesn't retain the GPU object paired to CPU object (could have used a custom IGPUObjectFromAssetConverter derived class with overrides to achieve this)
				am->removeCachedGPUObject(buffersToUpload[j],offsetPair);

				auto image = param.image[j];
				const auto& creationParameters = image->getCreationParameters();
				assert(asset::getTexelOrBlockBytesize(creationParameters.format)==param.colorTexelSize);
				// set up some image pitch and offset info
				shaderConstants.inImageTexelPitch[j] = image->getRegions().begin()[0].bufferRowLength;
				shaderConstants.inImageTexelOffset[j] = offsetPair->getOffset();
				assert(shaderConstants.inImageTexelOffset[j]%param.colorTexelSize==0u);
				shaderConstants.inImageTexelOffset[j] /= param.colorTexelSize;
			}
			// upload the constants to the GPU
			driver->pushConstants(sharedPipelineLayout.get(), video::IGPUSpecializedShader::ESS_COMPUTE, 0u, sizeof(CommonPushConstants), &shaderConstants);
		}

		// process
		{
			// bind shader resources
			{
				// create descriptor set
				auto descriptorSet = driver->createGPUDescriptorSet(core::smart_refctd_ptr(sharedDescriptorSetLayout));
				// write descriptor set
				{
					IGPUDescriptorSet::SDescriptorInfo infos[SharedDescriptorSetDescCount];
					infos[0].desc = core::smart_refctd_ptr<IGPUBuffer>(imagePixelBuffer.getObject());
					infos[0].buffer = {0u,imagePixelBuffer.getObject()->getMemoryReqs().vulkanReqs.size};
					infos[1].desc = core::smart_refctd_ptr<IGPUBuffer>(temporaryPixelBuffer.getObject());
					infos[1].buffer = {0u,temporaryPixelBuffer.getObject()->getMemoryReqs().vulkanReqs.size};
					infos[2].desc = core::smart_refctd_ptr<IGPUBuffer>(scratchBuffer.getObject());
					infos[2].buffer = {0u,HistogramBufferSize};
					infos[3].desc = core::smart_refctd_ptr<IGPUBuffer>(intensityBuffer.getObject());
					infos[3].buffer = {0u,intensityBuffer.getObject()->getMemoryReqs().vulkanReqs.size};
					IGPUDescriptorSet::SWriteDescriptorSet writes[SharedDescriptorSetDescCount];
					for (uint32_t i=0u; i<SharedDescriptorSetDescCount; i++)
						writes[i] = {descriptorSet.get(),i,0u,1u,EDT_STORAGE_BUFFER,infos+i};
					driver->updateDescriptorSets(SharedDescriptorSetDescCount,writes,0u,nullptr);
				}
				// bind descriptor set (for all shaders)
				driver->bindDescriptorSets(video::EPBP_COMPUTE,sharedPipelineLayout.get(),0u,1u,&descriptorSet.get(),nullptr);
			}
			// compute shader pre-preprocess (transform normals and compute luminosity)
			{
				// clear the histogram to 0s
				driver->fillBuffer(scratchBuffer.getObject(),0u,HistogramBufferSize,0u);
				// bind deinterleave pipeline
				driver->bindComputePipeline(deinterleavePipeline.get());
				// dispatch
				driver->dispatch((param.width+kComputeWGSize-1u)/kComputeWGSize,param.height,denoiserInputCount);
				COpenGLExtensionHandler::extGlMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
				// bind intensity pipeline
				driver->bindComputePipeline(intensityPipeline.get());
				// dispatch
				driver->dispatch(1u,1u,1u);
				// issue a full memory barrier (or at least all buffer read/write barrier)
				COpenGLExtensionHandler::extGlMemoryBarrier(GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT | GL_PIXEL_BUFFER_BARRIER_BIT | GL_TEXTURE_UPDATE_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);
			}

			// optix processing
			{
				// map buffer
				if (check_error(!cuda::CCUDAHandler::defaultHandleResult(cuda::CCUDAHandler::acquireAndGetPointers(bufferLinks,bufferLinks+DENOISER_BUFFER_COUNT,m_cudaStream)),"Error when mapping OpenGL Buffers to CUdeviceptr!"))
					return error_code;

				auto unmapBuffers = [&m_cudaStream,&bufferLinks]() -> void
				{
					void* scratch[DENOISER_BUFFER_COUNT*sizeof(CUgraphicsResource)];
					if (check_error(!cuda::CCUDAHandler::defaultHandleResult(cuda::CCUDAHandler::releaseResourcesToGraphics(scratch,bufferLinks,bufferLinks+DENOISER_BUFFER_COUNT,m_cudaStream)), "Error when unmapping CUdeviceptr back to OpenGL!"))
						exit(error_code);
				};
#undef DENOISER_BUFFER_COUNT
				core::SRAIIBasedExiter<decltype(unmapBuffers)> exitRoutine(unmapBuffers);

				// set up optix image
				OptixImage2D denoiserInputs[EII_COUNT];
				for (uint32_t j=0u; j<denoiserInputCount; j++)
				{				
					denoiserInputs[j].data = temporaryPixelBuffer.asBuffer.pointer+shaderConstants.outImageOffset[j]*sizeof(uint16_t); // sizeof(float16_t)
					denoiserInputs[j].width = param.width;
					denoiserInputs[j].height = param.height;
					denoiserInputs[j].rowStrideInBytes = param.width*forcedOptiXFormatPixelStride;
					denoiserInputs[j].pixelStrideInBytes = 0u;
					denoiserInputs[j].format = forcedOptiXFormat;
				}
				//
				{
					// set up denoiser
					auto& denoiser = denoisers[param.denoiserType];
					if (denoiser.m_denoiser->setup(m_cudaStream,&param.width,denoiserState,denoiser.stateSize,scratchBuffer,denoiser.scratchSize,denoiser.stateOffset)!=OPTIX_SUCCESS)
					{
						os::Printer::log(makeImageIDString(i) + "Could not setup the denoiser for the image resolution and denoiser buffers, skipping image!", ELL_ERROR);
						continue;
					}
					// invoke
					{
						OptixDenoiserParams denoiserParams = {};
						denoiserParams.blendFactor = denoiserBlendFactorBundle[i].value();
						denoiserParams.denoiseAlpha = 0u;
						denoiserParams.hdrIntensity = intensityBuffer.asBuffer.pointer+intensityBufferOffset;
						OptixImage2D denoiserOutput;
						denoiserOutput.data = imagePixelBuffer.asBuffer.pointer+shaderConstants.inImageTexelOffset[EII_COLOR];
						denoiserOutput.width = param.width;
						denoiserOutput.height = param.height;
						denoiserOutput.rowStrideInBytes = param.width*forcedOptiXFormatPixelStride;
						denoiserOutput.pixelStrideInBytes = 0u;
						denoiserOutput.format = forcedOptiXFormat;
						if (denoiser.m_denoiser->invoke(m_cudaStream,&denoiserParams,denoiserInputs,denoiserInputs+denoiserInputCount,&denoiserOutput,scratchBuffer,denoiser.scratchSize)!=OPTIX_SUCCESS)
						{
							os::Printer::log(makeImageIDString(i) + "Could not invoke the denoiser sucessfully, skipping image!", ELL_ERROR);
							continue;
						}
					}
				}

				// unmap buffer (implicit from the SRAIIExiter destructor)
			}

			// compute post-processing
			{
				// TODO: Bloom (FoV vs. Constant)
				{
				}
				// TODO: Tonemap
				{
				}
				driver->bindComputePipeline(interleavePipeline.get());
				driver->dispatch((param.width+kComputeWGSize-1u)/kComputeWGSize,param.height,1u);
				// issue a full memory barrier (or at least all buffer read/write barrier)
				COpenGLExtensionHandler::extGlMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
			}
			// delete descriptor sets (implicit from destructor)
		}

		{
			auto downloadStagingArea = driver->getDefaultDownStreamingBuffer();
			uint32_t address = std::remove_pointer<decltype(downloadStagingArea)>::type::invalid_address; // remember without initializing the address to be allocated to invalid_address you won't get an allocation!

			// image view
			core::smart_refctd_ptr<ICPUImageView> imageView;
			const uint32_t colorBufferBytesize = param.height * param.width * param.colorTexelSize;
			{
				// create image
				ICPUImage::SCreationParams imgParams;
				imgParams.flags = static_cast<ICPUImage::E_CREATE_FLAGS>(0u); // no flags
				imgParams.type = ICPUImage::ET_2D;
				imgParams.format = param.image[EII_COLOR]->getCreationParameters().format;
				imgParams.extent = {param.width,param.height,1u};
				imgParams.mipLevels = 1u;
				imgParams.arrayLayers = 1u;
				imgParams.samples = ICPUImage::ESCF_1_BIT;

				auto image = ICPUImage::create(std::move(imgParams));

				// get the data from the GPU
				{
					constexpr uint64_t timeoutInNanoSeconds = 300000000000u;

					// download buffer
					{
						const uint32_t alignment = 4096u; // common page size
						auto unallocatedSize = downloadStagingArea->multi_alloc(std::chrono::nanoseconds(timeoutInNanoSeconds), 1u, &address, &colorBufferBytesize, &alignment);
						if (unallocatedSize)
						{
							os::Printer::log(makeImageIDString(i)+"Could not download the buffer from the GPU!",ELL_ERROR);
							continue;
						}

						driver->copyBuffer(temporaryPixelBuffer.getObject(),downloadStagingArea->getBuffer(),0u,address,colorBufferBytesize);
					}
					auto downloadFence = driver->placeFence(true);

					// set up regions
					auto regions = core::make_refctd_dynamic_array<core::smart_refctd_dynamic_array<IImage::SBufferCopy> >(1u);
					{
						auto& region = regions->front();
						region.bufferOffset = 0u;
						region.bufferRowLength = param.image[EII_COLOR]->getRegions().begin()[0].bufferRowLength;
						region.bufferImageHeight = param.height;
						//region.imageSubresource.aspectMask = wait for Vulkan;
						region.imageSubresource.mipLevel = 0u;
						region.imageSubresource.baseArrayLayer = 0u;
						region.imageSubresource.layerCount = 1u;
						region.imageOffset = { 0u,0u,0u };
						region.imageExtent = imgParams.extent;
					}
					// the cpu is not touching the data yet because the custom CPUBuffer is adopting the memory (no copy)
					auto* data = reinterpret_cast<uint8_t*>(downloadStagingArea->getBufferPointer())+address;
					auto cpubufferalias = core::make_smart_refctd_ptr<asset::CCustomAllocatorCPUBuffer<core::null_allocator<uint8_t> > >(colorBufferBytesize, data, core::adopt_memory);
					image->setBufferAndRegions(std::move(cpubufferalias),regions);

					// wait for download fence and then invalidate the CPU cache
					{
						auto result = downloadFence->waitCPU(timeoutInNanoSeconds,true);
						if (result==E_DRIVER_FENCE_RETVAL::EDFR_TIMEOUT_EXPIRED||result==E_DRIVER_FENCE_RETVAL::EDFR_FAIL)
						{
							os::Printer::log(makeImageIDString(i)+"Could not download the buffer from the GPU, fence not signalled!",ELL_ERROR);
							downloadStagingArea->multi_free(1u, &address, &colorBufferBytesize, nullptr);
							continue;
						}
						if (downloadStagingArea->needsManualFlushOrInvalidate())
							driver->invalidateMappedMemoryRanges({{downloadStagingArea->getBuffer()->getBoundMemory(),address,colorBufferBytesize}});
					}
				}

				// create image view
				ICPUImageView::SCreationParams imgViewParams;
				imgViewParams.flags = static_cast<ICPUImageView::E_CREATE_FLAGS>(0u);
				imgViewParams.format = image->getCreationParameters().format;
				imgViewParams.image = std::move(image);
				imgViewParams.viewType = ICPUImageView::ET_2D;
				imgViewParams.subresourceRange = {static_cast<IImage::E_ASPECT_FLAGS>(0u),0u,1u,0u,1u};
				imageView = ICPUImageView::create(std::move(imgViewParams));
			}

			// save image
			IAssetWriter::SAssetWriteParams wp(imageView.get());
			am->writeAsset(outputFileBundle[i].value().c_str(), wp);

			// destroy link to CPUBuffer's data (we need to free it)
			imageView->convertToDummyObject(~0u);

			// free the staging area allocation (no fence, we've already waited on it)
			downloadStagingArea->multi_free(1u,&address,&colorBufferBytesize,nullptr);

			// destroy image (implicit)

			//
			driver->endScene();
		}
	}

	return 0;
}