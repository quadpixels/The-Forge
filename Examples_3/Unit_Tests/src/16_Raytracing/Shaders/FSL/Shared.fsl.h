/*
 * Copyright (c) 2017-2025 The Forge Interactive Inc.
 *
 * This file is part of The-Forge
 * (see https://github.com/ConfettiFX/The-Forge).
 *
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements. See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership. The ASF licenses this file
 * to you under the Apache License, Version 2.0.
 */
#pragma once

#if defined(NO_FSL_DEFINITIONS)
#define float4x4 mat4
#define packed_float3 float3
#endif
#define TOTAL_IMGS 256

struct ShadersConfigBlock
{
    float4x4 mCameraToWorld;
    float4x4 mWorldToCamera;
    float4x4 mCameraToProjection;
    float4x4 mWorldToProjectionPrevious;
    float4x4 mWorldMatrix;
    float2 mRtInvSize;
    float2 mZ1PlaneSize;
    float mProjNear;
    float mProjFarMinusNear;
    float mRandomSeed;
    uint mFrameIndex;
    packed_float3 mLightDirection;
    uint mFramesSinceCameraMove;
    float2 mSubpixelJitter;
    uint mWidth;
    uint mHeight;
    uint mMaxBounces;
    uint mPersistentBatchSize;
    uint mPersistentTotalThreads;
};
