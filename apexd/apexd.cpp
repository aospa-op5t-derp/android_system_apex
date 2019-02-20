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

#define LOG_TAG "apexd"

#include "apexd.h"
#include "apexd_private.h"

#include "apex_database.h"
#include "apex_file.h"
#include "apex_manifest.h"
#include "apexd_loop.h"
#include "apexd_prepostinstall.h"
#include "apexd_session.h"
#include "apexd_utils.h"
#include "status_or.h"
#include "string_log.h"

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/macros.h>
#include <android-base/properties.h>
#include <android-base/scopeguard.h>
#include <android-base/stringprintf.h>
#include <android-base/strings.h>
#include <android-base/unique_fd.h>
#include <libavb/libavb.h>
#include <libdm/dm.h>
#include <libdm/dm_table.h>
#include <libdm/dm_target.h>
#include <selinux/android.h>

#include <dirent.h>
#include <fcntl.h>
#include <linux/loop.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <iomanip>
#include <memory>
#include <string>
#include <unordered_set>

using android::base::EndsWith;
using android::base::Join;
using android::base::ReadFullyAtOffset;
using android::base::StartsWith;
using android::base::StringPrintf;
using android::base::unique_fd;
using android::dm::DeviceMapper;
using android::dm::DmDeviceState;
using android::dm::DmTable;
using android::dm::DmTargetVerity;

using apex::proto::SessionState;

namespace android {
namespace apex {

using MountedApexData = MountedApexDatabase::MountedApexData;

namespace {

static constexpr const char* kApexPackageSuffix = ".apex";
static constexpr const char* kApexKeySystemDirectory =
    "/system/etc/security/apex/";
static constexpr const char* kApexKeyProductDirectory =
    "/product/etc/security/apex/";

// These should be in-sync with system/sepolicy/public/property_contexts
static constexpr const char* kApexStatusSysprop = "apexd.status";
static constexpr const char* kApexStatusStarting = "starting";
static constexpr const char* kApexStatusReady = "ready";

static constexpr const char* kApexVerityOnSystemProp =
    "persist.apexd.verity_on_system";
static bool gForceDmVerityOnSystem =
    android::base::GetBoolProperty(kApexVerityOnSystemProp, false);

MountedApexDatabase gMountedApexes;

static constexpr size_t kLoopDeviceSetupAttempts = 3u;
static constexpr size_t kMountAttempts = 5u;

std::unique_ptr<DmTable> createVerityTable(const ApexVerityData& verity_data,
                                           const std::string& loop) {
  AvbHashtreeDescriptor* desc = verity_data.desc.get();
  auto table = std::make_unique<DmTable>();

  std::ostringstream hash_algorithm;
  hash_algorithm << desc->hash_algorithm;

  auto target = std::make_unique<DmTargetVerity>(
      0, desc->image_size / 512, desc->dm_verity_version, loop, loop,
      desc->data_block_size, desc->hash_block_size,
      desc->image_size / desc->data_block_size,
      desc->tree_offset / desc->hash_block_size, hash_algorithm.str(),
      verity_data.root_digest, verity_data.salt);

  target->IgnoreZeroBlocks();
  table->AddTarget(std::move(target));

  table->set_readonly(true);

  return table;
}

class DmVerityDevice {
 public:
  DmVerityDevice() : cleared_(true) {}
  explicit DmVerityDevice(const std::string& name)
      : name_(name), cleared_(false) {}
  DmVerityDevice(const std::string& name, const std::string& dev_path)
      : name_(name), dev_path_(dev_path), cleared_(false) {}

  DmVerityDevice(DmVerityDevice&& other)
      : name_(other.name_),
        dev_path_(other.dev_path_),
        cleared_(other.cleared_) {
    other.cleared_ = true;
  }

  DmVerityDevice& operator=(DmVerityDevice&& other) {
    name_ = other.name_;
    dev_path_ = other.dev_path_;
    cleared_ = other.cleared_;
    other.cleared_ = true;
    return *this;
  }

  ~DmVerityDevice() {
    if (!cleared_) {
      DeviceMapper& dm = DeviceMapper::Instance();
      dm.DeleteDevice(name_);
    }
  }

  const std::string& GetName() const { return name_; }
  const std::string& GetDevPath() const { return dev_path_; }
  void SetDevPath(const std::string& dev_path) { dev_path_ = dev_path; }

  void Release() { cleared_ = true; }

 private:
  std::string name_;
  std::string dev_path_;
  bool cleared_;
};

StatusOr<DmVerityDevice> createVerityDevice(const std::string& name,
                                            const DmTable& table) {
  DeviceMapper& dm = DeviceMapper::Instance();

  if (dm.GetState(name) != DmDeviceState::INVALID) {
    LOG(WARNING) << "Deleting existing dm device " << name;
    dm.DeleteDevice(name);
  }

  if (!dm.CreateDevice(name, table)) {
    return StatusOr<DmVerityDevice>::MakeError(
        "Couldn't create verity device.");
  }
  DmVerityDevice dev(name);

  std::string dev_path;
  if (!dm.GetDmDevicePathByName(name, &dev_path)) {
    return StatusOr<DmVerityDevice>::MakeError(
        "Couldn't get verity device path!");
  }
  dev.SetDevPath(dev_path);

  return StatusOr<DmVerityDevice>(std::move(dev));
}

template <char kTypeVal>
bool DTypeFilter(unsigned char d_type, const char* d_name ATTRIBUTE_UNUSED) {
  return d_type == kTypeVal;
}

StatusOr<std::vector<std::string>> FindApexFilesByName(const std::string& path,
                                                       bool include_dirs) {
  auto filter_fn = [include_dirs](unsigned char d_type, const char* d_name) {
    if (d_type == DT_REG && EndsWith(d_name, kApexPackageSuffix)) {
      return true;  // APEX file, take.
    }
    // Directory and asked to scan for flattened.
    return d_type == DT_DIR && include_dirs;
  };
  return ReadDir(path, filter_fn);
}

Status RemovePreviouslyActiveApexFiles(
    const std::unordered_set<std::string>& affected_packages,
    const std::unordered_set<std::string>& files_to_keep) {
  auto all_active_apex_files =
      FindApexFilesByName(kActiveApexPackagesDataDir, false /* include_dirs */);

  if (!all_active_apex_files.Ok()) {
    return all_active_apex_files.ErrorStatus();
  }

  for (const std::string& path : *all_active_apex_files) {
    StatusOr<ApexFile> apex_file = ApexFile::Open(path);
    if (!apex_file.Ok()) {
      return apex_file.ErrorStatus();
    }

    const std::string& package_name = apex_file->GetManifest().name();
    if (affected_packages.find(package_name) == affected_packages.end()) {
      // This apex belongs to a package that wasn't part of this stage sessions,
      // hence it should be kept.
      continue;
    }

    if (files_to_keep.find(apex_file->GetPath()) != files_to_keep.end()) {
      // This is a path that was staged and should be kept.
      continue;
    }

    LOG(DEBUG) << "Deleting previously active apex " << apex_file->GetPath();
    if (unlink(apex_file->GetPath().c_str()) != 0) {
      return Status::Fail(PStringLog()
                          << "Failed to unlink " << apex_file->GetPath());
    }
  }

  return Status::Success();
}

Status mountNonFlattened(const ApexFile& apex, const std::string& mountPoint,
                         MountedApexData* apex_data) {
  const ApexManifest& manifest = apex.GetManifest();
  const std::string& full_path = apex.GetPath();
  const std::string& packageId = GetPackageId(manifest);

  loop::LoopbackDeviceUniqueFd loopbackDevice;
  for (size_t attempts = 1;; ++attempts) {
    StatusOr<loop::LoopbackDeviceUniqueFd> ret = loop::createLoopDevice(
        full_path, apex.GetImageOffset(), apex.GetImageSize());
    if (ret.Ok()) {
      loopbackDevice = std::move(*ret);
      break;
    }
    if (attempts >= kLoopDeviceSetupAttempts) {
      return Status::Fail(StringLog()
                          << "Could not create loop device for " << full_path
                          << ": " << ret.ErrorMessage());
    }
  }
  LOG(VERBOSE) << "Loopback device created: " << loopbackDevice.name;

  auto verityData = apex.VerifyApexVerity(
      {kApexKeySystemDirectory, kApexKeyProductDirectory});
  if (!verityData.Ok()) {
    return Status(StringLog()
                  << "Failed to verify Apex Verity data for " << full_path
                  << ": " << verityData.ErrorMessage());
  }
  std::string blockDevice = loopbackDevice.name;
  apex_data->loop_name = loopbackDevice.name;

  // for APEXes in system partition, we don't need to mount them on dm-verity
  // because they are already in the dm-verity protected partition; system.
  // However, note that we don't skip verification to ensure that APEXes are
  // correctly signed.
  const bool mountOnVerity =
      gForceDmVerityOnSystem || !StartsWith(full_path, kApexPackageSystemDir);
  DmVerityDevice verityDev;
  if (mountOnVerity) {
    auto verityTable = createVerityTable(*verityData, loopbackDevice.name);
    StatusOr<DmVerityDevice> verityDevRes =
        createVerityDevice(packageId, *verityTable);
    if (!verityDevRes.Ok()) {
      return Status(StringLog()
                    << "Failed to create Apex Verity device " << full_path
                    << ": " << verityDevRes.ErrorMessage());
    }
    verityDev = std::move(*verityDevRes);
    blockDevice = verityDev.GetDevPath();

    Status readAheadStatus = loop::configureReadAhead(verityDev.GetDevPath());
    if (!readAheadStatus.Ok()) {
      return readAheadStatus;
    }
  }

  for (size_t count = 0; count < kMountAttempts; ++count) {
    if (mount(blockDevice.c_str(), mountPoint.c_str(), "ext4",
              MS_NOATIME | MS_NODEV | MS_DIRSYNC | MS_RDONLY, NULL) == 0) {
      LOG(INFO) << "Successfully mounted package " << full_path << " on "
                << mountPoint;

      // Time to accept the temporaries as good.
      if (mountOnVerity) {
        verityDev.Release();
      }
      loopbackDevice.CloseGood();

      return Status::Success();
    } else {
      // TODO(b/122059364): Even though the kernel has created the verity
      // device, we still depend on ueventd to run to actually create the
      // device node in userspace. To solve this properly we should listen on
      // the netlink socket for uevents, or use inotify. For now, this will
      // have to do.
      usleep(50000);
    }
  }
  return Status::Fail(PStringLog()
                      << "Mounting failed for package " << full_path);
}

Status mountFlattened(const ApexFile& apex, const std::string& mountPoint,
                      MountedApexData* apex_data) {
  if (!StartsWith(apex.GetPath(), kApexPackageSystemDir)) {
    return Status::Fail(StringLog()
                        << "Cannot activate flattened APEX " << apex.GetPath());
  }

  if (mount(apex.GetPath().c_str(), mountPoint.c_str(), nullptr, MS_BIND,
            nullptr) == 0) {
    LOG(INFO) << "Successfully bind-mounted flattened package "
              << apex.GetPath() << " on " << mountPoint;

    apex_data->loop_name = "";  // No loop device.

    return Status::Success();
  }
  return Status::Fail(PStringLog() << "Mounting failed for flattened package "
                                   << apex.GetPath());
}

Status deactivatePackageImpl(const ApexFile& apex) {
  // TODO: It's not clear what the right thing to do is for umount failures.

  const ApexManifest& manifest = apex.GetManifest();
  // Unmount "latest" bind-mount.
  // TODO: What if bind-mount isn't latest?
  {
    std::string mount_point = apexd_private::GetActiveMountPoint(manifest);
    LOG(VERBOSE) << "Unmounting and deleting " << mount_point;
    if (umount2(mount_point.c_str(), UMOUNT_NOFOLLOW | MNT_DETACH) != 0) {
      return Status::Fail(PStringLog() << "Failed to unmount " << mount_point);
    }
    if (rmdir(mount_point.c_str()) != 0) {
      PLOG(ERROR) << "Could not rmdir " << mount_point;
      // Continue here.
    }
  }

  std::string mount_point = apexd_private::GetPackageMountPoint(manifest);
  LOG(VERBOSE) << "Unmounting and deleting " << mount_point;
  if (umount2(mount_point.c_str(), UMOUNT_NOFOLLOW | MNT_DETACH) != 0) {
    return Status::Fail(PStringLog() << "Failed to unmount " << mount_point);
  }
  std::string error_msg;
  if (rmdir(mount_point.c_str()) != 0) {
    // If we cannot delete the directory, we're in a bad state (e.g., getting
    // active packages depends on directory existence right now).
    // TODO: consider additional delayed cleanups, and rewrite once we have
    //       a package database.
    error_msg = PStringLog() << "Failed to rmdir " << mount_point;
  }

  // TODO: Find the loop device connected with the mount. For now, just run the
  //       destroy-all and rely on EBUSY.
  if (!apex.IsFlattened()) {
    loop::destroyAllLoopDevices();
  }

  if (error_msg.empty()) {
    return Status::Success();
  } else {
    return Status::Fail(error_msg);
  }
}

template <typename HookFn, typename HookCall>
Status PrePostinstallPackages(const std::vector<ApexFile>& apexes, HookFn fn,
                              HookCall call) {
  if (apexes.empty()) {
    return Status::Fail("Empty set of inputs");
  }

  // 1) Check whether the APEXes have hooks.
  bool has_hooks = false;
  for (const ApexFile& apex_file : apexes) {
    if (!(apex_file.GetManifest().*fn)().empty()) {
      has_hooks = true;
      break;
    }
  }

  // 2) If we found hooks, run the pre/post-install.
  if (has_hooks) {
    Status install_status = (*call)(apexes);
    if (!install_status.Ok()) {
      return install_status;
    }
  }

  return Status::Success();
}

Status PreinstallPackages(const std::vector<ApexFile>& apexes) {
  return PrePostinstallPackages(apexes, &ApexManifest::preinstallhook,
                                &StagePreInstall);
}

Status PostinstallPackages(const std::vector<ApexFile>& apexes) {
  return PrePostinstallPackages(apexes, &ApexManifest::postinstallhook,
                                &StagePostInstall);
}

template <typename RetType, typename Fn>
RetType HandlePackages(const std::vector<std::string>& paths, Fn fn) {
  // 1) Open all APEXes.
  std::vector<ApexFile> apex_files;
  for (const std::string& path : paths) {
    StatusOr<ApexFile> apex_file = ApexFile::Open(path);
    if (!apex_file.Ok()) {
      return RetType::Fail(apex_file.ErrorMessage());
    }
    apex_files.emplace_back(std::move(*apex_file));
  }

  // 2) Dispatch.
  return fn(apex_files);
}

StatusOr<std::vector<ApexFile>> verifyPackages(
    const std::vector<std::string>& paths) {
  if (paths.empty()) {
    return StatusOr<std::vector<ApexFile>>::MakeError("Empty set of inputs");
  }
  LOG(DEBUG) << "verifyPackages() for " << Join(paths, ',');

  using StatusT = StatusOr<std::vector<ApexFile>>;
  auto verify_fn = [](std::vector<ApexFile>& apexes) {
    for (const ApexFile& apex_file : apexes) {
      StatusOr<ApexVerityData> verity_or = apex_file.VerifyApexVerity(
          {kApexKeySystemDirectory, kApexKeyProductDirectory});
      if (!verity_or.Ok()) {
        return StatusT::MakeError(verity_or.ErrorMessage());
      }
    }
    return StatusT(std::move(apexes));
  };
  return HandlePackages<StatusT>(paths, verify_fn);
}

StatusOr<ApexFile> verifySessionDir(const int session_id) {
  std::string sessionDirPath = std::string(kStagedSessionsDir) + "/session_" +
                               std::to_string(session_id);
  LOG(INFO) << "Scanning " << sessionDirPath
            << " looking for packages to be validated";
  StatusOr<std::vector<std::string>> scan =
      FindApexFilesByName(sessionDirPath, /* include_dirs=*/false);
  if (!scan.Ok()) {
    LOG(WARNING) << scan.ErrorMessage();
    return StatusOr<ApexFile>::MakeError(scan.ErrorMessage());
  }

  if (scan->size() > 1) {
    return StatusOr<ApexFile>::MakeError(
        "More than one APEX package found in the same session directory.");
  }

  auto verified = verifyPackages(*scan);
  if (!verified.Ok()) {
    return StatusOr<ApexFile>::MakeError(verified.ErrorStatus());
  }
  return StatusOr<ApexFile>(std::move((*verified)[0]));
}

}  // namespace

namespace apexd_private {

Status MountPackage(const ApexFile& apex, const std::string& mountPoint) {
  LOG(VERBOSE) << "Creating mount point: " << mountPoint;
  if (mkdir(mountPoint.c_str(), kMkdirMode) != 0) {
    return Status::Fail(PStringLog()
                        << "Could not create mount point " << mountPoint);
  }

  MountedApexData data("", apex.GetPath());
  Status st = apex.IsFlattened() ? mountFlattened(apex, mountPoint, &data)
                                 : mountNonFlattened(apex, mountPoint, &data);
  if (!st.Ok()) {
    if (rmdir(mountPoint.c_str()) != 0) {
      PLOG(WARNING) << "Could not rmdir " << mountPoint;
    }
    return st;
  }

  gMountedApexes.AddMountedApex(apex.GetManifest().name(), false,
                                std::move(data));
  return Status::Success();
}

Status UnmountPackage(const ApexFile& apex) {
  LOG(VERBOSE) << "Unmounting " << GetPackageId(apex.GetManifest());

  const ApexManifest& manifest = apex.GetManifest();

  const MountedApexData* data = nullptr;
  bool latest = false;

  gMountedApexes.ForallMountedApexes(manifest.name(),
                                     [&](const MountedApexData& d, bool l) {
                                       if (d.full_path == apex.GetPath()) {
                                         data = &d;
                                         latest = l;
                                       }
                                     });

  if (data == nullptr) {
    return Status::Fail(StringLog() << "Did not find " << apex.GetPath());
  }

  if (latest) {
    return Status::Fail(StringLog()
                        << "Package " << apex.GetPath() << " is active");
  }

  std::string mount_point = apexd_private::GetPackageMountPoint(manifest);
  // Lazily try to umount whatever is mounted.
  if (umount2(mount_point.c_str(), UMOUNT_NOFOLLOW | MNT_DETACH) != 0 &&
      errno != EINVAL && errno != ENOENT) {
    return Status::Fail(PStringLog()
                        << "Failed to unmount directory " << mount_point);
  }

  // Clean up gMountedApexes now, even though we're not fully done.
  std::string loop = data->loop_name;
  gMountedApexes.RemoveMountedApex(manifest.name(), apex.GetPath());

  // Attempt to delete the folder. If the folder is retained, other
  // data may be incorrect.
  if (rmdir(mount_point.c_str()) != 0) {
    PLOG(ERROR) << "Failed to rmdir directory " << mount_point;
  }

  // Try to free up the loop device.
  if (!loop.empty()) {
    auto log_fn = [](const std::string& path,
                     const std::string& id ATTRIBUTE_UNUSED) {
      LOG(VERBOSE) << "Freeing loop device " << path << "for unmount.";
    };
    loop::DestroyLoopDevice(loop, log_fn);
  }

  return Status::Success();
}

bool IsMounted(const std::string& name, const std::string& full_path) {
  bool found_mounted = false;
  gMountedApexes.ForallMountedApexes(
      name, [&](const MountedApexData& data, bool latest ATTRIBUTE_UNUSED) {
        if (full_path == data.full_path) {
          found_mounted = true;
        }
      });
  return found_mounted;
}

std::string GetPackageMountPoint(const ApexManifest& manifest) {
  return StringPrintf("%s/%s", kApexRoot, GetPackageId(manifest).c_str());
}

std::string GetActiveMountPoint(const ApexManifest& manifest) {
  return StringPrintf("%s/%s", kApexRoot, manifest.name().c_str());
}

}  // namespace apexd_private

Status activatePackage(const std::string& full_path) {
  LOG(INFO) << "Trying to activate " << full_path;

  StatusOr<ApexFile> apexFile = ApexFile::Open(full_path);
  if (!apexFile.Ok()) {
    return apexFile.ErrorStatus();
  }
  const ApexManifest& manifest = apexFile->GetManifest();

  // See whether we think it's active, and do not allow to activate the same
  // version. Also detect whether this is the highest version.
  // We roll this into a single check.
  bool is_newest_version = true;
  bool found_other_version = false;
  bool version_found_mounted = false;
  {
    uint64_t new_version = manifest.version();
    bool version_found_active = false;
    gMountedApexes.ForallMountedApexes(
        manifest.name(), [&](const MountedApexData& data, bool latest) {
          StatusOr<ApexFile> otherApex = ApexFile::Open(data.full_path);
          if (!otherApex.Ok()) {
            return;
          }
          found_other_version = true;
          if (static_cast<uint64_t>(otherApex->GetManifest().version()) ==
              new_version) {
            version_found_mounted = true;
            version_found_active = latest;
          }
          if (static_cast<uint64_t>(otherApex->GetManifest().version()) >
              new_version) {
            is_newest_version = false;
          }
        });
    if (version_found_active) {
      return Status::Fail("Package is already active.");
    }
  }

  std::string mountPoint = apexd_private::GetPackageMountPoint(manifest);

  if (!version_found_mounted) {
    Status mountStatus = apexd_private::MountPackage(*apexFile, mountPoint);
    if (!mountStatus.Ok()) {
      return mountStatus;
    }
  }

  bool mounted_latest = false;
  if (is_newest_version) {
    Status update_st = apexd_private::BindMount(
        apexd_private::GetActiveMountPoint(manifest), mountPoint);
    mounted_latest = update_st.Ok();
    if (!update_st.Ok()) {
      // TODO: Fail?
      LOG(ERROR) << update_st.ErrorMessage();
    }
  }
  if (mounted_latest) {
    gMountedApexes.SetLatest(manifest.name(), full_path);
  }

  return Status::Success();
}

Status deactivatePackage(const std::string& full_path) {
  LOG(INFO) << "Trying to deactivate " << full_path;

  StatusOr<ApexFile> apexFile = ApexFile::Open(full_path);
  if (!apexFile.Ok()) {
    return apexFile.ErrorStatus();
  }

  Status st = deactivatePackageImpl(*apexFile);

  if (st.Ok()) {
    gMountedApexes.RemoveMountedApex(apexFile->GetManifest().name(), full_path);
  }

  return st;
}

std::vector<ApexFile> getActivePackages() {
  std::vector<ApexFile> ret;
  gMountedApexes.ForallMountedApexes(
      [&](const std::string&, const MountedApexData& data, bool latest) {
        if (!latest) {
          return;
        }

        StatusOr<ApexFile> apexFile = ApexFile::Open(data.full_path);
        if (!apexFile.Ok()) {
          // TODO: Fail?
          return;
        }
        ret.emplace_back(std::move(*apexFile));
      });

  return ret;
}

StatusOr<ApexFile> getActivePackage(const std::string& packageName) {
  std::vector<ApexFile> packages = getActivePackages();
  for (ApexFile& apex : packages) {
    if (apex.GetManifest().name() == packageName) {
      return StatusOr<ApexFile>(std::move(apex));
    }
  }

  return StatusOr<ApexFile>::MakeError(
      PStringLog() << "Cannot find matching package for: " << packageName);
}

void unmountAndDetachExistingImages() {
  // TODO: this procedure should probably not be needed anymore when apexd
  // becomes an actual daemon. Remove if that's the case.
  LOG(INFO) << "Scanning " << kApexRoot
            << " looking for packages already mounted.";
  StatusOr<std::vector<std::string>> folders_status =
      ReadDir(kApexRoot, &DTypeFilter<DT_DIR>);
  if (!folders_status.Ok()) {
    LOG(ERROR) << folders_status.ErrorMessage();
    return;
  }

  // Sort the folders. This way, the "latest" folder will appear before any
  // versioned folder, so we'll unmount the bind-mount first.
  std::vector<std::string>& folders = *folders_status;
  std::sort(folders.begin(), folders.end());

  for (const std::string& full_path : folders) {
    LOG(INFO) << "Unmounting " << full_path;
    // Lazily try to umount whatever is mounted.
    if (umount2(full_path.c_str(), UMOUNT_NOFOLLOW | MNT_DETACH) != 0 &&
        errno != EINVAL && errno != ENOENT) {
      PLOG(ERROR) << "Failed to unmount directory " << full_path;
    }
    // Attempt to delete the folder. If the folder is retained, other
    // data may be incorrect.
    // TODO: Fix this.
    if (rmdir(full_path.c_str()) != 0) {
      PLOG(ERROR) << "Failed to rmdir directory " << full_path;
    }
  }

  loop::destroyAllLoopDevices();
}

void scanPackagesDirAndActivate(const char* apex_package_dir) {
  LOG(INFO) << "Scanning " << apex_package_dir << " looking for APEX packages.";

  const bool scanSystemApexes =
      StartsWith(apex_package_dir, kApexPackageSystemDir);
  StatusOr<std::vector<std::string>> scan =
      FindApexFilesByName(apex_package_dir, scanSystemApexes);
  if (!scan.Ok()) {
    LOG(WARNING) << scan.ErrorMessage();
    return;
  }

  for (const std::string& name : *scan) {
    LOG(INFO) << "Found " << name;

    Status res = activatePackage(name);
    if (!res.Ok()) {
      LOG(ERROR) << res.ErrorMessage();
    }
  }
}

void scanStagedSessionsDirAndStage() {
  LOG(INFO) << "Scanning " << kApexSessionsDir
            << " looking for sessions to be activated.";

  // TODO(b/118865310): Checkpoint the existing set of active packages in case
  // we need to rollback the session.
  // TODO(b/118865310) also get sessions in PENDING_RETRY state
  auto stagedSessions = ApexSession::GetSessionsInState(SessionState::STAGED);
  for (auto& session : stagedSessions) {
    auto sessionId = session.GetId();

    auto session_failed_fn = [&]() {
      // TODO(b/118865310): retry, and if it keeps failing, rollback the changes
      // and reboot the device.
      LOG(WARNING) << "Marking session " << sessionId << " as failed.";
      session.UpdateStateAndCommit(SessionState::ACTIVATION_FAILED);
    };
    auto scope_guard = android::base::make_scope_guard(session_failed_fn);

    std::vector<std::string> dirsToScan;
    if (session.GetChildSessionIds().empty()) {
      dirsToScan.push_back(std::string(kStagedSessionsDir) + "/session_" +
                           std::to_string(sessionId));
    } else {
      for (auto childSessionId : session.GetChildSessionIds()) {
        dirsToScan.push_back(std::string(kStagedSessionsDir) + "/session_" +
                             std::to_string(childSessionId));
      }
    }

    std::vector<std::string> apexes;
    bool scanSuccessful = true;
    for (auto dirToScan : dirsToScan) {
      StatusOr<std::vector<std::string>> scan =
          FindApexFilesByName(dirToScan, /* include_dirs=*/false);
      if (!scan.Ok()) {
        LOG(WARNING) << scan.ErrorMessage();
        scanSuccessful = false;
        break;
      }

      if (scan->size() > 1) {
        LOG(WARNING) << "More than one APEX package found in the same session "
                     << "directory " << dirToScan << ", skipping activation.";
        scanSuccessful = false;
        break;
      }

      if (scan->empty()) {
        LOG(WARNING) << "No APEX packages found while scanning " << dirToScan
                     << " session id: " << sessionId << ".";
        scanSuccessful = false;
        break;
      }
      apexes.push_back(std::move((*scan)[0]));
    }

    if (!scanSuccessful) {
      continue;
    }

    // Run postinstall, if necessary.
    Status postinstall_status = postinstallPackages(apexes);
    if (!postinstall_status.Ok()) {
      LOG(ERROR) << "Postinstall failed for session "
                 << std::to_string(sessionId) << ": "
                 << postinstall_status.ErrorMessage();
      continue;
    }

    const Status result = stagePackages(apexes, /* linkPackages */ true);
    if (!result.Ok()) {
      LOG(ERROR) << "Activation failed for packages " << Join(apexes, ',')
                 << ": " << result.ErrorMessage();
      continue;
    }

    // Session was OK, release scopeguard.
    scope_guard.Disable();

    session.UpdateStateAndCommit(SessionState::ACTIVATED);
  }
}

Status preinstallPackages(const std::vector<std::string>& paths) {
  if (paths.empty()) {
    return Status::Fail("Empty set of inputs");
  }
  LOG(DEBUG) << "preinstallPackages() for " << Join(paths, ',');
  return HandlePackages<Status>(paths, PreinstallPackages);
}

Status postinstallPackages(const std::vector<std::string>& paths) {
  if (paths.empty()) {
    return Status::Fail("Empty set of inputs");
  }
  LOG(DEBUG) << "postinstallPackages() for " << Join(paths, ',');
  return HandlePackages<Status>(paths, PostinstallPackages);
}

Status stagePackages(const std::vector<std::string>& tmpPaths,
                     bool linkPackages) {
  if (tmpPaths.empty()) {
    return Status::Fail("Empty set of inputs");
  }
  LOG(DEBUG) << "stagePackages() for " << Join(tmpPaths, ',');

  // Note: this function is temporary. As such the code is not optimized, e.g.,
  //       it will open ApexFiles multiple times.

  // 1) Verify all packages.
  auto verify_status = verifyPackages(tmpPaths);
  if (!verify_status.Ok()) {
    return Status::Fail(verify_status.ErrorMessage());
  }

  // 2) Now stage all of them.

  // Make sure that kActiveApexPackagesDataDir exists.
  auto create_dir_status =
      createDirIfNeeded(std::string(kActiveApexPackagesDataDir), 0750);
  if (!create_dir_status.Ok()) {
    return Status::Fail(create_dir_status.ErrorMessage());
  }

  auto path_fn = [](const ApexFile& apex_file) {
    return StringPrintf("%s/%s%s", kActiveApexPackagesDataDir,
                        GetPackageId(apex_file.GetManifest()).c_str(),
                        kApexPackageSuffix);
  };

  // Ensure the APEX gets removed on failure.
  std::unordered_set<std::string> staged_files;
  auto deleter = [&staged_files]() {
    for (const std::string& staged_path : staged_files) {
      if (TEMP_FAILURE_RETRY(unlink(staged_path.c_str())) != 0) {
        PLOG(ERROR) << "Unable to unlink " << staged_path;
      }
    }
  };
  auto scope_guard = android::base::make_scope_guard(deleter);

  std::unordered_set<std::string> staged_packages;
  for (const std::string& path : tmpPaths) {
    StatusOr<ApexFile> apex_file = ApexFile::Open(path);
    if (!apex_file.Ok()) {
      return apex_file.ErrorStatus();
    }
    std::string dest_path = path_fn(*apex_file);

    if (linkPackages) {
      if (link(apex_file->GetPath().c_str(), dest_path.c_str()) != 0) {
        // TODO: Get correct binder error status.
        return Status::Fail(PStringLog()
                            << "Unable to link " << apex_file->GetPath()
                            << " to " << dest_path);
      }
    } else {
      if (rename(apex_file->GetPath().c_str(), dest_path.c_str()) != 0) {
        // TODO: Get correct binder error status.
        return Status::Fail(PStringLog()
                            << "Unable to rename " << apex_file->GetPath()
                            << " to " << dest_path);
      }
    }
    staged_files.insert(dest_path);
    staged_packages.insert(apex_file->GetManifest().name());

    if (!linkPackages) {
      // TODO(b/112669193,b/118865310) remove this. Link files from staging
      // directory should be the only method allowed.
      if (selinux_android_restorecon(dest_path.c_str(), 0) < 0) {
        return Status::Fail(PStringLog()
                            << "Failed to restorecon " << dest_path);
      }
    }
    LOG(DEBUG) << "Success linking " << apex_file->GetPath() << " to "
               << dest_path;
  }

  scope_guard.Disable();  // Accept the state.

  return RemovePreviouslyActiveApexFiles(staged_packages, staged_files);
}

Status rollbackLastSession() {
  // TODO Unstage newly staged packages and call Checkpoint#abortCheckpoint
  LOG(INFO) << "Rolling back last session";
  return Status::Success();
}

void onStart() {
  LOG(INFO) << "Marking APEXd as starting";
  if (!android::base::SetProperty(kApexStatusSysprop, kApexStatusStarting)) {
    PLOG(ERROR) << "Failed to set " << kApexStatusSysprop << " to "
                << kApexStatusStarting;
  }
}

void onAllPackagesReady() {
  // Set a system property to let other components to know that APEXs are
  // correctly mounted and ready to be used. Before using any file from APEXs,
  // they can query this system property to ensure that they are okay to
  // access. Or they may have a on-property trigger to delay a task until
  // APEXs become ready.
  LOG(INFO) << "Marking APEXd as ready";
  if (!android::base::SetProperty(kApexStatusSysprop, kApexStatusReady)) {
    PLOG(ERROR) << "Failed to set " << kApexStatusSysprop << " to "
                << kApexStatusReady;
  }
}

StatusOr<std::vector<ApexFile>> submitStagedSession(
    const int session_id, const std::vector<int>& child_session_ids) {
  std::vector<int> ids_to_scan;
  if (!child_session_ids.empty()) {
    ids_to_scan = child_session_ids;
  } else {
    ids_to_scan = {session_id};
  }

  std::vector<ApexFile> ret;
  for (int id_to_scan : ids_to_scan) {
    auto verified = verifySessionDir(id_to_scan);
    if (!verified.Ok()) {
      return StatusOr<std::vector<ApexFile>>::MakeError(verified.ErrorStatus());
    }
    ret.push_back(std::move(*verified));
  }

  // Run preinstall, if necessary.
  Status preinstall_status = PreinstallPackages(ret);
  if (!preinstall_status.Ok()) {
    return StatusOr<std::vector<ApexFile>>::MakeError(preinstall_status);
  }

  auto session = ApexSession::CreateSession(session_id);
  if (!session.Ok()) {
    return StatusOr<std::vector<ApexFile>>::MakeError(session.ErrorMessage());
  }
  (*session).SetChildSessionIds(child_session_ids);
  Status commit_status =
      (*session).UpdateStateAndCommit(SessionState::VERIFIED);
  if (!commit_status.Ok()) {
    return StatusOr<std::vector<ApexFile>>::MakeError(commit_status);
  }

  return StatusOr<std::vector<ApexFile>>(std::move(ret));
}

Status markStagedSessionReady(const int session_id) {
  auto session = ApexSession::GetSession(session_id);
  if (!session.Ok()) {
    return session.ErrorStatus();
  }
  // We should only accept sessions in SessionState::VERIFIED or
  // SessionState::STAGED state. In the SessionState::STAGED case, this
  // function is effectively a no-op.
  auto session_state = (*session).GetState();
  if (session_state == SessionState::STAGED) {
    return Status::Success();
  }
  if (session_state == SessionState::VERIFIED) {
    return (*session).UpdateStateAndCommit(SessionState::STAGED);
  }
  return Status::Fail(StringLog() << "Invalid state for session " << session_id
                                  << ". Cannot mark it as ready.");
}

}  // namespace apex
}  // namespace android
