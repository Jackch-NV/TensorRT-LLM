#include "include/tensorrt_llm/executor/executor.h"
#include "tensorrt_llm/batch_manager/trtGptModelInflightBatching.h"
#include "tensorrt_llm/batch_manager/trtGptModelOptionalParams.h"
#include "tensorrt_llm/executor/types.h"
#include "tensorrt_llm/runtime/common.h"
#include "tensorrt_llm/runtime/iBuffer.h"
#include "tensorrt_llm/runtime/modelConfig.h"
#include "tensorrt_llm/runtime/rawEngine.h"
#include "tensorrt_llm/runtime/tllmLogger.h"
#include "tensorrt_llm/runtime/worldConfig.h"
#include "tests/utils/common.h"
#include "tests/utils/engines.h"
#include "tests/utils/executorUtils.h"

#include "gtest/gtest.h"
#include <NvInfer.h>
#include <NvInferRuntime.h>
#include <NvInferRuntimeBase.h>
#include <gmock/gmock.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <random>
#include <ratio>
#include <utility>
#include <vector>

namespace tensorrt_llm::testing
{

struct TrivialConstantDecoderWithTopKLogitsTestParameters
{
    using TupleT = std::tuple<runtime::SizeType32, runtime::SizeType32, runtime::SizeType32, runtime::SizeType32,
        runtime::SizeType32, runtime::SizeType32, runtime::SizeType32, runtime::SizeType32, runtime::SizeType32>;
    runtime::SizeType32 randomSeed;
    runtime::SizeType32 vocabSize;
    runtime::SizeType32 maxNumTokens;
    runtime::SizeType32 maxBeamWidth;
    runtime::SizeType32 maxBatchSize;
    runtime::SizeType32 numTopKLogits;
    runtime::SizeType32 numRequests;
    runtime::SizeType32 promptLength;
    runtime::SizeType32 maxOutputLength;

    // Constructor that takes a tuple
    TrivialConstantDecoderWithTopKLogitsTestParameters( // NOLINT: implicit to allow gtest to convert from tuple
                                                        // generated by 'combine'
        TupleT t)
        : randomSeed(std::get<0>(t))
        , vocabSize(std::get<1>(t))
        , maxNumTokens(std::get<2>(t))
        , maxBeamWidth(std::get<3>(t))
        , maxBatchSize(std::get<4>(t))
        , numTopKLogits(std::get<5>(t))
        , numRequests(std::get<6>(t))
        , promptLength(std::get<7>(t))
        , maxOutputLength(std::get<8>(t))
    {
    }
};

template <typename TLogits>
struct DecoderTestShared
{
    static constexpr runtime::SizeType32 kNumTokensPerBlock = 64;
    static constexpr runtime::SizeType32 kKvCacheMaxTokens = 2048 * 8;
    static constexpr auto kTopKTensorName = "topKLogits";

    DecoderTestShared(std::shared_ptr<runtime::TllmLogger> logger, std::mt19937 rng,
        std::shared_ptr<executor::Executor> executor, std::vector<TLogits> randomLogits)
        : logger(std::move(logger))
        , rng(rng)
        , executor(std::move(executor))
        , randomLogits(std::move(randomLogits)){};
    std::shared_ptr<runtime::TllmLogger> logger;
    std::mt19937 rng;
    std::shared_ptr<executor::Executor> executor;
    std::vector<TLogits> randomLogits;
};

template <typename TLogits>
std::unique_ptr<DecoderTestShared<TLogits>> SetupDecoderTest(
    TrivialConstantDecoderWithTopKLogitsTestParameters const& params)
{
    auto logger = std::make_shared<runtime::TllmLogger>();
    auto rng = std::mt19937(params.randomSeed);
    auto randomLogits = tensorrt_llm::testing::randomLogits<std::mt19937, TLogits>(params.vocabSize, &rng);
    auto const decoderParameters = tensorrt_llm::testing::utils::engines::ConstantTrivialDecoderParameters<TLogits>{
        tensorrt_llm::testing::utils::engines::TrivialDecoderParameters{params.vocabSize, params.maxBatchSize,
            params.maxNumTokens, DecoderTestShared<TLogits>::kNumTokensPerBlock, params.maxBeamWidth},
        randomLogits};
    auto engineHostMemory = tensorrt_llm::testing::utils::engines::createConstantTrivialDecoderWithTopKLogits<TLogits>(
        decoderParameters, params.numTopKLogits, DecoderTestShared<TLogits>::kTopKTensorName, logger);
    auto const engine = runtime::RawEngine(engineHostMemory.release());
    auto const dtype = runtime::TRTDataType<TLogits>::value;
    auto modelConfig = runtime::ModelConfig(params.vocabSize, 1, 1, 0, 1, 1, dtype);
    modelConfig.useGptAttentionPlugin(true);
    modelConfig.setModelVariant(runtime::ModelConfig::ModelVariant::kGpt);
    modelConfig.usePackedInput(true);
    modelConfig.setKVCacheType(runtime::ModelConfig::KVCacheType::kPAGED);
    modelConfig.setMaxNumTokens(params.maxNumTokens);
    modelConfig.setMaxBatchSize(params.maxBatchSize);
    modelConfig.setMaxBeamWidth(params.maxBeamWidth);
    modelConfig.setMaxSequenceLen(params.maxNumTokens);
    modelConfig.setMaxInputLen(params.maxNumTokens);
    modelConfig.setLayerTypes({runtime::ModelConfig::LayerType::kATTENTION});
    modelConfig.setTokensPerBlock(DecoderTestShared<TLogits>::kNumTokensPerBlock);
    modelConfig.setPagedContextFMHA(true);
    modelConfig.computeContextLogits(true);

    auto const worldConfig = runtime::WorldConfig();

    auto kvCacheConfig = executor::KvCacheConfig{};
    kvCacheConfig.setMaxTokens(DecoderTestShared<TLogits>::kKvCacheMaxTokens);

    auto const executorConfig
        = executor::ExecutorConfig(params.maxBeamWidth, executor::SchedulerConfig(), kvCacheConfig, true, true, 1, 1,
            executor::BatchingType::kINFLIGHT, params.maxBatchSize, params.maxNumTokens, std::nullopt, std::nullopt,
            std::nullopt, std::nullopt, 1, std::nullopt, executor::ExtendedRuntimePerfKnobConfig(), std::nullopt, 0,
            executor::ExecutorConfig::kDefaultMaxSeqIdleMicroseconds, std::nullopt, std::nullopt,
            std::vector<std::string>{DecoderTestShared<TLogits>::kTopKTensorName});

    auto optionalParams = batch_manager::TrtGptModelOptionalParams{executorConfig, false};
    auto model = std::make_shared<batch_manager::TrtGptModelInflightBatching>(
        logger, modelConfig, worldConfig, engine, false, optionalParams);

    return std::make_unique<DecoderTestShared<TLogits>>(
        logger, rng, std::make_shared<executor::Executor>(model, executorConfig), randomLogits);
}

template <typename TLogits>
class DecoderTopKGenerationLogitsTest
    : public ::testing::Test,
      public ::testing::WithParamInterface<TrivialConstantDecoderWithTopKLogitsTestParameters>
{
protected:
    std::unique_ptr<DecoderTestShared<TLogits>> state;

    DecoderTopKGenerationLogitsTest()
    {
        auto const params = GetParam();
        state = SetupDecoderTest<TLogits>(params);
    }

    void runTopKGenerationLogitsTest(TrivialConstantDecoderWithTopKLogitsTestParameters const& parameters)
    {
        auto const requestTokens = createConsecutiveTokenSequence(parameters.promptLength, parameters.vocabSize, 0);
        auto requests = std::vector<executor::Request>{};
        requests.reserve(static_cast<std::size_t>(parameters.numRequests));
        for (auto i = 0; i < parameters.numRequests; i++)
        {
            std::vector<executor::OutputConfig::AdditionalModelOutput> additionalOutputs{
                executor::OutputConfig::AdditionalModelOutput{DecoderTestShared<TLogits>::kTopKTensorName}};
            requests.emplace_back(requestTokens, parameters.maxOutputLength, false, executor::SamplingConfig{},
                executor::OutputConfig{false, false, false, true, false, false, additionalOutputs});
        }
        auto const accumulatedResponses
            = runThroughRequests(*state->executor, requests, std::chrono::duration<float, std::milli>(100000));
        ASSERT_EQ(accumulatedResponses.size(), parameters.numRequests);

        std::sort(state->randomLogits.begin(), state->randomLogits.end());
        std::reverse(state->randomLogits.begin(), state->randomLogits.end());
        for (auto const& [requestId, responses] : accumulatedResponses)
        {
            for (auto const& response : responses)
            {
                ASSERT_FALSE(response.hasError());
                auto const& tokensByBeam = response.getResult().outputTokenIds;
                auto const& additionalOutputs = response.getResult().additionalOutputs;
                ASSERT_EQ(additionalOutputs.size(), 1);
                auto const& topKLogits = additionalOutputs.front();
                auto const expectedOutputSize = (parameters.maxOutputLength - 1) * parameters.numTopKLogits;
                ASSERT_EQ(topKLogits.output.getSize(), expectedOutputSize);
                auto const* topKLogitsData = reinterpret_cast<TLogits const*>(topKLogits.output.getData());
                for (auto i = 0; i < parameters.numTopKLogits; i++)
                {
                    EXPECT_TRUE(almostEqual(topKLogitsData[i], state->randomLogits[i], 1e-5))
                        << "requestId " << requestId << " i " << i << ": " << topKLogitsData[i]
                        << " != " << state->randomLogits[i];
                }
                ASSERT_EQ(tokensByBeam.size(), 1);
                for (auto const& tokensForBeam : tokensByBeam)
                {
                    ASSERT_EQ(tokensForBeam.size(), parameters.maxOutputLength);
                }
            }
        }
    }
};

template <typename TLogits>
class DecoderTopKGenerationLogitsStreamingTest
    : public ::testing::Test,
      public ::testing::WithParamInterface<TrivialConstantDecoderWithTopKLogitsTestParameters>
{
protected:
    std::unique_ptr<DecoderTestShared<TLogits>> state;

    DecoderTopKGenerationLogitsStreamingTest()
    {
        auto const params = GetParam();
        state = SetupDecoderTest<TLogits>(params);
    }

    void runTopKGenerationLogitsStreamingTest(TrivialConstantDecoderWithTopKLogitsTestParameters const& parameters)
    {
        auto const requestTokens = createConsecutiveTokenSequence(parameters.promptLength, parameters.vocabSize, 0);
        auto requests = std::vector<executor::Request>{};
        requests.reserve(static_cast<std::size_t>(parameters.numRequests));
        for (auto i = 0; i < parameters.numRequests; i++)
        {
            std::vector<executor::OutputConfig::AdditionalModelOutput> additionalOutputs{
                executor::OutputConfig::AdditionalModelOutput{DecoderTestShared<TLogits>::kTopKTensorName}};
            requests.emplace_back(requestTokens, parameters.maxOutputLength, true, executor::SamplingConfig{},
                executor::OutputConfig{false, false, false, true, false, false, additionalOutputs});
        }
        auto const accumulatedResponses
            = runThroughRequests(*state->executor, requests, std::chrono::duration<float, std::milli>(100000));
        ASSERT_EQ(accumulatedResponses.size(), parameters.numRequests);

        std::sort(state->randomLogits.begin(), state->randomLogits.end());
        std::reverse(state->randomLogits.begin(), state->randomLogits.end());
        for (auto const& idResponsesKvp : accumulatedResponses)
        {
            auto const& [requestId, responses] = idResponsesKvp;
            auto numTokensForRequest = 0;
            for (auto const& response : responses)
            {
                ASSERT_FALSE(response.hasError());
                auto const& tokensByBeam = response.getResult().outputTokenIds;
                auto const& additionalOutputs = response.getResult().additionalOutputs;
                ASSERT_EQ(additionalOutputs.size(), 1);
                auto const& topKLogits = additionalOutputs.front();
                auto const expectedOutputSize = (parameters.maxOutputLength - 1) * parameters.numTopKLogits;
                ASSERT_EQ(topKLogits.output.getSize(), expectedOutputSize);
                auto const* topKLogitsData = reinterpret_cast<TLogits const*>(topKLogits.output.getData());
                for (auto i = 0; i < parameters.numTopKLogits; i++)
                {
                    EXPECT_TRUE(almostEqual(topKLogitsData[i], state->randomLogits[i], 1e-5))
                        << "requestId " << requestId << " i " << i << ": " << topKLogitsData[i]
                        << " != " << state->randomLogits[i];
                }
                ASSERT_EQ(tokensByBeam.size(), 1);
                for (auto const& tokensForBeam : tokensByBeam)
                {
                    numTokensForRequest += tokensForBeam.size();
                }
            }
            ASSERT_EQ(numTokensForRequest, parameters.maxOutputLength);
        }
    }
};

template <typename TLogits>
class DecoderTopKContextLogitsStreamingTest
    : public ::testing::Test,
      public ::testing::WithParamInterface<TrivialConstantDecoderWithTopKLogitsTestParameters>
{
protected:
    std::unique_ptr<DecoderTestShared<TLogits>> state;

    DecoderTopKContextLogitsStreamingTest()
    {
        auto const params = GetParam();
        state = SetupDecoderTest<TLogits>(params);
    }

    void runTopKContextLogitsTest(TrivialConstantDecoderWithTopKLogitsTestParameters const& parameters)
    {
        auto requests = std::vector<executor::Request>{};
        requests.reserve(static_cast<std::size_t>(parameters.numRequests));
        for (auto i = 0; i < parameters.numRequests; i++)
        {
            // create different sequence for each request to avoid KV cache reuse
            auto const requestTokens = createConsecutiveTokenSequence(parameters.promptLength, parameters.vocabSize, i);
            std::vector<executor::OutputConfig::AdditionalModelOutput> additionalOutputs{
                executor::OutputConfig::AdditionalModelOutput{DecoderTestShared<TLogits>::kTopKTensorName, true}};
            requests.emplace_back(requestTokens, parameters.maxOutputLength, true, executor::SamplingConfig{},
                executor::OutputConfig{false, false, false, true, false, false, additionalOutputs});
        }
        auto const& accumulatedResponses
            = runThroughRequests(*state->executor, requests, std::chrono::duration<float, std::milli>(100000));
        ASSERT_EQ(accumulatedResponses.size(), parameters.numRequests);

        std::sort(state->randomLogits.begin(), state->randomLogits.end());
        std::reverse(state->randomLogits.begin(), state->randomLogits.end());
        std::string const expectedAdditionalOutputName
            = std::string("context_") + DecoderTestShared<TLogits>::kTopKTensorName;
        for (auto const& idResponsesKvp : accumulatedResponses)
        {
            auto const& [requestId, responses] = idResponsesKvp;
            std::size_t numTokensForRequest{0};
            for (auto const& response : responses)
            {
                ASSERT_FALSE(response.hasError());
                auto const& tokensByBeam = response.getResult().outputTokenIds;
                auto const& additionalOutputs = response.getResult().additionalOutputs;
                ASSERT_EQ(additionalOutputs.size(), 2);
                auto const contextTopKLogitsPtr = std::find_if(additionalOutputs.cbegin(), additionalOutputs.cend(),
                    [&expectedAdditionalOutputName](auto const& ao)
                    { return ao.name == expectedAdditionalOutputName; });
                auto const expectedOutputSize = (parameters.promptLength) * parameters.numTopKLogits;
                ASSERT_EQ(contextTopKLogitsPtr->output.getSize(), expectedOutputSize);
                auto const* topKLogitsData = reinterpret_cast<TLogits const*>(contextTopKLogitsPtr->output.getData());
                for (auto i = 0; i < parameters.numTopKLogits; i++)
                {
                    EXPECT_TRUE(almostEqual(topKLogitsData[i], state->randomLogits[i], 1e-5))
                        << "requestId " << requestId << " i " << i << ": " << topKLogitsData[i]
                        << " != " << state->randomLogits[i];
                }
                ASSERT_EQ(tokensByBeam.size(), 1);
                for (auto const& tokensForBeam : tokensByBeam)
                {
                    numTokensForRequest += static_cast<std::size_t>(tokensForBeam.size());
                }
            }
            ASSERT_EQ(numTokensForRequest, parameters.maxOutputLength);
        }
    }
};

template <typename TLogits>
class DecoderTopKContextLogitsTest
    : public ::testing::Test,
      public ::testing::WithParamInterface<TrivialConstantDecoderWithTopKLogitsTestParameters>
{
protected:
    std::unique_ptr<DecoderTestShared<TLogits>> state;

    DecoderTopKContextLogitsTest()
    {
        auto const params = GetParam();
        state = SetupDecoderTest<TLogits>(params);
    }

    void runTopKContextLogitsTest(TrivialConstantDecoderWithTopKLogitsTestParameters const& parameters)
    {
        auto requests = std::vector<executor::Request>{};
        requests.reserve(static_cast<std::size_t>(parameters.numRequests));
        for (auto i = 0; i < parameters.numRequests; i++)
        {
            // create different sequence for each request to avoid KV cache reuse
            auto const requestTokens = createConsecutiveTokenSequence(parameters.promptLength, parameters.vocabSize, i);
            std::vector<executor::OutputConfig::AdditionalModelOutput> additionalOutputs{
                executor::OutputConfig::AdditionalModelOutput{DecoderTestShared<TLogits>::kTopKTensorName, true}};
            requests.emplace_back(requestTokens, parameters.maxOutputLength, false, executor::SamplingConfig{},
                executor::OutputConfig{false, false, false, true, false, false, additionalOutputs});
        }
        auto const accumulatedResponses
            = runThroughRequests(*state->executor, requests, std::chrono::duration<float, std::milli>(100000));
        ASSERT_EQ(accumulatedResponses.size(), parameters.numRequests);

        std::sort(state->randomLogits.begin(), state->randomLogits.end());
        std::reverse(state->randomLogits.begin(), state->randomLogits.end());
        std::string const expectedAdditionalOutputName
            = std::string("context_") + DecoderTestShared<TLogits>::kTopKTensorName;
        for (auto const& idResponsesKvp : accumulatedResponses)
        {
            auto const& [requestId, responses] = idResponsesKvp;
            for (auto const& response : responses)
            {
                ASSERT_FALSE(response.hasError());
                auto const& tokensByBeam = response.getResult().outputTokenIds;
                auto const& additionalOutputs = response.getResult().additionalOutputs;
                ASSERT_EQ(additionalOutputs.size(), 2);
                auto const contextTopKLogitsPtr = std::find_if(additionalOutputs.cbegin(), additionalOutputs.cend(),
                    [&expectedAdditionalOutputName](auto const& ao)
                    { return ao.name == expectedAdditionalOutputName; });
                auto const expectedOutputSize = (parameters.promptLength) * parameters.numTopKLogits;
                ASSERT_EQ(contextTopKLogitsPtr->output.getSize(), expectedOutputSize);
                auto const* topKLogitsData = reinterpret_cast<TLogits const*>(contextTopKLogitsPtr->output.getData());
                for (auto i = 0; i < parameters.numTopKLogits; i++)
                {
                    EXPECT_TRUE(almostEqual(topKLogitsData[i], state->randomLogits[i], 1e-5))
                        << "requestId " << requestId << " i " << i << ": " << topKLogitsData[i]
                        << " != " << state->randomLogits[i];
                }
                ASSERT_EQ(tokensByBeam.size(), 1);
                for (auto const& tokensForBeam : tokensByBeam)
                {
                    ASSERT_EQ(tokensForBeam.size(), parameters.maxOutputLength);
                }
            }
        }
    }
};

namespace
{
constexpr runtime::SizeType32 kRandomSeed1 = 45;
auto const randomSeeds = ::testing::Values(kRandomSeed1);

constexpr runtime::SizeType32 kMinVocabSize = 64;
constexpr runtime::SizeType32 kMaxVocabSize = 2048;
auto const vocabSizes = ::testing::Values(kMinVocabSize);

constexpr runtime::SizeType32 kMinMaxNumTokens = 2048;
auto const maxNumTokenses = ::testing::Values(kMinMaxNumTokens);

constexpr runtime::SizeType32 kMinBeamWidth = 1;
auto const beamWidths = ::testing::Values(kMinBeamWidth);

constexpr runtime::SizeType32 kMinMaxBatchSize = 2048;
auto const batchSizes = ::testing::Values(kMinMaxBatchSize);

constexpr runtime::SizeType32 kMinNumTopKLogits = 4;
constexpr runtime::SizeType32 kMaxNumTopKLogits = 32;
auto const numTopKLogitses = ::testing::Values(kMinNumTopKLogits, kMaxNumTopKLogits);

constexpr runtime::SizeType32 kMinNumRequests = 16;
constexpr runtime::SizeType32 kMaxNumRequests = 2048;
auto const numRequestses = ::testing::Values(kMinNumRequests);

constexpr runtime::SizeType32 kMinPromptLength = 4;
constexpr runtime::SizeType32 kMaxPromptLength = 512;
auto const promptLengths = ::testing::Values(kMinPromptLength, kMaxPromptLength);

constexpr runtime::SizeType32 kMinMaxOutputLength = 4;
constexpr runtime::SizeType32 kMaxMaxOutputLength = 256;
auto const maxOutputLengths = ::testing::Values(kMinMaxOutputLength, kMaxMaxOutputLength);

auto const paramGenerator = ::testing::ConvertGenerator<TrivialConstantDecoderWithTopKLogitsTestParameters::TupleT>(
    ::testing::Combine(randomSeeds, vocabSizes, maxNumTokenses, beamWidths, batchSizes, numTopKLogitses, numRequestses,
        promptLengths, maxOutputLengths));

auto const nameSuffixGenerator
    = [](::testing::TestParamInfo<TrivialConstantDecoderWithTopKLogitsTestParameters> const& info) -> std::string
{
    std::stringstream nameStringStream;
    nameStringStream << "_maxBatchSize_" << info.param.maxBatchSize << "_vocabSize_" << info.param.vocabSize
                     << "_maxBeamWidth_" << info.param.maxBeamWidth << "_maxNumTokens_" << info.param.maxNumTokens
                     << "_maxOutputLength_" << info.param.maxOutputLength << "_numRequests_" << info.param.numRequests
                     << "_numTopKLogits_" << info.param.numTopKLogits << "_promptLength_" << info.param.promptLength
                     << "_randomSeed_" << info.param.randomSeed;
    return nameStringStream.str();
};

} // namespace

using DecoderTopKGenerationLogitsFloatTest = DecoderTopKGenerationLogitsTest<float>;

TEST_P(DecoderTopKGenerationLogitsFloatTest, TestSizeAndValues)
{
    runTopKGenerationLogitsTest(GetParam());
}

INSTANTIATE_TEST_SUITE_P(Float, DecoderTopKGenerationLogitsFloatTest, paramGenerator, nameSuffixGenerator);

using DecoderTopKGenerationLogitsStreamingFloatTest = DecoderTopKGenerationLogitsStreamingTest<float>;

TEST_P(DecoderTopKGenerationLogitsStreamingFloatTest, TestSizeAndValues)
{
    runTopKGenerationLogitsStreamingTest(GetParam());
}

INSTANTIATE_TEST_SUITE_P(Float, DecoderTopKGenerationLogitsStreamingFloatTest, paramGenerator, nameSuffixGenerator);

using DecoderTopKContextLogitsStreamingFloatTest = DecoderTopKContextLogitsStreamingTest<float>;

TEST_P(DecoderTopKContextLogitsStreamingFloatTest, TestSizeAndValues)
{
    runTopKContextLogitsTest(GetParam());
}

INSTANTIATE_TEST_SUITE_P(Float, DecoderTopKContextLogitsStreamingFloatTest, paramGenerator, nameSuffixGenerator);

using DecoderTopKContextLogitsFloatTest = DecoderTopKContextLogitsTest<float>;

TEST_P(DecoderTopKContextLogitsFloatTest, TestSizeAndValues)
{
    runTopKContextLogitsTest(GetParam());
}

INSTANTIATE_TEST_SUITE_P(Float, DecoderTopKContextLogitsFloatTest, paramGenerator, nameSuffixGenerator);

} // namespace tensorrt_llm::testing
