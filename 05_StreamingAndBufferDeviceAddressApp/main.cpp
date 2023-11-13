// Copyright (C) 2018-2023 - DevSH Graphics Programming Sp. z O.O.
// This file is part of the "Nabla Engine".
// For conditions of distribution and use, see copyright notice in nabla.h


// I've moved out a tiny part of this example into a shared header for reuse, please open and read it.
#include "../common/MonoDeviceApplication.hpp"
#include "../common/MonoAssetManagerAndBuiltinResourceApplication.hpp"

using namespace nbl;
using namespace core;
using namespace system;
using namespace asset;
using namespace video;


#include "app_resources/common.hlsl"
#include "nbl/builtin/hlsl/bit.hlsl"


// In this application we'll cover buffer streaming, Buffer Device Address (BDA) and push constants 
class StreamingAndBufferDeviceAddressApp final : public examples::MonoDeviceApplication, public examples::MonoAssetManagerAndBuiltinResourceApplication
{
		using device_base_t = examples::MonoDeviceApplication;
		using asset_base_t = examples::MonoAssetManagerAndBuiltinResourceApplication;

		// This is the first example that submits multiple workloads in-flight. 
		// What the shader does is it computes the minimum distance of each point against K other random input points.
		// Having the GPU randomly access parts of the buffer requires it to be DEVICE_LOCAL for performance.
		// Then the CPU downloads the results and finds the median minimum distance via quick-select.
		// This bizzare synthetic workload was specifically chosen for its unfriendliness towards simple buffer usage.
		// The fact we have variable sized workloads and run them in a loop means we either have to dynamically
		// suballocate from a single buffer or have K worst-case sized buffers we round robin for K-workloads in flight.
		// Creating and destroying buffers at runtime is not an option as those are very expensive operations. 
		// Also since CPU needs to heapify the outputs, we need to have the GPU write them into RAM not VRAM.
		smart_refctd_ptr<IGPUComputePipeline> m_pipeline;

		// The Utility class has lots of methods to handle staging without relying on ReBAR or EXT_host_image_copy as well as more complex methods we'll cover later.
		// Until EXT_host_image_copy becomes ubiquitous across all Nabla Core Profile devices, you need to stage image copies from an IGPUBuffer to an IGPUImage.
		// Why use Staging for buffers in the age of ReBAR? While GPU workloads overlap the CPU, individual GPU workloads's execution might not overlap each other
		// but their data might. In this case you want to "precisely" time the data update on the GPU timeline between the end and start of a workload.
		// For very small updates you could use the commandbuffer updateBuffer method, but it has a size limit and the data enqueued takes up space in the commandpool.
		// Sometimes it might be unfeasible to either have multiple copies or update references to those copies without a cascade update.
		// One example is the transformation graph of nodes in a scene, where a copy-on-write of a node would require the update the offset/pointer held by
		// any other node that refers to it. This quickly turns into a cascade that would force you to basically create a full copy of the entire data structure
		// after most updates. Whereas with staging you'd "queue up" the much smaller set of updates to apply between each computation step which uses the graph.
		// Another example are UBO and SSBO bindings, where once you run out of dynamic bindings, you can no longer easily change offsets without introducting extra indirection in shaders.
		// Actually staging can help you re-use a commandbuffer because you don't need to re-record it if you don't need to change the offsets at which you bind!
		// Finally ReBAR is a precious resource, my 8GB RTX 3070 only reports a 214MB Heap backing HOST_VISIBLE and DEVICE_LOCAL device local memory type.
		smart_refctd_ptr<nbl::video::IUtilities> m_utils;

		// We call them downstreaming and upstreaming, simply by how we used them so far.
		// Meaning that upstreaming is uncached and usually ReBAR (DEVICE_LOCAL), for simple memcpy like sequential writes.
		// While the downstreaming is CACHED and not DEVICE_LOCAL for fast random acess by the CPU.
		// However there are cases when you'd want to use a buffer with flags identical to the default downstreaming buffer for uploads,
		// such cases is when a CPU needs to build a data-structure in-place (due to memory constraints) before GPU accesses it,
		// one example are Host Acceleration Structure builds (BVH building requires lots of repeated memory accesses).
		// When choosing the memory properties of a mapped buffer consider which processor (CPU or GPU) needs faster access in event of a cache-miss.
		nbl::video::StreamingTransientDataBufferMT<>* m_upStreamingBuffer;
		StreamingTransientDataBufferMT<>* m_downStreamingBuffer;
		// These are Buffer Device Addresses
		uint64_t m_upStreamingBufferAddress;
		uint64_t m_downStreamingBufferAddress;

		// You can ask the `nbl::core::GeneralpurposeAddressAllocator` used internally by the Streaming Buffers give out offsets aligned to a certain multiple (not only Power of Two!)
		uint32_t m_alignment;
		
		// The pool cache is just a formalized way of round-robining command pools and resetting + reusing them after their most recent submit signals finished.
		// Its a little more ergonomic to use if you don't have a 1:1 mapping between frames and pools.
		smart_refctd_ptr<nbl::video::ICommandPoolCache> m_poolCache;

		// We'll run the iterations in reverse, easier to write "keep running"
		uint32_t m_iteration = 200;

	public:
		// Yay thanks to multiple inheritance we cannot forward ctors anymore
		StreamingAndBufferDeviceAddressApp(const path& _localInputCWD, const path& _localOutputCWD, const path& _sharedInputCWD, const path& _sharedOutputCWD) :
			system::IApplicationFramework(_localInputCWD,_localOutputCWD,_sharedInputCWD,_sharedOutputCWD) {}

		// we stuff all our work here because its a "single shot" app
		bool onAppInitialized(smart_refctd_ptr<ISystem>&& system) override
		{
			// Remember to call the base class initialization!
			if (!device_base_t::onAppInitialized(std::move(system)))
				return false;
			if (!asset_base_t::onAppInitialized(std::move(system)))
				return false;

			// this time we load a shader directly from a file
			smart_refctd_ptr<IGPUSpecializedShader> shader;
			{
				IAssetLoader::SAssetLoadParams lp = {};
				lp.logger = m_logger.get();
				lp.workingDirectory = ""; // virtual root
				auto assetBundle = m_assetMgr->getAsset("app_resources/shader.comp.hlsl",lp);
				const auto assets = assetBundle.getContents();
				if (assets.empty())
					return logFail("Could not load shader!");

				// lets go straight from ICPUSpecializedShader to IGPUSpecializedShader
				auto source = IAsset::castDown<ICPUSpecializedShader>(assets[0]);
				// The down-cast should not fail!
				assert(source);

				IGPUObjectFromAssetConverter::SParams conversionParams = {};
				conversionParams.device = m_device.get();
				conversionParams.assetManager = m_assetMgr.get();
				created_gpu_object_array<ICPUSpecializedShader> convertedGPUObjects = std::make_unique<IGPUObjectFromAssetConverter>()->getGPUObjectsFromAssets(&source,&source+1,conversionParams);
				if (convertedGPUObjects->empty() || !convertedGPUObjects->front())
					return logFail("Conversion of a CPU Specialized Shader to GPU failed!");

				shader = convertedGPUObjects->front();
			}

			// The StreamingTransientDataBuffers are actually composed on top of another useful utility called `CAsyncSingleBufferSubAllocator`
			// The difference is that the streaming ones are made on top of ranges of `IGPUBuffer`s backed by mappable memory, whereas the
			// `CAsyncSingleBufferSubAllocator` just allows you suballocate subranges of any `IGPUBuffer` range with deferred/latched frees.
			constexpr uint32_t DownstreamBufferSize = sizeof(output_t)<<24;
			constexpr uint32_t UpstreamBufferSize = sizeof(input_t)<<24;
			m_utils = make_smart_refctd_ptr<IUtilities>(smart_refctd_ptr(m_device),smart_refctd_ptr(m_logger),DownstreamBufferSize,UpstreamBufferSize);
			if (!m_utils)
				return logFail("Failed to create Utilities!");
			m_upStreamingBuffer = m_utils->getDefaultUpStreamingBuffer();
			m_downStreamingBuffer = m_utils->getDefaultDownStreamingBuffer();
			m_upStreamingBufferAddress = m_device->getBufferDeviceAddress(m_upStreamingBuffer->getBuffer());
			m_downStreamingBufferAddress = m_device->getBufferDeviceAddress(m_downStreamingBuffer->getBuffer());

			// People love Reflection but I prefer Shader Sources instead!
			const nbl::asset::SPushConstantRange pcRange = {.stageFlags=IShader::ESS_COMPUTE,.offset=0,.size=sizeof(PushConstantData)};

			// This time we'll have no Descriptor Sets or Layouts because our workload has a widely varying size
			// and using traditional SSBO bindings would force us to update the Descriptor Set every frame.
			// I even started writing this sample with the use of Dynamic SSBOs, however the length of the buffer range is not dynamic
			// only the offset. This means that we'd have to write the "worst case" length into the descriptor set binding.
			// Then this has a knock-on effect that we couldn't allocate closer to the end of the streaming buffer than the "worst case" size.
			m_pipeline = m_device->createComputePipeline(nullptr,m_device->createPipelineLayout(&pcRange,&pcRange+1),std::move(shader));

			const auto& deviceLimits = m_device->getPhysicalDevice()->getLimits();
			// When you bind SSBOs at offsets you need to bind with a certain alignment, also the ranges of non-coherent mapped memory
			// you flush or invalidate need to be aligned too. We'll align anyway if we're coherent.
			// So we ask our streaming buffers during allocation to give us properly aligned offsets.
			m_alignment = core::max(deviceLimits.minSSBOAlignment,deviceLimits.nonCoherentAtomSize);

			// We'll allow subsequent iterations to overlap each other on the GPU, the only limiting factors are
			// the amount of memory in the streaming buffers and the number of commandpools we can use simultaenously.
			constexpr auto MaxConcurrency = 64;
			// Since this time we don't throw the Command Pools away and we'll reset them instead, we don't create the pools with the transient flag
			m_poolCache = make_smart_refctd_ptr<ICommandPoolCache>(m_device.get(),getComputeQueue()->getFamilyIndex(), IGPUCommandPool::ECF_NONE, MaxConcurrency);

			return true;
		}

		// Ok this time we'll actually have a work loop (maybe just for the sake of future WASM so we don't timeout a Browser Tab with an unresponsive script)
		bool keepRunning() override { return m_iteration; }

		// Finally the first actual work-loop
		void workLoopBody() override
		{
			m_iteration--;
			IGPUQueue* const queue = getComputeQueue();

			// Note that I'm using the sample struct with methods that have identical code which compiles as both C++ and HLSL
			auto rng = nbl::hlsl::Xoroshiro64StarStar::construct({m_iteration^0xdeadbeefu,std::hash<string>()(_NBL_APP_NAME_)});

			// we dynamically choose the number of elements for each iteration
			const auto elementCount = rng()%MaxPossibleElementCount;
			const uint32_t inputSize = sizeof(input_t)*elementCount;

			// The allocators can do multiple allocations at once for efficiency
			const uint32_t AllocationCount = 1;
			// It comes with a certain drawback that you need to remember to initialize your "yet unallocated" offsets to the Invalid value
			// this is to allow a set of allocations to fail, and you to re-try after doing something to free up space without repacking args.
			auto inputOffset = m_upStreamingBuffer->invalid_value;

			// We always just wait till an allocation becomes possible (during allocation previous "latched" frees get their latch conditions polled)
			// Freeing of Streaming Buffer Allocations can and should be deferred until an associated polled event signals done (more on that later).
			std::chrono::steady_clock::time_point waitTill(std::chrono::years(45));
			// note that the API takes a time-point not a duration, because there are multiple waits and preemptions possible, so the durations wouldn't add up properly
			m_upStreamingBuffer->multi_allocate(waitTill,AllocationCount,&inputOffset,&inputSize,&m_alignment);

			// Generate our data in-place on the allocated staging buffer
			{
				auto* const inputPtr = reinterpret_cast<input_t*>(reinterpret_cast<uint8_t*>(m_upStreamingBuffer->getBufferPointer())+inputOffset);
				for (auto j=0; j<elementCount; j++)
				{
					const nbl::hlsl::float32_t3 generated(rng(),rng(),rng());
					// make sure our bitpatterns are in [0,1]^2 as a float
					inputPtr[j] = generated/float(nbl::hlsl::numeric_limits<decltype(rng())>::max);
				}
				// Always remember to flush!
				if (m_upStreamingBuffer->needsManualFlushOrInvalidate())
				{
					const IDeviceMemoryAllocation::MappedMemoryRange range(m_upStreamingBuffer->getBuffer()->getBoundMemory(),inputOffset,inputSize);
					m_device->flushMappedMemoryRanges(1,&range);
				}
			}

			// Obtain our command pool once one gets recycled
			uint32_t poolIx;
			do
			{
				poolIx = m_poolCache->acquirePool();
			} while (poolIx==ICommandPoolCache::invalid_index);

			// finally allocate our output range
			const uint32_t outputSize = sizeof(output_t)*elementCount;
			auto outputOffset = m_downStreamingBuffer->invalid_value;
			m_downStreamingBuffer->multi_allocate(waitTill,AllocationCount,&outputOffset,&outputSize,&m_alignment);

			smart_refctd_ptr<IGPUCommandBuffer> cmdbuf;
			{
				m_device->createCommandBuffers(m_poolCache->getPool(poolIx),IGPUCommandBuffer::EL_PRIMARY,1,&cmdbuf);
				// lets record, its still a one time submit because we have to re-record with different push constants each time
				cmdbuf->begin(IGPUCommandBuffer::EU_ONE_TIME_SUBMIT_BIT);
				cmdbuf->bindComputePipeline(m_pipeline.get());
				// This is the new fun part, pushing constants
				const PushConstantData pc = {
					.inputAddress=m_upStreamingBufferAddress+inputOffset,
					.outputAddress=m_downStreamingBufferAddress+outputOffset,
					.dataElementCount=elementCount
				};
				cmdbuf->pushConstants(m_pipeline->getLayout(),IShader::ESS_COMPUTE,0u,sizeof(pc),&pc);
				// Good old trick to get rounded up divisions, in case you're not familiar
				cmdbuf->dispatch((elementCount-1)/WorkgroupSize+1,1,1);
				cmdbuf->end();
			}

			// TODO: redo with a single timeline semaphore
			auto fence = m_device->createFence(IGPUFence::ECF_UNSIGNALED);
			{
				IGPUQueue::SSubmitInfo submitInfo = {};
				submitInfo.commandBufferCount = 1;
				submitInfo.commandBuffers = &cmdbuf.get();

				queue->startCapture();
				queue->submit(1u,&submitInfo,fence.get());
				queue->endCapture();
			}
				
			// We can also actually latch our Command Pool reset and its return to the pool of free pools!
			m_poolCache->releaseSet(m_device.get(),smart_refctd_ptr(fence),poolIx);

			// As promised, we can defer a streaming buffer allocation until a fence is signalled
			// You can also attach an additional optional IReferenceCounted derived object to hold onto until deallocation.
			m_upStreamingBuffer->multi_deallocate(AllocationCount,&inputOffset,&inputSize,smart_refctd_ptr(fence));

			// Because C++17 and C++20 can't make their mind up about what to do with `this` in event of a [=] capture, lets triple ensure the m_iteration is captured by value.
			const auto savedIterNum = m_iteration;
				
			// Now a new and even more advanced usage of the latched events, we make our own refcounted object with a custom destructor and latch that like we did the commandbuffer.
			// Instead of making our own and duplicating logic, we'll use one from IUtilities meant for down-staging memory.
			// Its nice because it will also remember to invalidate our memory mapping if its not coherent.
			auto latchedConsumer = make_smart_refctd_ptr<IUtilities::CDownstreamingDataConsumer>(
				IDeviceMemoryAllocation::MemoryRange(outputOffset,outputSize),
				// Note the use of capture by-value [=] and not by-reference [&] because this lambda will be called asynchronously whenever the event signals
				[=](const size_t dstOffset, const void* bufSrc, const size_t size)->void
				{
					// The unused variable is used for letting the consumer know the subsection of the output we've managed to download
					// But here we're sure we can get the whole thing in one go because we allocated the whole range ourselves.
					assert(dstOffset==0 && size==outputSize);

					// I can const cast, we know the mapping is just a pointer
					output_t* const data = reinterpret_cast<output_t*>(const_cast<void*>(bufSrc));
					auto median = data+elementCount/2;
					std::nth_element(data,median,data+elementCount);

					m_logger->log("Iteration %d Median of Minimum Distances is %f",ILogger::ELL_INFO,savedIterNum,*median);
				},
				// Its also necessary to hold onto the commandbuffer, even though we take care to not reset the parent pool, because if it
				// hits its destructor, our automated reference counting will drop all references to objects used in the recorded commands.
				// It could also be latched in the upstreaming deallocate, because its the same fence.
				std::move(cmdbuf),m_downStreamingBuffer
			);
			// We put a function we want to execute 
			m_downStreamingBuffer->multi_deallocate(AllocationCount,&outputOffset,&outputSize,std::move(fence),&latchedConsumer.get());
		}

		bool onAppTerminated() override
		{
			// Need to make sure that there are no events outstanding if we want all lambdas to eventually execute
			while (m_upStreamingBuffer->cull_frees()) {}

			return device_base_t::onAppTerminated();
		}
};


NBL_MAIN_FUNC(StreamingAndBufferDeviceAddressApp)