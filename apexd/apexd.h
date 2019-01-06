/*
 * Copyright (C) 2018 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef ANDROID_APEXD_APEXD_H_
#define ANDROID_APEXD_APEXD_H_

#include <string>
#include <vector>

#include <android-base/macros.h>

#include "status.h"

namespace android {
namespace apex {

static constexpr const char* kApexPackageDataDir = "/data/apex";
static constexpr const char* kApexRoot = "/apex";
static constexpr const char* kApexPackageSystemDir = "/system/apex";

void unmountAndDetachExistingImages();

void scanPackagesDirAndActivate(const char* apex_package_dir);

Status stagePackage(const std::string& packageTmpPath) WARN_UNUSED;

Status activatePackage(const std::string& full_path) WARN_UNUSED;
Status deactivatePackage(const std::string& full_path) WARN_UNUSED;

std::vector<std::pair<std::string, uint64_t>> getActivePackages();

void onStart();
void onAllPackagesReady();

}  // namespace apex
}  // namespace android

#endif  // ANDROID_APEXD_APEXD_H_
