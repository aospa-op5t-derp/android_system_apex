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

#include "apexservice.h"

#include <fstream>

#include <stdio.h>

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/stringprintf.h>
#include <binder/IResultReceiver.h>

#include "apex_file.h"
#include "apex_manifest.h"

using android::base::StringPrintf;

namespace android {
namespace apex {

static constexpr const char* kApexPackageDir = "/data/apex";
static constexpr const char* kApexPackageSuffix = ".apex";

::android::binder::Status ApexService::installPackage(const std::string& packageTmpPath, bool* aidl_return) {
  *aidl_return = true;

  LOG(DEBUG) << "installPackage() received by ApexService, path " << packageTmpPath;

  std::unique_ptr<ApexFile> apex = ApexFile::Open(packageTmpPath);
  if (apex.get() == nullptr) {
    *aidl_return = false;
    // Error opening the file.
    return binder::Status::fromExceptionCode(binder::Status::EX_ILLEGAL_ARGUMENT);
  }

  std::unique_ptr<ApexManifest> manifest =
    ApexManifest::Open(apex->GetManifest());
  if (manifest.get() == nullptr) {
    // Error parsing manifest.
    return binder::Status::fromExceptionCode(binder::Status::EX_ILLEGAL_ARGUMENT);
  }
  std::string packageId =
      manifest->GetName() + "@" + std::to_string(manifest->GetVersion());

  std::string destPath = StringPrintf("%s/%s%s", kApexPackageDir, packageId.c_str(), kApexPackageSuffix);
  if (rename(packageTmpPath.c_str(), destPath.c_str()) != 0) {
    PLOG(WARNING) << "Unable to rename " << packageTmpPath << " to " << destPath;
    return binder::Status::fromExceptionCode(binder::Status::EX_ILLEGAL_STATE);
  }
  LOG(DEBUG) << "Success renaming " << packageTmpPath << " to " << destPath;
  return binder::Status::ok();
}

status_t ApexService::onTransact(uint32_t _aidl_code,
                                 const Parcel& _aidl_data,
                                 Parcel* _aidl_reply,
                                 uint32_t _aidl_flags) {
  switch (_aidl_code) {
    case IBinder::SHELL_COMMAND_TRANSACTION: {
      int in = _aidl_data.readFileDescriptor();
      int out = _aidl_data.readFileDescriptor();
      int err = _aidl_data.readFileDescriptor();
      int argc = _aidl_data.readInt32();
      Vector<String16> args;
      for (int i = 0; i < argc && _aidl_data.dataAvail() > 0; i++) {
        args.add(_aidl_data.readString16());
      }
      sp<IBinder> unusedCallback;
      sp<IResultReceiver> resultReceiver;
      status_t status;
      if ((status = _aidl_data.readNullableStrongBinder(&unusedCallback)) != OK)
        return status;
      if ((status = _aidl_data.readNullableStrongBinder(&resultReceiver)) != OK)
        return status;
      status = shellCommand(in, out, err, args);
      if (resultReceiver != nullptr) {
        resultReceiver->send(status);
      }
      return OK;
    }
  }
  return BnApexService::onTransact(_aidl_code, _aidl_data, _aidl_reply, _aidl_flags);
}

status_t ApexService::shellCommand(int in, int out, int err, const Vector<String16>& args) {
  if (in == BAD_TYPE || out == BAD_TYPE || err == BAD_TYPE) {
    return BAD_VALUE;
  }
  auto print_help = [](int fd) {
    auto stream = std::fstream(base::StringPrintf("/proc/self/fd/%d", fd));
    stream << "ApexService:" << std::endl
           << "  help - display this help" << std::endl
           << "  installPackage [packagePath] - install package from the given path" << std::endl;
  };

  if (args.size() == 2 && args[0] == String16("installPackage")) {
    bool ret_value;
    ::android::binder::Status status = installPackage(String8(args[1]).string(), &ret_value);
    if (status.isOk()) {
      return OK;
    }
    auto err_str = std::fstream(base::StringPrintf("/proc/self/fd/%d", err));
    err_str << "Failed to install package: " << status.toString8().string() << std::endl;
    return BAD_VALUE;
  }
  if (args.size() == 1 && args[0] == String16("help")) {
    print_help(out);
    return OK;
  }
  print_help(err);
  return BAD_VALUE;
}

};  // namespace apex
};  // namespace android
