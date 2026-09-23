/*
 * Copyright (c) 2017-2025 The Forge Interactive Inc.
 *
 * This file is part of The-Forge
 * (see https://github.com/ConfettiFX/The-Forge).
 *
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

// Unit Test to create Bottom and Top Level Acceleration Structures using Raytracing API.

// Interfaces
#include "../../../../Common_3/Application/Interfaces/IApp.h"
#include "../../../../Common_3/Application/Interfaces/ICameraController.h"
#include "../../../../Common_3/Application/Interfaces/IFont.h"
#include "../../../../Common_3/OS/Interfaces/IInput.h"
#include "../../../../Common_3/Application/Interfaces/IProfiler.h"
#include "../../../../Common_3/Application/Interfaces/IScreenshot.h"
#include "../../../../Common_3/Application/Interfaces/IUI.h"
#include "../../../../Common_3/Game/Interfaces/IScripting.h"
#include "../../../../Common_3/Graphics/Interfaces/IGraphics.h"
#include "../../../../Common_3/Resources/ResourceLoader/Interfaces/IResourceLoader.h"
#include "../../../../Common_3/Utilities/Interfaces/IFileSystem.h"
#include "../../../../Common_3/Utilities/Interfaces/ILog.h"
#include "../../../../Common_3/Utilities/Interfaces/ITime.h"
#include "../../../../Common_3/Application/ThirdParty/OpenSource/imgui/imgui.h"

#include "../../../../Common_3/Utilities/RingBuffer.h"

// Raytracing
#include "../../../../Common_3/Graphics/Interfaces/IRay.h"

// Math
#include "../../../../Common_3/Utilities/Math/MathTypes.h"

// Geometry
#include "../../../Visibility_Buffer/src/SanMiguel.h"

// fsl

#define NO_FSL_DEFINITIONS
#include "../../../../Common_3/Graphics/FSL/defaults.h"
#include "Shaders/FSL/Shared.fsl.h"
#include "Shaders/FSL/Global_wavefront.srt.h"

#include "../../../../Common_3/Utilities/Interfaces/IMemory.h"

#define USE_DENOISER  0
#define USE_RAY_QUERY 1

#define SCENE_SCALE   10.0f

bool gUseUavRwFallback = false;

// Optional D3D12 profiling aid. Disabled by default.
// SetStablePowerState generally requires Windows Developer Mode.
bool gStablePowerState = false;

static bool setStablePowerState(Renderer* pRenderer, bool enable)
{
#if defined(DIRECT3D12) && defined(_WINDOWS)
    if (!pRenderer || !pRenderer->mDx.pDevice)
        return false;

    const HRESULT hr = pRenderer->mDx.pDevice->SetStablePowerState(enable ? TRUE : FALSE);
    if (FAILED(hr))
    {
        LOGF(eERROR,
             "ID3D12Device::SetStablePowerState(%s) failed: HRESULT=0x%08X. "
             "Windows Developer Mode may need to be enabled.",
             enable ? "TRUE" : "FALSE", (uint32_t)hr);
        return false;
    }

    LOGF(eINFO, "D3D12 stable power state: %s", enable ? "enabled" : "disabled");
    return true;
#else
    UNREF_PARAM(pRenderer);
    UNREF_PARAM(enable);
    LOGF(eWARNING, "Stable power state is only implemented for D3D12 on Windows in this sample.");
    return false;
#endif
}

ICameraController* pCameraController = NULL;

ProfileToken gGpuProfileToken;

enum RaytracingTechnique
{
    RAY_QUERY = 0,
    WAVEFRONT_PATH_TRACING,
    WAVEFRONT_V2_PATH_TRACING,
    PERSISTENT_WAVE_256_PATH_TRACING,
    PERSISTENT_WAVE_128_PATH_TRACING,
    PERSISTENT_WAVE_64_PATH_TRACING,
    PERSISTENT_WAVEFRONT_SINGLE_BOUNCE_PATH_TRACING,
    RAYTRACING_TECHNIQUE_COUNT
};
uint32_t gRaytracingTechnique = RAY_QUERY;
bool     gRaytracingTechniqueSupported[RAYTRACING_TECHNIQUE_COUNT] = {};

// Persistent scheduler tuning. 2 means 64 resident groups by default.
uint32_t              gPersistentGroupPreset = 2;
static const uint32_t gPersistentGroupCounts[] = { 16, 32, 64, 128, 256, 512 };

uint32_t              gPersistentSchedulingMode = 0; // 0 = dynamic batch dequeue, 1 = static grid-stride
uint32_t              gPersistentBatchPreset = 0;    // default batch = 1 (previous behavior)
static const uint32_t gPersistentBatchSizes[] = { 1, 2, 4, 8 };

uint32_t              gPersistentWavefrontGroupPreset = 1; // 0 = TG64, 1 = TG128, 2 = TG256
static const uint32_t gPersistentWavefrontGroupSizes[] = { 64, 128, 256 };

uint32_t              gMaxBouncePreset = 1; // default = 2 bounces
static const uint32_t gMaxBounceValues[] = { 1, 2, 3, 4, 6, 8 };

static char    gRaytracingBenchmarkResultsStorage[64 * 1024] = {};
static char    gRaytracingBenchmarkTableStorage[64 * 1024] = {};
static char    gRaytracingBenchmarkProgressStorage[512] = {};
static bstring gRaytracingBenchmarkResults = bemptyfromarr(gRaytracingBenchmarkResultsStorage);
static bstring gRaytracingBenchmarkTable = bemptyfromarr(gRaytracingBenchmarkTableStorage);
static bstring gRaytracingBenchmarkProgress = bemptyfromarr(gRaytracingBenchmarkProgressStorage);
static float4 gRaytracingBenchmarkTextColor = float4(1.f, 1.f, 1.f, 1.f);

struct RaytracingPreset
{
    uint32_t mTechnique;
    uint32_t mBouncePreset;
    uint32_t mPersistentGroupPreset;
    uint32_t mPersistentSchedulingMode;
    uint32_t mPersistentBatchPreset;
};

static const RaytracingPreset gRaytracingPresets[] = {
    { RAY_QUERY, 1, 2, 0, 0 },                                  // RayQuery, 2 bounces
    { WAVEFRONT_V2_PATH_TRACING, 1, 2, 0, 0 },                   // Wavefront V2, 2 bounces
    { PERSISTENT_WAVE_256_PATH_TRACING, 1, 5, 0, 0 },            // Persistent warp TG256x512, 2 bounces
    { PERSISTENT_WAVEFRONT_SINGLE_BOUNCE_PATH_TRACING, 1, 5, 0, 0 }, // Persistent WaveFront TG128x512, 2 bounces
    { RAY_QUERY, 5, 2, 0, 0 },                                  // RayQuery, 8 bounces
    { WAVEFRONT_V2_PATH_TRACING, 5, 2, 0, 0 },                  // Wavefront V2, 8 bounces
    { PERSISTENT_WAVE_256_PATH_TRACING, 5, 5, 0, 0 },            // Persistent warp TG256x512, 8 bounces
    { PERSISTENT_WAVEFRONT_SINGLE_BOUNCE_PATH_TRACING, 5, 5, 0, 0 }, // Persistent WaveFront TG128x512, 8 bounces
};

static void applyRaytracingPreset(void* pUserData)
{
    const RaytracingPreset* pPreset = (const RaytracingPreset*)pUserData;
    gRaytracingTechnique = pPreset->mTechnique;
    gMaxBouncePreset = pPreset->mBouncePreset;
    gPersistentGroupPreset = pPreset->mPersistentGroupPreset;
    gPersistentSchedulingMode = pPreset->mPersistentSchedulingMode;
    gPersistentBatchPreset = pPreset->mPersistentBatchPreset;
    if (pPreset->mTechnique == PERSISTENT_WAVEFRONT_SINGLE_BOUNCE_PATH_TRACING)
        gPersistentWavefrontGroupPreset = 1;
    gStablePowerState = true;
}

class UnitTest_NativeRaytracing;
static UnitTest_NativeRaytracing* gRaytracingApp = NULL;
static void startRaytracingBenchmark(void* pUserData);
static void saveRaytracingBenchmark(void* pUserData);

struct PropData
{
    Geometry* pGeom = NULL;
    uint32_t  mMaterialCount = 0;
    Buffer*   pIndexBufferOffsetStream = NULL; // one per geometry
    Texture** pTextureStorage = NULL;
    mat4      mWorldMatrix;
};

PropData SanMiguelProp;
Scene*   pScene;

uint32_t gFontID = 0;

struct PathTracingData
{
    mat4   mHistoryProjView;
    float3 mHistoryLightDirection;
    uint   mFrameIndex;
    uint   mHaltonIndex;
    uint   mLastCameraMoveFrame;
    mat4   mWorldToCamera;
    mat4   mProjMat;
    mat4   mProjectView;
    mat4   mCameraToWorld;
    float  mProjNear;
    float  mProjFarMinusNear;
    float2 mZ1PlaneSize;
    float  mRandomSeed;
};

static float haltonSequence(uint index, uint base)
{
    float f = 1.f;
    float r = 0.f;

    while (index > 0)
    {
        f /= (float)base;
        r += f * (float)(index % base);
        index /= base;
    }

    return r;
}

class UnitTest_NativeRaytracing: public IApp
{
public:
    UnitTest_NativeRaytracing()
    {
        gRaytracingApp = this;
#ifdef TARGET_IOS
        mSettings.mContentScaleFactor = 1.f;
#endif
        ReadCmdArgs();
    }

    void startBenchmark()
    {
        if (mBenchmarkPhase != BENCHMARK_IDLE && mBenchmarkPhase != BENCHMARK_FINISHED)
            return;

        if (gRaytracingTechnique != WAVEFRONT_V2_PATH_TRACING &&
            gRaytracingTechnique != PERSISTENT_WAVE_256_PATH_TRACING &&
            gRaytracingTechnique != PERSISTENT_WAVEFRONT_SINGLE_BOUNCE_PATH_TRACING)
        {
            mBenchmarkPhase = BENCHMARK_IDLE;
            bassignliteral(&gRaytracingBenchmarkTable,
                           "Select Wavefront V2, Persistent Wave, or Persistent Wavefront before starting a scan.\n");
            updateBenchmarkResultsText();
            return;
        }

        if (!gRaytracingTechniqueSupported[gRaytracingTechnique])
        {
            mBenchmarkPhase = BENCHMARK_IDLE;
            bassignliteral(&gRaytracingBenchmarkTable, "The selected raytracing technique is not supported on this GPU.\n");
            updateBenchmarkResultsText();
            return;
        }

        mBenchmarkSavedTechnique = gRaytracingTechnique;
        mBenchmarkSavedGroupPreset = gPersistentGroupPreset;
        mBenchmarkSavedSchedulingMode = gPersistentSchedulingMode;
        mBenchmarkSavedBatchPreset = gPersistentBatchPreset;
        mBenchmarkSavedWavefrontGroupPreset = gPersistentWavefrontGroupPreset;
        mBenchmarkConfigCount = 0;
        mBenchmarkConfigIndex = 0;
        gStablePowerState = true;
        bassignliteral(&gRaytracingBenchmarkTable,
                       "Technique,Thread Group Size,Thread Groups,Average GPU (ms)\n");

        if (gRaytracingTechnique == WAVEFRONT_V2_PATH_TRACING)
        {
            // V2 currently has only the precompiled TG64 variant. Its dispatch count is
            // derived from the image size, so it cannot be reduced safely for a scan.
            mBenchmarkConfigs[mBenchmarkConfigCount++] = { WAVEFRONT_V2_PATH_TRACING, 64, UINT32_MAX };
        }
        else if (gRaytracingTechnique == PERSISTENT_WAVE_256_PATH_TRACING)
        {
            static const uint32_t threadGroupTechniques[] = {
                PERSISTENT_WAVE_64_PATH_TRACING,
                PERSISTENT_WAVE_128_PATH_TRACING,
                PERSISTENT_WAVE_256_PATH_TRACING,
            };
            static const uint32_t threadGroupSizes[] = { 64, 128, 256 };
            for (uint32_t techniqueIndex = 0; techniqueIndex < TF_ARRAY_COUNT(threadGroupTechniques); ++techniqueIndex)
            {
                const uint32_t technique = threadGroupTechniques[techniqueIndex];
                for (uint32_t groupPreset = 0; groupPreset < TF_ARRAY_COUNT(gPersistentGroupCounts); ++groupPreset)
                    mBenchmarkConfigs[mBenchmarkConfigCount++] = { technique, threadGroupSizes[techniqueIndex], groupPreset };
            }
        }
        else
        {
            for (uint32_t threadGroupPreset = 0; threadGroupPreset < TF_ARRAY_COUNT(gPersistentWavefrontGroupSizes); ++threadGroupPreset)
            {
                for (uint32_t groupPreset = 0; groupPreset < TF_ARRAY_COUNT(gPersistentGroupCounts); ++groupPreset)
                    mBenchmarkConfigs[mBenchmarkConfigCount++] = {
                        PERSISTENT_WAVEFRONT_SINGLE_BOUNCE_PATH_TRACING,
                        gPersistentWavefrontGroupSizes[threadGroupPreset],
                        groupPreset,
                    };
            }
        }

        prepareBenchmarkConfig();
        updateBenchmarkResultsText();
    }

    void saveBenchmark()
    {
        ImGui::SetClipboardText((const char*)bdata(&gRaytracingBenchmarkResults));

        FileStream stream = {};
        if (fsOpenStreamFromPath(RD_OTHER_FILES, "RaytracingBenchmark.txt", FM_WRITE, &stream))
        {
            fsWriteToStream(&stream, bdata(&gRaytracingBenchmarkResults), (size_t)blength(&gRaytracingBenchmarkResults));
            fsCloseStream(&stream);
            LOGF(eINFO, "Copied benchmark results to clipboard and saved RaytracingBenchmark.txt");
        }
        else
        {
            LOGF(eERROR, "Copied benchmark results to clipboard, but failed to save RaytracingBenchmark.txt");
        }
    }

private:
    enum BenchmarkPhase
    {
        BENCHMARK_IDLE,
        BENCHMARK_WARMUP,
        BENCHMARK_SAMPLE,
        BENCHMARK_FINISHED,
    };

    struct BenchmarkConfig
    {
        uint32_t mTechnique;
        uint32_t mThreadGroupSize;
        uint32_t mGroupPreset;
    };

    void updateBenchmarkResultsText()
    {
        char progress[256] = {};
        if (mBenchmarkPhase == BENCHMARK_WARMUP || mBenchmarkPhase == BENCHMARK_SAMPLE)
        {
            const char* phase = mBenchmarkPhase == BENCHMARK_WARMUP ? "warmup" : "sample";
            const uint32_t warmupFrames = mBenchmarkPhase == BENCHMARK_WARMUP ? mBenchmarkFrameCount : 20;
            const uint32_t sampleFrames = mBenchmarkPhase == BENCHMARK_SAMPLE ? mBenchmarkFrameCount : 0;
            const int64_t elapsedUsec = getUSec(false) - mBenchmarkStartUsec;
            snprintf(progress, sizeof(progress), "Configuration %u/%u | %s | warmup=%u/20 (%.1f/5000 ms) | sample=%u/20",
                     mBenchmarkConfigIndex + 1, mBenchmarkConfigCount, phase, warmupFrames,
                     min((double)elapsedUsec / 1000.0, 5000.0), sampleFrames);
        }
        else if (mBenchmarkPhase == BENCHMARK_FINISHED)
        {
            strcpy(progress, "Complete");
        }
        else
        {
            strcpy(progress, "Idle");
        }

        bassigncstr(&gRaytracingBenchmarkProgress, progress);
        bassign(&gRaytracingBenchmarkResults, &gRaytracingBenchmarkTable);
    }

    void prepareBenchmarkConfig()
    {
        const BenchmarkConfig& config = mBenchmarkConfigs[mBenchmarkConfigIndex];
        gRaytracingTechnique = config.mTechnique;
        if (config.mGroupPreset != UINT32_MAX)
            gPersistentGroupPreset = config.mGroupPreset;
        if (config.mTechnique == PERSISTENT_WAVEFRONT_SINGLE_BOUNCE_PATH_TRACING)
        {
            for (uint32_t i = 0; i < TF_ARRAY_COUNT(gPersistentWavefrontGroupSizes); ++i)
            {
                if (gPersistentWavefrontGroupSizes[i] == config.mThreadGroupSize)
                {
                    gPersistentWavefrontGroupPreset = i;
                    break;
                }
            }
        }
        mBenchmarkPhase = BENCHMARK_WARMUP;
        mBenchmarkFrameCount = 0;
        mBenchmarkGpuTimeSum = 0.0;
        mBenchmarkStartUsec = getUSec(false);
        mFrameIdx = 0;
        mPathTracingData = {};
        updateBenchmarkResultsText();
    }

    void processBenchmarkFrame()
    {
        if (mBenchmarkPhase == BENCHMARK_IDLE || mBenchmarkPhase == BENCHMARK_FINISHED)
            return;

        if (mBenchmarkPhase == BENCHMARK_WARMUP)
        {
            ++mBenchmarkFrameCount;
            const bool warmupDone = mBenchmarkFrameCount >= 20 || getUSec(false) - mBenchmarkStartUsec >= 5000000;
            if (warmupDone)
            {
                mBenchmarkPhase = BENCHMARK_SAMPLE;
                mBenchmarkFrameCount = 0;
                mBenchmarkGpuTimeSum = 0.0;
            }
            updateBenchmarkResultsText();
            return;
        }

        const float gpuTimeMs = getGpuProfileTime(mRaytracingGpuProfileToken);
        if (gpuTimeMs <= 0.0f)
            return;

        ++mBenchmarkFrameCount;
        mBenchmarkGpuTimeSum += gpuTimeMs;
        if (mBenchmarkFrameCount < 20)
        {
            updateBenchmarkResultsText();
            return;
        }

        const BenchmarkConfig& config = mBenchmarkConfigs[mBenchmarkConfigIndex];
        char resultLine[256] = {};
        if (config.mGroupPreset == UINT32_MAX)
        {
            snprintf(resultLine, sizeof(resultLine), "Wavefront V2,%u,derived,%.3f\n",
                     config.mThreadGroupSize, mBenchmarkGpuTimeSum / 20.0);
        }
        else
        {
            snprintf(resultLine, sizeof(resultLine), "%s,%u,%u,%.3f\n",
                     config.mTechnique == PERSISTENT_WAVEFRONT_SINGLE_BOUNCE_PATH_TRACING ? "Persistent Wavefront" : "Persistent Warp",
                     config.mThreadGroupSize, gPersistentGroupCounts[config.mGroupPreset], mBenchmarkGpuTimeSum / 20.0);
        }
        bcatcstr(&gRaytracingBenchmarkTable, resultLine);

        ++mBenchmarkConfigIndex;
        if (mBenchmarkConfigIndex < mBenchmarkConfigCount)
        {
            prepareBenchmarkConfig();
        }
        else
        {
            gRaytracingTechnique = mBenchmarkSavedTechnique;
            gPersistentGroupPreset = mBenchmarkSavedGroupPreset;
            gPersistentSchedulingMode = mBenchmarkSavedSchedulingMode;
            gPersistentBatchPreset = mBenchmarkSavedBatchPreset;
            gPersistentWavefrontGroupPreset = mBenchmarkSavedWavefrontGroupPreset;
            mBenchmarkPhase = BENCHMARK_FINISHED;
            mFrameIdx = 0;
            mPathTracingData = {};
            bcatcstr(&gRaytracingBenchmarkTable, "\nBenchmark complete.\n");
        }
        updateBenchmarkResultsText();
    }

public:

    void ReadCmdArgs()
    {
        for (int i = 0; i < argc; i += 1)
        {
            if (strcmp(argv[i], "-w") == 0 && i + 1 < argc)
                mSettings.mWidth = min(max(atoi(argv[i + 1]), 64), 10000);
            else if (strcmp(argv[i], "-h") == 0 && i + 1 < argc)
                mSettings.mHeight = min(max(atoi(argv[i + 1]), 64), 10000);
            else if (strcmp(argv[i], "-f") == 0)
            {
                mSettings.mFullScreen = true;
            }
            else if (strcmp(argv[i], "-preset") == 0 && i + 1 < argc)
            {
                const int presetNumber = atoi(argv[i + 1]);
                if (presetNumber >= 1 && presetNumber <= (int)TF_ARRAY_COUNT(gRaytracingPresets))
                    applyRaytracingPreset((void*)&gRaytracingPresets[presetNumber - 1]);
            }
        }
    }

    bool Init()
    {
        /************************************************************************/
        // 01 Init Raytracing
        /************************************************************************/
        RendererDesc settings = {};
        settings.mShaderTarget = SHADER_TARGET_6_3;
#if defined(SHADER_STATS_AVAILABLE)
        settings.mEnableShaderStats = true;
#endif
        initGPUConfiguration(settings.pExtendedSettings);
        initRenderer(GetName(), &settings, &pRenderer);
        // check for init success
        if (!pRenderer)
        {
            ShowUnsupportedMessage(getUnsupportedGPUMsg());
            return false;
        }
        setupGPUConfigurationPlatformParameters(pRenderer, settings.pExtendedSettings);

        /************************************************************************/
        // Raytracing setup
        /************************************************************************/
        initRaytracing(pRenderer, &pRaytracing);
        gRaytracingTechniqueSupported[RAY_QUERY] = pRenderer->pGpu->mRayQuerySupported;
        gRaytracingTechniqueSupported[WAVEFRONT_PATH_TRACING] = pRenderer->pGpu->mRayQuerySupported;
        gRaytracingTechniqueSupported[WAVEFRONT_V2_PATH_TRACING] = pRenderer->pGpu->mRayQuerySupported;
        gRaytracingTechniqueSupported[PERSISTENT_WAVE_256_PATH_TRACING] = pRenderer->pGpu->mRayQuerySupported;
        gRaytracingTechniqueSupported[PERSISTENT_WAVE_128_PATH_TRACING] = pRenderer->pGpu->mRayQuerySupported;
        gRaytracingTechniqueSupported[PERSISTENT_WAVE_64_PATH_TRACING] = pRenderer->pGpu->mRayQuerySupported;
        gRaytracingTechniqueSupported[PERSISTENT_WAVEFRONT_SINGLE_BOUNCE_PATH_TRACING] = pRenderer->pGpu->mRayQuerySupported;

        gUseUavRwFallback = !(pRenderer->pGpu->mFormatCaps[TinyImageFormat_R16G16B16A16_SFLOAT] & FORMAT_CAP_READ_WRITE);
        for (uint32_t t = WAVEFRONT_PATH_TRACING; t < RAYTRACING_TECHNIQUE_COUNT; ++t)
            gRaytracingTechniqueSupported[t] &= !gUseUavRwFallback;

        initResourceLoaderInterface(pRenderer);

        RootSignatureDesc rootDesc = {};
        INIT_RS_DESC(rootDesc, "default.rootsig", "compute.rootsig");
        initRootSignature(pRenderer, &rootDesc);

        QueueDesc queueDesc = {};
        queueDesc.mType = QUEUE_TYPE_GRAPHICS;
        queueDesc.mFlag = QUEUE_FLAG_INIT_MICROPROFILE;
        initQueue(pRenderer, &queueDesc, &pQueue);

        GpuCmdRingDesc cmdRingDesc = {};
        cmdRingDesc.pQueue = pQueue;
        cmdRingDesc.mPoolCount = gDataBufferCount;
        cmdRingDesc.mCmdPerPoolCount = 1;
        cmdRingDesc.mAddSyncPrimitives = true;
        initGpuCmdRing(pRenderer, &cmdRingDesc, &mCmdRing);

        initSemaphore(pRenderer, &pImageAcquiredSemaphore);

        // Load fonts
        FontDesc font = {};
        font.pFontPath = "TitilliumText/TitilliumText-Bold.otf";
        fntDefineFonts(&font, 1, &gFontID);

        FontSystemDesc fontRenderDesc = {};
        fontRenderDesc.pRenderer = pRenderer;
        if (!initFontSystem(&fontRenderDesc))
            return false; // report?

        // Initialize Forge User Interface Rendering
        UserInterfaceDesc uiRenderDesc = {};
        uiRenderDesc.pRenderer = pRenderer;
        initUserInterface(&uiRenderDesc);

        const char* ppGpuProfilerName[1] = { "Graphics" };

        // Initialize micro profiler and its UI.
        ProfilerDesc profiler = {};
        profiler.pRenderer = pRenderer;
        profiler.ppQueues = &pQueue;
        profiler.ppProfilerNames = ppGpuProfilerName;
        profiler.pProfileTokens = &gGpuProfileToken;
        profiler.mGpuProfilerCount = 1;
        initProfiler(&profiler);

        if (gRaytracingTechniqueSupported[RAY_QUERY])
        {
            VertexLayout vertexLayout = {};
            vertexLayout.mBindingCount = 3;
            vertexLayout.mAttribCount = 3;

            vertexLayout.mAttribs[0].mSemantic = SEMANTIC_POSITION;
            vertexLayout.mAttribs[0].mFormat = TinyImageFormat_R32G32B32_SFLOAT;
            vertexLayout.mAttribs[0].mBinding = 0;
            vertexLayout.mAttribs[0].mLocation = 0;
            vertexLayout.mAttribs[0].mOffset = 0;

            // normals
            vertexLayout.mAttribs[1].mSemantic = SEMANTIC_NORMAL;
            vertexLayout.mAttribs[1].mFormat = TinyImageFormat_R32_UINT;
            vertexLayout.mAttribs[1].mLocation = 1;
            vertexLayout.mAttribs[1].mBinding = 1;
            vertexLayout.mAttribs[1].mOffset = 0;

            // texture
            vertexLayout.mAttribs[2].mSemantic = SEMANTIC_TEXCOORD0;
            vertexLayout.mAttribs[2].mFormat = TinyImageFormat_R32_UINT;
            vertexLayout.mAttribs[2].mLocation = 2;
            vertexLayout.mAttribs[2].mBinding = 2;
            vertexLayout.mAttribs[2].mOffset = 0;

            SyncToken        token = {};
            GeometryLoadDesc loadDesc = {};
            loadDesc.pVertexLayout = &vertexLayout;
            loadDesc.mFlags = GEOMETRY_LOAD_FLAG_RAYTRACING_INPUT;
            pScene = initSanMiguel(&loadDesc, token, false);

            SanMiguelProp.mMaterialCount = pScene->geom->mDrawArgCount;
            SanMiguelProp.pGeom = pScene->geom;

            SanMiguelProp.pTextureStorage = (Texture**)tf_malloc(sizeof(Texture*) * SanMiguelProp.mMaterialCount);

            BufferLoadDesc desc = {};
            desc.mDesc.mDescriptors = DESCRIPTOR_TYPE_BUFFER_RAW;
            desc.mDesc.mMemoryUsage = RESOURCE_MEMORY_USAGE_GPU_ONLY;
            desc.mDesc.mStartState = RESOURCE_STATE_SHADER_RESOURCE;
            desc.mDesc.mSize = SanMiguelProp.pGeom->mDrawArgCount * sizeof(uint32_t);
            desc.mDesc.mElementCount = SanMiguelProp.pGeom->mDrawArgCount;
            desc.ppBuffer = &SanMiguelProp.pIndexBufferOffsetStream;
            addResource(&desc, NULL);

            BufferUpdateDesc updateDesc = {};
            updateDesc.pBuffer = SanMiguelProp.pIndexBufferOffsetStream;
            updateDesc.mCurrentState = desc.mDesc.mStartState;
            beginUpdateResource(&updateDesc);
            for (uint32_t i = 0, count = 0; i < SanMiguelProp.pGeom->mDrawArgCount; ++i)
            {
                ((uint32_t*)updateDesc.pMappedData)[count++] = SanMiguelProp.pGeom->pDrawArgs[i].mStartIndex;
            }
            endUpdateResource(&updateDesc);

            for (uint32_t i = 0; i < SanMiguelProp.mMaterialCount; ++i)
            {
                TextureLoadDesc texDesc = {};
                texDesc.pFileName = pScene->textures[i];
                texDesc.ppTexture = &SanMiguelProp.pTextureStorage[i];
                // Textures representing color should be stored in SRGB or HDR format
                texDesc.mCreationFlag = TEXTURE_CREATION_FLAG_SRGB;
                addResource(&texDesc, NULL);
            }

            waitForAllResourceLoads();

            /************************************************************************/
            // 02 Creation Acceleration Structure
            /************************************************************************/
            AccelerationStructureDesc         asDesc = {};
            AccelerationStructureGeometryDesc geomDescs[1024] = {};

            for (uint32_t i = 0; i < SanMiguelProp.pGeom->mDrawArgCount; i++)
            {
                IndirectDrawIndexArguments& drawArg = SanMiguelProp.pGeom->pDrawArgs[i];
                MaterialFlags               materialFlag = pScene->materialFlags[i];

                geomDescs[i].mFlags = (materialFlag & MATERIAL_FLAG_ALPHA_TESTED)
                                          ? ACCELERATION_STRUCTURE_GEOMETRY_FLAG_NO_DUPLICATE_ANYHIT_INVOCATION
                                          : ACCELERATION_STRUCTURE_GEOMETRY_FLAG_OPAQUE;
                geomDescs[i].pVertexBuffer = SanMiguelProp.pGeom->pVertexBuffers[0];
                geomDescs[i].mVertexCount = (uint32_t)SanMiguelProp.pGeom->mVertexCount;
                geomDescs[i].mVertexStride = SanMiguelProp.pGeom->mVertexStrides[0];
                geomDescs[i].mVertexFormat = TinyImageFormat_R32G32B32_SFLOAT;
                geomDescs[i].pIndexBuffer = SanMiguelProp.pGeom->pIndexBuffer;
                geomDescs[i].mIndexCount = drawArg.mIndexCount;
                geomDescs[i].mIndexOffset = drawArg.mStartIndex * sizeof(uint32_t);
                geomDescs[i].mIndexType = INDEX_TYPE_UINT32;
            }

            asDesc.mBottom.mDescCount = SanMiguelProp.pGeom->mDrawArgCount;
            asDesc.mBottom.pGeometryDescs = geomDescs;
            asDesc.mType = ACCELERATION_STRUCTURE_TYPE_BOTTOM;
            asDesc.mFlags = ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
            addAccelerationStructure(pRaytracing, &asDesc, &pSanMiguelBottomAS);

            // The transformation matrices for the instances
            SanMiguelProp.mWorldMatrix = mat4::scale(vec3(SCENE_SCALE));

            // Construct descriptions for Acceleration Structures Instances
            AccelerationStructureInstanceDesc instanceDesc = {};
            instanceDesc.mFlags = ACCELERATION_STRUCTURE_INSTANCE_FLAG_NONE;
            instanceDesc.mInstanceContributionToHitGroupIndex = 0;
            instanceDesc.mInstanceID = 0;
            instanceDesc.mInstanceMask = 1;
            memcpy(instanceDesc.mTransform, &SanMiguelProp.mWorldMatrix, sizeof(float[12]));
            instanceDesc.pBottomAS = pSanMiguelBottomAS;

            asDesc = {};
            asDesc.mType = ACCELERATION_STRUCTURE_TYPE_TOP;
            asDesc.mFlags = ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
            asDesc.mTop.mDescCount = 1;
            asDesc.mTop.pInstanceDescs = &instanceDesc;
            addAccelerationStructure(pRaytracing, &asDesc, &pSanMiguelAS);

            GpuCmdRingElement elem = getNextGpuCmdRingElement(&mCmdRing, true, 1);
            resetCmdPool(pRenderer, elem.pCmdPool);

            // Build Acceleration Structures
            RaytracingBuildASDesc buildASDesc = {};
            buildASDesc.pAccelerationStructure = pSanMiguelBottomAS;
            buildASDesc.mIssueRWBarrier = true;
            beginCmd(elem.pCmds[0]);
            cmdBuildAccelerationStructure(elem.pCmds[0], pRaytracing, &buildASDesc);

            buildASDesc = {};
            buildASDesc.pAccelerationStructure = pSanMiguelAS;
            cmdBuildAccelerationStructure(elem.pCmds[0], pRaytracing, &buildASDesc);

            endCmd(elem.pCmds[0]);

            QueueSubmitDesc submitDesc = {};
            submitDesc.mCmdCount = 1;
            submitDesc.ppCmds = elem.pCmds;
            submitDesc.pSignalFence = elem.pFence;
            submitDesc.mSubmitDone = true;
            queueSubmit(pQueue, &submitDesc);
            waitForFences(pRenderer, 1, &elem.pFence);

            removeAccelerationStructureScratch(pRaytracing, pSanMiguelBottomAS);
            removeAccelerationStructureScratch(pRaytracing, pSanMiguelAS);

            /************************************************************************/
            // 04 - Create Shader Binding Table to connect Pipeline with Acceleration Structure
            /************************************************************************/
            BufferLoadDesc ubDesc = {};
            ubDesc.mDesc.mDescriptors = DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            ubDesc.mDesc.mMemoryUsage = RESOURCE_MEMORY_USAGE_CPU_TO_GPU;
            ubDesc.mDesc.mFlags = BUFFER_CREATION_FLAG_PERSISTENT_MAP_BIT;
            ubDesc.mDesc.mSize = sizeof(ShadersConfigBlock);
            for (uint32_t i = 0; i < gDataBufferCount; i++)
            {
                ubDesc.ppBuffer = &pRayGenConfigBuffer[i];
                addResource(&ubDesc, NULL);
            }

#if USE_DENOISER
            addSSVGFDenoiser(pRenderer, &pDenoiser);
#endif
        }

        CameraMotionParameters cmp{ 200.0f, 250.0f, 300.0f };
        vec3                   camPos{ 80.0f, 60.0f, 50.0f };
        vec3                   lookAt{ 1.0f, 0.5f, 0.0f };

        pCameraController = initFpsCameraController(camPos, lookAt);
        pCameraController->setMotionParameters(cmp);

        // App Actions
        AddCustomInputBindings();
        initScreenshotCapturer(pRenderer, pQueue, GetName());
        mFrameIdx = 0;

        waitForAllResourceLoads();

        return true;
    }

    void Exit()
    {
        exitScreenshotCapturer();

        exitCameraController(pCameraController);

        exitProfiler();

        exitUserInterface();

        exitFontSystem();

        if (pScene)
        {
            removeResource(pScene->geomData);
            pScene->geomData = NULL;
            exitSanMiguel(pScene);
        }

        if (gRaytracingTechniqueSupported[RAY_QUERY])
        {
#if USE_DENOISER
            removeSSVGFDenoiser(pDenoiser);
#endif

            for (uint32_t i = 0; i < SanMiguelProp.mMaterialCount; ++i)
            {
                removeResource(SanMiguelProp.pTextureStorage[i]);
            }
            tf_free(SanMiguelProp.pTextureStorage);

            for (uint32_t i = 0; i < gDataBufferCount; i++)
            {
                removeResource(pRayGenConfigBuffer[i]);
            }

            removeAccelerationStructure(pRaytracing, pSanMiguelAS);
            removeAccelerationStructure(pRaytracing, pSanMiguelBottomAS);
            removeResource(SanMiguelProp.pGeom);
            removeResource(SanMiguelProp.pIndexBufferOffsetStream);
        }

        exitSemaphore(pRenderer, pImageAcquiredSemaphore);
        exitGpuCmdRing(pRenderer, &mCmdRing);
        exitQueue(pRenderer, pQueue);
        exitRootSignature(pRenderer);
        exitResourceLoaderInterface(pRenderer);

        // Do not leave the device in stable-power mode when the sample exits.
        if (gStablePowerState)
        {
            setStablePowerState(pRenderer, false);
            gStablePowerState = false;
        }

        exitRaytracing(pRenderer, pRaytracing);
        exitRenderer(pRenderer);
        exitGPUConfiguration();
        pRenderer = NULL;
    }

    bool Load(ReloadDesc* pReloadDesc)
    {
        mPathTracingData = {};
        mFrameIdx = 0;

        if (pReloadDesc->mType & (RELOAD_TYPE_RESIZE | RELOAD_TYPE_RENDERTARGET))
        {
            loadProfilerUI(mSettings.mWidth, mSettings.mHeight);

            UIComponentDesc guiDesc = {};
            guiDesc.mStartPosition = vec2(mSettings.mWidth * 0.01f, mSettings.mHeight * 0.2f);
            uiAddComponent(GetName(), &guiDesc, &pGuiWindow);

            static const char* raytracingOptions[] = {
                "Ray Query (MegaKernel)",
                "Wavefront v1 (full-dispatch generic)",
                "Wavefront v2 (compacted + indirect)",
                "Persistent Warps (TG256)",
                "Persistent Warps (TG128)",
                "Persistent Warps (TG64)",
                "Persistent Wavefront (1 Bounce/Task, TG128)",
            };
            COMPILE_ASSERT(TF_ARRAY_COUNT(raytracingOptions) == RAYTRACING_TECHNIQUE_COUNT);
            if (RAYTRACING_TECHNIQUE_COUNT > 1)
            {
                DropdownWidget raytracingDropdown;
                raytracingDropdown.mCount = TF_ARRAY_COUNT(raytracingOptions);
                raytracingDropdown.pNames = raytracingOptions;
                raytracingDropdown.pData = &gRaytracingTechnique;
                UIWidget* widget = uiAddComponentWidget(pGuiWindow, "Raytracing Technique", &raytracingDropdown, WIDGET_TYPE_DROPDOWN);
                luaRegisterWidget(widget);
            }
            else
            {
                LabelWidget raytracingLabel;
                uiAddComponentWidget(pGuiWindow, raytracingOptions[0], &raytracingLabel, WIDGET_TYPE_LABEL);
            }

            static const char* maxBounceOptions[] = { "1", "2", "3", "4", "6", "8" };
            DropdownWidget     maxBounceDropdown;
            maxBounceDropdown.mCount = TF_ARRAY_COUNT(maxBounceOptions);
            maxBounceDropdown.pNames = maxBounceOptions;
            maxBounceDropdown.pData = &gMaxBouncePreset;
            UIWidget* maxBounceWidget = uiAddComponentWidget(pGuiWindow, "Max Bounces", &maxBounceDropdown, WIDGET_TYPE_DROPDOWN);
            luaRegisterWidget(maxBounceWidget);

            static const char* raytracingPresetOptions[] = {
                "1", "2", "3", "4", "5", "6", "7", "8"
            };
            COMPILE_ASSERT(TF_ARRAY_COUNT(raytracingPresetOptions) == TF_ARRAY_COUNT(gRaytracingPresets));

            SeparatorWidget presetSeparator;
            uiAddComponentWidget(pGuiWindow, "", &presetSeparator, WIDGET_TYPE_SEPARATOR);
            LabelWidget presetLabel;
            uiAddComponentWidget(pGuiWindow, "Raytracing Presets", &presetLabel, WIDGET_TYPE_LABEL);
            for (uint32_t i = 0; i < TF_ARRAY_COUNT(raytracingPresetOptions); ++i)
            {
                ButtonWidget presetButton;
                UIWidget*   presetWidget =
                    uiAddComponentWidget(pGuiWindow, raytracingPresetOptions[i], &presetButton, WIDGET_TYPE_BUTTON);
                presetWidget->mSameLine = true;
                uiSetWidgetOnEditedCallback(presetWidget, (void*)&gRaytracingPresets[i], applyRaytracingPreset);
                luaRegisterWidget(presetWidget);
            }

            SeparatorWidget benchmarkSeparator;
            uiAddComponentWidget(pGuiWindow, "", &benchmarkSeparator, WIDGET_TYPE_SEPARATOR);
            LabelWidget benchmarkLabel;
            uiAddComponentWidget(pGuiWindow, "Raytracing Parameter Scan", &benchmarkLabel, WIDGET_TYPE_LABEL);

            ButtonWidget startBenchmarkButton;
            UIWidget*   startBenchmarkWidget =
                uiAddComponentWidget(pGuiWindow, "Start Parameter Scan", &startBenchmarkButton, WIDGET_TYPE_BUTTON);
            uiSetWidgetOnEditedCallback(startBenchmarkWidget, NULL, startRaytracingBenchmark);
            luaRegisterWidget(startBenchmarkWidget);

            ButtonWidget saveBenchmarkButton;
            UIWidget*   saveBenchmarkWidget =
                uiAddComponentWidget(pGuiWindow, "Save Benchmark Results", &saveBenchmarkButton, WIDGET_TYPE_BUTTON);
            uiSetWidgetOnEditedCallback(saveBenchmarkWidget, NULL, saveRaytracingBenchmark);
            luaRegisterWidget(saveBenchmarkWidget);

            DynamicTextWidget benchmarkResultsText = {};
            benchmarkResultsText.pText = &gRaytracingBenchmarkResults;
            benchmarkResultsText.pColor = &gRaytracingBenchmarkTextColor;
            luaRegisterWidget(uiAddComponentWidget(pGuiWindow, "Benchmark Results", &benchmarkResultsText, WIDGET_TYPE_DYNAMIC_TEXT));

            CheckboxWidget stablePowerStateCheckbox;
            stablePowerStateCheckbox.pData = &gStablePowerState;
            UIWidget* stablePowerWidget =
                uiAddComponentWidget(pGuiWindow, "Stable Power State (D3D12)", &stablePowerStateCheckbox, WIDGET_TYPE_CHECKBOX);
            luaRegisterWidget(stablePowerWidget);

            static const char* persistentGroupOptions[] = { "16", "32", "64", "128", "256", "512" };
            DropdownWidget     persistentGroupDropdown;
            persistentGroupDropdown.mCount = TF_ARRAY_COUNT(persistentGroupOptions);
            persistentGroupDropdown.pNames = persistentGroupOptions;
            persistentGroupDropdown.pData = &gPersistentGroupPreset;
            UIWidget* persistentGroupWidget =
                uiAddComponentWidget(pGuiWindow, "Persistent Resident Groups", &persistentGroupDropdown, WIDGET_TYPE_DROPDOWN);
            luaRegisterWidget(persistentGroupWidget);

            static const char* persistentWavefrontGroupOptions[] = { "64", "128", "256" };
            DropdownWidget     persistentWavefrontGroupDropdown;
            persistentWavefrontGroupDropdown.mCount = TF_ARRAY_COUNT(persistentWavefrontGroupOptions);
            persistentWavefrontGroupDropdown.pNames = persistentWavefrontGroupOptions;
            persistentWavefrontGroupDropdown.pData = &gPersistentWavefrontGroupPreset;
            UIWidget* persistentWavefrontGroupWidget = uiAddComponentWidget(
                pGuiWindow, "Persistent Wavefront Thread Group Size", &persistentWavefrontGroupDropdown, WIDGET_TYPE_DROPDOWN);
            luaRegisterWidget(persistentWavefrontGroupWidget);

            static const char* persistentSchedulingOptions[] = {
                "Dynamic Batch Dequeue (Atomic)",
                "Static Grid-Stride (No Atomic)",
            };
            DropdownWidget persistentSchedulingDropdown;
            persistentSchedulingDropdown.mCount = TF_ARRAY_COUNT(persistentSchedulingOptions);
            persistentSchedulingDropdown.pNames = persistentSchedulingOptions;
            persistentSchedulingDropdown.pData = &gPersistentSchedulingMode;
            luaRegisterWidget(
                uiAddComponentWidget(pGuiWindow, "Persistent Scheduling", &persistentSchedulingDropdown, WIDGET_TYPE_DROPDOWN));

            static const char* persistentBatchOptions[] = { "1", "2", "4", "8" };
            DropdownWidget     persistentBatchDropdown;
            persistentBatchDropdown.mCount = TF_ARRAY_COUNT(persistentBatchOptions);
            persistentBatchDropdown.pNames = persistentBatchOptions;
            persistentBatchDropdown.pData = &gPersistentBatchPreset;
            luaRegisterWidget(uiAddComponentWidget(pGuiWindow, "Persistent Batch Dequeue", &persistentBatchDropdown, WIDGET_TYPE_DROPDOWN));

            LabelWidget notSupportedLabel;
            uiAddDynamicWidgets(&mDynamicWidgets[0], "Raytracing technique is not supported on this GPU", &notSupportedLabel,
                                WIDGET_TYPE_LABEL);

            if (gRaytracingTechniqueSupported[RAY_QUERY])
            {
                SliderFloatWidget lightDirXSlider;
                lightDirXSlider.pData = &mLightDirection.x;
                lightDirXSlider.mMin = -2.0f;
                lightDirXSlider.mMax = 2.0f;
                lightDirXSlider.mStep = 0.001f;
                luaRegisterWidget(
                    uiAddDynamicWidgets(&mDynamicWidgets[1], "Light Direction X", &lightDirXSlider, WIDGET_TYPE_SLIDER_FLOAT));

                SliderFloatWidget lightDirYSlider;
                lightDirYSlider.pData = &mLightDirection.y;
                lightDirYSlider.mMin = -2.0f;
                lightDirYSlider.mMax = 2.0f;
                lightDirYSlider.mStep = 0.001f;
                luaRegisterWidget(
                    uiAddDynamicWidgets(&mDynamicWidgets[1], "Light Direction Y", &lightDirYSlider, WIDGET_TYPE_SLIDER_FLOAT));

                SliderFloatWidget lightDirZSlider;
                lightDirZSlider.pData = &mLightDirection.z;
                lightDirZSlider.mMin = -2.0f;
                lightDirZSlider.mMax = 2.0f;
                lightDirZSlider.mStep = 0.001f;
                luaRegisterWidget(
                    uiAddDynamicWidgets(&mDynamicWidgets[1], "Light Direction Z", &lightDirZSlider, WIDGET_TYPE_SLIDER_FLOAT));
            }

            if (!addSwapChain())
                return false;
        }

        if (gRaytracingTechniqueSupported[RAY_QUERY])
        {
            if (pReloadDesc->mType & RELOAD_TYPE_SHADER)
            {
                addShaders();
                addDescriptorSets();
            }

#if USE_DENOISER
            RenderTargetDesc rtDesc = {};
            rtDesc.mClearValue = { { FLT_MAX, 0, 0, 0 } };
            rtDesc.mWidth = mSettings.mWidth;
            rtDesc.mHeight = mSettings.mHeight;
            rtDesc.mDepth = 1;
            rtDesc.mSampleCount = SAMPLE_COUNT_1;
            rtDesc.mSampleQuality = 0;
            rtDesc.mArraySize = 1;
            rtDesc.mStartState = RESOURCE_STATE_SHADER_RESOURCE;

            rtDesc.mFormat = TinyImageFormat_R16G16B16A16_SFLOAT;
            addRenderTarget(pRenderer, &rtDesc, &pDepthNormalRenderTarget[0]);
            addRenderTarget(pRenderer, &rtDesc, &pDepthNormalRenderTarget[1]);

            rtDesc.mFormat = TinyImageFormat_R16G16_SFLOAT;
            rtDesc.mClearValue = { { 0, 0 } };
            addRenderTarget(pRenderer, &rtDesc, &pMotionVectorRenderTarget);

            rtDesc.mStartState = RESOURCE_STATE_DEPTH_WRITE;
            rtDesc.mFormat = TinyImageFormat_D32_SFLOAT;
            rtDesc.mClearValue = { { 0.0f, 0 } };
            rtDesc.mFlags = TEXTURE_CREATION_FLAG_ON_TILE;
            addRenderTarget(pRenderer, &rtDesc, &pDepthRenderTarget);
#endif

            if (pReloadDesc->mType & RELOAD_TYPE_RESIZE)
            {
                TextureDesc uavDesc = {};
                uavDesc.mArraySize = 1;
                uavDesc.mDepth = 1;
                uavDesc.mFormat = TinyImageFormat_R16G16B16A16_SFLOAT;
                uavDesc.mHeight = mSettings.mHeight;
                uavDesc.mMipLevels = 1;
                uavDesc.mSampleCount = SAMPLE_COUNT_1;
                uavDesc.mStartState = RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                uavDesc.mDescriptors = DESCRIPTOR_TYPE_TEXTURE | DESCRIPTOR_TYPE_RW_TEXTURE;
                uavDesc.mWidth = mSettings.mWidth;
                uavDesc.pName = "gOutput";
                TextureLoadDesc loadDesc = {};
                loadDesc.pDesc = &uavDesc;
                loadDesc.ppTexture = &pComputeOutput;
                addResource(&loadDesc, NULL);

                // Experimental path scheduling resources.
                // These are resolution-dependent, so create them together with the output UAV.
                const uint32_t pixelCount = mSettings.mWidth * mSettings.mHeight;

                BufferLoadDesc pathStateDesc = {};
                pathStateDesc.mDesc.mDescriptors = DESCRIPTOR_TYPE_RW_BUFFER;
                pathStateDesc.mDesc.mMemoryUsage = RESOURCE_MEMORY_USAGE_GPU_ONLY;
                pathStateDesc.mDesc.mStartState = RESOURCE_STATE_UNORDERED_ACCESS;
                // 10 float4 / pixel gives two ping-pong banks of 5 float4/path.
                // Wavefront V1 still uses the first bank with pixel-indexed path IDs.
                pathStateDesc.mDesc.mElementCount = pixelCount * 10;
                // RWBuffer(float4) is a TYPED buffer view, not a structured view.
                pathStateDesc.mDesc.mFormat = TinyImageFormat_R32G32B32A32_SFLOAT;
                pathStateDesc.mDesc.mStructStride = 0;
                pathStateDesc.mDesc.mSize = uint64_t(pathStateDesc.mDesc.mElementCount) * sizeof(float4);
                pathStateDesc.mDesc.pName = "WavefrontPathState";
                pathStateDesc.ppBuffer = &pWavefrontPathState;
                addResource(&pathStateDesc, NULL);

                BufferLoadDesc queueDesc = {};
                queueDesc.mDesc.mDescriptors = DESCRIPTOR_TYPE_RW_BUFFER;
                queueDesc.mDesc.mMemoryUsage = RESOURCE_MEMORY_USAGE_GPU_ONLY;
                queueDesc.mDesc.mStartState = RESOURCE_STATE_UNORDERED_ACCESS;
                queueDesc.mDesc.mElementCount = pixelCount;
                // RWBuffer(uint) is also a TYPED buffer view.
                queueDesc.mDesc.mFormat = TinyImageFormat_R32_UINT;
                queueDesc.mDesc.mStructStride = 0;
                queueDesc.mDesc.mSize = uint64_t(pixelCount) * sizeof(uint32_t);

                queueDesc.mDesc.pName = "WavefrontQueueA";
                queueDesc.ppBuffer = &pWavefrontQueueA;
                addResource(&queueDesc, NULL);

                queueDesc.mDesc.pName = "WavefrontQueueB";
                queueDesc.ppBuffer = &pWavefrontQueueB;
                addResource(&queueDesc, NULL);

                BufferLoadDesc counterDesc = {};
                counterDesc.mDesc.mDescriptors = DESCRIPTOR_TYPE_RW_BUFFER;
                counterDesc.mDesc.mMemoryUsage = RESOURCE_MEMORY_USAGE_GPU_ONLY;
                counterDesc.mDesc.mStartState = RESOURCE_STATE_UNORDERED_ACCESS;
                // 32 uint counters: enough for the generic persistent-wavefront
                // scheduler (8 heads + 8 counts + 8 outstanding + phase/status).
                counterDesc.mDesc.mElementCount = 32;
                counterDesc.mDesc.mFormat = TinyImageFormat_R32_UINT;
                counterDesc.mDesc.mStructStride = 0;
                counterDesc.mDesc.mSize = 32 * sizeof(uint32_t);
                counterDesc.mDesc.pName = "WavefrontCounters";
                counterDesc.ppBuffer = &pWavefrontCounters;
                addResource(&counterDesc, NULL);

                BufferLoadDesc indirectDesc = {};
                indirectDesc.mDesc.mDescriptors = DESCRIPTOR_TYPE_RW_BUFFER | DESCRIPTOR_TYPE_INDIRECT_BUFFER;
                indirectDesc.mDesc.mMemoryUsage = RESOURCE_MEMORY_USAGE_GPU_ONLY;
                indirectDesc.mDesc.mStartState = RESOURCE_STATE_INDIRECT_ARGUMENT;
                indirectDesc.mDesc.mElementCount = 3;
                indirectDesc.mDesc.mFormat = TinyImageFormat_R32_UINT;
                indirectDesc.mDesc.mStructStride = 0;
                indirectDesc.mDesc.mSize = sizeof(IndirectDispatchArguments);
                indirectDesc.mDesc.pName = "WavefrontIndirectArgsA";
                indirectDesc.ppBuffer = &pWavefrontIndirectArgs;
                addResource(&indirectDesc, NULL);

                indirectDesc.mDesc.pName = "WavefrontIndirectArgsB";
                indirectDesc.ppBuffer = &pWavefrontIndirectArgsB;
                addResource(&indirectDesc, NULL);

#if USE_DENOISER
                uavDesc.mFormat = TinyImageFormat_B10G10R10A2_UNORM;
                loadDesc.ppTexture = &pAlbedoTexture;
                addResource(&loadDesc, NULL);
#endif
            }

            if (pReloadDesc->mType & (RELOAD_TYPE_SHADER | RELOAD_TYPE_RENDERTARGET))
            {
                addPipelines();
            }

            prepareDescriptorSets();
        }

        UserInterfaceLoadDesc uiLoad = {};
        uiLoad.mColorFormat = pSwapChain->ppRenderTargets[0]->mFormat;
        uiLoad.mHeight = mSettings.mHeight;
        uiLoad.mWidth = mSettings.mWidth;
        uiLoad.mLoadType = pReloadDesc->mType;
        loadUserInterface(&uiLoad);

        FontSystemLoadDesc fontLoad = {};
        fontLoad.mColorFormat = pSwapChain->ppRenderTargets[0]->mFormat;
        fontLoad.mHeight = mSettings.mHeight;
        fontLoad.mWidth = mSettings.mWidth;
        fontLoad.mLoadType = pReloadDesc->mType;
        loadFontSystem(&fontLoad);

        updateUIVisibility();

        return true;
    }

    void Unload(ReloadDesc* pReloadDesc)
    {
        waitQueueIdle(pQueue);

        unloadFontSystem(pReloadDesc->mType);
        unloadUserInterface(pReloadDesc->mType);

        if (pReloadDesc->mType & (RELOAD_TYPE_RESIZE | RELOAD_TYPE_RENDERTARGET))
        {
            removeSwapChain(pRenderer, pSwapChain);

            uiRemoveDynamicWidgets(&mDynamicWidgets[0]);
            uiRemoveDynamicWidgets(&mDynamicWidgets[1]);
            uiRemoveComponent(pGuiWindow);
            unloadProfilerUI();
        }

        if (gRaytracingTechniqueSupported[RAY_QUERY])
        {
            if (pReloadDesc->mType & (RELOAD_TYPE_SHADER | RELOAD_TYPE_RENDERTARGET))
            {
                removePipelines();
            }

            if (pReloadDesc->mType & (RELOAD_TYPE_RESIZE | RELOAD_TYPE_RENDERTARGET))
            {
#if USE_DENOISER
                removeRenderTarget(pRenderer, pMotionVectorRenderTarget);
                removeRenderTarget(pRenderer, pDepthNormalRenderTarget[0]);
                removeRenderTarget(pRenderer, pDepthNormalRenderTarget[1]);
                removeRenderTarget(pRenderer, pDepthRenderTarget);
#endif
            }

            if (pReloadDesc->mType & RELOAD_TYPE_RESIZE)
            {
                removeResource(pComputeOutput);
                removeResource(pWavefrontPathState);
                removeResource(pWavefrontQueueA);
                removeResource(pWavefrontQueueB);
                removeResource(pWavefrontCounters);
                removeResource(pWavefrontIndirectArgs);
                removeResource(pWavefrontIndirectArgsB);
#if USE_DENOISER
                removeResource(pAlbedoTexture);
#endif
            }

            if (pReloadDesc->mType & RELOAD_TYPE_SHADER)
            {
                removeDescriptorSets();
                removeShaders();
            }
        }
    }

    void Update(float deltaTime)
    {
        PROFILER_SET_CPU_SCOPE("Cpu Profile", "update", 0x222222);

        if (!uiIsFocused())
        {
            pCameraController->onMove({ inputGetValue(0, CUSTOM_MOVE_X), inputGetValue(0, CUSTOM_MOVE_Y) });
            pCameraController->onRotate({ inputGetValue(0, CUSTOM_LOOK_X), inputGetValue(0, CUSTOM_LOOK_Y) });
            pCameraController->onMoveY(inputGetValue(0, CUSTOM_MOVE_UP));
            if (inputGetValue(0, CUSTOM_RESET_VIEW))
            {
                pCameraController->resetView();
            }
            if (inputGetValue(0, CUSTOM_TOGGLE_FULLSCREEN))
            {
                toggleFullscreen(pWindow);
            }
            if (inputGetValue(0, CUSTOM_TOGGLE_UI))
            {
                uiToggleActive();
            }
            if (inputGetValue(0, CUSTOM_DUMP_PROFILE))
            {
                dumpProfileData(GetName());
            }
            if (inputGetValue(0, CUSTOM_EXIT))
            {
                requestShutdown();
            }
        }

        pCameraController->update(deltaTime);

        static uint32_t prevRaytracingTechnique = UINT32_MAX;
        static uint32_t prevMaxBouncePreset = UINT32_MAX;
        static uint32_t prevPersistentGroupPreset = UINT32_MAX;
        static uint32_t prevPersistentSchedulingMode = UINT32_MAX;
        static uint32_t prevPersistentBatchPreset = UINT32_MAX;
        static uint32_t prevPersistentWavefrontGroupPreset = UINT32_MAX;
        if (gRaytracingTechnique != prevRaytracingTechnique || gMaxBouncePreset != prevMaxBouncePreset ||
            gPersistentGroupPreset != prevPersistentGroupPreset || gPersistentSchedulingMode != prevPersistentSchedulingMode ||
            gPersistentBatchPreset != prevPersistentBatchPreset ||
            gPersistentWavefrontGroupPreset != prevPersistentWavefrontGroupPreset)
        {
            mFrameIdx = 0;
            prevRaytracingTechnique = gRaytracingTechnique;
            prevMaxBouncePreset = gMaxBouncePreset;
            prevPersistentGroupPreset = gPersistentGroupPreset;
            prevPersistentSchedulingMode = gPersistentSchedulingMode;
            prevPersistentBatchPreset = gPersistentBatchPreset;
            prevPersistentWavefrontGroupPreset = gPersistentWavefrontGroupPreset;
            mPathTracingData = {};
            updateUIVisibility();
        }

        // Apply the UI toggle only when it changes. If the D3D12 call fails,
        // restore the checkbox to the last successfully applied state.
        static bool stablePowerInitialized = false;
        static bool stablePowerApplied = false;
        if (!stablePowerInitialized || gStablePowerState != stablePowerApplied)
        {
            if (setStablePowerState(pRenderer, gStablePowerState))
            {
                stablePowerApplied = gStablePowerState;
            }
            else
            {
                gStablePowerState = stablePowerApplied;
            }
            stablePowerInitialized = true;
        }

        mat4 viewMat = pCameraController->getViewMatrix().mCamera;

        const float aspectInverse = (float)mSettings.mHeight / (float)mSettings.mWidth;
        const float horizontalFOV = PI / 2.0f;
        const float nearPlane = 0.1f;
        const float farPlane = 1000.f;
        mat4        projMat = mat4::perspectiveLH_ReverseZ(horizontalFOV, aspectInverse, nearPlane, farPlane);
        mat4        projectView = projMat * viewMat;

        mPathTracingData.mWorldToCamera = viewMat;
        mPathTracingData.mProjMat = projMat;
        mPathTracingData.mProjectView = projectView;
        mPathTracingData.mCameraToWorld = inverse(viewMat);
        mPathTracingData.mProjNear = nearPlane;
        mPathTracingData.mProjFarMinusNear = farPlane - nearPlane;
        mPathTracingData.mZ1PlaneSize = float2(1.0f / projMat.getElem(0, 0), 1.0f / projMat.getElem(1, 1));

        mPathTracingData.mRandomSeed = randomFloat01();
    }

    void Draw()
    {
        if ((bool)pSwapChain->mEnableVsync != mSettings.mVSyncEnabled)
        {
            waitQueueIdle(pQueue);
            ::toggleVSync(pRenderer, &pSwapChain);
        }

        PROFILER_SET_CPU_SCOPE("Cpu Profile", "draw", 0xffffff);

        uint32_t swapchainImageIndex;
        acquireNextImage(pRenderer, pSwapChain, pImageAcquiredSemaphore, NULL, &swapchainImageIndex);

        GpuCmdRingElement elem = getNextGpuCmdRingElement(&mCmdRing, true, 1);
        FenceStatus       fenceStatus = {};
        getFenceStatus(pRenderer, elem.pFence, &fenceStatus);
        if (fenceStatus == FENCE_STATUS_INCOMPLETE)
            waitForFences(pRenderer, 1, &elem.pFence);

        resetCmdPool(pRenderer, elem.pCmdPool);

        Cmd* pCmd = elem.pCmds[0];
        beginCmd(pCmd);
        cmdBeginGpuFrameProfile(pCmd, gGpuProfileToken);

        RenderTarget* pRenderTarget = pSwapChain->ppRenderTargets[swapchainImageIndex];

        const bool raytracingTechniqueSupported = gRaytracingTechniqueSupported[gRaytracingTechnique];

        if (raytracingTechniqueSupported)
        {
            bool cameraMoved = memcmp(&mPathTracingData.mProjectView, &mPathTracingData.mHistoryProjView, sizeof(mat4)) != 0;
            bool lightMoved = memcmp(&mLightDirection, &mPathTracingData.mHistoryLightDirection, sizeof(float3)) != 0; //-V1014

#if USE_DENOISER
            if (lightMoved)
            {
                clearSSVGFDenoiserTemporalHistory(pDenoiser);
            }
#else
            if (cameraMoved || lightMoved)
            {
                mPathTracingData.mFrameIndex = 0;
                mPathTracingData.mHaltonIndex = 0;
            }
#endif
            if (cameraMoved)
            {
                mPathTracingData.mLastCameraMoveFrame = mPathTracingData.mFrameIndex;
            }

            ShadersConfigBlock cb;
            cb.mCameraToWorld = mPathTracingData.mCameraToWorld;
            cb.mProjNear = mPathTracingData.mProjNear;
            cb.mProjFarMinusNear = mPathTracingData.mProjFarMinusNear;
            cb.mZ1PlaneSize = mPathTracingData.mZ1PlaneSize;
            cb.mLightDirection = v3ToF3(normalize(f3Tov3(mLightDirection)));

            cb.mRandomSeed = mPathTracingData.mRandomSeed;

            // Loop through the first 16 items in the Halton sequence.
            // The Halton sequence takes one-based indices.
            cb.mSubpixelJitter =
                float2(haltonSequence(mPathTracingData.mHaltonIndex + 1, 2), haltonSequence(mPathTracingData.mHaltonIndex + 1, 3));

            cb.mFrameIndex = mPathTracingData.mFrameIndex;

            cb.mFramesSinceCameraMove = mPathTracingData.mFrameIndex - mPathTracingData.mLastCameraMoveFrame;
            cb.mWidth = mSettings.mWidth;
            cb.mHeight = mSettings.mHeight;
            cb.mWorldToCamera = mPathTracingData.mWorldToCamera;
            cb.mCameraToProjection = mPathTracingData.mProjMat;
            cb.mWorldToProjectionPrevious = mPathTracingData.mHistoryProjView;
            cb.mRtInvSize = float2(1.0f / mSettings.mWidth, 1.0f / mSettings.mHeight);

            cb.mWorldMatrix = SanMiguelProp.mWorldMatrix;

            cb.mMaxBounces = gMaxBounceValues[gMaxBouncePreset];
            cb.mPersistentBatchSize = gPersistentBatchSizes[gPersistentBatchPreset];
            uint32_t persistentTgSizeForCB = 64;
            if (PERSISTENT_WAVEFRONT_SINGLE_BOUNCE_PATH_TRACING == gRaytracingTechnique)
                persistentTgSizeForCB = gPersistentWavefrontGroupSizes[gPersistentWavefrontGroupPreset];
            else if (PERSISTENT_WAVE_128_PATH_TRACING == gRaytracingTechnique)
                persistentTgSizeForCB = 128;
            else if (PERSISTENT_WAVE_256_PATH_TRACING == gRaytracingTechnique)
                persistentTgSizeForCB = 256;
            const uint32_t persistentPixelCountForCB = mSettings.mWidth * mSettings.mHeight;
            const uint32_t persistentNormalGroupsForCB = round_up(persistentPixelCountForCB, persistentTgSizeForCB) / persistentTgSizeForCB;
            const uint32_t persistentActualGroupsForCB = min(gPersistentGroupCounts[gPersistentGroupPreset], persistentNormalGroupsForCB);
            cb.mPersistentTotalThreads = persistentActualGroupsForCB * persistentTgSizeForCB;

            BufferUpdateDesc bufferUpdate = { pRayGenConfigBuffer[mFrameIdx] };
            beginUpdateResource(&bufferUpdate);
            memcpy(bufferUpdate.pMappedData, &cb, sizeof(cb));
            endUpdateResource(&bufferUpdate);

            mPathTracingData.mHistoryProjView = mPathTracingData.mProjectView;
            mPathTracingData.mHistoryLightDirection = mLightDirection;
            mPathTracingData.mFrameIndex += 1;
            mPathTracingData.mHaltonIndex = (mPathTracingData.mHaltonIndex + 1) % 16;

#if USE_DENOISER
            RenderTarget* depthNormalTarget = pDepthNormalRenderTarget[mPathTracingData.mFrameIndex & 0x1];

            RenderTargetBarrier barriers[] = {
                { depthNormalTarget, RESOURCE_STATE_SHADER_RESOURCE, RESOURCE_STATE_RENDER_TARGET },
                { pMotionVectorRenderTarget, RESOURCE_STATE_SHADER_RESOURCE, RESOURCE_STATE_RENDER_TARGET },
            };
            cmdResourceBarrier(pCmd, 0, NULL, 0, NULL, 2, barriers);

            RenderTarget*   denoiserRTs[] = { depthNormalTarget, pMotionVectorRenderTarget };
            LoadActionsDesc loadActions = {};
            loadActions.mLoadActionsColor[0] = LOAD_ACTION_CLEAR;
            loadActions.mClearColorValues[0] = { { FLT_MAX, 0, 0, 0 } };
            loadActions.mLoadActionsColor[1] = LOAD_ACTION_CLEAR;
            loadActions.mClearColorValues[1] = { { 0, 0, 0, 0 } };
            loadActions.mLoadActionDepth = LOAD_ACTION_CLEAR;
            loadActions.mClearDepth = { { 0.f } };

            cmdBeginGpuTimestampQuery(pCmd, gGpuProfileToken, "Generate Denoiser Inputs");
            BindRenderTargetsDesc bindRenderTargets = {};
            bindRenderTargets.mRenderTargetCount = 2;
            bindRenderTargets.ppRenderTargets = denoiserRTs;
            bindRenderTargets.pDepthStencil = pDepthRenderTarget;
            bindRenderTargets.pLoadActions = &loadActions;
            bindRenderTargets.mDepthArraySlice = 0;
            bindRenderTargets.mDepthMipSlice = 0;
            cmdBindRenderTargets(pCmd, &bindRenderTargets);
            cmdSetViewport(pCmd, 0.0f, 0.0f, (float)pDepthRenderTarget->mWidth, (float)pDepthRenderTarget->mHeight, 0.0f, 1.0f);
            cmdSetScissor(pCmd, 0, 0, pDepthRenderTarget->mWidth, pDepthRenderTarget->mHeight);

            cmdBindPipeline(pCmd, pDenoiserInputsPipeline);

            cmdBindDescriptorSet(pCmd, mFrameIdx, pDenoiserInputsDescriptorSet);

            cmdBindVertexBuffer(pCmd, 2, SanMiguelProp.pGeom->pVertexBuffers, SanMiguelProp.pGeom->mVertexStrides, NULL);

            cmdBindIndexBuffer(pCmd, SanMiguelProp.pGeom->pIndexBuffer, 0, (IndexType)SanMiguelProp.pGeom->mIndexType);
            cmdDrawIndexed(pCmd, SanMiguelProp.pGeom->mIndexCount, 0, 0);

            cmdBindRenderTargets(pCmd, NULL);

            barriers[0] = { depthNormalTarget, RESOURCE_STATE_RENDER_TARGET, RESOURCE_STATE_SHADER_RESOURCE };
            barriers[1] = { pMotionVectorRenderTarget, RESOURCE_STATE_RENDER_TARGET, RESOURCE_STATE_SHADER_RESOURCE };
            cmdResourceBarrier(pCmd, 0, NULL, 0, NULL, 2, barriers);

            cmdEndGpuTimestampQuery(pCmd, gGpuProfileToken);
#endif
            /************************************************************************/
            // Transition UAV texture so raytracing shader can write to it
            /************************************************************************/
                mRaytracingGpuProfileToken = cmdBeginGpuTimestampQuery(pCmd, gGpuProfileToken, "Path Trace Scene");
            TextureBarrier uavBarriers[] = {
                { pComputeOutput, RESOURCE_STATE_PIXEL_SHADER_RESOURCE, RESOURCE_STATE_UNORDERED_ACCESS },
            };
            cmdResourceBarrier(pCmd, 0, NULL, TF_ARRAY_COUNT(uavBarriers), uavBarriers, 0, NULL);
            /************************************************************************/
            // Perform raytracing
            /************************************************************************/
            if (RAY_QUERY == gRaytracingTechnique)
            {
                cmdBindPipeline(pCmd, pPipeline[RAY_QUERY]);
                cmdBindDescriptorSet(pCmd, 0, pDescriptorSetRaytracing[gRaytracingTechnique]);
                cmdBindDescriptorSet(pCmd, 0, pDescriptorSetRaytracingPerBatch[gRaytracingTechnique]);
                cmdBindDescriptorSet(pCmd, mFrameIdx, pDescriptorSetUniforms[gRaytracingTechnique]);

                const uint32_t* numThreads = pShaderRayQuery->mNumThreadsPerGroup;
                uint32_t        groupX = round_up(mSettings.mWidth, numThreads[0]) / numThreads[0];
                uint32_t        groupY = round_up(mSettings.mHeight, numThreads[1]) / numThreads[1];
                cmdDispatch(pCmd, groupX, groupY, 1);
            }
            else if (WAVEFRONT_PATH_TRACING == gRaytracingTechnique)
            {
                // Generic full-dispatch wavefront baseline.
                // Every bounce compacts survivors into the alternate uint path-ID queue,
                // but each bounce still launches the full pixel-sized dispatch.
                const uint32_t pixelCount = mSettings.mWidth * mSettings.mHeight;
                const uint32_t threads = pShaderWavefrontGenerate->mNumThreadsPerGroup[0];
                const uint32_t groups = round_up(pixelCount, threads) / threads;
                const uint32_t maxBounces = gMaxBounceValues[gMaxBouncePreset];

                BufferBarrier uavSync[] = {
                    { pWavefrontPathState, RESOURCE_STATE_UNORDERED_ACCESS, RESOURCE_STATE_UNORDERED_ACCESS },
                    { pWavefrontQueueA, RESOURCE_STATE_UNORDERED_ACCESS, RESOURCE_STATE_UNORDERED_ACCESS },
                    { pWavefrontQueueB, RESOURCE_STATE_UNORDERED_ACCESS, RESOURCE_STATE_UNORDERED_ACCESS },
                    { pWavefrontCounters, RESOURCE_STATE_UNORDERED_ACCESS, RESOURCE_STATE_UNORDERED_ACCESS },
                };

                cmdBindPipeline(pCmd, pPipelineWavefrontGenerate);
                cmdBindDescriptorSet(pCmd, 0, pDescriptorSetRaytracing[gRaytracingTechnique]);
                cmdBindDescriptorSet(pCmd, 0, pDescriptorSetRaytracingPerBatch[gRaytracingTechnique]);
                cmdBindDescriptorSet(pCmd, mFrameIdx, pDescriptorSetUniforms[gRaytracingTechnique]);
                cmdDispatch(pCmd, groups, 1, 1);
                cmdResourceBarrier(pCmd, TF_ARRAY_COUNT(uavSync), uavSync, 0, NULL, 0, NULL);

                for (uint32_t bounce = 0; bounce < maxBounces; ++bounce)
                {
                    // Reset only the OUTPUT queue count. The input queue remains intact.
                    Pipeline* pResetPipeline = (bounce & 1u) ? pPipelineWavefrontResetA : pPipelineWavefrontResetB;
                    cmdBindPipeline(pCmd, pResetPipeline);
                    cmdBindDescriptorSet(pCmd, 0, pDescriptorSetRaytracing[gRaytracingTechnique]);
                    cmdBindDescriptorSet(pCmd, 0, pDescriptorSetRaytracingPerBatch[gRaytracingTechnique]);
                    cmdBindDescriptorSet(pCmd, mFrameIdx, pDescriptorSetUniforms[gRaytracingTechnique]);
                    cmdDispatch(pCmd, 1, 1, 1);
                    cmdResourceBarrier(pCmd, 1, &uavSync[3], 0, NULL, 0, NULL);

                    Pipeline* pBouncePipeline = (bounce & 1u) ? pPipelineWavefrontBounce1 : pPipelineWavefrontBounce0;
                    cmdBindPipeline(pCmd, pBouncePipeline);
                    cmdBindDescriptorSet(pCmd, 0, pDescriptorSetRaytracing[gRaytracingTechnique]);
                    cmdBindDescriptorSet(pCmd, 0, pDescriptorSetRaytracingPerBatch[gRaytracingTechnique]);
                    cmdBindDescriptorSet(pCmd, mFrameIdx, pDescriptorSetUniforms[gRaytracingTechnique]);
                    cmdDispatch(pCmd, groups, 1, 1);
                    cmdResourceBarrier(pCmd, TF_ARRAY_COUNT(uavSync), uavSync, 0, NULL, 0, NULL);
                }

                cmdBindPipeline(pCmd, pPipelineWavefrontResolve);
                cmdBindDescriptorSet(pCmd, 0, pDescriptorSetRaytracing[gRaytracingTechnique]);
                cmdBindDescriptorSet(pCmd, 0, pDescriptorSetRaytracingPerBatch[gRaytracingTechnique]);
                cmdBindDescriptorSet(pCmd, mFrameIdx, pDescriptorSetUniforms[gRaytracingTechnique]);
                cmdDispatch(pCmd, groups, 1, 1);
            }
            else if (WAVEFRONT_V2_PATH_TRACING == gRaytracingTechnique)
            {
                // Generic compacted wavefront:
                // primary -> bank A -> bank B -> bank A ... using DispatchIndirect
                // for every continuation bounce.
                const uint32_t pixelCount = mSettings.mWidth * mSettings.mHeight;
                const uint32_t threads = pShaderWavefrontV2Primary->mNumThreadsPerGroup[0];
                const uint32_t groups = round_up(pixelCount, threads) / threads;
                const uint32_t maxBounces = gMaxBounceValues[gMaxBouncePreset];

                // Keep both indirect buffers in UAV state while producers/reset shaders write
                // them. Each one is temporarily transitioned to INDIRECT_ARGUMENT only for
                // the dispatch that consumes it.
                BufferBarrier indirectToUav[] = {
                    { pWavefrontIndirectArgs, RESOURCE_STATE_INDIRECT_ARGUMENT, RESOURCE_STATE_UNORDERED_ACCESS },
                    { pWavefrontIndirectArgsB, RESOURCE_STATE_INDIRECT_ARGUMENT, RESOURCE_STATE_UNORDERED_ACCESS },
                };
                cmdResourceBarrier(pCmd, TF_ARRAY_COUNT(indirectToUav), indirectToUav, 0, NULL, 0, NULL);

                // Reset queue/args A for the primary producer.
                cmdBindPipeline(pCmd, pPipelineWavefrontV2Reset);
                cmdBindDescriptorSet(pCmd, 0, pDescriptorSetRaytracing[gRaytracingTechnique]);
                cmdBindDescriptorSet(pCmd, 0, pDescriptorSetRaytracingPerBatch[gRaytracingTechnique]);
                cmdBindDescriptorSet(pCmd, mFrameIdx, pDescriptorSetUniforms[gRaytracingTechnique]);
                cmdDispatch(pCmd, 1, 1, 1);

                BufferBarrier primaryResetSync[] = {
                    { pWavefrontCounters, RESOURCE_STATE_UNORDERED_ACCESS, RESOURCE_STATE_UNORDERED_ACCESS },
                    { pWavefrontIndirectArgs, RESOURCE_STATE_UNORDERED_ACCESS, RESOURCE_STATE_UNORDERED_ACCESS },
                };
                cmdResourceBarrier(pCmd, TF_ARRAY_COUNT(primaryResetSync), primaryResetSync, 0, NULL, 0, NULL);

                cmdBindPipeline(pCmd, pPipelineWavefrontV2Primary);
                cmdBindDescriptorSet(pCmd, 0, pDescriptorSetRaytracing[gRaytracingTechnique]);
                cmdBindDescriptorSet(pCmd, 0, pDescriptorSetRaytracingPerBatch[gRaytracingTechnique]);
                cmdBindDescriptorSet(pCmd, mFrameIdx, pDescriptorSetUniforms[gRaytracingTechnique]);
                cmdDispatch(pCmd, groups, 1, 1);

                BufferBarrier producerSync[] = {
                    { pWavefrontPathState, RESOURCE_STATE_UNORDERED_ACCESS, RESOURCE_STATE_UNORDERED_ACCESS },
                    { pWavefrontCounters, RESOURCE_STATE_UNORDERED_ACCESS, RESOURCE_STATE_UNORDERED_ACCESS },
                    { pWavefrontIndirectArgs, RESOURCE_STATE_UNORDERED_ACCESS, RESOURCE_STATE_UNORDERED_ACCESS },
                    { pWavefrontIndirectArgsB, RESOURCE_STATE_UNORDERED_ACCESS, RESOURCE_STATE_UNORDERED_ACCESS },
                };
                cmdResourceBarrier(pCmd, TF_ARRAY_COUNT(producerSync), producerSync, 0, NULL, 0, NULL);

                // bounce=1 consumes A and produces B. bounce=2 consumes B and produces A.
                for (uint32_t bounce = 1; bounce < maxBounces; ++bounce)
                {
                    const bool inputIsA = (bounce & 1u) != 0;
                    Buffer*    pInputArgs = inputIsA ? pWavefrontIndirectArgs : pWavefrontIndirectArgsB;
                    Buffer*    pOutputArgs = inputIsA ? pWavefrontIndirectArgsB : pWavefrontIndirectArgs;
                    Pipeline*  pResetOutput = inputIsA ? pPipelineWavefrontV2ResetB : pPipelineWavefrontV2Reset;
                    Pipeline*  pBouncePipeline = inputIsA ? pPipelineWavefrontV2Secondary : pPipelineWavefrontV2SecondaryB;

                    // Zero the next compacted queue and its indirect dispatch args.
                    cmdBindPipeline(pCmd, pResetOutput);
                    cmdBindDescriptorSet(pCmd, 0, pDescriptorSetRaytracing[gRaytracingTechnique]);
                    cmdBindDescriptorSet(pCmd, 0, pDescriptorSetRaytracingPerBatch[gRaytracingTechnique]);
                    cmdBindDescriptorSet(pCmd, mFrameIdx, pDescriptorSetUniforms[gRaytracingTechnique]);
                    cmdDispatch(pCmd, 1, 1, 1);

                    BufferBarrier resetSync[] = {
                        { pWavefrontCounters, RESOURCE_STATE_UNORDERED_ACCESS, RESOURCE_STATE_UNORDERED_ACCESS },
                        { pOutputArgs, RESOURCE_STATE_UNORDERED_ACCESS, RESOURCE_STATE_UNORDERED_ACCESS },
                    };
                    cmdResourceBarrier(pCmd, TF_ARRAY_COUNT(resetSync), resetSync, 0, NULL, 0, NULL);

                    BufferBarrier toIndirect = { pInputArgs, RESOURCE_STATE_UNORDERED_ACCESS, RESOURCE_STATE_INDIRECT_ARGUMENT };
                    cmdResourceBarrier(pCmd, 1, &toIndirect, 0, NULL, 0, NULL);

                    cmdBindPipeline(pCmd, pBouncePipeline);
                    cmdBindDescriptorSet(pCmd, 0, pDescriptorSetRaytracing[gRaytracingTechnique]);
                    cmdBindDescriptorSet(pCmd, 0, pDescriptorSetRaytracingPerBatch[gRaytracingTechnique]);
                    cmdBindDescriptorSet(pCmd, mFrameIdx, pDescriptorSetUniforms[gRaytracingTechnique]);
                    cmdExecuteIndirect(pCmd, INDIRECT_DISPATCH, 1, pInputArgs, 0, NULL, 0);

                    BufferBarrier backToUav = { pInputArgs, RESOURCE_STATE_INDIRECT_ARGUMENT, RESOURCE_STATE_UNORDERED_ACCESS };
                    cmdResourceBarrier(pCmd, 1, &backToUav, 0, NULL, 0, NULL);

                    cmdResourceBarrier(pCmd, TF_ARRAY_COUNT(producerSync), producerSync, 0, NULL, 0, NULL);
                }

                // Restore the invariant expected at the start of the next frame.
                BufferBarrier indirectToIdle[] = {
                    { pWavefrontIndirectArgs, RESOURCE_STATE_UNORDERED_ACCESS, RESOURCE_STATE_INDIRECT_ARGUMENT },
                    { pWavefrontIndirectArgsB, RESOURCE_STATE_UNORDERED_ACCESS, RESOURCE_STATE_INDIRECT_ARGUMENT },
                };
                cmdResourceBarrier(pCmd, TF_ARRAY_COUNT(indirectToIdle), indirectToIdle, 0, NULL, 0, NULL);
            }
            else if (PERSISTENT_WAVEFRONT_SINGLE_BOUNCE_PATH_TRACING == gRaytracingTechnique)
            {
                // Persistent wavefront: ONE BOUNCE is one schedulable work item.
                // The same persistent dispatch iterates up to Max Bounces; survivors are
                // compacted at every bounce boundary into ping-pong continuation banks.
                const uint32_t pixelCount = mSettings.mWidth * mSettings.mHeight;
                Shader*   pPersistentWavefrontShader = pShaderPersistentWavefront128;
                Pipeline* pPersistentWavefrontPipeline = pPipelinePersistentWavefront128;
                if (gPersistentWavefrontGroupPreset == 0)
                {
                    pPersistentWavefrontShader = pShaderPersistentWavefront64;
                    pPersistentWavefrontPipeline = pPipelinePersistentWavefront64;
                }
                else if (gPersistentWavefrontGroupPreset == 2)
                {
                    pPersistentWavefrontShader = pShaderPersistentWavefront256;
                    pPersistentWavefrontPipeline = pPipelinePersistentWavefront256;
                }

                const uint32_t threads = pPersistentWavefrontShader->mNumThreadsPerGroup[0];
                const uint32_t normalGroups = round_up(pixelCount, threads) / threads;
                const uint32_t persistentGroups = min(gPersistentGroupCounts[gPersistentGroupPreset], normalGroups);

                BufferBarrier resetSync[] = {
                    { pWavefrontPathState, RESOURCE_STATE_UNORDERED_ACCESS, RESOURCE_STATE_UNORDERED_ACCESS },
                    { pWavefrontCounters, RESOURCE_STATE_UNORDERED_ACCESS, RESOURCE_STATE_UNORDERED_ACCESS },
                };

                cmdBindPipeline(pCmd, pPipelinePersistentWavefrontReset);
                cmdBindDescriptorSet(pCmd, 0, pDescriptorSetRaytracing[gRaytracingTechnique]);
                cmdBindDescriptorSet(pCmd, 0, pDescriptorSetRaytracingPerBatch[gRaytracingTechnique]);
                cmdBindDescriptorSet(pCmd, mFrameIdx, pDescriptorSetUniforms[gRaytracingTechnique]);
                cmdDispatch(pCmd, 1, 1, 1);
                cmdResourceBarrier(pCmd, TF_ARRAY_COUNT(resetSync), resetSync, 0, NULL, 0, NULL);

                cmdBindPipeline(pCmd, pPersistentWavefrontPipeline);
                cmdBindDescriptorSet(pCmd, 0, pDescriptorSetRaytracing[gRaytracingTechnique]);
                cmdBindDescriptorSet(pCmd, 0, pDescriptorSetRaytracingPerBatch[gRaytracingTechnique]);
                cmdBindDescriptorSet(pCmd, mFrameIdx, pDescriptorSetUniforms[gRaytracingTechnique]);
                cmdDispatch(pCmd, persistentGroups, 1, 1);
            }
            else if (PERSISTENT_WAVE_256_PATH_TRACING == gRaytracingTechnique || PERSISTENT_WAVE_128_PATH_TRACING == gRaytracingTechnique ||
                     PERSISTENT_WAVE_64_PATH_TRACING == gRaytracingTechnique)
            {
                const uint32_t pixelCount = mSettings.mWidth * mSettings.mHeight;

                Shader*   pPersistentShader = NULL;
                Pipeline* pPersistentPipeline = NULL;
                if (PERSISTENT_WAVE_64_PATH_TRACING == gRaytracingTechnique)
                {
                    pPersistentShader = (gPersistentSchedulingMode == 0) ? pShaderPersistentWave64 : pShaderPersistentStatic64;
                    pPersistentPipeline = (gPersistentSchedulingMode == 0) ? pPipelinePersistentWave64 : pPipelinePersistentStatic64;
                }
                else if (PERSISTENT_WAVE_128_PATH_TRACING == gRaytracingTechnique)
                {
                    pPersistentShader = (gPersistentSchedulingMode == 0) ? pShaderPersistentWave128 : pShaderPersistentStatic128;
                    pPersistentPipeline = (gPersistentSchedulingMode == 0) ? pPipelinePersistentWave128 : pPipelinePersistentStatic128;
                }
                else
                {
                    pPersistentShader = (gPersistentSchedulingMode == 0) ? pShaderPersistentWave256 : pShaderPersistentStatic256;
                    pPersistentPipeline = (gPersistentSchedulingMode == 0) ? pPipelinePersistentWave256 : pPipelinePersistentStatic256;
                }

                const uint32_t threads = pPersistentShader->mNumThreadsPerGroup[0];
                const uint32_t normalGroups = round_up(pixelCount, threads) / threads;
                const uint32_t persistentGroups = min(gPersistentGroupCounts[gPersistentGroupPreset], normalGroups);

                if (gPersistentSchedulingMode == 0)
                {
                    BufferBarrier counterSync = { pWavefrontCounters, RESOURCE_STATE_UNORDERED_ACCESS, RESOURCE_STATE_UNORDERED_ACCESS };
                    cmdBindPipeline(pCmd, pPipelinePersistentReset);
                    cmdBindDescriptorSet(pCmd, 0, pDescriptorSetRaytracing[gRaytracingTechnique]);
                    cmdBindDescriptorSet(pCmd, 0, pDescriptorSetRaytracingPerBatch[gRaytracingTechnique]);
                    cmdBindDescriptorSet(pCmd, mFrameIdx, pDescriptorSetUniforms[gRaytracingTechnique]);
                    cmdDispatch(pCmd, 1, 1, 1);
                    cmdResourceBarrier(pCmd, 1, &counterSync, 0, NULL, 0, NULL);
                }

                cmdBindPipeline(pCmd, pPersistentPipeline);
                cmdBindDescriptorSet(pCmd, 0, pDescriptorSetRaytracing[gRaytracingTechnique]);
                cmdBindDescriptorSet(pCmd, 0, pDescriptorSetRaytracingPerBatch[gRaytracingTechnique]);
                cmdBindDescriptorSet(pCmd, mFrameIdx, pDescriptorSetUniforms[gRaytracingTechnique]);
                cmdDispatch(pCmd, persistentGroups, 1, 1);
            }
            /************************************************************************/
            // Transition UAV to be used as source and swapchain as destination in copy operation
            /************************************************************************/
            TextureBarrier copyBarriers[] = {
                { pComputeOutput, RESOURCE_STATE_UNORDERED_ACCESS, RESOURCE_STATE_PIXEL_SHADER_RESOURCE },
            };
            RenderTargetBarrier rtCopyBarriers[] = {
                { pRenderTarget, RESOURCE_STATE_PRESENT, RESOURCE_STATE_RENDER_TARGET },
            };
            cmdResourceBarrier(pCmd, 0, NULL, 1, copyBarriers, 1, rtCopyBarriers);

#if USE_DENOISER
            Texture* denoisedTexture = NULL;
            cmdSSVGFDenoise(pCmd, pDenoiser, pComputeOutput, pMotionVectorRenderTarget->pTexture,
                            pDepthNormalRenderTarget[mPathTracingData.mFrameIndex & 0x1]->pTexture,
                            pDepthNormalRenderTarget[(mPathTracingData.mFrameIndex + 1) & 0x1]->pTexture, &denoisedTexture);

            DescriptorData params[1] = {};
            params[0].mIndex = SRT_RES_IDX(SrtData, PerFrame, gDisplayTexture);
            params[0].ppTextures = &denoisedTexture;
            updateDescriptorSet(pRenderer, mFrameIdx, pDescriptorSetTexture, 1, params);

            removeResource(denoisedTexture);
#endif

            cmdEndGpuTimestampQuery(pCmd, gGpuProfileToken);
        }
        else
        {
            RenderTargetBarrier rtCopyBarriers[] = {
                { pRenderTarget, RESOURCE_STATE_PRESENT, RESOURCE_STATE_RENDER_TARGET },
            };
            cmdResourceBarrier(pCmd, 0, NULL, 0, NULL, 1, rtCopyBarriers);
        }
        /************************************************************************/
        // Present to screen
        /************************************************************************/
        if (raytracingTechniqueSupported)
        {
            cmdBeginGpuTimestampQuery(pCmd, gGpuProfileToken, "Render result");
        }

        BindRenderTargetsDesc bindRenderTargets = {};
        bindRenderTargets.mRenderTargetCount = 1;
        bindRenderTargets.mRenderTargets[0] = { pRenderTarget, LOAD_ACTION_CLEAR };
        cmdBindRenderTargets(pCmd, &bindRenderTargets);
        cmdSetViewport(pCmd, 0.0f, 0.0f, (float)mSettings.mWidth, (float)mSettings.mHeight, 0.0f, 1.0f);
        cmdSetScissor(pCmd, 0, 0, mSettings.mWidth, mSettings.mHeight);

        if (raytracingTechniqueSupported)
        {
            /************************************************************************/
            // Perform copy
            /************************************************************************/
            // Draw computed results
            cmdBindPipeline(pCmd, pDisplayTexturePipeline);
            cmdBindDescriptorSet(pCmd, 0, pDescriptorSetRaytracing[gRaytracingTechnique]);
            cmdBindDescriptorSet(pCmd, 0, pDescriptorSetRaytracingPerBatch[gRaytracingTechnique]);
            cmdBindDescriptorSet(pCmd, mFrameIdx, pDescriptorSetTexture);
            cmdDraw(pCmd, 3, 0);
            cmdEndGpuTimestampQuery(pCmd, gGpuProfileToken);
        }

        cmdBeginDebugMarker(pCmd, 0, 1, 0, "Draw UI");

        FontDrawDesc frameTimeDraw;
        frameTimeDraw.mFontColor = 0xff0080ff;
        frameTimeDraw.mFontSize = 18.0f;
        frameTimeDraw.mFontID = gFontID;
        float2 txtSize = cmdDrawCpuProfile(pCmd, float2(8.0f, 15.0f), &frameTimeDraw);
        cmdDrawGpuProfile(pCmd, float2(8.f, txtSize.y + 75.f), gGpuProfileToken, &frameTimeDraw);

        cmdDrawUserInterface(pCmd);
        cmdBindRenderTargets(pCmd, NULL);
        cmdEndDebugMarker(pCmd);

        RenderTargetBarrier presentBarrier = { pRenderTarget, RESOURCE_STATE_RENDER_TARGET, RESOURCE_STATE_PRESENT };
        cmdResourceBarrier(pCmd, 0, NULL, 0, NULL, 1, &presentBarrier);

        cmdEndGpuFrameProfile(pCmd, gGpuProfileToken);

        endCmd(pCmd);

        FlushResourceUpdateDesc flushUpdateDesc = {};
        flushUpdateDesc.mNodeIndex = 0;
        flushResourceUpdates(&flushUpdateDesc);
        Semaphore* waitSemaphores[2] = { flushUpdateDesc.pOutSubmittedSemaphore, pImageAcquiredSemaphore };

        QueueSubmitDesc submitDesc = {};
        submitDesc.mCmdCount = 1;
        submitDesc.mSignalSemaphoreCount = 1;
        submitDesc.mWaitSemaphoreCount = TF_ARRAY_COUNT(waitSemaphores);
        submitDesc.ppCmds = &pCmd;
        submitDesc.ppSignalSemaphores = &elem.pSemaphore;
        submitDesc.ppWaitSemaphores = waitSemaphores;
        submitDesc.pSignalFence = elem.pFence;
        queueSubmit(pQueue, &submitDesc);
        QueuePresentDesc presentDesc = {};
        presentDesc.mIndex = (uint8_t)swapchainImageIndex;
        presentDesc.mWaitSemaphoreCount = 1;
        presentDesc.ppWaitSemaphores = &elem.pSemaphore;
        presentDesc.pSwapChain = pSwapChain;
        presentDesc.mSubmitDone = true;
        queuePresent(pQueue, &presentDesc);
        flipProfiler();

        processBenchmarkFrame();

        mFrameIdx = (mFrameIdx + 1) % gDataBufferCount;
        /************************************************************************/
        /************************************************************************/
    }

    bool addSwapChain()
    {
        SwapChainDesc swapChainDesc = {};
        swapChainDesc.mColorClearValue = {};
        swapChainDesc.mEnableVsync = mSettings.mVSyncEnabled;
        swapChainDesc.mWidth = mSettings.mWidth;
        swapChainDesc.mHeight = mSettings.mHeight;
        swapChainDesc.mImageCount = getRecommendedSwapchainImageCount(pRenderer, &pWindow->handle);
        swapChainDesc.ppPresentQueues = &pQueue;
        swapChainDesc.mPresentQueueCount = 1;
        swapChainDesc.mWindowHandle = pWindow->handle;
        swapChainDesc.mColorFormat = getSupportedSwapchainFormat(pRenderer, &swapChainDesc, COLOR_SPACE_SDR_SRGB);
        swapChainDesc.mColorSpace = COLOR_SPACE_SDR_SRGB;
        ::addSwapChain(pRenderer, &swapChainDesc, &pSwapChain);

        return pSwapChain != NULL;
    }

    void addDescriptorSets()
    {
        for (uint32_t t = 0; t < RAYTRACING_TECHNIQUE_COUNT; ++t)
        {
            DescriptorSetDesc setDesc = SRT_SET_DESC_LARGE_RW(SrtData, Persistent, 1, 0);
            addDescriptorSet(pRenderer, &setDesc, &pDescriptorSetRaytracing[t]);
            setDesc = SRT_SET_DESC_LARGE_RW(SrtData, PerFrame, gDataBufferCount, 0);
            addDescriptorSet(pRenderer, &setDesc, &pDescriptorSetUniforms[t]);
            setDesc = SRT_SET_DESC_LARGE_RW(SrtData, PerBatch, 1, 0);
            addDescriptorSet(pRenderer, &setDesc, &pDescriptorSetRaytracingPerBatch[t]);
        }

        DescriptorSetDesc setDesc = SRT_SET_DESC_LARGE_RW(SrtData, PerFrame, gDataBufferCount, 0);
        addDescriptorSet(pRenderer, &setDesc, &pDescriptorSetTexture);

#if USE_DENOISER
        setDesc = SRT_SET_DESC(SrtData, PerFrame, gDataBufferCount, 0, pDescriptorSetRaytracing[0]);
        addDescriptorSet(pRenderer, &setDesc, &pDenoiserInputsDescriptorSet);
#endif
    }

    void removeDescriptorSets()
    {
#if USE_DENOISER
        removeDescriptorSet(pRenderer, pDenoiserInputsDescriptorSet);
#endif
        removeDescriptorSet(pRenderer, pDescriptorSetTexture);

        for (uint32_t t = 0; t < RAYTRACING_TECHNIQUE_COUNT; ++t)
        {
            removeDescriptorSet(pRenderer, pDescriptorSetRaytracingPerBatch[t]);
            removeDescriptorSet(pRenderer, pDescriptorSetRaytracing[t]);
            removeDescriptorSet(pRenderer, pDescriptorSetUniforms[t]);
        }
    }

    void addShaders()
    {
        /************************************************************************/
        // Create Raytracing Shaders
        /************************************************************************/
        if (gRaytracingTechniqueSupported[RAY_QUERY])
        {
            ShaderLoadDesc desc = {};
            desc.mComp.pFileName = USE_DENOISER ? (gUseUavRwFallback ? "RayQuery_denoise_rw_fallback.comp" : "RayQuery_denoise.comp")
                                                : (gUseUavRwFallback ? "RayQuery_rw_fallback.comp" : "RayQuery.comp");
            addShader(pRenderer, &desc, &pShaderRayQuery);

            ShaderLoadDesc wfGen = {};
            wfGen.mComp.pFileName = "RayQueryWavefrontGenerate.comp";
            addShader(pRenderer, &wfGen, &pShaderWavefrontGenerate);

            ShaderLoadDesc wfBounce0 = {};
            wfBounce0.mComp.pFileName = "RayQueryWavefrontBounce0.comp";
            addShader(pRenderer, &wfBounce0, &pShaderWavefrontBounce0);

            ShaderLoadDesc wfBounce1 = {};
            wfBounce1.mComp.pFileName = "RayQueryWavefrontBounce1.comp";
            addShader(pRenderer, &wfBounce1, &pShaderWavefrontBounce1);

            ShaderLoadDesc wfResolve = {};
            wfResolve.mComp.pFileName = "RayQueryWavefrontResolve.comp";
            addShader(pRenderer, &wfResolve, &pShaderWavefrontResolve);

            ShaderLoadDesc wfResetA = {};
            wfResetA.mComp.pFileName = "RayQueryWavefrontResetA.comp";
            addShader(pRenderer, &wfResetA, &pShaderWavefrontResetA);

            ShaderLoadDesc wfResetB = {};
            wfResetB.mComp.pFileName = "RayQueryWavefrontResetB.comp";
            addShader(pRenderer, &wfResetB, &pShaderWavefrontResetB);

            ShaderLoadDesc wfV2Reset = {};
            wfV2Reset.mComp.pFileName = "RayQueryWavefrontV2Reset.comp";
            addShader(pRenderer, &wfV2Reset, &pShaderWavefrontV2Reset);

            ShaderLoadDesc wfV2ResetB = {};
            wfV2ResetB.mComp.pFileName = "RayQueryWavefrontV2ResetB.comp";
            addShader(pRenderer, &wfV2ResetB, &pShaderWavefrontV2ResetB);

            ShaderLoadDesc wfV2Primary = {};
            wfV2Primary.mComp.pFileName = "RayQueryWavefrontV2Primary.comp";
            addShader(pRenderer, &wfV2Primary, &pShaderWavefrontV2Primary);

            ShaderLoadDesc wfV2Secondary = {};
            wfV2Secondary.mComp.pFileName = "RayQueryWavefrontV2Secondary.comp";
            addShader(pRenderer, &wfV2Secondary, &pShaderWavefrontV2Secondary);

            ShaderLoadDesc wfV2SecondaryB = {};
            wfV2SecondaryB.mComp.pFileName = "RayQueryWavefrontV2SecondaryB.comp";
            addShader(pRenderer, &wfV2SecondaryB, &pShaderWavefrontV2SecondaryB);

            ShaderLoadDesc persistentReset = {};
            persistentReset.mComp.pFileName = "RayQueryPersistentReset.comp";
            addShader(pRenderer, &persistentReset, &pShaderPersistentReset);

            ShaderLoadDesc persistentWave256 = {};
            persistentWave256.mComp.pFileName = "RayQueryPersistentWave256.comp";
            addShader(pRenderer, &persistentWave256, &pShaderPersistentWave256);

            ShaderLoadDesc persistentWave128 = {};
            persistentWave128.mComp.pFileName = "RayQueryPersistentWave128.comp";
            addShader(pRenderer, &persistentWave128, &pShaderPersistentWave128);

            ShaderLoadDesc persistentWave64 = {};
            persistentWave64.mComp.pFileName = "RayQueryPersistentWave64.comp";
            addShader(pRenderer, &persistentWave64, &pShaderPersistentWave64);

            ShaderLoadDesc persistentStatic256 = {};
            persistentStatic256.mComp.pFileName = "RayQueryPersistentStatic256.comp";
            addShader(pRenderer, &persistentStatic256, &pShaderPersistentStatic256);

            ShaderLoadDesc persistentStatic128 = {};
            persistentStatic128.mComp.pFileName = "RayQueryPersistentStatic128.comp";
            addShader(pRenderer, &persistentStatic128, &pShaderPersistentStatic128);

            ShaderLoadDesc persistentStatic64 = {};
            persistentStatic64.mComp.pFileName = "RayQueryPersistentStatic64.comp";
            addShader(pRenderer, &persistentStatic64, &pShaderPersistentStatic64);

            ShaderLoadDesc persistentWavefrontReset = {};
            persistentWavefrontReset.mComp.pFileName = "RayQueryPersistentWavefrontReset.comp";
            addShader(pRenderer, &persistentWavefrontReset, &pShaderPersistentWavefrontReset);

            ShaderLoadDesc persistentWavefront = {};
            persistentWavefront.mComp.pFileName = "RayQueryPersistentWavefront128.comp";
            addShader(pRenderer, &persistentWavefront, &pShaderPersistentWavefront128);

            ShaderLoadDesc persistentWavefront64 = {};
            persistentWavefront64.mComp.pFileName = "RayQueryPersistentWavefront64.comp";
            addShader(pRenderer, &persistentWavefront64, &pShaderPersistentWavefront64);

            ShaderLoadDesc persistentWavefront256 = {};
            persistentWavefront256.mComp.pFileName = "RayQueryPersistentWavefront256.comp";
            addShader(pRenderer, &persistentWavefront256, &pShaderPersistentWavefront256);
        }

#if USE_DENOISER
        ShaderLoadDesc denoiserShader = {};
        denoiserShader.mVert.pFileName = "DenoiserInputsPass.vert";
        denoiserShader.mFrag.pFileName = "DenoiserInputsPass.frag";
        addShader(pRenderer, &denoiserShader, &pDenoiserInputsShader);
#endif

        /************************************************************************/
        // Blit texture
        /************************************************************************/
        const char* displayTextureVertShader[2] = { "DisplayTexture.vert", "DisplayTexture_USE_DENOISER.vert" };

        const char* displayTextureFragShader[2] = { "DisplayTexture.frag", "DisplayTexture_USE_DENOISER.frag" };

        ShaderLoadDesc displayShader = {};
        displayShader.mVert.pFileName = displayTextureVertShader[USE_DENOISER];
        displayShader.mFrag.pFileName = displayTextureFragShader[USE_DENOISER];
        addShader(pRenderer, &displayShader, &pDisplayTextureShader);
    }

    void removeShaders()
    {
#if USE_DENOISER
        removeShader(pRenderer, pDenoiserInputsShader);
#endif
        removeShader(pRenderer, pDisplayTextureShader);

        if (gRaytracingTechniqueSupported[RAY_QUERY])
        {
            removeShader(pRenderer, pShaderRayQuery);
            removeShader(pRenderer, pShaderWavefrontGenerate);
            removeShader(pRenderer, pShaderWavefrontBounce0);
            removeShader(pRenderer, pShaderWavefrontBounce1);
            removeShader(pRenderer, pShaderWavefrontResolve);
            removeShader(pRenderer, pShaderWavefrontResetA);
            removeShader(pRenderer, pShaderWavefrontResetB);
            removeShader(pRenderer, pShaderWavefrontV2Reset);
            removeShader(pRenderer, pShaderWavefrontV2ResetB);
            removeShader(pRenderer, pShaderWavefrontV2Primary);
            removeShader(pRenderer, pShaderWavefrontV2Secondary);
            removeShader(pRenderer, pShaderWavefrontV2SecondaryB);
            removeShader(pRenderer, pShaderPersistentReset);
            removeShader(pRenderer, pShaderPersistentWave256);
            removeShader(pRenderer, pShaderPersistentWave128);
            removeShader(pRenderer, pShaderPersistentWave64);
            removeShader(pRenderer, pShaderPersistentStatic256);
            removeShader(pRenderer, pShaderPersistentStatic128);
            removeShader(pRenderer, pShaderPersistentStatic64);
            removeShader(pRenderer, pShaderPersistentWavefrontReset);
            removeShader(pRenderer, pShaderPersistentWavefront128);
            removeShader(pRenderer, pShaderPersistentWavefront64);
            removeShader(pRenderer, pShaderPersistentWavefront256);
        }
    }

    void addPipelines()
    {
        /************************************************************************/
        //  Create Raytracing Pipelines
        /************************************************************************/
        if (gRaytracingTechniqueSupported[RAY_QUERY])
        {
            PipelineDesc rtPipelineDesc = {};
            rtPipelineDesc.mType = PIPELINE_TYPE_COMPUTE;
            PIPELINE_LAYOUT_DESC(rtPipelineDesc, SRT_LAYOUT_DESC(SrtData, Persistent), SRT_LAYOUT_DESC(SrtData, PerFrame),
                                 SRT_LAYOUT_DESC(SrtData, PerBatch), NULL);
            ComputePipelineDesc& pipelineDesc = rtPipelineDesc.mComputeDesc;
            pipelineDesc.pShaderProgram = pShaderRayQuery;
            addPipeline(pRenderer, &rtPipelineDesc, &pPipeline[RAY_QUERY]);

            auto addExperimentalComputePipeline = [&](Shader* pShader, Pipeline** ppPipeline)
            {
                PipelineDesc desc = {};
                desc.mType = PIPELINE_TYPE_COMPUTE;
                PIPELINE_LAYOUT_DESC(desc, SRT_LAYOUT_DESC(SrtData, Persistent), SRT_LAYOUT_DESC(SrtData, PerFrame),
                                     SRT_LAYOUT_DESC(SrtData, PerBatch), NULL);
                desc.mComputeDesc.pShaderProgram = pShader;
                addPipeline(pRenderer, &desc, ppPipeline);
            };

            addExperimentalComputePipeline(pShaderWavefrontGenerate, &pPipelineWavefrontGenerate);
            addExperimentalComputePipeline(pShaderWavefrontBounce0, &pPipelineWavefrontBounce0);
            addExperimentalComputePipeline(pShaderWavefrontBounce1, &pPipelineWavefrontBounce1);
            addExperimentalComputePipeline(pShaderWavefrontResolve, &pPipelineWavefrontResolve);
            addExperimentalComputePipeline(pShaderWavefrontResetA, &pPipelineWavefrontResetA);
            addExperimentalComputePipeline(pShaderWavefrontResetB, &pPipelineWavefrontResetB);
            addExperimentalComputePipeline(pShaderWavefrontV2Reset, &pPipelineWavefrontV2Reset);
            addExperimentalComputePipeline(pShaderWavefrontV2ResetB, &pPipelineWavefrontV2ResetB);
            addExperimentalComputePipeline(pShaderWavefrontV2Primary, &pPipelineWavefrontV2Primary);
            addExperimentalComputePipeline(pShaderWavefrontV2Secondary, &pPipelineWavefrontV2Secondary);
            addExperimentalComputePipeline(pShaderWavefrontV2SecondaryB, &pPipelineWavefrontV2SecondaryB);
            addExperimentalComputePipeline(pShaderPersistentReset, &pPipelinePersistentReset);
            addExperimentalComputePipeline(pShaderPersistentWave256, &pPipelinePersistentWave256);
            addExperimentalComputePipeline(pShaderPersistentWave128, &pPipelinePersistentWave128);
            addExperimentalComputePipeline(pShaderPersistentWave64, &pPipelinePersistentWave64);
            addExperimentalComputePipeline(pShaderPersistentStatic256, &pPipelinePersistentStatic256);
            addExperimentalComputePipeline(pShaderPersistentStatic128, &pPipelinePersistentStatic128);
            addExperimentalComputePipeline(pShaderPersistentStatic64, &pPipelinePersistentStatic64);
            addExperimentalComputePipeline(pShaderPersistentWavefrontReset, &pPipelinePersistentWavefrontReset);
            addExperimentalComputePipeline(pShaderPersistentWavefront128, &pPipelinePersistentWavefront128);
            addExperimentalComputePipeline(pShaderPersistentWavefront64, &pPipelinePersistentWavefront64);
            addExperimentalComputePipeline(pShaderPersistentWavefront256, &pPipelinePersistentWavefront256);

#if defined(SHADER_STATS_AVAILABLE)
            {
                PipelineStats pipelineStats = {};
                addPipelineStats(pRenderer, pPipeline[RAY_QUERY], false, &pipelineStats);
                ShaderStats& stats = pipelineStats.mComp;
                if (stats.mValid)
                {
                    LOGF(eINFO,
                         "Ray query shader stats\n"
                         "    VGPRS         : Used %u / Physical %u / Available %u\n"
                         "    SGPRS         : Used %u / Physical %u / Available %u\n"
                         "    LDS size      : %u\n"
                         "    LDS usage     : %u\n"
                         "    Scratch usage : %u\n",
                         stats.mUsedVgprs, stats.mPhysicalVgprs, stats.mAvailableVgprs, stats.mUsedSgprs, stats.mPhysicalSgprs,
                         stats.mAvailableSgprs, stats.mLdsSizePerLocalWorkGroup, stats.mLdsUsageSizeInBytes, stats.mScratchMemUsageInBytes);
                }
                removePipelineStats(pRenderer, &pipelineStats);
            }
#endif
        }

#if USE_DENOISER
        {
            RasterizerStateDesc rasterState = {};
            rasterState.mCullMode = CULL_MODE_BACK;
            rasterState.mFrontFace = FRONT_FACE_CW;

            DepthStateDesc depthStateDesc = {};
            depthStateDesc.mDepthTest = true;
            depthStateDesc.mDepthWrite = true;
            depthStateDesc.mDepthFunc = CMP_GEQUAL;

            PipelineDesc pipelineDesc = {};
            pipelineDesc.mType = PIPELINE_TYPE_GRAPHICS;
            PIPELINE_LAYOUT_DESC(pipelineDesc, SRT_LAYOUT_DESC(SrtData, Persistent), SRT_LAYOUT_DESC(SrtData, PerFrame),
                                 SRT_LAYOUT_DESC(SrtData, PerBatch), NULL);

            TinyImageFormat rtFormats[] = { pDepthNormalRenderTarget[0]->mFormat, pMotionVectorRenderTarget->mFormat };

            VertexLayout vertexLayout = {};
            vertexLayout.mBindingCount = 2;
            vertexLayout.mAttribCount = 2;
            vertexLayout.mAttribs[0].mSemantic = SEMANTIC_POSITION;
            vertexLayout.mAttribs[0].mFormat = TinyImageFormat_R32G32B32_SFLOAT;
            vertexLayout.mAttribs[0].mBinding = 0;
            vertexLayout.mAttribs[0].mLocation = 0;
            vertexLayout.mAttribs[1].mSemantic = SEMANTIC_NORMAL;
            vertexLayout.mAttribs[1].mFormat = TinyImageFormat_R32G32B32_SFLOAT;
            vertexLayout.mAttribs[1].mBinding = 1;
            vertexLayout.mAttribs[1].mLocation = 1;

            GraphicsPipelineDesc& pipelineSettings = pipelineDesc.mGraphicsDesc;
            pipelineSettings.mPrimitiveTopo = PRIMITIVE_TOPO_TRI_LIST;
            pipelineSettings.pRasterizerState = &rasterState;
            pipelineSettings.mRenderTargetCount = 2;
            pipelineSettings.pColorFormats = rtFormats;
            pipelineSettings.mDepthStencilFormat = pDepthRenderTarget->mFormat;
            pipelineSettings.pDepthState = &depthStateDesc;
            pipelineSettings.mSampleCount = SAMPLE_COUNT_1;
            pipelineSettings.mSampleQuality = 0;
            pipelineSettings.pVertexLayout = &vertexLayout;
            pipelineSettings.pRootSignature = pDenoiserInputsRootSignature;
            pipelineSettings.pShaderProgram = pDenoiserInputsShader;

            addPipeline(pRenderer, &pipelineDesc, &pDenoiserInputsPipeline);
        }
#endif

        RasterizerStateDesc rasterizerStateDesc = {};
        rasterizerStateDesc.mCullMode = CULL_MODE_NONE;

        PipelineDesc graphicsPipelineDesc = {};
        graphicsPipelineDesc.mType = PIPELINE_TYPE_GRAPHICS;
        PIPELINE_LAYOUT_DESC(graphicsPipelineDesc, SRT_LAYOUT_DESC(SrtData, Persistent), SRT_LAYOUT_DESC(SrtData, PerFrame),
                             SRT_LAYOUT_DESC(SrtData, PerBatch), NULL);
        GraphicsPipelineDesc& pipelineSettings = graphicsPipelineDesc.mGraphicsDesc;
        pipelineSettings.mPrimitiveTopo = PRIMITIVE_TOPO_TRI_LIST;
        pipelineSettings.pRasterizerState = &rasterizerStateDesc;
        pipelineSettings.mRenderTargetCount = 1;
        pipelineSettings.pColorFormats = &pSwapChain->ppRenderTargets[0]->mFormat;
        pipelineSettings.mSampleCount = pSwapChain->ppRenderTargets[0]->mSampleCount;
        pipelineSettings.mSampleQuality = pSwapChain->ppRenderTargets[0]->mSampleQuality;
        pipelineSettings.pVertexLayout = NULL;
        pipelineSettings.pShaderProgram = pDisplayTextureShader;
        addPipeline(pRenderer, &graphicsPipelineDesc, &pDisplayTexturePipeline);
    }

    void removePipelines()
    {
        removePipeline(pRenderer, pDisplayTexturePipeline);
#if USE_DENOISER
        removePipeline(pRenderer, pDenoiserInputsPipeline);
#endif
        for (uint32_t t = 0; t < RAYTRACING_TECHNIQUE_COUNT; ++t)
        {
            if (pPipeline[t])
                removePipeline(pRenderer, pPipeline[t]);
        }
        removePipeline(pRenderer, pPipelineWavefrontGenerate);
        removePipeline(pRenderer, pPipelineWavefrontBounce0);
        removePipeline(pRenderer, pPipelineWavefrontBounce1);
        removePipeline(pRenderer, pPipelineWavefrontResolve);
        removePipeline(pRenderer, pPipelineWavefrontResetA);
        removePipeline(pRenderer, pPipelineWavefrontResetB);
        removePipeline(pRenderer, pPipelineWavefrontV2Reset);
        removePipeline(pRenderer, pPipelineWavefrontV2ResetB);
        removePipeline(pRenderer, pPipelineWavefrontV2Primary);
        removePipeline(pRenderer, pPipelineWavefrontV2Secondary);
        removePipeline(pRenderer, pPipelineWavefrontV2SecondaryB);
        removePipeline(pRenderer, pPipelinePersistentReset);
        removePipeline(pRenderer, pPipelinePersistentWave256);
        removePipeline(pRenderer, pPipelinePersistentWave128);
        removePipeline(pRenderer, pPipelinePersistentWave64);
        removePipeline(pRenderer, pPipelinePersistentStatic256);
        removePipeline(pRenderer, pPipelinePersistentStatic128);
        removePipeline(pRenderer, pPipelinePersistentStatic64);
        removePipeline(pRenderer, pPipelinePersistentWavefrontReset);
        removePipeline(pRenderer, pPipelinePersistentWavefront128);
        removePipeline(pRenderer, pPipelinePersistentWavefront64);
        removePipeline(pRenderer, pPipelinePersistentWavefront256);
    }

    void prepareDescriptorSets()
    {
        DescriptorData perFrameParams[13] = {};
        DescriptorData perBatchParams[5] = {};

        perFrameParams[0].mIndex = SRT_RES_IDX(SrtData, Persistent, gRtScene);
        perFrameParams[0].ppAccelerationStructures = &pSanMiguelAS;
        perFrameParams[1].mIndex = SRT_RES_IDX(SrtData, Persistent, gIndexDataBuffer);
        perFrameParams[1].ppBuffers = &SanMiguelProp.pGeom->pIndexBuffer;
        perFrameParams[2].mIndex = SRT_RES_IDX(SrtData, Persistent, gVertexPositionBuffer);
        perFrameParams[2].ppBuffers = &SanMiguelProp.pGeom->pVertexBuffers[0];
        perFrameParams[3].mIndex = SRT_RES_IDX(SrtData, Persistent, gVertexNormalBuffer);
        perFrameParams[3].ppBuffers = &SanMiguelProp.pGeom->pVertexBuffers[1];
        perFrameParams[4].mIndex = SRT_RES_IDX(SrtData, Persistent, gVertexTexCoordBuffer);
        perFrameParams[4].ppBuffers = &SanMiguelProp.pGeom->pVertexBuffers[2];
        perFrameParams[5].mIndex = SRT_RES_IDX(SrtData, Persistent, gIndexOffsets);
        perFrameParams[5].ppBuffers = &SanMiguelProp.pIndexBufferOffsetStream;
        perFrameParams[6].mIndex = SRT_RES_IDX(SrtData, Persistent, gMaterialTextures);
        perFrameParams[6].ppTextures = SanMiguelProp.pTextureStorage;
        perFrameParams[6].mCount = SanMiguelProp.mMaterialCount;

        perFrameParams[7].mIndex = SRT_RES_IDX(SrtData, Persistent, gWavefrontPathState);
        perFrameParams[7].ppBuffers = &pWavefrontPathState;
        perFrameParams[8].mIndex = SRT_RES_IDX(SrtData, Persistent, gWavefrontQueueA);
        perFrameParams[8].ppBuffers = &pWavefrontQueueA;
        perFrameParams[9].mIndex = SRT_RES_IDX(SrtData, Persistent, gWavefrontQueueB);
        perFrameParams[9].ppBuffers = &pWavefrontQueueB;
        perFrameParams[10].mIndex = SRT_RES_IDX(SrtData, Persistent, gWavefrontCounters);
        perFrameParams[10].ppBuffers = &pWavefrontCounters;
        perFrameParams[11].mIndex = SRT_RES_IDX(SrtData, Persistent, gWavefrontIndirectArgs);
        perFrameParams[11].ppBuffers = &pWavefrontIndirectArgs;
        perFrameParams[12].mIndex = SRT_RES_IDX(SrtData, Persistent, gWavefrontIndirectArgsB);
        perFrameParams[12].ppBuffers = &pWavefrontIndirectArgsB;

        perBatchParams[0].mIndex = SRT_RES_IDX(SrtData, PerBatch, gOutput);
        perBatchParams[0].ppTextures = &pComputeOutput;

        uint32_t paramIndex = 1;
        if (gUseUavRwFallback)
        {
            perBatchParams[paramIndex].mIndex = SRT_RES_IDX(SrtData, PerBatch, gInput);
            perBatchParams[paramIndex].ppTextures = &pComputeOutput;
            ++paramIndex;
        }
#if USE_DENOISER
        perBatchParams[paramIndex].mIndex = SRT_RES_IDX(SrtData, PerBatch, gAlbedoOutput);
        perBatchParams[paramIndex].ppTextures = &pAlbedoTexture;
        ++paramIndex;
        if (gUseUavRwFallback)
        {
            perBatchParams[paramIndex].mIndex = SRT_RES_IDX(SrtData, PerBatch, gAlbedoInput);
            perBatchParams[paramIndex].ppTextures = &pAlbedoTexture;
            ++paramIndex;
        }
#endif
        for (uint32_t t = 0; t < RAYTRACING_TECHNIQUE_COUNT; ++t)
        {
            updateDescriptorSet(pRenderer, 0, pDescriptorSetRaytracing[t], 13, perFrameParams);
            updateDescriptorSet(pRenderer, 0, pDescriptorSetRaytracingPerBatch[t], paramIndex, perBatchParams);

            for (uint32_t i = 0; i < gDataBufferCount; ++i)
            {
                DescriptorData uParams[1] = {};
                uParams[0].mIndex = SRT_RES_IDX(SrtData, PerFrame, gSettings);
                uParams[0].ppBuffers = &pRayGenConfigBuffer[i];
                updateDescriptorSet(pRenderer, i, pDescriptorSetUniforms[t], 1, uParams);
            }
        }
        DescriptorData params[7] = {};
        for (uint32_t i = 0; i < gDataBufferCount; ++i)
        {
            params[0].mIndex = SRT_RES_IDX(SrtData, PerFrame, gDisplayTexture);
            params[0].ppTextures = &pComputeOutput;
#if USE_DENOISER
            params[1].mIndex = SRT_RES_IDX(SrtData, PerFrame, gAlbedoTex);
            params[1].ppTextures = &pAlbedoTexture;
#endif
            updateDescriptorSet(pRenderer, i, pDescriptorSetTexture, 1 + USE_DENOISER, params);
        }

#if USE_DENOISER
        params[0].pName = "gSettings";
        for (uint32_t i = 0; i < gDataBufferCount; ++i)
        {
            params[0].ppBuffers = &pRayGenConfigBuffer[i];
            updateDescriptorSet(pRenderer, i, pDenoiserInputsDescriptorSet, 1, params);
        }
#endif
    }

    void updateUIVisibility()
    {
        uiHideDynamicWidgets(&mDynamicWidgets[0], pGuiWindow);
        uiHideDynamicWidgets(&mDynamicWidgets[1], pGuiWindow);

        const bool raytracingTechniqueSupported = gRaytracingTechniqueSupported[gRaytracingTechnique];
        uiShowDynamicWidgets(&mDynamicWidgets[raytracingTechniqueSupported], pGuiWindow);
        uiHideDynamicWidgets(&mDynamicWidgets[!raytracingTechniqueSupported], pGuiWindow);
    }

    const char* GetName() { return "16_Raytracing"; }
    /************************************************************************/
    // Data
    /************************************************************************/
private:
    // Two sets of resources (one in flight and one being used on CPU)
    static const uint32_t gDataBufferCount = 2;

    Renderer*              pRenderer = NULL;
    Raytracing*            pRaytracing = NULL;
    Queue*                 pQueue = NULL;
    GpuCmdRing             mCmdRing = {};
    Buffer*                pRayGenConfigBuffer[gDataBufferCount] = {};
    AccelerationStructure* pSanMiguelBottomAS = NULL;
    AccelerationStructure* pSanMiguelAS = NULL;
    Shader*                pShaderRayQuery = NULL;
    Shader*                pShaderWavefrontGenerate = NULL;
    Shader*                pShaderWavefrontBounce0 = NULL;
    Shader*                pShaderWavefrontBounce1 = NULL;
    Shader*                pShaderWavefrontResolve = NULL;
    Shader*                pShaderWavefrontResetA = NULL;
    Shader*                pShaderWavefrontResetB = NULL;
    Shader*                pShaderWavefrontV2Reset = NULL;
    Shader*                pShaderWavefrontV2ResetB = NULL;
    Shader*                pShaderWavefrontV2Primary = NULL;
    Shader*                pShaderWavefrontV2Secondary = NULL;
    Shader*                pShaderWavefrontV2SecondaryB = NULL;
    Shader*                pShaderPersistentReset = NULL;
    Shader*                pShaderPersistentWave256 = NULL;
    Shader*                pShaderPersistentWave128 = NULL;
    Shader*                pShaderPersistentWave64 = NULL;
    Shader*                pShaderPersistentStatic256 = NULL;
    Shader*                pShaderPersistentStatic128 = NULL;
    Shader*                pShaderPersistentStatic64 = NULL;
    Shader*                pShaderPersistentWavefrontReset = NULL;
    Shader*                pShaderPersistentWavefront64 = NULL;
    Shader*                pShaderPersistentWavefront128 = NULL;
    Shader*                pShaderPersistentWavefront256 = NULL;
    Shader*                pDisplayTextureShader = NULL;
    DescriptorSet*         pDescriptorSetRaytracing[RAYTRACING_TECHNIQUE_COUNT] = {};
    DescriptorSet*         pDescriptorSetRaytracingPerBatch[RAYTRACING_TECHNIQUE_COUNT] = {};
    DescriptorSet*         pDescriptorSetUniforms[RAYTRACING_TECHNIQUE_COUNT] = {};
    DescriptorSet*         pDescriptorSetTexture = NULL;
    Pipeline*              pPipeline[RAYTRACING_TECHNIQUE_COUNT] = {};
    Pipeline*              pPipelineWavefrontGenerate = NULL;
    Pipeline*              pPipelineWavefrontBounce0 = NULL;
    Pipeline*              pPipelineWavefrontBounce1 = NULL;
    Pipeline*              pPipelineWavefrontResolve = NULL;
    Pipeline*              pPipelineWavefrontResetA = NULL;
    Pipeline*              pPipelineWavefrontResetB = NULL;
    Pipeline*              pPipelineWavefrontV2Reset = NULL;
    Pipeline*              pPipelineWavefrontV2ResetB = NULL;
    Pipeline*              pPipelineWavefrontV2Primary = NULL;
    Pipeline*              pPipelineWavefrontV2Secondary = NULL;
    Pipeline*              pPipelineWavefrontV2SecondaryB = NULL;
    Pipeline*              pPipelinePersistentReset = NULL;
    Pipeline*              pPipelinePersistentWave256 = NULL;
    Pipeline*              pPipelinePersistentWave128 = NULL;
    Pipeline*              pPipelinePersistentWave64 = NULL;
    Pipeline*              pPipelinePersistentStatic256 = NULL;
    Pipeline*              pPipelinePersistentStatic128 = NULL;
    Pipeline*              pPipelinePersistentStatic64 = NULL;
    Pipeline*              pPipelinePersistentWavefrontReset = NULL;
    Pipeline*              pPipelinePersistentWavefront64 = NULL;
    Pipeline*              pPipelinePersistentWavefront128 = NULL;
    Pipeline*              pPipelinePersistentWavefront256 = NULL;
    Pipeline*              pDisplayTexturePipeline = NULL;
    SwapChain*             pSwapChain = NULL;
    Texture*               pComputeOutput = NULL;
    Buffer*                pWavefrontPathState = NULL;
    Buffer*                pWavefrontQueueA = NULL;
    Buffer*                pWavefrontQueueB = NULL;
    Buffer*                pWavefrontCounters = NULL;
    Buffer*                pWavefrontIndirectArgs = NULL;
    Buffer*                pWavefrontIndirectArgsB = NULL;
    Semaphore*             pImageAcquiredSemaphore = NULL;
    uint32_t               mFrameIdx = 0;
    PathTracingData        mPathTracingData = {};
    UIComponent*           pGuiWindow = NULL;
    DynamicUIWidgets       mDynamicWidgets[2] = {};
    float3                 mLightDirection = float3(0.2f, 1.8f, 0.1f);
    ProfileToken            mRaytracingGpuProfileToken = {};
    BenchmarkPhase          mBenchmarkPhase = BENCHMARK_IDLE;
    BenchmarkConfig         mBenchmarkConfigs[32] = {};
    uint32_t                mBenchmarkConfigCount = 0;
    uint32_t                mBenchmarkConfigIndex = 0;
    uint32_t                mBenchmarkFrameCount = 0;
    int64_t                 mBenchmarkStartUsec = 0;
    double                  mBenchmarkGpuTimeSum = 0.0;
    uint32_t                mBenchmarkSavedTechnique = RAY_QUERY;
    uint32_t                mBenchmarkSavedGroupPreset = 2;
    uint32_t                mBenchmarkSavedSchedulingMode = 0;
    uint32_t                mBenchmarkSavedBatchPreset = 0;
    uint32_t                mBenchmarkSavedWavefrontGroupPreset = 1;

#if USE_DENOISER
    Texture*       pAlbedoTexture = NULL;
    DescriptorSet* pDenoiserInputsDescriptorSet = NULL;
    RenderTarget*  pDepthNormalRenderTarget[2] = {};
    RenderTarget*  pMotionVectorRenderTarget = NULL;
    RenderTarget*  pDepthRenderTarget = NULL;
    RootSignature* pDenoiserInputsRootSignature = NULL;
    Shader*        pDenoiserInputsShader = NULL;
    Pipeline*      pDenoiserInputsPipeline = NULL;
    SSVGFDenoiser* pDenoiser = NULL;
#endif
};

static void startRaytracingBenchmark(void* pUserData)
{
    UNREF_PARAM(pUserData);
    if (gRaytracingApp)
        gRaytracingApp->startBenchmark();
}

static void saveRaytracingBenchmark(void* pUserData)
{
    UNREF_PARAM(pUserData);
    if (gRaytracingApp)
        gRaytracingApp->saveBenchmark();
}

DEFINE_APPLICATION_MAIN(UnitTest_NativeRaytracing)
