// Copyright (C) 2018-2020 - DevSH Graphics Programming Sp. z O.O.
// This file is part of the "Nabla Engine".
// For conditions of distribution and use, see copyright notice in nabla.h

#define _NBL_STATIC_LIB_
#include <nabla.h>

#include "../common/CommonAPI.h"
#include "nbl/ext/ScreenShot/ScreenShot.h"

using namespace nbl;
using namespace nbl::asset;
using namespace nbl::core;
using namespace nbl::video;

#define FATAL_LOG(x, ...) {logger->log(##x, system::ILogger::ELL_ERROR, __VA_ARGS__); exit(-1);}

using ScaledBoxKernel = asset::CScaledImageFilterKernel<CBoxImageFilterKernel>;
using ScaledTriangleKernel = asset::CScaledImageFilterKernel<CTriangleImageFilterKernel>;
using ScaledKaiserKernel = asset::CScaledImageFilterKernel<CKaiserImageFilterKernel<>>;
using ScaledMitchellKernel = asset::CScaledImageFilterKernel<CMitchellImageFilterKernel<>>;
using ScaledMitchellDerivativeKernel = asset::CDerivativeImageFilterKernel<ScaledMitchellKernel>;
using ScaledChannelIndependentKernel = asset::CChannelIndependentImageFilterKernel<ScaledBoxKernel, ScaledMitchellKernel, ScaledKaiserKernel>;

// dims[3] is layer count
core::smart_refctd_ptr<ICPUImage> createCPUImage(const core::vectorSIMDu32& dims, const asset::IImage::E_TYPE imageType, const asset::E_FORMAT format, const uint32_t mipLevels = 1, const bool fillWithTestData = false)
{
	IImage::SCreationParams imageParams = {};
	imageParams.flags = static_cast<asset::IImage::E_CREATE_FLAGS>(asset::IImage::ECF_MUTABLE_FORMAT_BIT | asset::IImage::ECF_EXTENDED_USAGE_BIT);
	imageParams.type = imageType;
	imageParams.format = format;
	imageParams.extent = { dims[0], dims[1], dims[2] };
	imageParams.mipLevels = mipLevels;
	imageParams.arrayLayers = dims[3];
	imageParams.samples = asset::ICPUImage::ESCF_1_BIT;
	imageParams.usage = asset::IImage::EUF_SAMPLED_BIT;

	auto imageRegions = core::make_refctd_dynamic_array<core::smart_refctd_dynamic_array<asset::IImage::SBufferCopy>>(1ull);
	auto& region = (*imageRegions)[0];
	region.bufferImageHeight = 0u;
	region.bufferOffset = 0ull;
	region.bufferRowLength = dims[0];
	region.imageExtent = { dims[0], dims[1], dims[2] };
	region.imageOffset = { 0u, 0u, 0u };
	region.imageSubresource.aspectMask = asset::IImage::EAF_COLOR_BIT;
	region.imageSubresource.baseArrayLayer = 0u;
	region.imageSubresource.layerCount = imageParams.arrayLayers;
	region.imageSubresource.mipLevel = 0;

	size_t bufferSize = imageParams.arrayLayers * asset::getTexelOrBlockBytesize(imageParams.format) * static_cast<size_t>(region.imageExtent.width) * region.imageExtent.height * region.imageExtent.depth;
	auto imageBuffer = core::make_smart_refctd_ptr<asset::ICPUBuffer>(bufferSize);

	core::smart_refctd_ptr<ICPUImage> image = ICPUImage::create(std::move(imageParams));
	if (!image)
		return nullptr;

	image->setBufferAndRegions(core::smart_refctd_ptr(imageBuffer), imageRegions);

	if (fillWithTestData)
	{
		double pixelValueUpperBound = 20.0;
		if (asset::isNormalizedFormat(format) || format == asset::EF_B10G11R11_UFLOAT_PACK32)
			pixelValueUpperBound = 1.00000000001;

		std::uniform_real_distribution<double> dist(0.0, pixelValueUpperBound);
		std::mt19937 prng;

		uint8_t* bytePtr = reinterpret_cast<uint8_t*>(image->getBuffer()->getPointer());
		const auto layerSize = bufferSize / imageParams.arrayLayers;

		double dummyVal = 1.0;
		for (auto layer = 0; layer < image->getCreationParameters().arrayLayers; ++layer)
		{
			// double dummyVal = 1.0;

			for (uint64_t k = 0u; k < dims[2]; ++k)
			{
				for (uint64_t j = 0u; j < dims[1]; ++j)
				{
					for (uint64_t i = 0; i < dims[0]; ++i)
					{
						const double dummyValToPut = dummyVal++;
						double decodedPixel[4] = { 0 };
						for (uint32_t ch = 0u; ch < asset::getFormatChannelCount(format); ++ch)
							// decodedPixel[ch] = dummyValToPut;
							decodedPixel[ch] = dist(prng);

						const uint64_t pixelIndex = (k * dims[1] * dims[0]) + (j * dims[0]) + i;
						asset::encodePixelsRuntime(format, bytePtr + layer*layerSize + pixelIndex * asset::getTexelOrBlockBytesize(format), decodedPixel);
					}
				}
			}
		}
	}

	return image;
}

static inline asset::IImageView<asset::ICPUImage>::E_TYPE getImageViewTypeFromImageType_CPU(const asset::IImage::E_TYPE type)
{
	switch (type)
	{
	case asset::IImage::ET_1D:
		return asset::ICPUImageView::ET_1D;
	case asset::IImage::ET_2D:
		return asset::ICPUImageView::ET_2D;
	case asset::IImage::ET_3D:
		return asset::ICPUImageView::ET_3D;
	default:
		assert(!"Invalid code path.");
		return static_cast<asset::IImageView<asset::ICPUImage>::E_TYPE>(0u);
	}
}

static inline video::IGPUImageView::E_TYPE getImageViewTypeFromImageType_GPU(const video::IGPUImage::E_TYPE type)
{
	switch (type)
	{
	case video::IGPUImage::ET_1D:
		return video::IGPUImageView::ET_1D_ARRAY;
	case video::IGPUImage::ET_2D:
		return video::IGPUImageView::ET_2D_ARRAY;
	case video::IGPUImage::ET_3D:
		return video::IGPUImageView::ET_3D;
	default:
		assert(!"Invalid code path.");
		return static_cast<video::IGPUImageView::E_TYPE>(0u);
	}
}

class BlitFilterTestApp : public ApplicationBase
{
	constexpr static uint32_t SC_IMG_COUNT = 3u;
	constexpr static uint64_t MAX_TIMEOUT = 99999999999999ull;

public:
	void onAppInitialized_impl() override
	{
		CommonAPI::InitParams initParams;
		initParams.apiType = video::EAT_VULKAN;
		initParams.appName = { "BlitFilterTest" };
		initParams.swapchainImageUsage = nbl::asset::IImage::E_USAGE_FLAGS(0);
		auto initOutput = CommonAPI::Init(std::move(initParams));

		system = std::move(initOutput.system);
		window = std::move(initParams.window);
		windowCb = std::move(initParams.windowCb);
		apiConnection = std::move(initOutput.apiConnection);
		surface = std::move(initOutput.surface);
		physicalDevice = std::move(initOutput.physicalDevice);
		logicalDevice = std::move(initOutput.logicalDevice);
		utilities = std::move(initOutput.utilities);
		queues = std::move(initOutput.queues);
		commandPools = std::move(initOutput.commandPools);
		assetManager = std::move(initOutput.assetManager);
		cpu2gpuParams = std::move(initOutput.cpu2gpuParams);
		logger = std::move(initOutput.logger);
		inputSystem = std::move(initOutput.inputSystem);

		constexpr bool TestCPUBlitFilter = true;
		constexpr bool TestFlattenFilter = true;
		constexpr bool TestConvertFilter = true;
		constexpr bool TestGPUBlitFilter = true;

		auto loadImage = [this](const char* path) -> core::smart_refctd_ptr<asset::ICPUImage>
		{
			constexpr auto cachingFlags = static_cast<asset::IAssetLoader::E_CACHING_FLAGS>(asset::IAssetLoader::ECF_DONT_CACHE_REFERENCES & asset::IAssetLoader::ECF_DONT_CACHE_TOP_LEVEL);
			asset::IAssetLoader::SAssetLoadParams loadParams(0ull, nullptr, cachingFlags);
			auto imageBundle = assetManager->getAsset(path, loadParams);
			auto imageContents = imageBundle.getContents();

			if (imageContents.empty())
				return nullptr;

			auto asset = *imageContents.begin();

			core::smart_refctd_ptr<asset::ICPUImage> result;
			{
				if (asset->getAssetType() == asset::IAsset::ET_IMAGE_VIEW)
					result = std::move(core::smart_refctd_ptr_static_cast<asset::ICPUImageView>(asset)->getCreationParameters().image);
				else if (asset->getAssetType() == asset::IAsset::ET_IMAGE)
					result = std::move(core::smart_refctd_ptr_static_cast<asset::ICPUImage>(asset));
				else
					assert(!"Invalid code path.");
			}

			// TODO(achal): Boot this out of here and do it in GPU specific tests.
			result->addImageUsageFlags(asset::IImage::EUF_SAMPLED_BIT);

			return result;
		};

		auto writeImage = [this](core::smart_refctd_ptr<asset::ICPUImage>&& image, const char* path, const asset::IImageView<asset::ICPUImage>::E_TYPE imageViewType)
		{
			asset::ICPUImageView::SCreationParams viewParams = {};
			viewParams.flags = static_cast<decltype(viewParams.flags)>(0u);
			viewParams.image = std::move(image);
			viewParams.format = viewParams.image->getCreationParameters().format;
			viewParams.viewType = imageViewType;
			viewParams.subresourceRange.aspectMask = asset::IImage::EAF_COLOR_BIT;
			viewParams.subresourceRange.baseArrayLayer = 0u;
			viewParams.subresourceRange.layerCount = viewParams.image->getCreationParameters().arrayLayers;
			viewParams.subresourceRange.baseMipLevel = 0u;
			viewParams.subresourceRange.levelCount = viewParams.image->getCreationParameters().mipLevels;

			auto imageViewToWrite = asset::ICPUImageView::create(std::move(viewParams));
			if (!imageViewToWrite)
			{
				logger->log("Failed to create image view for the output image to write it to disk.", system::ILogger::ELL_ERROR);
				return;
			}

			asset::IAssetWriter::SAssetWriteParams writeParams(imageViewToWrite.get());
			if (!assetManager->writeAsset(path, writeParams))
			{
				logger->log("Failed to write the output image.", system::ILogger::ELL_ERROR);
				return;
			}
		};

		if (TestCPUBlitFilter)
		{
			logger->log("CBlitImageFilter", system::ILogger::ELL_INFO);

			constexpr const char* TestImagePaths[] =
			{
				"../../media/GLI/kueken7_rgba_dxt1_unorm.dds",
				"../../media/GLI/kueken7_rgba_dxt5_unorm.dds",
				"../../media/GLI/dice_bc3.dds"
			};
			constexpr auto TestImagePathsCount = sizeof(TestImagePaths) / sizeof(const char*);

			for (const char* pathToImage : TestImagePaths)
			{
				logger->log("Image: \t%s", system::ILogger::ELL_INFO, pathToImage);

				const auto& inImage = loadImage(pathToImage);
				if (!inImage)
				{
					logger->log("Cannot find the image.", system::ILogger::ELL_ERROR);
					continue;
				}

				const auto& inImageExtent = inImage->getCreationParameters().extent;
				const auto& inImageFormat = inImage->getCreationParameters().format;
				const uint32_t inImageMipCount = inImage->getCreationParameters().mipLevels;

				const auto outFormat = asset::EF_R32G32B32A32_SFLOAT;

				auto outImage = createCPUImage(core::vectorSIMDu32(inImageExtent.width, inImageExtent.height, inImageExtent.depth, inImage->getCreationParameters().arrayLayers), inImage->getCreationParameters().type, outFormat);
				if (!outImage)
				{
					logger->log("Failed to create CPU image for output.", system::ILogger::ELL_ERROR);
					continue;
				}

				const core::vectorSIMDf scaleX(1.f, 1.f, 1.f, 1.f);
				const core::vectorSIMDf scaleY(1.f, 1.f, 1.f, 1.f);
				const core::vectorSIMDf scaleZ(1.f, 1.f, 1.f, 1.f);

				auto kernelX = ScaledBoxKernel(scaleX, asset::CBoxImageFilterKernel());
				auto kernelY = ScaledBoxKernel(scaleY, asset::CBoxImageFilterKernel());
				auto kernelZ = ScaledBoxKernel(scaleZ, asset::CBoxImageFilterKernel());

				using BlitFilter = asset::CBlitImageFilter<asset::VoidSwizzle, asset::IdentityDither, void, false, decltype(kernelX), decltype(kernelY), decltype(kernelZ), float>;
				typename BlitFilter::state_type blitFilterState(std::move(kernelX), std::move(kernelY), std::move(kernelZ));

				blitFilterState.inOffsetBaseLayer = core::vectorSIMDu32();
				blitFilterState.inExtentLayerCount = core::vectorSIMDu32(0u, 0u, 0u, inImage->getCreationParameters().arrayLayers) + inImage->getMipSize();
				blitFilterState.inImage = inImage.get();

				blitFilterState.outImage = outImage.get();

				blitFilterState.outOffsetBaseLayer = core::vectorSIMDu32();
				blitFilterState.outExtentLayerCount = blitFilterState.inExtentLayerCount;

				blitFilterState.scratchMemoryByteSize = BlitFilter::getRequiredScratchByteSize(&blitFilterState);
				blitFilterState.scratchMemory = reinterpret_cast<uint8_t*>(_NBL_ALIGNED_MALLOC(blitFilterState.scratchMemoryByteSize, 32));

				if (!BlitFilter::blit_utils_t::template computeScaledKernelPhasedLUT<float>(blitFilterState.scratchMemory + BlitFilter::getScratchOffset(&blitFilterState, BlitFilter::ESU_SCALED_KERNEL_PHASED_LUT), blitFilterState.inExtentLayerCount, blitFilterState.outExtentLayerCount, blitFilterState.inImage->getCreationParameters().type, kernelX, kernelY, kernelZ))
				{
					logger->log("Failed to compute the LUT for blitting.\n", system::ILogger::ELL_ERROR);
					continue;
				}

				if (!BlitFilter::execute(core::execution::par_unseq, &blitFilterState))
				{
					logger->log("Failed to blit.\n", system::ILogger::ELL_ERROR);
					continue;
				}

				_NBL_ALIGNED_FREE(blitFilterState.scratchMemory);

				std::filesystem::path filename, inFileExtension;
				core::splitFilename(pathToImage, nullptr, &filename, &inFileExtension);

				assert(outFormat == asset::EF_R32G32B32A32_SFLOAT);
				constexpr std::string_view outFileExtension = ".exr";
				std::string outFileName = "CBlitImageFilter_" + filename.string() + outFileExtension.data();

				writeImage(std::move(outImage), outFileName.c_str(), asset::ICPUImageView::ET_2D);
			}
		}

		if (TestFlattenFilter)
		{
			logger->log("CFlattenRegionsImageFilter", system::ILogger::ELL_INFO);

			constexpr const char* TestImagePaths[] =
			{
				"../../media/GLI/kueken7_rgba_dxt1_unorm.dds",
				"../../media/GLI/kueken7_rgba_dxt5_unorm.dds",
				"../../media/GLI/dice_bc3.dds"
			};
			constexpr auto TestImagePathsCount = sizeof(TestImagePaths) / sizeof(const char*);

			for (const char* pathToImage : TestImagePaths)
			{
				logger->log("Image: \t%s", system::ILogger::ELL_INFO, pathToImage);

				const auto& inImage = loadImage(pathToImage);
				if (!inImage)
				{
					logger->log("Cannot find the image.", system::ILogger::ELL_ERROR);
					continue;
				}

				const auto& inImageExtent = inImage->getCreationParameters().extent;
				const auto& inImageFormat = inImage->getCreationParameters().format;
				const uint32_t inImageMipCount = inImage->getCreationParameters().mipLevels;

				// We use the very first block of the image as fill value for the filter.
				std::unique_ptr<uint8_t[]> fillValueBlock = std::make_unique<uint8_t[]>(asset::getTexelOrBlockBytesize(inImageFormat));

				core::smart_refctd_ptr<ICPUImage> flattenInImage;
				{
					const uint64_t bufferSizeNeeded = (inImageExtent.width * inImageExtent.height * inImageExtent.depth * asset::getTexelOrBlockBytesize(inImageFormat)) / 2ull;

					IImage::SCreationParams imageParams = {};
					imageParams.type = asset::ICPUImage::ET_2D;
					imageParams.format = inImageFormat;
					imageParams.extent = { inImageExtent.width, inImageExtent.height, inImageExtent.depth };
					imageParams.mipLevels = 1u;
					imageParams.arrayLayers = 1u;
					imageParams.samples = asset::ICPUImage::ESCF_1_BIT;

					auto imageRegions = core::make_refctd_dynamic_array<core::smart_refctd_dynamic_array<asset::IImage::SBufferCopy>>(2ull);
					{
						auto& region = (*imageRegions)[0];
						region.bufferOffset = 0ull;
						region.bufferRowLength = imageParams.extent.width / 2;
						region.bufferImageHeight = imageParams.extent.height / 2;
						region.imageExtent = { imageParams.extent.width / 2, imageParams.extent.height / 2, core::max(imageParams.extent.depth / 2, 1) };
						region.imageOffset = { 0u, 0u, 0u };
						region.imageSubresource.aspectMask = asset::IImage::EAF_COLOR_BIT;
						region.imageSubresource.baseArrayLayer = 0u;
						region.imageSubresource.layerCount = imageParams.arrayLayers;
						region.imageSubresource.mipLevel = 0;
					}
					{
						auto& region = (*imageRegions)[1];
						region.bufferOffset = bufferSizeNeeded / 2ull;
						region.bufferRowLength = imageParams.extent.width / 2;
						region.bufferImageHeight = imageParams.extent.height / 2;
						region.imageExtent = { imageParams.extent.width / 2, imageParams.extent.height / 2, core::max(imageParams.extent.depth / 2, 1) };
						region.imageOffset = { imageParams.extent.width / 2, imageParams.extent.height / 2, 0u };
						region.imageSubresource.aspectMask = asset::IImage::EAF_COLOR_BIT;
						region.imageSubresource.baseArrayLayer = 0u;
						region.imageSubresource.layerCount = imageParams.arrayLayers;
						region.imageSubresource.mipLevel = 0;
					}

					auto imageBuffer = core::make_smart_refctd_ptr<asset::ICPUBuffer>(bufferSizeNeeded);
					if (!imageBuffer)
					{
						logger->log("Failed to create backing buffer for flatten input image.", system::ILogger::ELL_ERROR);
						continue;
					}

					flattenInImage = ICPUImage::create(std::move(imageParams));
					if (!flattenInImage)
					{
						logger->log("Failed to create the flatten input image.", system::ILogger::ELL_ERROR);
						continue;
					}

					flattenInImage->setBufferAndRegions(core::smart_refctd_ptr(imageBuffer), imageRegions);

					const auto blockDim = asset::getBlockDimensions(inImageFormat);
					const uint32_t blockCountX = inImageExtent.width / blockDim.x;
					const uint32_t blockCountY = inImageExtent.height / blockDim.y;
					const auto blockSize = asset::getTexelOrBlockBytesize(inImageFormat);

					uint8_t* src = reinterpret_cast<uint8_t*>(inImage->getBuffer()->getPointer());
					uint8_t* dst = reinterpret_cast<uint8_t*>(flattenInImage->getBuffer()->getPointer());
					for (uint32_t y = 0; y < blockCountY / 2; ++y)
					{
						for (uint32_t x = 0; x < blockCountX / 2; ++x)
						{
							if (x == 0 && y == 0)
								memcpy(fillValueBlock.get(), src, blockSize);

							const uint64_t byteOffset = (y * blockCountX + x) * blockSize;
							memcpy(dst, src + byteOffset, blockSize);
							dst += blockSize;
						}
					}

					const auto& regions = flattenInImage->getRegions();

					src = reinterpret_cast<uint8_t*>(inImage->getBuffer()->getPointer());
					dst = reinterpret_cast<uint8_t*>(flattenInImage->getBuffer()->getPointer()) + regions.begin()[1].bufferOffset;
					for (uint32_t y = 0; y < blockCountY / 2; ++y)
					{
						for (uint32_t x = 0; x < blockCountX / 2; ++x)
						{
							const uint64_t byteOffset = ((y + (blockCountY / 2)) * blockCountX + (x + (blockCountX / 2))) * blockSize;
							memcpy(dst, src + byteOffset, blockSize);
							dst += blockSize;
						}
					}
				}

#if 0
				writeImage(core::smart_refctd_ptr(flattenInImage), "flatten_input.dds", getImageViewTypeFromImageType_CPU(flattenInImage->getCreationParameters().type));
#endif
				asset::CFlattenRegionsImageFilter::CState filterState;
				filterState.inImage = flattenInImage.get();
				filterState.outImage = nullptr;
				filterState.preFill = true;
				memcpy(filterState.fillValue.asCompressedBlock, fillValueBlock.get(), asset::getTexelOrBlockBytesize(filterState.inImage->getCreationParameters().format));

				if (!asset::CFlattenRegionsImageFilter::execute(&filterState))
				{
					logger->log("CFlattenRegionsImageFilter failed.", system::ILogger::ELL_ERROR);
					continue;
				}

				std::filesystem::path filename, inFileExtension;
				core::splitFilename(pathToImage, nullptr, &filename, &inFileExtension);

				std::string outFileName = "CFlattenRegionsImageFilter_" + filename.string() + inFileExtension.string();

				writeImage(core::smart_refctd_ptr(filterState.outImage), outFileName.c_str(), asset::ICPUImageView::ET_2D);
			}
		}

		if (TestConvertFilter)
		{
			logger->log("CConvertFormatImageFilter", system::ILogger::ELL_INFO);

			constexpr const char* TestImagePaths[] =
			{
				"../../media/GLI/kueken7_rgba_dxt1_unorm.dds",
				"../../media/GLI/kueken7_rgba_dxt5_unorm.dds",
				"../../media/GLI/dice_bc3.dds"
			};
			constexpr auto TestImagePathsCount = sizeof(TestImagePaths)/sizeof(const char*);

			for (const char* pathToImage : TestImagePaths)
			{
				logger->log("Image: \t%s", system::ILogger::ELL_INFO, pathToImage);

				const auto& inImage = loadImage(pathToImage);
				if (!inImage)
				{
					logger->log("Cannot find the image.", system::ILogger::ELL_ERROR);
					continue;
				}

				const auto& inImageExtent = inImage->getCreationParameters().extent;
				const auto& inImageFormat = inImage->getCreationParameters().format;
				const uint32_t inImageMipCount = inImage->getCreationParameters().mipLevels;

				const auto outFormat = asset::EF_R32G32B32A32_SFLOAT;
				auto outImage = createCPUImage(core::vectorSIMDu32(inImageExtent.width, inImageExtent.height, inImageExtent.depth, inImage->getCreationParameters().arrayLayers), inImage->getCreationParameters().type, outFormat, inImageMipCount);
				if (!outImage)
				{
					logger->log("Failed to create CPU image for output.", system::ILogger::ELL_ERROR);
					continue;
				}

				const auto& outImageExtent = outImage->getCreationParameters().extent;

				asset::CConvertFormatImageFilter<>::state_type filterState = {};
				assert((inImageExtent.width == outImageExtent.width) && (inImageExtent.height == outImageExtent.height) && (inImageExtent.depth == outImageExtent.depth) && (inImage->getCreationParameters().arrayLayers == outImage->getCreationParameters().arrayLayers));
				filterState.extentLayerCount = core::vectorSIMDu32(inImageExtent.width, inImageExtent.height, inImageExtent.depth, inImage->getCreationParameters().arrayLayers);

				filterState.inOffsetBaseLayer = core::vectorSIMDu32(0, 0, 0, 0);
				filterState.outOffsetBaseLayer = core::vectorSIMDu32(0, 0, 0, 0);

				assert(inImageMipCount == outImage->getCreationParameters().mipLevels);

				filterState.inImage = inImage.get();
				filterState.outImage = outImage.get();

				// TODO(achal): If I'm outputting to a format which doesn't support mips then I might as well pick any random mip to convert, or write
				// to separate output images.
				for (uint32_t i = 0; i < inImageMipCount; ++i)
				{
					filterState.inMipLevel = i;
					filterState.outMipLevel = i;

					if (!asset::CConvertFormatImageFilter<>::execute(&filterState))
					{
						logger->log("CConvertFormatImageFilter failed for mip level %u.", system::ILogger::ELL_ERROR, i);
						return;
					}
				}

				std::filesystem::path filename, inFileExtension;
				core::splitFilename(pathToImage, nullptr, &filename, &inFileExtension);

				assert(outFormat == asset::EF_R32G32B32A32_SFLOAT);
				constexpr std::string_view outFileExtension = ".exr";
				std::string outFileName = "CConvertFormatImageFilter_" + filename.string() + outFileExtension.data();

				writeImage(std::move(outImage), outFileName.c_str(), asset::ICPUImageView::ET_2D);
			}
		}		

		if (TestGPUBlitFilter)
		{

			if (1)
			{
				logger->log("Test #1");

				const auto layerCount = 10;
				const core::vectorSIMDu32 inImageDim(59u, 1u, 1u, layerCount);
				const asset::IImage::E_TYPE inImageType = asset::IImage::ET_1D;
				const asset::E_FORMAT inImageFormat = asset::EF_R32_SFLOAT;
				auto inImage = createCPUImage(inImageDim, inImageType, inImageFormat, 1, true);

				const core::vectorSIMDu32 outImageDim(800u, 1u, 1u, layerCount);
				const IBlitUtilities::E_ALPHA_SEMANTIC alphaSemantic = IBlitUtilities::EAS_NONE_OR_PREMULTIPLIED;

				const core::vectorSIMDf scaleX(0.35f, 1.f, 1.f, 1.f);
				const core::vectorSIMDf scaleY(1.f, 1.f, 1.f, 1.f);
				const core::vectorSIMDf scaleZ(1.f, 1.f, 1.f, 1.f);

				auto kernelX = ScaledMitchellKernel(scaleX, asset::CMitchellImageFilterKernel());
				auto kernelY = ScaledMitchellKernel(scaleY, asset::CMitchellImageFilterKernel());
				auto kernelZ = ScaledMitchellKernel(scaleZ, asset::CMitchellImageFilterKernel());

				using LutDataType = uint16_t;
				blitTest<LutDataType>(std::move(inImage), outImageDim, kernelX, kernelY, kernelZ, alphaSemantic);
			}

			if (1)
			{
				logger->log("Test #2");

				const char* pathToInputImage = "../../media/colorexr.exr";
				core::smart_refctd_ptr<asset::ICPUImage> inImage = loadImage(pathToInputImage);
				if (!inImage)
					FATAL_LOG("Failed to load the image at path %s\n", pathToInputImage);

				const auto& inExtent = inImage->getCreationParameters().extent;
				const auto layerCount = inImage->getCreationParameters().arrayLayers;
				const core::vectorSIMDu32 outImageDim(inExtent.width / 3u, inExtent.height / 7u, inExtent.depth, layerCount);
				const IBlitUtilities::E_ALPHA_SEMANTIC alphaSemantic = IBlitUtilities::EAS_NONE_OR_PREMULTIPLIED;

				const core::vectorSIMDf scaleX(1.f, 1.f, 1.f, 1.f);
				const core::vectorSIMDf scaleY(1.f, 1.f, 1.f, 1.f);
				const core::vectorSIMDf scaleZ(1.f, 1.f, 1.f, 1.f);

				auto kernelX = ScaledMitchellKernel(scaleX, asset::CMitchellImageFilterKernel());
				auto kernelY = ScaledMitchellKernel(scaleY, asset::CMitchellImageFilterKernel());
				auto kernelZ = ScaledMitchellKernel(scaleZ, asset::CMitchellImageFilterKernel());

				using LutDataType = float;
				blitTest<LutDataType>(std::move(inImage), outImageDim, kernelX, kernelY, kernelZ, alphaSemantic);
			}

			if (1)
			{
				logger->log("Test #3");

				const auto layerCount = 1u;
				const core::vectorSIMDu32 inImageDim(2u, 3u, 4u, layerCount);
				const asset::IImage::E_TYPE inImageType = asset::IImage::ET_3D;
				const asset::E_FORMAT inImageFormat = asset::EF_R32G32B32A32_SFLOAT;
				auto inImage = createCPUImage(inImageDim, inImageType, inImageFormat, 1, true);

				const core::vectorSIMDu32 outImageDim(3u, 4u, 2u, layerCount);
				const IBlitUtilities::E_ALPHA_SEMANTIC alphaSemantic = IBlitUtilities::EAS_NONE_OR_PREMULTIPLIED;

				const core::vectorSIMDf scaleX(0.35f, 1.f, 1.f, 1.f);
				const core::vectorSIMDf scaleY(1.f, 9.f/16.f, 1.f, 1.f);
				const core::vectorSIMDf scaleZ(1.f, 1.f, 1.f, 1.f);

				auto kernelX = ScaledMitchellKernel(scaleX, asset::CMitchellImageFilterKernel());
				auto kernelY = ScaledMitchellKernel(scaleY, asset::CMitchellImageFilterKernel());
				auto kernelZ = ScaledMitchellKernel(scaleZ, asset::CMitchellImageFilterKernel());

				using LutDataType = uint16_t;
				blitTest<LutDataType>(std::move(inImage), outImageDim, kernelX, kernelY, kernelZ, alphaSemantic);
			}

			if (0)
			{
				logger->log("Test #4");

				// TODO(achal): Need to change this path.
				const char* pathToInputImage = "alpha_test_input.exr";
				core::smart_refctd_ptr<asset::ICPUImage> inImage = loadImage(pathToInputImage);
				if (!inImage)
					FATAL_LOG("Failed to load the image at path %s\n", pathToInputImage);

				const auto& inExtent = inImage->getCreationParameters().extent;
				const auto layerCount = inImage->getCreationParameters().arrayLayers;
				const core::vectorSIMDu32 outImageDim(inExtent.width / 3u, inExtent.height / 7u, inExtent.depth, layerCount);
				const IBlitUtilities::E_ALPHA_SEMANTIC alphaSemantic = IBlitUtilities::EAS_REFERENCE_OR_COVERAGE;
				const float referenceAlpha = 0.5f;
				const auto alphaBinCount = 1024;

				const core::vectorSIMDf scaleX(1.f, 1.f, 1.f, 1.f);
				const core::vectorSIMDf scaleY(1.f, 1.f, 1.f, 1.f);
				const core::vectorSIMDf scaleZ(1.f, 1.f, 1.f, 1.f);

				auto kernelX = ScaledMitchellKernel(scaleX, asset::CMitchellImageFilterKernel());
				auto kernelY = ScaledMitchellKernel(scaleY, asset::CMitchellImageFilterKernel());
				auto kernelZ = ScaledMitchellKernel(scaleZ, asset::CMitchellImageFilterKernel());

				using LutDataType = float;
				blitTest<LutDataType>(std::move(inImage), outImageDim, kernelX, kernelY, kernelZ, alphaSemantic, referenceAlpha, alphaBinCount);
			}

			if (1)
			{
				logger->log("Test #5");

				const auto layerCount = 1;
				const core::vectorSIMDu32 inImageDim(257u, 129u, 63u, layerCount);
				const asset::IImage::E_TYPE inImageType = asset::IImage::ET_3D;
				const asset::E_FORMAT inImageFormat = asset::EF_B10G11R11_UFLOAT_PACK32;
				auto inImage = createCPUImage(inImageDim, inImageType, inImageFormat, 1, true);

				const core::vectorSIMDu32 outImageDim(256u, 128u, 64u, layerCount);
				const IBlitUtilities::E_ALPHA_SEMANTIC alphaSemantic = IBlitUtilities::EAS_NONE_OR_PREMULTIPLIED;

				const core::vectorSIMDf scaleX(1.f, 1.f, 1.f, 1.f);
				const core::vectorSIMDf scaleY(1.f, 1.f, 1.f, 1.f);
				const core::vectorSIMDf scaleZ(1.f, 1.f, 1.f, 1.f);

				auto kernelX = ScaledMitchellKernel(scaleX, asset::CMitchellImageFilterKernel());
				auto kernelY = ScaledMitchellKernel(scaleY, asset::CMitchellImageFilterKernel());
				auto kernelZ = ScaledMitchellKernel(scaleZ, asset::CMitchellImageFilterKernel());

				using LutDataType = uint16_t;
				blitTest<LutDataType>(std::move(inImage), outImageDim, kernelX, kernelY, kernelZ, alphaSemantic);
			}

			if (1)
			{
				const auto layerCount = 7;
				logger->log("Test #6");
				const core::vectorSIMDu32 inImageDim(511u, 1024u, 1u, layerCount);
				const asset::IImage::E_TYPE inImageType = asset::IImage::ET_2D;
				const asset::E_FORMAT inImageFormat = EF_R16G16B16A16_SNORM;
				auto inImage = createCPUImage(inImageDim, inImageType, inImageFormat, 1, true);

				const core::vectorSIMDu32 outImageDim(512u, 257u, 1u, layerCount);
				const IBlitUtilities::E_ALPHA_SEMANTIC alphaSemantic = IBlitUtilities::EAS_REFERENCE_OR_COVERAGE;
				const float referenceAlpha = 0.5f;
				const auto alphaBinCount = 4096;

				const core::vectorSIMDf scaleX(1.f, 1.f, 1.f, 1.f);
				const core::vectorSIMDf scaleY(1.f, 1.f, 1.f, 1.f);
				const core::vectorSIMDf scaleZ(1.f, 1.f, 1.f, 1.f);

				auto kernelX = ScaledMitchellKernel(scaleX, asset::CMitchellImageFilterKernel());
				auto kernelY = ScaledMitchellKernel(scaleY, asset::CMitchellImageFilterKernel());
				auto kernelZ = ScaledMitchellKernel(scaleZ, asset::CMitchellImageFilterKernel());

				using LutDataType = float;
				blitTest<LutDataType>(std::move(inImage), outImageDim, kernelX, kernelY, kernelZ, alphaSemantic, referenceAlpha, alphaBinCount);
			}
		}
	}

	void onAppTerminated_impl() override
	{
		logicalDevice->waitIdle();
	}

	void workLoopBody() override
	{
	}

	bool keepRunning() override
	{
		return false;
	}

private:
	template<typename LutDataType, typename KernelX, typename KernelY, typename KernelZ>
	void blitTest(core::smart_refctd_ptr<asset::ICPUImage>&& inImageCPU, const core::vectorSIMDu32& outExtent, const KernelX& kernelX, const KernelY& kernelY, const KernelZ& kernelZ, const asset::IBlitUtilities::E_ALPHA_SEMANTIC alphaSemantic, const float referenceAlpha = 0.f, const uint32_t alphaBinCount = asset::IBlitUtilities::DefaultAlphaBinCount)
	{
		assert(inImageCPU->getCreationParameters().mipLevels == 1);
		using BlitFilter = asset::CBlitImageFilter<asset::VoidSwizzle, asset::IdentityDither, void, false, KernelX, KernelY, KernelZ, LutDataType>;

		const asset::E_FORMAT inImageFormat = inImageCPU->getCreationParameters().format;
		const asset::E_FORMAT outImageFormat = inImageFormat; // I can test with different input and output image formats later
		const auto layerCount = inImageCPU->getCreationParameters().arrayLayers;
		assert(outExtent.w == layerCount);

		// CPU
		core::vector<uint8_t> cpuOutput(static_cast<uint64_t>(outExtent[0]) * outExtent[1] * outExtent[2] * asset::getTexelOrBlockBytesize(outImageFormat) * layerCount);
		{
			auto outImageCPU = createCPUImage(outExtent, inImageCPU->getCreationParameters().type, outImageFormat, 1);

			KernelX kernelX_(kernelX);
			KernelY kernelY_(kernelY);
			KernelZ kernelZ_(kernelZ);
			typename BlitFilter::state_type blitFilterState(std::move(kernelX_), std::move(kernelY_), std::move(kernelZ_));

			blitFilterState.inOffsetBaseLayer = core::vectorSIMDu32();
			blitFilterState.inExtentLayerCount = core::vectorSIMDu32(0u, 0u, 0u, layerCount) + inImageCPU->getMipSize();
			blitFilterState.inImage = inImageCPU.get();
			blitFilterState.outImage = outImageCPU.get();

			blitFilterState.outOffsetBaseLayer = core::vectorSIMDu32();
			const uint32_t outImageLayerCount = inImageCPU->getCreationParameters().arrayLayers;
			blitFilterState.outExtentLayerCount = core::vectorSIMDu32(outExtent[0], outExtent[1], outExtent[2], outImageLayerCount);

			blitFilterState.axisWraps[0] = asset::ISampler::ETC_CLAMP_TO_EDGE;
			blitFilterState.axisWraps[1] = asset::ISampler::ETC_CLAMP_TO_EDGE;
			blitFilterState.axisWraps[2] = asset::ISampler::ETC_CLAMP_TO_EDGE;
			blitFilterState.borderColor = asset::ISampler::E_TEXTURE_BORDER_COLOR::ETBC_FLOAT_OPAQUE_WHITE;

			blitFilterState.alphaSemantic = alphaSemantic;
			blitFilterState.alphaBinCount = alphaBinCount;

			blitFilterState.scratchMemoryByteSize = BlitFilter::getRequiredScratchByteSize(&blitFilterState);
			blitFilterState.scratchMemory = reinterpret_cast<uint8_t*>(_NBL_ALIGNED_MALLOC(blitFilterState.scratchMemoryByteSize, 32));

			if (!BlitFilter::blit_utils_t::template computeScaledKernelPhasedLUT<LutDataType>(blitFilterState.scratchMemory + BlitFilter::getScratchOffset(&blitFilterState, BlitFilter::ESU_SCALED_KERNEL_PHASED_LUT), blitFilterState.inExtentLayerCount, blitFilterState.outExtentLayerCount, blitFilterState.inImage->getCreationParameters().type, kernelX, kernelY, kernelZ))
				logger->log("Failed to compute the LUT for blitting\n", system::ILogger::ELL_ERROR);

			logger->log("CPU begin..");
			if (!BlitFilter::execute(core::execution::par_unseq, &blitFilterState))
				logger->log("Failed to blit\n", system::ILogger::ELL_ERROR);
			logger->log("CPU end..");

			if (alphaSemantic == IBlitUtilities::EAS_REFERENCE_OR_COVERAGE)
				logger->log("CPU alpha coverage: %f", system::ILogger::ELL_DEBUG, computeAlphaCoverage(referenceAlpha, outImageCPU.get()));

			memcpy(cpuOutput.data(), outImageCPU->getBuffer()->getPointer(), cpuOutput.size());

			_NBL_ALIGNED_FREE(blitFilterState.scratchMemory);
		}

		// GPU
		core::vector<uint8_t> gpuOutput(static_cast<uint64_t>(outExtent[0]) * outExtent[1] * outExtent[2] * asset::getTexelOrBlockBytesize(outImageFormat) * layerCount);
		{
			constexpr auto BlitWorkgroupSize = video::CComputeBlit::DefaultBlitWorkgroupSize;

			assert(inImageCPU->getCreationParameters().mipLevels == 1);

			auto transitionImageLayout = [this](core::smart_refctd_ptr<video::IGPUImage>&& image, const asset::IImage::E_LAYOUT finalLayout)
			{
				core::smart_refctd_ptr<video::IGPUCommandBuffer> cmdbuf = nullptr;
				logicalDevice->createCommandBuffers(commandPools[CommonAPI::InitOutput::EQT_COMPUTE][0].get(), video::IGPUCommandBuffer::EL_PRIMARY, 1u, &cmdbuf);

				auto fence = logicalDevice->createFence(video::IGPUFence::ECF_UNSIGNALED);

				video::IGPUCommandBuffer::SImageMemoryBarrier barrier = {};
				barrier.oldLayout = asset::IImage::EL_UNDEFINED;
				barrier.newLayout = finalLayout;
				barrier.srcQueueFamilyIndex = ~0u;
				barrier.dstQueueFamilyIndex = ~0u;
				barrier.image = image;
				barrier.subresourceRange.aspectMask = video::IGPUImage::EAF_COLOR_BIT;
				barrier.subresourceRange.levelCount = image->getCreationParameters().mipLevels;
				barrier.subresourceRange.layerCount = image->getCreationParameters().arrayLayers;

				cmdbuf->begin(video::IGPUCommandBuffer::EU_ONE_TIME_SUBMIT_BIT);
				cmdbuf->pipelineBarrier(asset::EPSF_TOP_OF_PIPE_BIT, asset::EPSF_BOTTOM_OF_PIPE_BIT, asset::EDF_NONE, 0u, nullptr, 0u, nullptr, 1u, &barrier);
				cmdbuf->end();

				video::IGPUQueue::SSubmitInfo submitInfo = {};
				submitInfo.commandBufferCount = 1u;
				submitInfo.commandBuffers = &cmdbuf.get();
				queues[CommonAPI::InitOutput::EQT_COMPUTE]->submit(1u, &submitInfo, fence.get());
				logicalDevice->blockForFences(1u, &fence.get());
			};

			core::smart_refctd_ptr<video::IGPUImage> inImage = nullptr;
			{
				cpu2gpuParams.beginCommandBuffers();
				auto gpuArray = cpu2gpu.getGPUObjectsFromAssets(&inImageCPU, &inImageCPU + 1ull, cpu2gpuParams);
				cpu2gpuParams.waitForCreationToComplete();
				if (!gpuArray || gpuArray->size() < 1ull || (!(*gpuArray)[0]))
					FATAL_LOG("Cannot convert the inpute CPU image to GPU image\n");

				inImage = gpuArray->begin()[0];

				// Do layout transition to SHADER_READ_ONLY_OPTIMAL 
				// I think it might be a good idea to allow the user to change asset::ICPUImage's initialLayout and have the asset converter
				// do the layout transition for them.
				transitionImageLayout(core::smart_refctd_ptr(inImage), asset::IImage::EL_SHADER_READ_ONLY_OPTIMAL);
			}

			core::smart_refctd_ptr<video::IGPUImage> outImage = nullptr;
			{
				video::IGPUImage::SCreationParams creationParams = {};
				creationParams.flags = video::IGPUImage::ECF_MUTABLE_FORMAT_BIT;
				creationParams.type = inImage->getCreationParameters().type;
				creationParams.format = outImageFormat;
				creationParams.extent = { outExtent.x, outExtent.y, outExtent.z };
				creationParams.mipLevels = inImageCPU->getCreationParameters().mipLevels; // Asset converter will make the mip levels 10 for inImage, so use the original value of inImageCPU
				creationParams.arrayLayers = layerCount;
				creationParams.samples = video::IGPUImage::ESCF_1_BIT;
				creationParams.tiling = video::IGPUImage::ET_OPTIMAL;
				creationParams.usage = static_cast<video::IGPUImage::E_USAGE_FLAGS>(video::IGPUImage::EUF_STORAGE_BIT | video::IGPUImage::EUF_TRANSFER_SRC_BIT | video::IGPUImage::EUF_SAMPLED_BIT);
				
				outImage = logicalDevice->createImage(std::move(creationParams));
				auto memReqs = outImage->getMemoryReqs();
				memReqs.memoryTypeBits &= logicalDevice->getPhysicalDevice()->getDeviceLocalMemoryTypeBits();
				logicalDevice->allocate(memReqs, outImage.get());

				transitionImageLayout(core::smart_refctd_ptr(outImage), asset::IImage::EL_GENERAL);
			}

			// Create resources needed to do the blit
			auto blitFilter = video::CComputeBlit::create(core::smart_refctd_ptr(logicalDevice));

			const asset::E_FORMAT outImageViewFormat = blitFilter->getOutImageViewFormat(outImageFormat);

			const auto layersToBlit = layerCount;
			core::smart_refctd_ptr<video::IGPUImageView> inImageView = nullptr;
			core::smart_refctd_ptr<video::IGPUImageView> outImageView = nullptr;
			{
				video::IGPUImageView::SCreationParams creationParams = {};
				creationParams.image = inImage;
				creationParams.viewType = getImageViewTypeFromImageType_GPU(inImage->getCreationParameters().type);
				creationParams.format = inImage->getCreationParameters().format;
				creationParams.subresourceRange.aspectMask = video::IGPUImage::EAF_COLOR_BIT;
				creationParams.subresourceRange.baseMipLevel = 0;
				creationParams.subresourceRange.levelCount = 1;
				creationParams.subresourceRange.baseArrayLayer = 0;
				creationParams.subresourceRange.layerCount = layersToBlit;

				video::IGPUImageView::SCreationParams outCreationParams = creationParams;
				outCreationParams.image = outImage;
				outCreationParams.format = outImageViewFormat;

				inImageView = logicalDevice->createImageView(std::move(creationParams));
				outImageView = logicalDevice->createImageView(std::move(outCreationParams));
			}

			core::smart_refctd_ptr<video::IGPUImageView> normalizationInImageView = outImageView;
			core::smart_refctd_ptr<video::IGPUImage> normalizationInImage = outImage;
			auto normalizationInFormat = outImageFormat;
			if (alphaSemantic == IBlitUtilities::EAS_REFERENCE_OR_COVERAGE)
			{
				normalizationInFormat = video::CComputeBlit::getCoverageAdjustmentIntermediateFormat(outImageFormat);

				if (normalizationInFormat != outImageFormat)
				{
					video::IGPUImage::SCreationParams creationParams;
					creationParams = outImage->getCreationParameters();
					creationParams.format = normalizationInFormat;
					creationParams.usage = static_cast<video::IGPUImage::E_USAGE_FLAGS>(video::IGPUImage::EUF_STORAGE_BIT | video::IGPUImage::EUF_SAMPLED_BIT);
					normalizationInImage = logicalDevice->createImage(std::move(creationParams));
					auto memReqs = normalizationInImage->getMemoryReqs();
					memReqs.memoryTypeBits &= logicalDevice->getPhysicalDevice()->getDeviceLocalMemoryTypeBits();
					logicalDevice->allocate(memReqs, normalizationInImage.get());
					transitionImageLayout(core::smart_refctd_ptr(normalizationInImage), asset::IImage::EL_GENERAL); // First we do the blit which requires storage image so starting layout is GENERAL

					video::IGPUImageView::SCreationParams viewCreationParams = {};
					viewCreationParams.image = normalizationInImage;
					viewCreationParams.viewType = getImageViewTypeFromImageType_GPU(inImage->getCreationParameters().type);
					viewCreationParams.format = normalizationInImage->getCreationParameters().format;
					viewCreationParams.subresourceRange.aspectMask = video::IGPUImage::EAF_COLOR_BIT;
					viewCreationParams.subresourceRange.baseMipLevel = 0;
					viewCreationParams.subresourceRange.levelCount = 1;
					viewCreationParams.subresourceRange.baseArrayLayer = 0;
					viewCreationParams.subresourceRange.layerCount = layersToBlit;

					normalizationInImageView = logicalDevice->createImageView(std::move(viewCreationParams));
				}
			}

			const core::vectorSIMDu32 inExtent(inImage->getCreationParameters().extent.width, inImage->getCreationParameters().extent.height, inImage->getCreationParameters().extent.depth, 1);
			const auto inImageType = inImage->getCreationParameters().type;

			// create scratch buffer
			core::smart_refctd_ptr<video::IGPUBuffer> coverageAdjustmentScratchBuffer = nullptr;
			{
				const size_t scratchSize = blitFilter->getCoverageAdjustmentScratchSize(alphaSemantic, inImageType, alphaBinCount, layersToBlit);
				if (scratchSize > 0)
				{
					video::IGPUBuffer::SCreationParams creationParams = {};
					creationParams.size = scratchSize;
					creationParams.usage = static_cast<video::IGPUBuffer::E_USAGE_FLAGS>(video::IGPUBuffer::EUF_TRANSFER_DST_BIT | video::IGPUBuffer::EUF_STORAGE_BUFFER_BIT);

					coverageAdjustmentScratchBuffer = logicalDevice->createBuffer(std::move(creationParams));
					auto memReqs = coverageAdjustmentScratchBuffer->getMemoryReqs();
					memReqs.memoryTypeBits &= physicalDevice->getDeviceLocalMemoryTypeBits();
					logicalDevice->allocate(memReqs, coverageAdjustmentScratchBuffer.get());

					asset::SBufferRange<video::IGPUBuffer> bufferRange = {};
					bufferRange.offset = 0ull;
					bufferRange.size = coverageAdjustmentScratchBuffer->getSize();
					bufferRange.buffer = coverageAdjustmentScratchBuffer;

					core::vector<uint32_t> fillValues(scratchSize / sizeof(uint32_t), 0u);
					utilities->updateBufferRangeViaStagingBufferAutoSubmit(bufferRange, fillValues.data(), queues[CommonAPI::InitOutput::EQT_COMPUTE]);
				}
			}

			// create scaledKernelPhasedLUT and its view
			core::smart_refctd_ptr<video::IGPUBufferView> scaledKernelPhasedLUTView = nullptr;
			{
				using blit_utils_t = asset::CBlitUtilities<KernelX, KernelY, KernelZ>;
				const auto lutSize = blit_utils_t::template getScaledKernelPhasedLUTSize<LutDataType>(inExtent, outExtent, inImageType, kernelX, kernelY, kernelZ);

				uint8_t* lutMemory = reinterpret_cast<uint8_t*>(_NBL_ALIGNED_MALLOC(lutSize, 32));
				if (!blit_utils_t::template computeScaledKernelPhasedLUT<LutDataType>(lutMemory, inExtent, outExtent, inImageType, kernelX, kernelY, kernelZ))
					FATAL_LOG("Failed to compute scaled kernel phased LUT for the GPU case!\n");

				video::IGPUBuffer::SCreationParams creationParams = {};
				creationParams.usage = static_cast<video::IGPUBuffer::E_USAGE_FLAGS>(video::IGPUBuffer::EUF_STORAGE_BUFFER_BIT | video::IGPUBuffer::EUF_UNIFORM_TEXEL_BUFFER_BIT | video::IGPUBuffer::EUF_TRANSFER_DST_BIT);
				creationParams.size = lutSize;
				auto scaledKernelPhasedLUT = logicalDevice->createBuffer(std::move(creationParams));
				auto memReqs = scaledKernelPhasedLUT->getMemoryReqs();
				memReqs.memoryTypeBits &= physicalDevice->getDeviceLocalMemoryTypeBits();
				logicalDevice->allocate(memReqs, scaledKernelPhasedLUT.get());

				// fill it up with data
				asset::SBufferRange<video::IGPUBuffer> bufferRange = {};
				bufferRange.offset = 0ull;
				bufferRange.size = lutSize;
				bufferRange.buffer = scaledKernelPhasedLUT;
				utilities->updateBufferRangeViaStagingBufferAutoSubmit(bufferRange, lutMemory, queues[CommonAPI::InitOutput::EQT_COMPUTE]);

				asset::E_FORMAT bufferViewFormat;
				if constexpr (std::is_same_v<LutDataType, uint16_t>)
					bufferViewFormat = asset::EF_R16G16B16A16_SFLOAT;
				else if constexpr (std::is_same_v<LutDataType, float>)
					bufferViewFormat = asset::EF_R32G32B32A32_SFLOAT;
				else
					assert(false);

				scaledKernelPhasedLUTView = logicalDevice->createBufferView(scaledKernelPhasedLUT.get(), bufferViewFormat, 0ull, scaledKernelPhasedLUT->getSize());

				_NBL_ALIGNED_FREE(lutMemory);
			}

			auto blitDSLayout = blitFilter->getDefaultBlitDescriptorSetLayout(alphaSemantic);
			auto kernelWeightsDSLayout = blitFilter->getDefaultKernelWeightsDescriptorSetLayout();
			auto blitPipelineLayout = blitFilter->getDefaultBlitPipelineLayout(alphaSemantic);

			video::IGPUDescriptorSetLayout* blitDSLayouts_raw[] = { blitDSLayout.get(), kernelWeightsDSLayout.get() };
			uint32_t dsCounts[] = { 2, 1 };
			auto descriptorPool = logicalDevice->createDescriptorPoolForDSLayouts(video::IDescriptorPool::ECF_NONE, blitDSLayouts_raw, blitDSLayouts_raw + 2ull, dsCounts);

			core::smart_refctd_ptr<video::IGPUComputePipeline> blitPipeline = nullptr;
			core::smart_refctd_ptr<video::IGPUDescriptorSet> blitDS = nullptr;
			core::smart_refctd_ptr<video::IGPUDescriptorSet> blitWeightsDS = nullptr;

			core::smart_refctd_ptr<video::IGPUComputePipeline> alphaTestPipeline = nullptr;
			core::smart_refctd_ptr<video::IGPUComputePipeline> normalizationPipeline = nullptr;
			core::smart_refctd_ptr<video::IGPUDescriptorSet> normalizationDS = nullptr;

			if (alphaSemantic == IBlitUtilities::EAS_REFERENCE_OR_COVERAGE)
			{
				alphaTestPipeline = blitFilter->getAlphaTestPipeline(alphaBinCount, inImageType);
				normalizationPipeline = blitFilter->getNormalizationPipeline(normalizationInImage->getCreationParameters().type, outImageFormat, alphaBinCount);

				normalizationDS = logicalDevice->createDescriptorSet(descriptorPool.get(), core::smart_refctd_ptr(blitDSLayout));
				blitFilter->updateDescriptorSet(normalizationDS.get(), nullptr, normalizationInImageView, outImageView, coverageAdjustmentScratchBuffer, nullptr);
			}

			blitPipeline = blitFilter->getBlitPipeline(outImageFormat, inImageType, inExtent, outExtent, alphaSemantic, kernelX, kernelY, kernelZ, BlitWorkgroupSize, alphaBinCount);
			blitDS = logicalDevice->createDescriptorSet(descriptorPool.get(), core::smart_refctd_ptr(blitDSLayout));
			blitWeightsDS = logicalDevice->createDescriptorSet(descriptorPool.get(), core::smart_refctd_ptr(kernelWeightsDSLayout));

			blitFilter->updateDescriptorSet(blitDS.get(), blitWeightsDS.get(), inImageView, normalizationInImageView, coverageAdjustmentScratchBuffer, scaledKernelPhasedLUTView);

			logger->log("GPU begin..");
			blitFilter->blit<decltype(kernelX), decltype(kernelY), decltype(kernelZ)>(
				queues[CommonAPI::InitOutput::EQT_COMPUTE], alphaSemantic,
				blitDS.get(), alphaTestPipeline.get(),
				blitDS.get(), blitWeightsDS.get(), blitPipeline.get(),
				normalizationDS.get(), normalizationPipeline.get(),
				inExtent, inImageType, inImageFormat, normalizationInImage, kernelX, kernelY, kernelZ,
				layersToBlit,
				coverageAdjustmentScratchBuffer, referenceAlpha,
				alphaBinCount, BlitWorkgroupSize);
			logger->log("GPU end..");

			if (outImage->getCreationParameters().type == asset::IImage::ET_2D)
			{
				if (layerCount > 1)
				{
					// This can be removed once ext::ScreenShot::createScreenShot works for multiple layers.
					logger->log("Layer count (%d) is greater than 1 for a 2D image, not calculating GPU alpha coverage..\n", system::ILogger::ELL_WARNING, layerCount);
				}
				else
				{
					auto outCPUImageView = ext::ScreenShot::createScreenShot(
						logicalDevice.get(),
						queues[CommonAPI::InitOutput::EQT_COMPUTE],
						nullptr,
						outImageView.get(),
						asset::EAF_NONE,
						asset::IImage::EL_GENERAL);

					if (alphaSemantic == IBlitUtilities::EAS_REFERENCE_OR_COVERAGE)
						logger->log("GPU alpha coverage: %f", system::ILogger::ELL_DEBUG, computeAlphaCoverage(referenceAlpha, outCPUImageView->getCreationParameters().image.get()));
				}
			}

			// download results to check
			{
				const size_t downloadSize = gpuOutput.size();

				video::IGPUBuffer::SCreationParams creationParams = {};
				creationParams.usage = video::IGPUBuffer::EUF_TRANSFER_DST_BIT;
				creationParams.size = downloadSize;
				core::smart_refctd_ptr<video::IGPUBuffer> downloadBuffer = logicalDevice->createBuffer(std::move(creationParams));

				auto memReqs = downloadBuffer->getMemoryReqs();
				memReqs.memoryTypeBits &= physicalDevice->getDownStreamingMemoryTypeBits();
				logicalDevice->allocate(memReqs, downloadBuffer.get());

				core::smart_refctd_ptr<video::IGPUCommandBuffer> cmdbuf = nullptr;
				logicalDevice->createCommandBuffers(commandPools[CommonAPI::InitOutput::EQT_COMPUTE][0].get(), video::IGPUCommandBuffer::EL_PRIMARY, 1u, &cmdbuf);
				auto fence = logicalDevice->createFence(video::IGPUFence::ECF_UNSIGNALED);

				cmdbuf->begin(video::IGPUCommandBuffer::EU_ONE_TIME_SUBMIT_BIT);

				asset::ICPUImage::SBufferCopy downloadRegion = {};
				downloadRegion.imageSubresource.aspectMask = video::IGPUImage::EAF_COLOR_BIT;
				downloadRegion.imageSubresource.layerCount = layerCount;
				downloadRegion.imageExtent = outImage->getCreationParameters().extent;

				// Todo(achal): Transition layout to TRANSFER_SRC_OPTIMAL
				cmdbuf->copyImageToBuffer(outImage.get(), asset::IImage::EL_GENERAL, downloadBuffer.get(), 1u, &downloadRegion);

				cmdbuf->end();

				video::IGPUQueue::SSubmitInfo submitInfo = {};
				submitInfo.commandBufferCount = 1u;
				submitInfo.commandBuffers = &cmdbuf.get();
				queues[CommonAPI::InitOutput::EQT_COMPUTE]->submit(1u, &submitInfo, fence.get());

				logicalDevice->blockForFences(1u, &fence.get());

				video::IDeviceMemoryAllocation::MappedMemoryRange memoryRange = {};
				memoryRange.memory = downloadBuffer->getBoundMemory();
				memoryRange.length = downloadSize;
				uint8_t* mappedGPUData = reinterpret_cast<uint8_t*>(logicalDevice->mapMemory(memoryRange));

				memcpy(gpuOutput.data(), mappedGPUData, gpuOutput.size());
				logicalDevice->unmapMemory(downloadBuffer->getBoundMemory());
			}
		}

		assert(gpuOutput.size() == cpuOutput.size());

		const uint32_t outChannelCount = asset::getFormatChannelCount(outImageFormat);

		double sqErr = 0.0;
		uint8_t* cpuBytePtr = cpuOutput.data();
		uint8_t* gpuBytePtr = gpuOutput.data();
		const auto layerSize = outExtent[2]*outExtent[1]*outExtent[0]*asset::getTexelOrBlockBytesize(outImageFormat);

		for (auto layer = 0; layer < layerCount; ++layer)
		{
			for (uint64_t k = 0u; k < outExtent[2]; ++k)
			{
				for (uint64_t j = 0u; j < outExtent[1]; ++j)
				{
					for (uint64_t i = 0; i < outExtent[0]; ++i)
					{
						const uint64_t pixelIndex = (k * outExtent[1] * outExtent[0]) + (j * outExtent[0]) + i;
						core::vectorSIMDu32 dummy;

						const void* cpuEncodedPixel = cpuBytePtr + (layer * layerSize) + pixelIndex * asset::getTexelOrBlockBytesize(outImageFormat);
						const void* gpuEncodedPixel = gpuBytePtr + (layer * layerSize) + pixelIndex * asset::getTexelOrBlockBytesize(outImageFormat);

						double cpuDecodedPixel[4];
						asset::decodePixelsRuntime(outImageFormat, &cpuEncodedPixel, cpuDecodedPixel, dummy.x, dummy.y);

						double gpuDecodedPixel[4];
						asset::decodePixelsRuntime(outImageFormat, &gpuEncodedPixel, gpuDecodedPixel, dummy.x, dummy.y);

						for (uint32_t ch = 0u; ch < outChannelCount; ++ch)
						{
#if 0
							if (std::isnan(cpuDecodedPixel[ch]) || std::isinf(cpuDecodedPixel[ch]))
								__debugbreak();

							if (std::isnan(gpuDecodedPixel[ch]) || std::isinf(gpuDecodedPixel[ch]))
								__debugbreak();

							if (std::abs(cpuDecodedPixel[ch] - gpuDecodedPixel[ch]) > 1e-3f)
								__debugbreak();
#endif

							sqErr += (cpuDecodedPixel[ch] - gpuDecodedPixel[ch]) * (cpuDecodedPixel[ch] - gpuDecodedPixel[ch]);
					}
				}
			}
		}
	}

		// compute alpha coverage
		const uint64_t totalPixelCount = static_cast<uint64_t>(outExtent[2]) * outExtent[1] * outExtent[0]*layerCount;
		const double RMSE = core::sqrt(sqErr / totalPixelCount);
		logger->log("RMSE: %f\n", system::ILogger::ELL_DEBUG, RMSE);
	}

	float computeAlphaCoverage(const double referenceAlpha, asset::ICPUImage* image)
	{
		const uint32_t mipLevel = 0u;

		uint32_t alphaTestPassCount = 0u;

		const auto& extent = image->getCreationParameters().extent;
		const auto layerCount = image->getCreationParameters().arrayLayers;

		for (auto layer = 0; layer < layerCount; ++layer)
		{
			for (uint32_t z = 0u; z < extent.depth; ++z)
			{
				for (uint32_t y = 0u; y < extent.height; ++y)
				{
					for (uint32_t x = 0u; x < extent.width; ++x)
					{
						const core::vectorSIMDu32 texCoord(x, y, z, layer);
						core::vectorSIMDu32 dummy;
						const void* encodedPixel = image->getTexelBlockData(mipLevel, texCoord, dummy);

						double decodedPixel[4];
						asset::decodePixelsRuntime(image->getCreationParameters().format, &encodedPixel, decodedPixel, dummy.x, dummy.y);

						if (decodedPixel[3] > referenceAlpha)
							++alphaTestPassCount;
					}
				}
			}
		}

		const float alphaCoverage = float(alphaTestPassCount) / float(extent.width * extent.height * extent.depth*layerCount);
		return alphaCoverage;
	};

	core::smart_refctd_ptr<nbl::ui::IWindowManager> windowManager;
	core::smart_refctd_ptr<nbl::ui::IWindow> window;
	core::smart_refctd_ptr<CommonAPI::CommonAPIEventCallback> windowCb;
	core::smart_refctd_ptr<nbl::video::IAPIConnection> apiConnection;
	core::smart_refctd_ptr<nbl::video::ISurface> surface;
	core::smart_refctd_ptr<nbl::video::IUtilities> utilities;
	core::smart_refctd_ptr<nbl::video::ILogicalDevice> logicalDevice;
	video::IPhysicalDevice* physicalDevice;
	std::array<video::IGPUQueue*, CommonAPI::InitOutput::MaxQueuesCount> queues;
	core::smart_refctd_ptr<nbl::video::ISwapchain> swapchain;
	core::smart_refctd_ptr<video::IGPURenderpass> renderpass = nullptr;
	std::array<nbl::core::smart_refctd_ptr<video::IGPUFramebuffer>, CommonAPI::InitOutput::MaxSwapChainImageCount> fbos;
	std::array<std::array<nbl::core::smart_refctd_ptr<nbl::video::IGPUCommandPool>, CommonAPI::InitOutput::MaxFramesInFlight>, CommonAPI::InitOutput::MaxQueuesCount> commandPools;
	core::smart_refctd_ptr<nbl::system::ISystem> system;
	core::smart_refctd_ptr<nbl::asset::IAssetManager> assetManager;
	video::IGPUObjectFromAssetConverter::SParams cpu2gpuParams;
	core::smart_refctd_ptr<nbl::system::ILogger> logger;
	core::smart_refctd_ptr<CommonAPI::InputSystem> inputSystem;
	video::IGPUObjectFromAssetConverter cpu2gpu;

public:
	void setWindow(core::smart_refctd_ptr<nbl::ui::IWindow>&& wnd) override
	{
		window = std::move(wnd);
	}
	void setSystem(core::smart_refctd_ptr<nbl::system::ISystem>&& s) override
	{
		system = std::move(s);
	}
	nbl::ui::IWindow* getWindow() override
	{
		return window.get();
	}
	video::IAPIConnection* getAPIConnection() override
	{
		return apiConnection.get();
	}
	video::ILogicalDevice* getLogicalDevice()  override
	{
		return logicalDevice.get();
	}
	video::IGPURenderpass* getRenderpass() override
	{
		return renderpass.get();
	}
	void setSurface(core::smart_refctd_ptr<video::ISurface>&& s) override
	{
		surface = std::move(s);
	}
	void setFBOs(std::vector<core::smart_refctd_ptr<video::IGPUFramebuffer>>& f) override
	{
		for (int i = 0; i < f.size(); i++)
		{
			fbos[i] = core::smart_refctd_ptr(f[i]);
		}
	}
	void setSwapchain(core::smart_refctd_ptr<video::ISwapchain>&& s) override
	{
		swapchain = std::move(s);
	}
	uint32_t getSwapchainImageCount() override
	{
		return SC_IMG_COUNT;
	}
	virtual nbl::asset::E_FORMAT getDepthFormat() override
	{
		return nbl::asset::EF_D32_SFLOAT;
	}

	APP_CONSTRUCTOR(BlitFilterTestApp);
};

NBL_COMMON_API_MAIN(BlitFilterTestApp)

extern "C" {  _declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001; }