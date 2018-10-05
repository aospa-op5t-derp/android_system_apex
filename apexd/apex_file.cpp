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

#include <android-base/logging.h>
#include <ziparchive/zip_archive.h>
#include <memory>
#include <string>

#include "apex_file.h"

namespace android {
namespace apex {

std::unique_ptr<ApexFile> ApexFile::Open(const std::string& apex_filename) {
  std::unique_ptr<ApexFile> ret(new ApexFile(apex_filename));
  if (ret->OpenInternal() < 0) {
    return nullptr;
  }
  return ret;
}

ApexFile::~ApexFile() {
  if (handle_ != nullptr) {
    CloseArchive(handle_);
  }
}

static constexpr const char* kImageFilename = "image.img";
static constexpr const char* kManifestFilename = "manifest.json";

int ApexFile::OpenInternal() {
  if (handle_ != nullptr) {
    // Already opened.
    return 0;
  }
  int ret = OpenArchive(apex_filename_.c_str(), &handle_);
  if (ret < 0) {
    LOG(ERROR) << "Failed to open package " << apex_filename_ << ": "
               << ErrorCodeString(ret);
    return ret;
  }

  // Locate the mountable image within the zipfile and store offset and size.
  ZipEntry entry;
  ret = FindEntry(handle_, ZipString(kImageFilename), &entry);
  if (ret < 0) {
    LOG(ERROR) << "Could not find entry \"" << kImageFilename
               << "\" in package " << apex_filename_ << ": "
               << ErrorCodeString(ret);
    return ret;
  }
  image_offset_ = entry.offset;
  image_size_ = entry.uncompressed_length;

  ret = FindEntry(handle_, ZipString(kManifestFilename), &entry);
  if (ret < 0) {
    LOG(ERROR) << "Could not find entry \"" << kManifestFilename
               << "\" in package " << apex_filename_ << ": "
               << ErrorCodeString(ret);
    return ret;
  }

  uint32_t length = entry.uncompressed_length;
  manifest_.resize(length, '\0');
  ret = ExtractToMemory(handle_, &entry,
                        reinterpret_cast<uint8_t*>(&(manifest_)[0]), length);
  if (ret != 0) {
    LOG(ERROR) << "Failed to extract manifest from package " << apex_filename_
               << ": " << ErrorCodeString(ret);
    return ret;
  }
  return 0;
}

}  // namespace apex
}  // namespace android
