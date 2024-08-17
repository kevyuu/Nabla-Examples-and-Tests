#ifndef __NBL_THIS_EXAMPLE_SCENE_H_INCLUDED__
#define __NBL_THIS_EXAMPLE_SCENE_H_INCLUDED__

#include <nabla.h>

#include "nbl/asset/utils/CGeometryCreator.h"
#include "geometry/creator/spirv/builtin/CArchive.h"
#include "geometry/creator/spirv/builtin/builtinResources.h"

/*
	Rendering to offline framebuffer which we don't present, color 
	scene attachment texture we use for second UI renderpass 
	sampling it & rendering into desired GUI area
*/

class CScene final : public nbl::core::IReferenceCounted
{
public:
	CScene(nbl::core::smart_refctd_ptr<nbl::video::ILogicalDevice> _device, nbl::core::smart_refctd_ptr<nbl::system::ILogger> _logger, const nbl::asset::IGeometryCreator* _geometryCreator)
		: m_device(nbl::core::smart_refctd_ptr(_device)), m_logger(nbl::core::smart_refctd_ptr(_logger))
	{
		auto pipelineLayout = createPipelineLayoutAndDS();
		createOfflineSceneRenderPass();
		createOfflineSceneFramebuffer();

		struct
		{
			const nbl::asset::IGeometryCreator* gc;

			const std::vector<OBJECT_CPU> basic =
			{
				{.meta = {.type = EOT_CUBE, .name = "Cube Mesh" }, .data = gc->createCubeMesh(nbl::core::vector3df(1.f, 1.f, 1.f)) },
				{.meta = {.type = EOT_SPHERE, .name = "Sphere Mesh" }, .data = gc->createSphereMesh(2, 16, 16) },
				{.meta = {.type = EOT_CYLINDER, .name = "Cylinder Mesh" }, .data = gc->createCylinderMesh(2, 2, 20) },
				{.meta = {.type = EOT_RECTANGLE, .name = "Rectangle Mesh" }, .data = gc->createRectangleMesh(nbl::core::vector2df_SIMD(1.5, 3)) },
				{.meta = {.type = EOT_DISK, .name = "Disk Mesh" }, .data = gc->createDiskMesh(2, 30) },
				{.meta = {.type = EOT_ARROW, .name = "Arrow Mesh" }, .data = gc->createArrowMesh() },
			}, cone =
			{
				{.meta = {.type = EOT_CONE, .name = "Cone Mesh" }, .data = gc->createConeMesh(2, 3, 10) }
			}, ico =
			{
				{.meta = {.type = EOT_ICOSPHERE, .name = "Icoshpere Mesh" }, .data = gc->createIcoSphere(1, 3, true) }
			};
		} geometries { .gc = _geometryCreator };

		auto createBundleGPUData = [&]<nbl::core::StringLiteral vPath, nbl::core::StringLiteral fPath>(const std::vector<OBJECT_CPU>& objects) -> void
		{
			SHADERS_GPU shaders;
			{
				struct
				{
					const nbl::system::SBuiltinFile vertex = ::geometry::creator::spirv::builtin::get_resource<vPath>();
					const nbl::system::SBuiltinFile fragment = ::geometry::creator::spirv::builtin::get_resource<fPath>();
				} spirv;

				auto createShader = [&](const nbl::system::SBuiltinFile& in, nbl::asset::IShader::E_SHADER_STAGE stage) -> nbl::core::smart_refctd_ptr<nbl::video::IGPUShader>
				{
					const auto buffer = nbl::core::make_smart_refctd_ptr<nbl::asset::CCustomAllocatorCPUBuffer<nbl::core::null_allocator<uint8_t>, true> >(in.size, (void*)in.contents, nbl::core::adopt_memory);
					const auto shader = nbl::core::make_smart_refctd_ptr<nbl::asset::ICPUShader>(nbl::core::smart_refctd_ptr(buffer), stage, nbl::asset::IShader::E_CONTENT_TYPE::ECT_SPIRV, "");

					return m_device->createShader(shader.get()); // also first should look for cached/already created to not duplicate
				};

				shaders.vertex = createShader(spirv.vertex, nbl::asset::IShader::E_SHADER_STAGE::ESS_VERTEX);
				shaders.fragment = createShader(spirv.fragment, nbl::asset::IShader::E_SHADER_STAGE::ESS_FRAGMENT);
			}

			for (const auto& inObject : objects)
			{
				auto& outObject = referenceObjects.emplace_back();

				if (!createGPUData<vPath, fPath>(inObject, outObject, shaders, pipelineLayout.get()))
					m_logger->log("Could not create GPU Scene Object data!", nbl::system::ILogger::ELL_ERROR);
			}
		};

		// we create vertex & index buffers for basic geometries + their pipelines
		createBundleGPUData.template operator() < NBL_CORE_UNIQUE_STRING_LITERAL_TYPE("geometryCreator/spirv/gc.basic.vertex.spv"), NBL_CORE_UNIQUE_STRING_LITERAL_TYPE("geometryCreator/spirv/gc.basic.fragment.spv") > (geometries.basic);
		createBundleGPUData.template operator() < NBL_CORE_UNIQUE_STRING_LITERAL_TYPE("geometryCreator/spirv/gc.cone.vertex.spv"), NBL_CORE_UNIQUE_STRING_LITERAL_TYPE("geometryCreator/spirv/gc.basic.fragment.spv") > (geometries.cone);		// note we reuse basic fragment shader
		createBundleGPUData.template operator() < NBL_CORE_UNIQUE_STRING_LITERAL_TYPE("geometryCreator/spirv/gc.ico.vertex.spv"), NBL_CORE_UNIQUE_STRING_LITERAL_TYPE("geometryCreator/spirv/gc.basic.fragment.spv") > (geometries.ico);		// note we reuse basic fragment shader
	}
	~CScene() {}

	bool render()
	{

	}
private:

	_NBL_STATIC_INLINE_CONSTEXPR uint32_t FRAMEBUFFER_W = 1280, FRAMEBUFFER_H = 720;
	_NBL_STATIC_INLINE_CONSTEXPR auto COLOR_FBO_ATTACHMENT_FORMAT = nbl::asset::EF_R8G8B8A8_SRGB, DEPTH_FBO_ATTACHMENT_FORMAT = nbl::asset::EF_D16_UNORM;
	_NBL_STATIC_INLINE_CONSTEXPR auto SAMPLES = nbl::video::IGPUImage::ESCF_1_BIT;

	enum E_OBJECT_TYPE : uint8_t
	{
		EOT_CUBE,
		EOT_SPHERE,
		EOT_CYLINDER,
		EOT_RECTANGLE,
		EOT_DISK,
		EOT_ARROW,
		EOT_CONE,
		EOT_ICOSPHERE
	};

	struct OBJECT_META
	{
		E_OBJECT_TYPE type;
		std::string_view name;
	};

	struct OBJECT_GPU
	{
		nbl::core::smart_refctd_ptr<nbl::video::IGPUGraphicsPipeline> pipeline;
		nbl::core::smart_refctd_ptr<nbl::video::IGPUBuffer> vertexBuffer, indexBuffer;
		nbl::asset::E_INDEX_TYPE indexType;
		uint32_t indexCount;
	};

	struct OBJECT_CPU
	{
		OBJECT_META meta;
		nbl::asset::CGeometryCreator::return_type data;
	};

	struct SHADERS_GPU
	{
		nbl::core::smart_refctd_ptr<nbl::video::IGPUShader> vertex, geometry, fragment;
	};

	using OBJECT_DATA = std::pair<OBJECT_GPU, OBJECT_META>;

	template<nbl::core::StringLiteral vPath, nbl::core::StringLiteral fPath>
	bool createGPUData(const OBJECT_CPU& inData, OBJECT_DATA& outData, const SHADERS_GPU& shaders, const nbl::video::IGPUPipelineLayout* pipelineLayout)
	{
		// meta
		outData.second.name = inData.meta.name;
		outData.second.type = inData.meta.type;

		nbl::asset::SBlendParams blendParams{};
		{
			blendParams.logicOp = nbl::asset::ELO_NO_OP;

			auto& param = blendParams.blendParams[0];
			param.srcColorFactor = nbl::asset::EBF_SRC_ALPHA;//VK_BLEND_FACTOR_SRC_ALPHA;
			param.dstColorFactor = nbl::asset::EBF_ONE_MINUS_SRC_ALPHA;//VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
			param.colorBlendOp = nbl::asset::EBO_ADD;//VK_BLEND_OP_ADD;
			param.srcAlphaFactor = nbl::asset::EBF_ONE_MINUS_SRC_ALPHA;//VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
			param.dstAlphaFactor = nbl::asset::EBF_ZERO;//VK_BLEND_FACTOR_ZERO;
			param.alphaBlendOp = nbl::asset::EBO_ADD;//VK_BLEND_OP_ADD;
			param.colorWriteMask = (1u << 0u) | (1u << 1u) | (1u << 2u) | (1u << 3u);//VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
		}

		nbl::asset::SRasterizationParams rasterizationParams{};
		rasterizationParams.faceCullingMode = nbl::asset::EFCM_NONE;
		{
			const nbl::video::IGPUShader::SSpecInfo specs[] =
			{
				{.entryPoint = "VSMain", .shader = shaders.vertex.get() },
				{.entryPoint = "PSMain", .shader = shaders.fragment.get() }
			};

			nbl::video::IGPUGraphicsPipeline::SCreationParams params[1];
			{
				auto& param = params[0];
				param.layout = pipelineLayout;
				param.shaders = specs;
				param.renderpass = m_renderpass.get();
				param.cached = { .vertexInput = inData.data.inputParams, .primitiveAssembly = inData.data.assemblyParams, .rasterization = rasterizationParams, .blend = blendParams, .subpassIx = 0u };
			};

			outData.first.indexCount = inData.data.indexCount;
			outData.first.indexType = inData.data.indexType;

			// first should look for cached pipeline to not duplicate but lets leave how it is now
			// TODO: cache it & try to first lookup for it

			if (!m_device->createGraphicsPipelines(nullptr, params, &outData.first.pipeline))
				return false;

			if (!createVIBuffers(inData, outData))
				return false;

			return true;
		}
	}

	bool createVIBuffers(const OBJECT_CPU& inData, OBJECT_DATA& outData)
	{
		const auto mask = m_device->getPhysicalDevice()->getUpStreamingMemoryTypeBits();

		auto vBuffer = nbl::core::smart_refctd_ptr(inData.data.bindings[0].buffer); // no offset
		auto iBuffer = nbl::core::smart_refctd_ptr(inData.data.indexBuffer.buffer); // no offset

		outData.first.vertexBuffer = m_device->createBuffer({ {.size = vBuffer->getSize(), .usage = nbl::core::bitflag(nbl::asset::IBuffer::EUF_VERTEX_BUFFER_BIT) | nbl::asset::IBuffer::EUF_TRANSFER_DST_BIT | nbl::asset::IBuffer::EUF_INLINE_UPDATE_VIA_CMDBUF} });
		outData.first.indexBuffer = iBuffer ? m_device->createBuffer({ {.size = iBuffer->getSize(), .usage = nbl::core::bitflag(nbl::asset::IBuffer::EUF_INDEX_BUFFER_BIT) | nbl::asset::IBuffer::EUF_VERTEX_BUFFER_BIT | nbl::asset::IBuffer::EUF_TRANSFER_DST_BIT | nbl::asset::IBuffer::EUF_INLINE_UPDATE_VIA_CMDBUF} }) : nullptr;

		if (!outData.first.vertexBuffer)
			return false;

		if (inData.data.indexType != nbl::asset::EIT_UNKNOWN)
			if (!outData.first.indexBuffer)
				return false;

		for (auto it : { outData.first.vertexBuffer , outData.first.indexBuffer })
		{
			if (it)
			{
				auto reqs = it->getMemoryReqs();
				reqs.memoryTypeBits &= mask;

				m_device->allocate(reqs, it.get());
			}
		}

		{
			auto fillGPUBuffer = [&m_logger = m_logger](nbl::core::smart_refctd_ptr<nbl::asset::ICPUBuffer> cBuffer, nbl::core::smart_refctd_ptr<nbl::video::IGPUBuffer> gBuffer)
			{
				auto binding = gBuffer->getBoundMemory();

				if (!binding.memory->map({ 0ull, binding.memory->getAllocationSize() }, nbl::video::IDeviceMemoryAllocation::EMCAF_READ))
				{
					m_logger->log("Could not map device memory", nbl::system::ILogger::ELL_ERROR);
					return false;
				}

				if (!binding.memory->isCurrentlyMapped())
				{
					m_logger->log("Buffer memory is not mapped!", nbl::system::ILogger::ELL_ERROR);
					return false;
				}

				auto* mPointer = binding.memory->getMappedPointer();
				memcpy(mPointer, cBuffer->getPointer(), gBuffer->getSize());
				binding.memory->unmap();

				return true;
			};

			if (!fillGPUBuffer(vBuffer, outData.first.vertexBuffer))
				return false;

			if (outData.first.indexBuffer)
				if (!fillGPUBuffer(iBuffer, outData.first.indexBuffer))
					return false;
		}

		return true;
	}

	nbl::core::smart_refctd_ptr<nbl::video::IGPUPipelineLayout> createPipelineLayoutAndDS()
	{
		nbl::video::IGPUDescriptorSetLayout::SBinding bindings[] =
		{
			{
				.binding = 0u,
				.type = nbl::asset::IDescriptor::E_TYPE::ET_UNIFORM_BUFFER,
				.createFlags = nbl::video::IGPUDescriptorSetLayout::SBinding::E_CREATE_FLAGS::ECF_NONE,
				.stageFlags = nbl::asset::IShader::E_SHADER_STAGE::ESS_VERTEX | nbl::asset::IShader::E_SHADER_STAGE::ESS_FRAGMENT,
				.count = 1u,
			}
		};

		auto descriptorSetLayout = m_device->createDescriptorSetLayout(bindings);
		{
			const nbl::video::IGPUDescriptorSetLayout* const layouts[] = { nullptr, descriptorSetLayout.get() };
			const uint32_t setCounts[] = { 0u, 1u };

			m_descriptorPool = m_device->createDescriptorPoolForDSLayouts(nbl::video::IDescriptorPool::E_CREATE_FLAGS::ECF_NONE, layouts, setCounts);

			if (!m_descriptorPool)
			{
				m_logger->log("Could not create Descriptor Pool!", nbl::system::ILogger::ELL_ERROR);
				return nullptr;
			}
		}

		m_gpuDescriptorSet = m_descriptorPool->createDescriptorSet(descriptorSetLayout);

		if (!m_gpuDescriptorSet)
		{
			m_logger->log("Could not create Descriptor Set!", nbl::system::ILogger::ELL_ERROR);
			return nullptr;
		}

		auto pipelineLayout = m_device->createPipelineLayout({}, nullptr, std::move(descriptorSetLayout));

		if (!pipelineLayout)
			m_logger->log("Could not create Pipeline Layout!", nbl::system::ILogger::ELL_ERROR);

		return pipelineLayout;
	}

	bool createOfflineSceneRenderPass()
	{
		// Create the renderpass
		{
			_NBL_STATIC_INLINE_CONSTEXPR nbl::video::IGPURenderpass::SCreationParams::SColorAttachmentDescription colorAttachments[] =
			{
				{
					{
						{
							.format = COLOR_FBO_ATTACHMENT_FORMAT,
							.samples = SAMPLES,
							.mayAlias = false
						},
						/*.loadOp = */nbl::video::IGPURenderpass::LOAD_OP::CLEAR,
						/*.storeOp = */nbl::video::IGPURenderpass::STORE_OP::STORE,
						/*.initialLayout = */nbl::video::IGPUImage::LAYOUT::UNDEFINED, // because we clear we don't care about contents when we grab the triple buffer img again
						/*.finalLayout = */nbl::video::IGPUImage::LAYOUT::TRANSFER_SRC_OPTIMAL // put it already in the correct layout for the blit operation
					}
				},
				nbl::video::IGPURenderpass::SCreationParams::ColorAttachmentsEnd
			};

			_NBL_STATIC_INLINE_CONSTEXPR nbl::video::IGPURenderpass::SCreationParams::SDepthStencilAttachmentDescription depthAttachments[] =
			{
				{
					{
						{
							.format = DEPTH_FBO_ATTACHMENT_FORMAT,
							.samples = SAMPLES,
							.mayAlias = false
						},
						/*.loadOp = */{nbl::video::IGPURenderpass::LOAD_OP::CLEAR},
						/*.storeOp = */{nbl::video::IGPURenderpass::STORE_OP::STORE},
						/*.initialLayout = */{nbl::video::IGPUImage::LAYOUT::UNDEFINED}, // because we clear we don't care about contents
						/*.finalLayout = */{nbl::video::IGPUImage::LAYOUT::ATTACHMENT_OPTIMAL} // transition to presentation right away so we can skip a barrier
					}
				},
				nbl::video::IGPURenderpass::SCreationParams::DepthStencilAttachmentsEnd
			};

			nbl::video::IGPURenderpass::SCreationParams::SSubpassDescription subpasses[] =
			{
				{},
				nbl::video::IGPURenderpass::SCreationParams::SubpassesEnd
			};

			subpasses[0].depthStencilAttachment.render = { .attachmentIndex = 0u,.layout = nbl::video::IGPUImage::LAYOUT::ATTACHMENT_OPTIMAL };
			subpasses[0].colorAttachments[0] = { .render = { .attachmentIndex = 0u, .layout = nbl::video::IGPUImage::LAYOUT::ATTACHMENT_OPTIMAL } };

			_NBL_STATIC_INLINE_CONSTEXPR nbl::video::IGPURenderpass::SCreationParams::SSubpassDependency dependencies[] =
			{
				// wipe-transition of Color to ATTACHMENT_OPTIMAL
				{
					.srcSubpass = nbl::video::IGPURenderpass::SCreationParams::SSubpassDependency::External,
					.dstSubpass = 0,
					.memoryBarrier = 
					{
						// last place where the depth can get modified in previous frame
						.srcStageMask = nbl::asset::PIPELINE_STAGE_FLAGS::LATE_FRAGMENT_TESTS_BIT,
						// only write ops, reads can't be made available
						.srcAccessMask = nbl::asset::ACCESS_FLAGS::DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
						// destination needs to wait as early as possible
						.dstStageMask = nbl::asset::PIPELINE_STAGE_FLAGS::EARLY_FRAGMENT_TESTS_BIT,
						// because of depth test needing a read and a write
						.dstAccessMask = nbl::asset::ACCESS_FLAGS::DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | nbl::asset::ACCESS_FLAGS::DEPTH_STENCIL_ATTACHMENT_READ_BIT
					}
					// leave view offsets and flags default
				},
				// color from ATTACHMENT_OPTIMAL to PRESENT_SRC
				{
					.srcSubpass = 0,
					.dstSubpass = nbl::video::IGPURenderpass::SCreationParams::SSubpassDependency::External,
					.memoryBarrier = 
					{
						// last place where the depth can get modified
						.srcStageMask = nbl::asset::PIPELINE_STAGE_FLAGS::COLOR_ATTACHMENT_OUTPUT_BIT,
						// only write ops, reads can't be made available
						.srcAccessMask = nbl::asset::ACCESS_FLAGS::COLOR_ATTACHMENT_WRITE_BIT
						// spec says nothing is needed when presentation is the destination
					}
					// leave view offsets and flags default
				},
				nbl::video::IGPURenderpass::SCreationParams::DependenciesEnd
			};

			nbl::video::IGPURenderpass::SCreationParams params = {};
			params.colorAttachments = colorAttachments;
			params.depthStencilAttachments = depthAttachments;
			params.subpasses = subpasses;
			params.dependencies = dependencies;

			m_renderpass = m_device->createRenderpass(params);

			if (!m_renderpass)
			{
				m_logger->log("Could not create RenderPass!", nbl::system::ILogger::ELL_ERROR);
				return false;
			}
		}

		return true;
	}

	bool createOfflineSceneFramebuffer()
	{
		auto createGPUImageView = [&](nbl::asset::E_FORMAT format)
		{
			{
				nbl::video::IGPUImage::SCreationParams imgInfo;
				imgInfo.format = format;
				imgInfo.type = nbl::video::IGPUImage::ET_2D;
				imgInfo.extent.width = FRAMEBUFFER_W;
				imgInfo.extent.height = FRAMEBUFFER_H;
				imgInfo.extent.depth = 1u;
				imgInfo.mipLevels = 1u;
				imgInfo.arrayLayers = 1u;
				imgInfo.samples = nbl::asset::ICPUImage::ESCF_1_BIT;
				imgInfo.flags = static_cast<nbl::asset::IImage::E_CREATE_FLAGS>(0u);

				auto image = m_device->createImage(std::move(imgInfo));

				nbl::video::IGPUImageView::SCreationParams imgViewInfo;
				imgViewInfo.image = std::move(image);
				imgViewInfo.format = format;
				imgViewInfo.viewType = nbl::video::IGPUImageView::ET_2D;
				imgViewInfo.flags = static_cast<nbl::video::IGPUImageView::E_CREATE_FLAGS>(0u);
				imgViewInfo.subresourceRange.baseArrayLayer = 0u;
				imgViewInfo.subresourceRange.baseMipLevel = 0u;
				imgViewInfo.subresourceRange.layerCount = 1u;
				imgViewInfo.subresourceRange.levelCount = 1u;

				return m_device->createImageView(std::move(imgViewInfo));
			}
		};

		m_colorAttachment = createGPUImageView(COLOR_FBO_ATTACHMENT_FORMAT);
		m_depthAttachment = createGPUImageView(DEPTH_FBO_ATTACHMENT_FORMAT);

		nbl::video::IGPUFramebuffer::SCreationParams params =
		{ 
			{
				.renderpass = nbl::core::smart_refctd_ptr(m_renderpass),
				.depthStencilAttachments = &m_depthAttachment.get(),
				.colorAttachments = &m_colorAttachment.get(),
				.width = FRAMEBUFFER_W,
				.height = FRAMEBUFFER_H,
				.layers = 1u
			} 
		};

		m_frameBuffer = m_device->createFramebuffer(std::move(params));

		if (!m_frameBuffer)
		{
			m_logger->log("Could not create Frame Buffer!", nbl::system::ILogger::ELL_ERROR);
			return false;
		}

		return true;
	}

	std::vector<OBJECT_DATA> referenceObjects; // all possible objects & their buffers

	nbl::core::smart_refctd_ptr<nbl::video::ILogicalDevice> m_device;
	nbl::core::smart_refctd_ptr<nbl::system::ILogger> m_logger;

	nbl::core::smart_refctd_ptr<nbl::video::IGPURenderpass> m_renderpass;
	nbl::core::smart_refctd_ptr<nbl::video::IGPUFramebuffer> m_frameBuffer;
	nbl::core::smart_refctd_ptr<nbl::video::IGPUImageView> m_colorAttachment, m_depthAttachment;
	nbl::core::smart_refctd_ptr<nbl::video::IDescriptorPool> m_descriptorPool;
	nbl::core::smart_refctd_ptr<nbl::video::IGPUDescriptorSet> m_gpuDescriptorSet;
};

#endif // __NBL_THIS_EXAMPLE_SCENE_H_INCLUDED__