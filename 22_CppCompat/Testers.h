#ifndef _NBL_EXAMPLES_TESTS_22_CPP_COMPAT_TESTERS_INCLUDED_
#define _NBL_EXAMPLES_TESTS_22_CPP_COMPAT_TESTERS_INCLUDED_

#include <nabla.h>
#include "app_resources/common.hlsl"
#include "nbl/application_templates/MonoDeviceApplication.hpp"
#include "nbl/application_templates/MonoAssetManagerAndBuiltinResourceApplication.hpp"

using namespace nbl;

class ITester 
{
public:
    virtual ~ITester()
    {
        m_outputBufferAllocation.memory->unmap();
    };

    struct PipelineSetupData
    {
        std::string testShaderPath;

        core::smart_refctd_ptr<video::ILogicalDevice> device;
        core::smart_refctd_ptr<video::CVulkanConnection> api;
        core::smart_refctd_ptr<asset::IAssetManager> assetMgr;
        core::smart_refctd_ptr<system::ILogger> logger;
        video::IPhysicalDevice* physicalDevice;
        uint32_t computeFamilyIndex;
    };

    template<typename InputStruct, typename OutputStruct>
    void setupPipeline(const PipelineSetupData& pipleineSetupData)
    {
        // setting up pipeline in the constructor
        m_device = core::smart_refctd_ptr(pipleineSetupData.device);
        m_physicalDevice = pipleineSetupData.physicalDevice;
        m_api = core::smart_refctd_ptr(pipleineSetupData.api);
        m_assetMgr = core::smart_refctd_ptr(pipleineSetupData.assetMgr);
        m_logger = core::smart_refctd_ptr(pipleineSetupData.logger);
        m_queueFamily = pipleineSetupData.computeFamilyIndex;
        m_semaphoreCounter = 0;
        m_semaphore = m_device->createSemaphore(0);
        m_cmdpool = m_device->createCommandPool(m_queueFamily, video::IGPUCommandPool::CREATE_FLAGS::RESET_COMMAND_BUFFER_BIT);
        if (!m_cmdpool->createCommandBuffers(video::IGPUCommandPool::BUFFER_LEVEL::PRIMARY, 1u, &m_cmdbuf))
            logFail("Failed to create Command Buffers!\n");

        // Load shaders, set up pipeline
        core::smart_refctd_ptr<video::IGPUShader> shader;
        {
            asset::IAssetLoader::SAssetLoadParams lp = {};
            lp.logger = m_logger.get();
            lp.workingDirectory = ""; // virtual root
            auto assetBundle = m_assetMgr->getAsset(pipleineSetupData.testShaderPath, lp);
            const auto assets = assetBundle.getContents();
            if (assets.empty())
            {
                logFail("Could not load shader!");
                assert(0);
            }

            // It would be super weird if loading a shader from a file produced more than 1 asset
            assert(assets.size() == 1);
            core::smart_refctd_ptr<asset::ICPUShader> source = asset::IAsset::castDown<asset::ICPUShader>(assets[0]);

            auto* compilerSet = m_assetMgr->getCompilerSet();

            asset::IShaderCompiler::SCompilerOptions options = {};
            options.stage = source->getStage();
            options.targetSpirvVersion = m_device->getPhysicalDevice()->getLimits().spirvVersion;
            options.spirvOptimizer = nullptr;
            options.debugInfoFlags |= asset::IShaderCompiler::E_DEBUG_INFO_FLAGS::EDIF_SOURCE_BIT;
            options.preprocessorOptions.sourceIdentifier = source->getFilepathHint();
            options.preprocessorOptions.logger = m_logger.get();
            options.preprocessorOptions.includeFinder = compilerSet->getShaderCompiler(source->getContentType())->getDefaultIncludeFinder();

            auto spirv = compilerSet->compileToSPIRV(source.get(), options);

            video::ILogicalDevice::SShaderCreationParameters params{};
            params.cpushader = spirv.get();
            shader = m_device->createShader(params);
        }

        if (!shader)
            logFail("Failed to create a GPU Shader, seems the Driver doesn't like the SPIR-V we're feeding it!\n");

        video::IGPUDescriptorSetLayout::SBinding bindings[2] = {
            {
                .binding = 0,
                .type = asset::IDescriptor::E_TYPE::ET_STORAGE_BUFFER,
                .createFlags = video::IGPUDescriptorSetLayout::SBinding::E_CREATE_FLAGS::ECF_NONE,
                .stageFlags = ShaderStage::ESS_COMPUTE,
                .count = 1
            },
            {
                .binding = 1,
                .type = asset::IDescriptor::E_TYPE::ET_STORAGE_BUFFER,
                .createFlags = video::IGPUDescriptorSetLayout::SBinding::E_CREATE_FLAGS::ECF_NONE,
                .stageFlags = ShaderStage::ESS_COMPUTE,
                .count = 1
            }
        };

        core::smart_refctd_ptr<video::IGPUDescriptorSetLayout> dsLayout = m_device->createDescriptorSetLayout(bindings);
        if (!dsLayout)
            logFail("Failed to create a Descriptor Layout!\n");

        m_pplnLayout = m_device->createPipelineLayout({}, core::smart_refctd_ptr(dsLayout));
        if (!m_pplnLayout)
            logFail("Failed to create a Pipeline Layout!\n");

        {
            video::IGPUComputePipeline::SCreationParams params = {};
            params.layout = m_pplnLayout.get();
            params.shader.entryPoint = "main";
            params.shader.shader = shader.get();
            if (!m_device->createComputePipelines(nullptr, { &params,1 }, &m_pipeline))
                logFail("Failed to create pipelines (compile & link shaders)!\n");
        }

        // Allocate memory of the input buffer
        {
            constexpr size_t BufferSize = sizeof(InputStruct);

            video::IGPUBuffer::SCreationParams params = {};
            params.size = BufferSize;
            params.usage = video::IGPUBuffer::EUF_STORAGE_BUFFER_BIT;
            core::smart_refctd_ptr<video::IGPUBuffer> inputBuff = m_device->createBuffer(std::move(params));
            if (!inputBuff)
                logFail("Failed to create a GPU Buffer of size %d!\n", params.size);

            inputBuff->setObjectDebugName("emulated_float64_t output buffer");

            video::IDeviceMemoryBacked::SDeviceMemoryRequirements reqs = inputBuff->getMemoryReqs();
            reqs.memoryTypeBits &= m_physicalDevice->getHostVisibleMemoryTypeBits();

            m_inputBufferAllocation = m_device->allocate(reqs, inputBuff.get(), video::IDeviceMemoryAllocation::EMAF_NONE);
            if (!m_inputBufferAllocation.isValid())
                logFail("Failed to allocate Device Memory compatible with our GPU Buffer!\n");

            assert(inputBuff->getBoundMemory().memory == m_inputBufferAllocation.memory.get());
            core::smart_refctd_ptr<video::IDescriptorPool> pool = m_device->createDescriptorPoolForDSLayouts(video::IDescriptorPool::ECF_NONE, { &dsLayout.get(),1 });

            m_ds = pool->createDescriptorSet(core::smart_refctd_ptr(dsLayout));
            {
                video::IGPUDescriptorSet::SDescriptorInfo info[1];
                info[0].desc = core::smart_refctd_ptr(inputBuff);
                info[0].info.buffer = { .offset = 0,.size = BufferSize };
                video::IGPUDescriptorSet::SWriteDescriptorSet writes[1] = {
                    {.dstSet = m_ds.get(),.binding = 0,.arrayElement = 0,.count = 1,.info = info}
                };
                m_device->updateDescriptorSets(writes, {});
            }
        }

        // Allocate memory of the output buffer
        {
            constexpr size_t BufferSize = sizeof(OutputStruct);

            video::IGPUBuffer::SCreationParams params = {};
            params.size = BufferSize;
            params.usage = video::IGPUBuffer::EUF_STORAGE_BUFFER_BIT;
            core::smart_refctd_ptr<video::IGPUBuffer> outputBuff = m_device->createBuffer(std::move(params));
            if (!outputBuff)
                logFail("Failed to create a GPU Buffer of size %d!\n", params.size);

            outputBuff->setObjectDebugName("emulated_float64_t output buffer");

            video::IDeviceMemoryBacked::SDeviceMemoryRequirements reqs = outputBuff->getMemoryReqs();
            reqs.memoryTypeBits &= m_physicalDevice->getHostVisibleMemoryTypeBits();

            m_outputBufferAllocation = m_device->allocate(reqs, outputBuff.get(), video::IDeviceMemoryAllocation::EMAF_NONE);
            if (!m_outputBufferAllocation.isValid())
                logFail("Failed to allocate Device Memory compatible with our GPU Buffer!\n");

            assert(outputBuff->getBoundMemory().memory == m_outputBufferAllocation.memory.get());
            core::smart_refctd_ptr<video::IDescriptorPool> pool = m_device->createDescriptorPoolForDSLayouts(video::IDescriptorPool::ECF_NONE, { &dsLayout.get(),1 });

            {
                video::IGPUDescriptorSet::SDescriptorInfo info[1];
                info[0].desc = core::smart_refctd_ptr(outputBuff);
                info[0].info.buffer = { .offset = 0,.size = BufferSize };
                video::IGPUDescriptorSet::SWriteDescriptorSet writes[1] = {
                    {.dstSet = m_ds.get(),.binding = 1,.arrayElement = 0,.count = 1,.info = info}
                };
                m_device->updateDescriptorSets(writes, {});
            }
        }

        if (!m_outputBufferAllocation.memory->map({ 0ull,m_outputBufferAllocation.memory->getAllocationSize() }, video::IDeviceMemoryAllocation::EMCAF_READ))
            logFail("Failed to map the Device Memory!\n");

        // if the mapping is not coherent the range needs to be invalidated to pull in new data for the CPU's caches
        const video::ILogicalDevice::MappedMemoryRange memoryRange(m_outputBufferAllocation.memory.get(), 0ull, m_outputBufferAllocation.memory->getAllocationSize());
        if (!m_outputBufferAllocation.memory->getMemoryPropertyFlags().hasFlags(video::IDeviceMemoryAllocation::EMPF_HOST_COHERENT_BIT))
            m_device->invalidateMappedMemoryRanges(1, &memoryRange);

        assert(memoryRange.valid() && memoryRange.length >= sizeof(OutputStruct));

        m_queue = m_device->getQueue(m_queueFamily, 0);
    }

    enum class TestType
    {
        CPU,
        GPU
    };

    template<typename T>
    void verifyTestValue(const std::string& memberName, const T& expectedVal, const T& testVal, const TestType testType)
    {
        static constexpr float MaxAllowedError = 0.1f;
        if (std::abs(double(expectedVal) - double(testVal)) <= MaxAllowedError)
            return;

        std::stringstream ss;
        switch (testType)
        {
        case TestType::CPU:
            ss << "CPU TEST ERROR:\n";
        case TestType::GPU:
            ss << "GPU TEST ERROR:\n";
        }

        ss << "nbl::hlsl::" << memberName << " produced incorrect output! test value: " << testVal << " expected value: " << expectedVal << '\n';

        m_logger->log(ss.str().c_str(), system::ILogger::ELL_ERROR);
    }

    template<typename T>
    void verifyTestVector3dValue(const std::string& memberName, const nbl::hlsl::vector<T, 3>& expectedVal, const nbl::hlsl::vector<T, 3>& testVal, const TestType testType)
    {
        static constexpr float MaxAllowedError = 0.1f;
        if (std::abs(double(expectedVal.x) - double(testVal.x)) <= MaxAllowedError &&
            std::abs(double(expectedVal.y) - double(testVal.y)) <= MaxAllowedError &&
            std::abs(double(expectedVal.z) - double(testVal.z)) <= MaxAllowedError)
            return;

        std::stringstream ss;
        switch (testType)
        {
        case TestType::CPU:
            ss << "CPU TEST ERROR:\n";
        case TestType::GPU:
            ss << "GPU TEST ERROR:\n";
        }

        ss << "nbl::hlsl::" << memberName << " produced incorrect output! test value: " <<
            testVal.x << ' ' << testVal.y << ' ' << testVal.z <<
            " expected value: " << expectedVal.x << ' ' << expectedVal.y << ' ' << expectedVal.z << '\n';

        m_logger->log(ss.str().c_str(), system::ILogger::ELL_ERROR);
    }

    template<typename T>
    void verifyTestMatrix3x3Value(const std::string& memberName, const nbl::hlsl::matrix<T, 3, 3>& expectedVal, const nbl::hlsl::matrix<T, 3, 3>& testVal, const TestType testType)
    {
        for (int i = 0; i < 3; ++i)
        {
            auto expectedValRow = expectedVal[i];
            auto testValRow = testVal[i];
            verifyTestVector3dValue(memberName, expectedValRow, testValRow, testType);
        }
    }

protected:
    uint32_t m_queueFamily;
    core::smart_refctd_ptr<video::ILogicalDevice> m_device;
    core::smart_refctd_ptr<video::CVulkanConnection> m_api;
    video::IPhysicalDevice* m_physicalDevice;
    core::smart_refctd_ptr<asset::IAssetManager> m_assetMgr;
    core::smart_refctd_ptr<system::ILogger> m_logger;
    video::IDeviceMemoryAllocator::SAllocation m_inputBufferAllocation = {};
    video::IDeviceMemoryAllocator::SAllocation m_outputBufferAllocation = {};
    core::smart_refctd_ptr<video::IGPUCommandBuffer> m_cmdbuf = nullptr;
    core::smart_refctd_ptr<video::IGPUCommandPool> m_cmdpool = nullptr;
    core::smart_refctd_ptr<video::IGPUDescriptorSet> m_ds = nullptr;
    core::smart_refctd_ptr<video::IGPUPipelineLayout> m_pplnLayout = nullptr;
    core::smart_refctd_ptr<video::IGPUComputePipeline> m_pipeline;
    core::smart_refctd_ptr<video::ISemaphore> m_semaphore;
    video::IQueue* m_queue;
    uint64_t m_semaphoreCounter;
    
    template<typename InputStruct, typename OutputStruct>
    OutputStruct dispatch(const InputStruct& input)
    {
        // Update input buffer
        if (!m_inputBufferAllocation.memory->map({ 0ull,m_inputBufferAllocation.memory->getAllocationSize() }, video::IDeviceMemoryAllocation::EMCAF_READ))
            logFail("Failed to map the Device Memory!\n");

        const video::ILogicalDevice::MappedMemoryRange memoryRange(m_inputBufferAllocation.memory.get(), 0ull, m_inputBufferAllocation.memory->getAllocationSize());
        if (!m_inputBufferAllocation.memory->getMemoryPropertyFlags().hasFlags(video::IDeviceMemoryAllocation::EMPF_HOST_COHERENT_BIT))
            m_device->invalidateMappedMemoryRanges(1, &memoryRange);

        std::memcpy(static_cast<InputStruct*>(m_inputBufferAllocation.memory->getMappedPointer()), &input, sizeof(InputStruct));

        m_inputBufferAllocation.memory->unmap();

        // record command buffer
        m_cmdbuf->reset(video::IGPUCommandBuffer::RESET_FLAGS::NONE);
        m_cmdbuf->begin(video::IGPUCommandBuffer::USAGE::NONE);
        m_cmdbuf->beginDebugMarker("test", core::vector4df_SIMD(0, 1, 0, 1));
        m_cmdbuf->bindComputePipeline(m_pipeline.get());
        m_cmdbuf->bindDescriptorSets(nbl::asset::EPBP_COMPUTE, m_pplnLayout.get(), 0, 1, &m_ds.get());
        m_cmdbuf->dispatch(16, 1, 1);
        m_cmdbuf->endDebugMarker();
        m_cmdbuf->end();

        video::IQueue::SSubmitInfo submitInfos[1] = {};
        const video::IQueue::SSubmitInfo::SCommandBufferInfo cmdbufs[] = { {.cmdbuf = m_cmdbuf.get()} };
        submitInfos[0].commandBuffers = cmdbufs;
        const video::IQueue::SSubmitInfo::SSemaphoreInfo signals[] = { {.semaphore = m_semaphore.get(), .value = ++m_semaphoreCounter, .stageMask = asset::PIPELINE_STAGE_FLAGS::COMPUTE_SHADER_BIT} };
        submitInfos[0].signalSemaphores = signals;

        m_api->startCapture();
        m_queue->submit(submitInfos);
        m_api->endCapture();

        m_device->waitIdle();
        OutputStruct output;
        std::memcpy(&output, static_cast<OutputStruct*>(m_outputBufferAllocation.memory->getMappedPointer()), sizeof(OutputStruct));
        m_device->waitIdle();

        return output;
    }

private:
    template<typename... Args>
    inline void logFail(const char* msg, Args&&... args)
    {
        m_logger->log(msg, system::ILogger::ELL_ERROR, std::forward<Args>(args)...);
        exit(-1);
    }
};

class CTgmathTester final : public ITester
{
public:
    void performTests()
    {
        std::random_device rd;
        std::mt19937 mt(rd());

        std::uniform_real_distribution<float> realDistributionNeg(-50.0f, -1.0f);
        std::uniform_real_distribution<float> realDistributionPos(1.0f, 50.0f);
        std::uniform_real_distribution<float> realDistribution(-100.0f, 100.0f);
        std::uniform_real_distribution<float> realDistributionSmall(1.0f, 4.0f);
        std::uniform_int_distribution<int> intDistribution(-100, 100);
        std::uniform_int_distribution<int> coinFlipDistribution(0, 1);

        m_logger->log("tgmath.hlsl TESTS:", system::ILogger::ELL_PERFORMANCE);
        for (int i = 0; i < Iterations; ++i)
        {
            // Set input thest values that will be used in both CPU and GPU tests
            TgmathIntputTestValues commonTestInputValues;
            commonTestInputValues.floor = realDistribution(mt);
            commonTestInputValues.isnan = coinFlipDistribution(mt) ? realDistribution(mt) : std::numeric_limits<float>::quiet_NaN();
            commonTestInputValues.isinf = coinFlipDistribution(mt) ? realDistribution(mt) : std::numeric_limits<float>::infinity();
            commonTestInputValues.powX = realDistributionSmall(mt);
            commonTestInputValues.powY = realDistributionSmall(mt);
            commonTestInputValues.exp = realDistributionSmall(mt);
            commonTestInputValues.exp2 = realDistributionSmall(mt);
            commonTestInputValues.log = realDistribution(mt);
            commonTestInputValues.log2 = realDistribution(mt);
            commonTestInputValues.absF = realDistribution(mt);
            commonTestInputValues.absI = intDistribution(mt);
            commonTestInputValues.sqrt = realDistribution(mt);
            commonTestInputValues.sin = realDistribution(mt);
            commonTestInputValues.cos = realDistribution(mt);
            commonTestInputValues.acos = realDistribution(mt);
            commonTestInputValues.modf = realDistribution(mt);
            commonTestInputValues.round = realDistribution(mt);
            commonTestInputValues.roundEven = coinFlipDistribution(mt) ? realDistributionSmall(mt) : (static_cast<float32_t>(intDistribution(mt) / 2) + 0.5f);
            commonTestInputValues.trunc = realDistribution(mt);
            commonTestInputValues.ceil = realDistribution(mt);
            commonTestInputValues.fmaX = realDistribution(mt);
            commonTestInputValues.fmaY = realDistribution(mt);
            commonTestInputValues.fmaZ = realDistribution(mt);
            commonTestInputValues.ldexpArg = realDistributionSmall(mt);
            commonTestInputValues.ldexpExp = intDistribution(mt);

            commonTestInputValues.floorVec = float32_t3(realDistribution(mt), realDistribution(mt), realDistribution(mt));
            commonTestInputValues.isnanVec = float32_t3(realDistribution(mt), realDistribution(mt), realDistribution(mt));
            commonTestInputValues.isinfVec = float32_t3(realDistribution(mt), realDistribution(mt), realDistribution(mt));
            commonTestInputValues.powXVec = float32_t3(realDistributionSmall(mt), realDistributionSmall(mt), realDistributionSmall(mt));
            commonTestInputValues.powYVec = float32_t3(realDistributionSmall(mt), realDistributionSmall(mt), realDistributionSmall(mt));
            commonTestInputValues.expVec = float32_t3(realDistributionSmall(mt), realDistributionSmall(mt), realDistributionSmall(mt));
            commonTestInputValues.exp2Vec = float32_t3(realDistributionSmall(mt), realDistributionSmall(mt), realDistributionSmall(mt));
            commonTestInputValues.logVec = float32_t3(realDistribution(mt), realDistribution(mt), realDistribution(mt));
            commonTestInputValues.log2Vec = float32_t3(realDistribution(mt), realDistribution(mt), realDistribution(mt));
            commonTestInputValues.absFVec = float32_t3(realDistribution(mt), realDistribution(mt), realDistribution(mt));
            commonTestInputValues.absIVec = int32_t3(intDistribution(mt), intDistribution(mt), intDistribution(mt));
            commonTestInputValues.sqrtVec = float32_t3(realDistribution(mt), realDistribution(mt), realDistribution(mt));
            commonTestInputValues.sinVec = float32_t3(realDistribution(mt), realDistribution(mt), realDistribution(mt));
            commonTestInputValues.cosVec = float32_t3(realDistribution(mt), realDistribution(mt), realDistribution(mt));
            commonTestInputValues.acosVec = float32_t3(realDistribution(mt), realDistribution(mt), realDistribution(mt));
            commonTestInputValues.modfVec = float32_t3(realDistribution(mt), realDistribution(mt), realDistribution(mt));

            // use std library functions to determine expected test values, the output of functions from tgmath.hlsl will be verified against these values
            TgmathTestValues expectedTestValues;
            expectedTestValues.floor = std::floor(commonTestInputValues.floor);
            expectedTestValues.isnan = std::isnan(commonTestInputValues.isnan);
            expectedTestValues.isinf = std::isinf(commonTestInputValues.isinf);
            expectedTestValues.pow = std::pow(commonTestInputValues.powX, commonTestInputValues.powY);
            expectedTestValues.exp = std::exp(commonTestInputValues.exp);
            expectedTestValues.exp2 = std::exp2(commonTestInputValues.exp2);
            expectedTestValues.log = std::log(commonTestInputValues.log);
            expectedTestValues.log2 = std::log2(commonTestInputValues.log2);
            expectedTestValues.absF = std::abs(commonTestInputValues.absF);
            expectedTestValues.absI = std::abs(commonTestInputValues.absI);
            expectedTestValues.sqrt = std::sqrt(commonTestInputValues.sqrt);
            expectedTestValues.sin = std::sin(commonTestInputValues.sin);
            expectedTestValues.cos = std::cos(commonTestInputValues.cos);
            expectedTestValues.acos = std::acos(commonTestInputValues.acos);
            {
                float tmp;
                expectedTestValues.modf = std::modf(commonTestInputValues.modf, &tmp);
            }
            expectedTestValues.round = std::round(commonTestInputValues.round);
            // TODO: uncomment when C++23
            //expectedTestValues.roundEven = std::roundeven(commonTestInputValues.roundEven);
            // TODO: remove when C++23
            auto roundeven = [](const float& val) -> float
                {
                    float tmp;
                    if (std::abs(std::modf(val, &tmp)) == 0.5f)
                    {
                        int32_t result = static_cast<int32_t>(val);
                        if (result % 2 != 0)
                            result >= 0 ? ++result : --result;
                        return result;
                    }

                    return std::round(val);
                };
            expectedTestValues.roundEven = roundeven(commonTestInputValues.roundEven);

            expectedTestValues.trunc = std::trunc(commonTestInputValues.trunc);
            expectedTestValues.ceil = std::ceil(commonTestInputValues.ceil);
            expectedTestValues.fma = std::fma(commonTestInputValues.fmaX, commonTestInputValues.fmaY, commonTestInputValues.fmaZ);
            expectedTestValues.ldexp = std::ldexp(commonTestInputValues.ldexpArg, commonTestInputValues.ldexpExp);

            expectedTestValues.floorVec = float32_t3(std::floor(commonTestInputValues.floorVec.x), std::floor(commonTestInputValues.floorVec.y), std::floor(commonTestInputValues.floorVec.z));

            expectedTestValues.isnanVec = float32_t3(std::isnan(commonTestInputValues.isnanVec.x), std::isnan(commonTestInputValues.isnanVec.y), std::isnan(commonTestInputValues.isnanVec.z));
            expectedTestValues.isinfVec = float32_t3(std::isinf(commonTestInputValues.isinfVec.x), std::isinf(commonTestInputValues.isinfVec.y), std::isinf(commonTestInputValues.isinfVec.z));

            expectedTestValues.powVec.x = std::pow(commonTestInputValues.powXVec.x, commonTestInputValues.powYVec.x);
            expectedTestValues.powVec.y = std::pow(commonTestInputValues.powXVec.y, commonTestInputValues.powYVec.y);
            expectedTestValues.powVec.z = std::pow(commonTestInputValues.powXVec.z, commonTestInputValues.powYVec.z);

            expectedTestValues.expVec = float32_t3(std::exp(commonTestInputValues.expVec.x), std::exp(commonTestInputValues.expVec.y), std::exp(commonTestInputValues.expVec.z));
            expectedTestValues.exp2Vec = float32_t3(std::exp2(commonTestInputValues.exp2Vec.x), std::exp2(commonTestInputValues.exp2Vec.y), std::exp2(commonTestInputValues.exp2Vec.z));
            expectedTestValues.logVec = float32_t3(std::log(commonTestInputValues.logVec.x), std::log(commonTestInputValues.logVec.y), std::log(commonTestInputValues.logVec.z));
            expectedTestValues.log2Vec = float32_t3(std::log2(commonTestInputValues.log2Vec.x), std::log2(commonTestInputValues.log2Vec.y), std::log2(commonTestInputValues.log2Vec.z));
            expectedTestValues.absFVec = float32_t3(std::abs(commonTestInputValues.absFVec.x), std::abs(commonTestInputValues.absFVec.y), std::abs(commonTestInputValues.absFVec.z));
            expectedTestValues.absIVec = float32_t3(std::abs(commonTestInputValues.absIVec.x), std::abs(commonTestInputValues.absIVec.y), std::abs(commonTestInputValues.absIVec.z));
            expectedTestValues.sqrtVec = float32_t3(std::sqrt(commonTestInputValues.sqrtVec.x), std::sqrt(commonTestInputValues.sqrtVec.y), std::sqrt(commonTestInputValues.sqrtVec.z));
            expectedTestValues.cosVec = float32_t3(std::cos(commonTestInputValues.cosVec.x), std::cos(commonTestInputValues.cosVec.y), std::cos(commonTestInputValues.cosVec.z));
            expectedTestValues.sinVec = float32_t3(std::sin(commonTestInputValues.sinVec.x), std::sin(commonTestInputValues.sinVec.y), std::sin(commonTestInputValues.sinVec.z));
            expectedTestValues.acosVec = float32_t3(std::acos(commonTestInputValues.acosVec.x), std::acos(commonTestInputValues.acosVec.y), std::acos(commonTestInputValues.acosVec.z));
            {
                float tmp;
                expectedTestValues.modfVec = float32_t3(std::modf(commonTestInputValues.modfVec.x, &tmp), std::modf(commonTestInputValues.modfVec.y, &tmp), std::modf(commonTestInputValues.modfVec.z, &tmp));
            }
            expectedTestValues.roundVec = float32_t3(
                std::round(commonTestInputValues.roundVec.x),
                std::round(commonTestInputValues.roundVec.y),
                std::round(commonTestInputValues.roundVec.z)
            );
            // TODO: uncomment when C++23
            //expectedTestValues.roundEven = float32_t(
            //    std::roundeven(commonTestInputValues.roundEvenVec.x),
            //    std::roundeven(commonTestInputValues.roundEvenVec.y),
            //    std::roundeven(commonTestInputValues.roundEvenVec.z)
            //    );
            // TODO: remove when C++23
            expectedTestValues.roundEvenVec = float32_t3(
                roundeven(commonTestInputValues.roundEvenVec.x),
                roundeven(commonTestInputValues.roundEvenVec.y),
                roundeven(commonTestInputValues.roundEvenVec.z)
            );

            expectedTestValues.truncVec = float32_t3(std::trunc(commonTestInputValues.truncVec.x), std::trunc(commonTestInputValues.truncVec.y), std::trunc(commonTestInputValues.truncVec.z));
            expectedTestValues.ceilVec = float32_t3(std::ceil(commonTestInputValues.ceilVec.x), std::ceil(commonTestInputValues.ceilVec.y), std::ceil(commonTestInputValues.ceilVec.z));
            expectedTestValues.fmaVec = float32_t3(
                std::fma(commonTestInputValues.fmaXVec.x, commonTestInputValues.fmaYVec.x, commonTestInputValues.fmaZVec.x),
                std::fma(commonTestInputValues.fmaXVec.y, commonTestInputValues.fmaYVec.y, commonTestInputValues.fmaZVec.y),
                std::fma(commonTestInputValues.fmaXVec.z, commonTestInputValues.fmaYVec.z, commonTestInputValues.fmaZVec.z)
            );
            expectedTestValues.ldexpVec = float32_t3(
                std::ldexp(commonTestInputValues.ldexpArgVec.x, commonTestInputValues.ldexpExpVec.x),
                std::ldexp(commonTestInputValues.ldexpArgVec.y, commonTestInputValues.ldexpExpVec.y),
                std::ldexp(commonTestInputValues.ldexpArgVec.z, commonTestInputValues.ldexpExpVec.z)
            );

            performCpuTests(commonTestInputValues, expectedTestValues);
            performGpuTests(commonTestInputValues, expectedTestValues);
        }
        m_logger->log("tgmath.hlsl TESTS DONE.", system::ILogger::ELL_PERFORMANCE);
    }

private:
    inline static constexpr int Iterations = 100u;

    void performCpuTests(const TgmathIntputTestValues& commonTestInputValues, const TgmathTestValues& expectedTestValues)
    {
        TgmathTestValues cpuTestValues;
        cpuTestValues.fillTestValues(commonTestInputValues);
        verifyTestValues(expectedTestValues, cpuTestValues, ITester::TestType::CPU);
        
    }

    void performGpuTests(const TgmathIntputTestValues& commonTestInputValues, const TgmathTestValues& expectedTestValues)
    {
        TgmathTestValues gpuTestValues;
        gpuTestValues = dispatch<TgmathIntputTestValues, TgmathTestValues>(commonTestInputValues);
        verifyTestValues(expectedTestValues, gpuTestValues, ITester::TestType::GPU);
    }

    void verifyTestValues(const TgmathTestValues& expectedTestValues, const TgmathTestValues& testValues, ITester::TestType testType)
    {
        verifyTestValue("floor", expectedTestValues.floor, testValues.floor, testType);
        verifyTestValue("isnan", expectedTestValues.isnan, testValues.isnan, testType);
        verifyTestValue("isinf", expectedTestValues.isinf, testValues.isinf, testType);
        verifyTestValue("pow", expectedTestValues.pow, testValues.pow, testType);
        verifyTestValue("exp", expectedTestValues.exp, testValues.exp, testType);
        verifyTestValue("exp2", expectedTestValues.exp2, testValues.exp2, testType);
        verifyTestValue("log", expectedTestValues.log, testValues.log, testType);
        verifyTestValue("log2", expectedTestValues.log2, testValues.log2, testType);
        verifyTestValue("absF", expectedTestValues.absF, testValues.absF, testType);
        verifyTestValue("absI", expectedTestValues.absI, testValues.absI, testType);
        verifyTestValue("sqrt", expectedTestValues.sqrt, testValues.sqrt, testType);
        verifyTestValue("sin", expectedTestValues.sin, testValues.sin, testType);
        verifyTestValue("cos", expectedTestValues.cos, testValues.cos, testType);
        verifyTestValue("acos", expectedTestValues.acos, testValues.acos, testType);
        verifyTestValue("modf", expectedTestValues.modf, testValues.modf, testType);
        verifyTestValue("round", expectedTestValues.round, testValues.round, testType);
        verifyTestValue("roundEven", expectedTestValues.roundEven, testValues.roundEven, testType);
        verifyTestValue("trunc", expectedTestValues.trunc, testValues.trunc, testType);
        verifyTestValue("ceil", expectedTestValues.ceil, testValues.ceil, testType);
        verifyTestValue("fma", expectedTestValues.fma, testValues.fma, testType);
        verifyTestValue("ldexp", expectedTestValues.ldexp, testValues.ldexp, testType);

        verifyTestVector3dValue("floorVec", expectedTestValues.floorVec, testValues.floorVec, testType);
        verifyTestVector3dValue("isnanVec", expectedTestValues.isnanVec, testValues.isnanVec, testType);
        verifyTestVector3dValue("isinfVec", expectedTestValues.isinfVec, testValues.isinfVec, testType);
        verifyTestVector3dValue("powVec", expectedTestValues.powVec, testValues.powVec, testType);
        verifyTestVector3dValue("expVec", expectedTestValues.expVec, testValues.expVec, testType);
        verifyTestVector3dValue("exp2Vec", expectedTestValues.exp2Vec, testValues.exp2Vec, testType);
        verifyTestVector3dValue("logVec", expectedTestValues.logVec, testValues.logVec, testType);
        verifyTestVector3dValue("log2Vec", expectedTestValues.log2Vec, testValues.log2Vec, testType);
        verifyTestVector3dValue("absFVec", expectedTestValues.absFVec, testValues.absFVec, testType);
        verifyTestVector3dValue("absIVec", expectedTestValues.absIVec, testValues.absIVec, testType);
        verifyTestVector3dValue("sqrtVec", expectedTestValues.sqrtVec, testValues.sqrtVec, testType);
        verifyTestVector3dValue("sinVec", expectedTestValues.sinVec, testValues.sinVec, testType);
        verifyTestVector3dValue("cosVec", expectedTestValues.cosVec, testValues.cosVec, testType);
        verifyTestVector3dValue("acosVec", expectedTestValues.acosVec, testValues.acosVec, testType);
        verifyTestVector3dValue("modfVec", expectedTestValues.modfVec, testValues.modfVec, testType);
        verifyTestVector3dValue("roundVec", expectedTestValues.roundVec, testValues.roundVec, testType);
        verifyTestVector3dValue("roundEvenVec", expectedTestValues.roundEvenVec, testValues.roundEvenVec, testType);
        verifyTestVector3dValue("truncVec", expectedTestValues.truncVec, testValues.truncVec, testType);
        verifyTestVector3dValue("ceilVec", expectedTestValues.ceilVec, testValues.ceilVec, testType);
        verifyTestVector3dValue("fmaVec", expectedTestValues.fmaVec, testValues.fmaVec, testType);
        verifyTestVector3dValue("ldexp", expectedTestValues.ldexpVec, testValues.ldexpVec, testType);
    }
};

class CIntrinsicsTester final : public ITester
{
public:
    void performTests()
    {
        std::random_device rd;
        std::mt19937 mt(rd());

        std::uniform_real_distribution<float> realDistributionNeg(-50.0f, -1.0f);
        std::uniform_real_distribution<float> realDistributionPos(1.0f, 50.0f);
        std::uniform_real_distribution<float> realDistributionZeroToOne(0.0f, 1.0f);
        std::uniform_real_distribution<float> realDistribution(-100.0f, 100.0f);
        std::uniform_real_distribution<float> realDistributionSmall(1.0f, 4.0f);
        std::uniform_int_distribution<int> intDistribution(-100, 100);
        std::uniform_int_distribution<uint32_t> uintDistribution(0, 100);

        m_logger->log("intrinsics.hlsl TESTS:", system::ILogger::ELL_PERFORMANCE);
        for (int i = 0; i < Iterations; ++i)
        {
            // Set input thest values that will be used in both CPU and GPU tests
            IntrinsicsIntputTestValues testInput;
            testInput.bitCount = intDistribution(mt);
            testInput.crossLhs = float32_t3(realDistribution(mt), realDistribution(mt), realDistribution(mt));
            testInput.crossRhs = float32_t3(realDistribution(mt), realDistribution(mt), realDistribution(mt));
            testInput.clampVal = realDistribution(mt);
            testInput.clampMin = realDistributionNeg(mt);
            testInput.clampMax = realDistributionPos(mt);
            testInput.length = float32_t3(realDistribution(mt), realDistribution(mt), realDistribution(mt));
            testInput.normalize = float32_t3(realDistribution(mt), realDistribution(mt), realDistribution(mt));
            testInput.dotLhs = float32_t3(realDistributionSmall(mt), realDistributionSmall(mt), realDistributionSmall(mt));
            testInput.dotRhs = float32_t3(realDistributionSmall(mt), realDistributionSmall(mt), realDistributionSmall(mt));
            testInput.determinant = float32_t3x3(
                realDistributionSmall(mt), realDistributionSmall(mt), realDistributionSmall(mt),
                realDistributionSmall(mt), realDistributionSmall(mt), realDistributionSmall(mt),
                realDistributionSmall(mt), realDistributionSmall(mt), realDistributionSmall(mt)
            );
            testInput.findMSB = realDistribution(mt);
            testInput.findLSB = realDistribution(mt);
            testInput.inverse = float32_t3x3(
                realDistribution(mt), realDistribution(mt), realDistribution(mt),
                realDistribution(mt), realDistribution(mt), realDistribution(mt),
                realDistribution(mt), realDistribution(mt), realDistribution(mt)
            );
            testInput.transpose = float32_t3x3(
                realDistribution(mt), realDistribution(mt), realDistribution(mt),
                realDistribution(mt), realDistribution(mt), realDistribution(mt),
                realDistribution(mt), realDistribution(mt), realDistribution(mt)
            );
            testInput.mulLhs = float32_t3x3(
                realDistribution(mt), realDistribution(mt), realDistribution(mt),
                realDistribution(mt), realDistribution(mt), realDistribution(mt),
                realDistribution(mt), realDistribution(mt), realDistribution(mt)
            );
            testInput.mulRhs = float32_t3x3(
                realDistribution(mt), realDistribution(mt), realDistribution(mt),
                realDistribution(mt), realDistribution(mt), realDistribution(mt),
                realDistribution(mt), realDistribution(mt), realDistribution(mt)
            );
            testInput.minA = realDistribution(mt);
            testInput.minB = realDistribution(mt);
            testInput.maxA = realDistribution(mt);
            testInput.maxB = realDistribution(mt);
            testInput.rsqrt = realDistributionPos(mt);
            testInput.bitReverse = realDistribution(mt);
            testInput.frac = realDistribution(mt);
            testInput.mixX = realDistributionNeg(mt);
            testInput.mixY = realDistributionPos(mt);
            testInput.mixA = realDistributionZeroToOne(mt);
            testInput.sign = realDistribution(mt);
            testInput.radians = realDistribution(mt);
            testInput.degrees = realDistribution(mt);
            testInput.stepEdge = realDistribution(mt);
            testInput.stepX = realDistribution(mt);
            testInput.smoothStepEdge0 = realDistributionNeg(mt);
            testInput.smoothStepEdge1 = realDistributionPos(mt);
            testInput.smoothStepX = realDistribution(mt);

            testInput.bitCountVec = int32_t3(intDistribution(mt), intDistribution(mt), intDistribution(mt));
            testInput.clampValVec = float32_t3(realDistribution(mt), realDistribution(mt), realDistribution(mt));
            testInput.clampMinVec = float32_t3(realDistributionNeg(mt), realDistributionNeg(mt), realDistributionNeg(mt));
            testInput.clampMaxVec = float32_t3(realDistributionPos(mt), realDistributionPos(mt), realDistributionPos(mt));
            testInput.findMSBVec = uint32_t3(uintDistribution(mt), uintDistribution(mt), uintDistribution(mt));
            testInput.findLSBVec = uint32_t3(uintDistribution(mt), uintDistribution(mt), uintDistribution(mt));
            testInput.minAVec = float32_t3(realDistribution(mt), realDistribution(mt), realDistribution(mt));
            testInput.minBVec = float32_t3(realDistribution(mt), realDistribution(mt), realDistribution(mt));
            testInput.maxAVec = float32_t3(realDistribution(mt), realDistribution(mt), realDistribution(mt));
            testInput.maxBVec = float32_t3(realDistribution(mt), realDistribution(mt), realDistribution(mt));
            testInput.rsqrtVec = float32_t3(realDistributionPos(mt), realDistributionPos(mt), realDistributionPos(mt));
            testInput.bitReverseVec = uint32_t3(uintDistribution(mt), uintDistribution(mt), uintDistribution(mt));
            testInput.fracVec = float32_t3(realDistribution(mt), realDistribution(mt), realDistribution(mt));
            testInput.mixXVec = float32_t3(realDistributionNeg(mt), realDistributionNeg(mt), realDistributionNeg(mt));
            testInput.mixYVec = float32_t3(realDistributionPos(mt), realDistributionPos(mt), realDistributionPos(mt));
            testInput.mixAVec = float32_t3(realDistributionZeroToOne(mt), realDistributionZeroToOne(mt), realDistributionZeroToOne(mt));

            testInput.signVec = float32_t3(realDistribution(mt), realDistribution(mt), realDistribution(mt));
            testInput.radiansVec = float32_t3(realDistribution(mt), realDistribution(mt), realDistribution(mt));
            testInput.degreesVec = float32_t3(realDistribution(mt), realDistribution(mt), realDistribution(mt));
            testInput.stepEdgeVec = float32_t3(realDistribution(mt), realDistribution(mt), realDistribution(mt));
            testInput.stepXVec = float32_t3(realDistribution(mt), realDistribution(mt), realDistribution(mt));
            testInput.smoothStepEdge0Vec = float32_t3(realDistributionNeg(mt), realDistributionNeg(mt), realDistributionNeg(mt));
            testInput.smoothStepEdge1Vec = float32_t3(realDistributionPos(mt), realDistributionPos(mt), realDistributionPos(mt));
            testInput.smoothStepXVec = float32_t3(realDistribution(mt), realDistribution(mt), realDistribution(mt));
            testInput.faceForwardN = float32_t3(realDistribution(mt), realDistribution(mt), realDistribution(mt));
            testInput.faceForwardI = float32_t3(realDistribution(mt), realDistribution(mt), realDistribution(mt));
            testInput.faceForwardNref = float32_t3(realDistribution(mt), realDistribution(mt), realDistribution(mt));
            testInput.reflectI = float32_t3(realDistribution(mt), realDistribution(mt), realDistribution(mt));
            testInput.reflectN = glm::normalize(float32_t3(realDistribution(mt), realDistribution(mt), realDistribution(mt)));
            testInput.refractI = float32_t3(realDistribution(mt), realDistribution(mt), realDistribution(mt));
            testInput.refractN = glm::normalize(float32_t3(realDistribution(mt), realDistribution(mt), realDistribution(mt)));
            testInput.refractEta = realDistribution(mt);

            testInput.modfStruct = realDistribution(mt);
            testInput.modfStructVec = float32_t3(realDistribution(mt), realDistribution(mt), realDistribution(mt));
            testInput.frexpStruct = realDistribution(mt);
            testInput.frexpStructVec = float32_t3(realDistribution(mt), realDistribution(mt), realDistribution(mt));

            // use std library or glm functions to determine expected test values, the output of functions from intrinsics.hlsl will be verified against these values
            IntrinsicsTestValues expected;
            expected.bitCount = glm::bitCount(testInput.bitCount);
            expected.clamp = glm::clamp(testInput.clampVal, testInput.clampMin, testInput.clampMax);
            expected.length = glm::length(testInput.length);
            expected.dot = glm::dot(testInput.dotLhs, testInput.dotRhs);
            expected.determinant = glm::determinant(reinterpret_cast<typename float32_t3x3::Base const&>(testInput.determinant));
            expected.findMSB = glm::findMSB(testInput.findMSB);
            expected.findLSB = glm::findLSB(testInput.findLSB);
            expected.min = glm::min(testInput.minA, testInput.minB);
            expected.max = glm::max(testInput.maxA, testInput.maxB);
            expected.rsqrt = (1.0f / std::sqrt(testInput.rsqrt));
            expected.mix = std::lerp(testInput.mixX, testInput.mixY, testInput.mixA);
            expected.sign = glm::sign(testInput.sign);
            expected.radians = glm::radians(testInput.radians);
            expected.degrees = glm::degrees(testInput.degrees);
            expected.step = glm::step(testInput.stepEdge, testInput.stepX);
            expected.smoothStep = glm::smoothstep(testInput.smoothStepEdge0, testInput.smoothStepEdge1, testInput.smoothStepX);

            expected.frac = testInput.frac - std::floor(testInput.frac);
            expected.bitReverse = glm::bitfieldReverse(testInput.bitReverse);

            expected.normalize = glm::normalize(testInput.normalize);
            expected.cross = glm::cross(testInput.crossLhs, testInput.crossRhs);
            expected.bitCountVec = int32_t3(glm::bitCount(testInput.bitCountVec.x), glm::bitCount(testInput.bitCountVec.y), glm::bitCount(testInput.bitCountVec.z));
            expected.clampVec = float32_t3(
                glm::clamp(testInput.clampValVec.x, testInput.clampMinVec.x, testInput.clampMaxVec.x),
                glm::clamp(testInput.clampValVec.y, testInput.clampMinVec.y, testInput.clampMaxVec.y),
                glm::clamp(testInput.clampValVec.z, testInput.clampMinVec.z, testInput.clampMaxVec.z)
            );
            expected.findMSBVec = glm::findMSB(testInput.findMSBVec);
            expected.findLSBVec = glm::findLSB(testInput.findLSBVec);
            expected.minVec = float32_t3(
                glm::min(testInput.minAVec.x, testInput.minBVec.x),
                glm::min(testInput.minAVec.y, testInput.minBVec.y),
                glm::min(testInput.minAVec.z, testInput.minBVec.z)
            );
            expected.maxVec = float32_t3(
                glm::max(testInput.maxAVec.x, testInput.maxBVec.x),
                glm::max(testInput.maxAVec.y, testInput.maxBVec.y),
                glm::max(testInput.maxAVec.z, testInput.maxBVec.z)
            );
            expected.rsqrtVec = float32_t3(1.0f / std::sqrt(testInput.rsqrtVec.x), 1.0f / std::sqrt(testInput.rsqrtVec.y), 1.0f / std::sqrt(testInput.rsqrtVec.z));
            expected.bitReverseVec = glm::bitfieldReverse(testInput.bitReverseVec);
            expected.fracVec = float32_t3(
                testInput.fracVec.x - std::floor(testInput.fracVec.x),
                testInput.fracVec.y - std::floor(testInput.fracVec.y),
                testInput.fracVec.z - std::floor(testInput.fracVec.z));
            expected.mixVec.x = std::lerp(testInput.mixXVec.x, testInput.mixYVec.x, testInput.mixAVec.x);
            expected.mixVec.y = std::lerp(testInput.mixXVec.y, testInput.mixYVec.y, testInput.mixAVec.y);
            expected.mixVec.z = std::lerp(testInput.mixXVec.z, testInput.mixYVec.z, testInput.mixAVec.z);

            expected.signVec = glm::sign(testInput.signVec);
            expected.radiansVec = glm::radians(testInput.radiansVec);
            expected.degreesVec = glm::degrees(testInput.degreesVec);
            expected.stepVec = glm::step(testInput.stepEdgeVec, testInput.stepXVec);
            expected.smoothStepVec = glm::smoothstep(testInput.smoothStepEdge0Vec, testInput.smoothStepEdge1Vec, testInput.smoothStepXVec);
            expected.faceForward = glm::faceforward(testInput.faceForwardN, testInput.faceForwardI, testInput.faceForwardNref);
            expected.reflect = glm::reflect(testInput.reflectI, testInput.reflectN);
            expected.refract = glm::refract(testInput.refractI, testInput.refractN, testInput.refractEta);

            auto mulGlm = nbl::hlsl::mul(testInput.mulLhs, testInput.mulRhs);
            expected.mul = reinterpret_cast<float32_t3x3&>(mulGlm);
            auto transposeGlm = glm::transpose(reinterpret_cast<typename float32_t3x3::Base const&>(testInput.transpose));
            expected.transpose = reinterpret_cast<float32_t3x3&>(transposeGlm);
            auto inverseGlm = glm::inverse(reinterpret_cast<typename float32_t3x3::Base const&>(testInput.inverse));
            expected.inverse = reinterpret_cast<float32_t3x3&>(inverseGlm);

            {
                ModfOutput<float> expectedModfStructOutput;
                expectedModfStructOutput.fractionalPart = std::modf(testInput.modfStruct, &expectedModfStructOutput.wholeNumberPart);
                expected.modfStruct = expectedModfStructOutput;

                ModfOutput<float32_t3> expectedModfStructOutputVec;
                for (int i = 0; i < 3; ++i)
                    expectedModfStructOutputVec.fractionalPart[i] = std::modf(testInput.modfStructVec[i], &expectedModfStructOutputVec.wholeNumberPart[i]);
                expected.modfStructVec = expectedModfStructOutputVec;
            }

            {
                FrexpOutput<float> expectedFrexpStructOutput;
                expectedFrexpStructOutput.significand = std::frexp(testInput.frexpStruct, &expectedFrexpStructOutput.exponent);
                expected.frexpStruct = expectedFrexpStructOutput;

                FrexpOutput<float32_t3> expectedFrexpStructOutputVec;
                for (int i = 0; i < 3; ++i)
                    expectedFrexpStructOutputVec.significand[i] = std::frexp(testInput.frexpStructVec[i], &expectedFrexpStructOutputVec.exponent[i]);
                expected.frexpStructVec = expectedFrexpStructOutputVec;
            }

            performCpuTests(testInput, expected);
            performGpuTests(testInput, expected);
        }
        m_logger->log("intrinsics.hlsl TESTS DONE.", system::ILogger::ELL_PERFORMANCE);
    }

private:
    inline static constexpr int Iterations = 100u;

    void performCpuTests(const IntrinsicsIntputTestValues& commonTestInputValues, const IntrinsicsTestValues& expectedTestValues)
    {
        IntrinsicsTestValues cpuTestValues;
        cpuTestValues.fillTestValues(commonTestInputValues);
        verifyTestValues(expectedTestValues, cpuTestValues, ITester::TestType::CPU);

    }

    void performGpuTests(const IntrinsicsIntputTestValues& commonTestInputValues, const IntrinsicsTestValues& expectedTestValues)
    {
        IntrinsicsTestValues gpuTestValues;
        gpuTestValues = dispatch<IntrinsicsIntputTestValues, IntrinsicsTestValues>(commonTestInputValues);
        verifyTestValues(expectedTestValues, gpuTestValues, ITester::TestType::GPU);
    }

    void verifyTestValues(const IntrinsicsTestValues& expectedTestValues, const IntrinsicsTestValues& testValues, ITester::TestType testType)
    {
        verifyTestValue("bitCount", expectedTestValues.bitCount, testValues.bitCount, testType);
        verifyTestValue("clamp", expectedTestValues.clamp, testValues.clamp, testType);
        verifyTestValue("length", expectedTestValues.length, testValues.length, testType);
        verifyTestValue("dot", expectedTestValues.dot, testValues.dot, testType);
        verifyTestValue("determinant", expectedTestValues.determinant, testValues.determinant, testType);
        verifyTestValue("findMSB", expectedTestValues.findMSB, testValues.findMSB, testType);
        verifyTestValue("findLSB", expectedTestValues.findLSB, testValues.findLSB, testType);
        //verifyTestValue("min", expectedTestValues.min, testValues.min, testType);
        //verifyTestValue("max", expectedTestValues.max, testValues.max, testType);
        verifyTestValue("rsqrt", expectedTestValues.rsqrt, testValues.rsqrt, testType);
        verifyTestValue("frac", expectedTestValues.frac, testValues.frac, testType);
        verifyTestValue("bitReverse", expectedTestValues.bitReverse, testValues.bitReverse, testType);
        verifyTestValue("mix", expectedTestValues.mix, testValues.mix, testType);
        verifyTestValue("sign", expectedTestValues.sign, testValues.sign, testType);
        verifyTestValue("radians", expectedTestValues.radians, testValues.radians, testType);
        verifyTestValue("degrees", expectedTestValues.degrees, testValues.degrees, testType);
        verifyTestValue("step", expectedTestValues.step, testValues.step, testType);
        verifyTestValue("smoothStep", expectedTestValues.smoothStep, testValues.smoothStep, testType);

        verifyTestVector3dValue("normalize", expectedTestValues.normalize, testValues.normalize, testType);
        verifyTestVector3dValue("cross", expectedTestValues.cross, testValues.cross, testType);
        verifyTestVector3dValue("bitCountVec", expectedTestValues.bitCountVec, testValues.bitCountVec, testType);
        verifyTestVector3dValue("clampVec", expectedTestValues.clampVec, testValues.clampVec, testType);
        verifyTestVector3dValue("findMSBVec", expectedTestValues.findMSBVec, testValues.findMSBVec, testType);
        verifyTestVector3dValue("findLSBVec", expectedTestValues.findLSBVec, testValues.findLSBVec, testType);
        //verifyTestVector3dValue("minVec", expectedTestValues.minVec, testValues.minVec, testType);
        //verifyTestVector3dValue("maxVec", expectedTestValues.maxVec, testValues.maxVec, testType);
        verifyTestVector3dValue("rsqrtVec", expectedTestValues.rsqrtVec, testValues.rsqrtVec, testType);
        verifyTestVector3dValue("bitReverseVec", expectedTestValues.bitReverseVec, testValues.bitReverseVec, testType);
        verifyTestVector3dValue("fracVec", expectedTestValues.fracVec, testValues.fracVec, testType);
        verifyTestVector3dValue("mixVec", expectedTestValues.mixVec, testValues.mixVec, testType);

        verifyTestVector3dValue("signVec", expectedTestValues.signVec, testValues.signVec, testType);
        verifyTestVector3dValue("radiansVec", expectedTestValues.radiansVec, testValues.radiansVec, testType);
        verifyTestVector3dValue("degreesVec", expectedTestValues.degreesVec, testValues.degreesVec, testType);
        verifyTestVector3dValue("stepVec", expectedTestValues.stepVec, testValues.stepVec, testType);
        verifyTestVector3dValue("smoothStepVec", expectedTestValues.smoothStepVec, testValues.smoothStepVec, testType);
        verifyTestVector3dValue("faceForward", expectedTestValues.faceForward, testValues.faceForward, testType);
        verifyTestVector3dValue("reflect", expectedTestValues.reflect, testValues.reflect, testType);
        verifyTestVector3dValue("refract", expectedTestValues.refract, testValues.refract, testType);

        verifyTestMatrix3x3Value("mul", expectedTestValues.mul, testValues.mul, testType);
        verifyTestMatrix3x3Value("transpose", expectedTestValues.transpose, testValues.transpose, testType);
        verifyTestMatrix3x3Value("inverse", expectedTestValues.inverse, testValues.inverse, testType);

        // verify output of struct producing functions
        verifyTestValue("modfStruct", expectedTestValues.modfStruct.fractionalPart, testValues.modfStruct.fractionalPart, testType);
        verifyTestValue("modfStruct", expectedTestValues.modfStruct.wholeNumberPart, testValues.modfStruct.wholeNumberPart, testType);
        verifyTestVector3dValue("modfStructVec", expectedTestValues.modfStructVec.fractionalPart, testValues.modfStructVec.fractionalPart, testType);
        verifyTestVector3dValue("modfStructVec", expectedTestValues.modfStructVec.wholeNumberPart, testValues.modfStructVec.wholeNumberPart, testType);

        verifyTestValue("frexpStruct", expectedTestValues.frexpStruct.significand, testValues.frexpStruct.significand, testType);
        verifyTestValue("frexpStruct", expectedTestValues.frexpStruct.exponent, testValues.frexpStruct.exponent, testType);
        verifyTestVector3dValue("frexpStructVec", expectedTestValues.frexpStructVec.significand, testValues.frexpStructVec.significand, testType);
        verifyTestVector3dValue("frexpStructVec", expectedTestValues.frexpStructVec.exponent, testValues.frexpStructVec.exponent, testType);
    }
};

#undef VERIFY_TEST_VALUE
#undef VERIFY_TEST_VECTOR_VALUE

#endif