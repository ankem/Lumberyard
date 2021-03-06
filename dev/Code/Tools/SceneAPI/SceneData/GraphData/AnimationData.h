#pragma once

/*
* All or portions of this file Copyright (c) Amazon.com, Inc. or its affiliates or
* its licensors.
*
* For complete copyright and license terms please see the LICENSE at the root of this
* distribution (the "License"). All use of this software is governed by the License,
* or, if provided, by the license below or the license accompanying this file. Do not
* remove or modify any license notices. This file is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
*
*/

#include <AzCore/Math/Transform.h>
#include <AzCore/std/containers/vector.h>
#include <SceneAPI/SceneData/SceneDataConfiguration.h>
#include <SceneAPI/SceneCore/DataTypes/GraphData/IAnimationData.h>

namespace AZ
{
    namespace SceneData
    {
        namespace GraphData
        {
            class AnimationData
                : public SceneAPI::DataTypes::IAnimationData
            {
            public:
                AZ_RTTI(AnimationData, "{D350732E-4727-41C8-95E0-FBAF5F2AC074}", SceneAPI::DataTypes::IAnimationData);

                SCENE_DATA_API AnimationData();
                SCENE_DATA_API ~AnimationData() override = default;
                SCENE_DATA_API virtual void AddKeyFrame(const Transform& keyFrameTransform);
                SCENE_DATA_API virtual void ReserveKeyFrames(size_t count);
                SCENE_DATA_API virtual void SetTimeStepBetweenFrames(double timeStep);

                SCENE_DATA_API size_t GetKeyFrameCount() const override;
                SCENE_DATA_API const Transform& GetKeyFrame(size_t index) const override;

                SCENE_DATA_API double GetTimeStepBetweenFrames() const override;

            protected:
                AZStd::vector<Transform>    m_keyFrames;
                double                      m_timeStepBetweenFrames;
            };
        }
    }
}