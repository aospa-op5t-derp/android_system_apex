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
#include "apexd_preinstall.h"
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

using android::base::EndsWith;
using android::base::ReadFullyAtOffset;
using android::base::StringPrintf;
using android::base::unique_fd;
using android::dm::DeviceMapper;
using android::dm::DmDeviceState;
using android::dm::DmTable;
using android::dm::DmTargetVerity;

namespace android {
namespace apex {

using MountedApexData = MountedApexDatabase::MountedApexData;

namespace {

static constexpr const char* kApexPackageSuffix = ".apex";
static constexpr const char* kApexLoopIdPrefix = "apex:";
static constexpr const char* kApexKeySystemDirectory =
    "/system/etc/security/apex/";
static constexpr const char* kApexKeyProductDirectory =
    "/product/etc/security/apex/";

// 128 kB read-ahead, which we currently use for /system as well
static constexpr const char* kReadAheadKb = "128";

// These should be in-sync with system/sepolicy/public/property_contexts
static constexpr const char* kApexStatusSysprop = "apexd.status";
static constexpr const char* kApexStatusStarting = "starting";
static constexpr const char* kApexStatusReady = "ready";

MountedApexDatabase gMountedApexes;

struct LoopbackDeviceUniqueFd {
  unique_fd device_fd;
  std::string name;

  LoopbackDeviceUniqueFd() {}
  LoopbackDeviceUniqueFd(unique_fd&& fd, const std::string& name)
      : device_fd(std::move(fd)), name(name) {}

  LoopbackDeviceUniqueFd(LoopbackDeviceUniqueFd&& fd) noexcept
      : device_fd(std::move(fd.device_fd)), name(fd.name) {}
  LoopbackDeviceUniqueFd& operator=(LoopbackDeviceUniqueFd&& other) noexcept {
    MaybeCloseBad();
    device_fd = std::move(other.device_fd);
    name = std::move(other.name);
    return *this;
  }

  ~LoopbackDeviceUniqueFd() { MaybeCloseBad(); }

  void MaybeCloseBad() {
    if (device_fd.get() != -1) {
      // Disassociate any files.
      if (ioctl(device_fd.get(), LOOP_CLR_FD) == -1) {
        PLOG(ERROR) << "Unable to clear fd for loopback device";
      }
    }
  }

  void CloseGood() { device_fd.reset(-1); }

  int get() { return device_fd.get(); }
};

Status configureReadAhead(const std::string& device_path) {
  auto pos = device_path.find("/dev/block/");
  if (pos != 0) {
    return Status::Fail(StringLog()
                        << "Device path does not start with /dev/block.");
  }
  pos = device_path.find_last_of("/");
  std::string device_name = device_path.substr(pos + 1, std::string::npos);

  std::string sysfs_device =
      StringPrintf("/sys/block/%s/queue/read_ahead_kb", device_name.c_str());
  unique_fd sysfs_fd(open(sysfs_device.c_str(), O_RDWR | O_CLOEXEC));
  if (sysfs_fd.get() == -1) {
    return Status::Fail(PStringLog() << "Failed to open " << sysfs_device);
  }

  int ret = TEMP_FAILURE_RETRY(
      write(sysfs_fd.get(), kReadAheadKb, strlen(kReadAheadKb) + 1));
  if (ret < 0) {
    return Status::Fail(PStringLog() << "Failed to write to " << sysfs_device);
  }

  return Status::Success();
}

StatusOr<LoopbackDeviceUniqueFd> createLoopDevice(const std::string& target,
                                                  const int32_t imageOffset,
                                                  const size_t imageSize) {
  using Failed = StatusOr<LoopbackDeviceUniqueFd>;
  unique_fd ctl_fd(open("/dev/loop-control", O_RDWR | O_CLOEXEC));
  if (ctl_fd.get() == -1) {
    return Failed::MakeError(PStringLog() << "Failed to open loop-control");
  }

  int num = ioctl(ctl_fd.get(), LOOP_CTL_GET_FREE);
  if (num == -1) {
    return Failed::MakeError(PStringLog() << "Failed LOOP_CTL_GET_FREE");
  }

  std::string device = StringPrintf("/dev/block/loop%d", num);

  unique_fd target_fd(open(target.c_str(), O_RDONLY | O_CLOEXEC));
  if (target_fd.get() == -1) {
    return Failed::MakeError(PStringLog() << "Failed to open " << target);
  }
  LoopbackDeviceUniqueFd device_fd(
      unique_fd(open(device.c_str(), O_RDWR | O_CLOEXEC)), device);
  if (device_fd.get() == -1) {
    return Failed::MakeError(PStringLog() << "Failed to open " << device);
  }

  if (ioctl(device_fd.get(), LOOP_SET_FD, target_fd.get()) == -1) {
    return Failed::MakeError(PStringLog() << "Failed to LOOP_SET_FD");
  }

  struct loop_info64 li;
  memset(&li, 0, sizeof(li));
  strlcpy((char*)li.lo_crypt_name, kApexLoopIdPrefix, LO_NAME_SIZE);
  li.lo_offset = imageOffset;
  li.lo_sizelimit = imageSize;
  if (ioctl(device_fd.get(), LOOP_SET_STATUS64, &li) == -1) {
    return Failed::MakeError(PStringLog() << "Failed to LOOP_SET_STATUS64");
  }

  if (ioctl(device_fd.get(), BLKFLSBUF, 0) == -1) {
    // This works around a kernel bug where the following happens.
    // 1) The device runs with a value of loop.max_part > 0
    // 2) As part of LOOP_SET_FD above, we do a partition scan, which loads
    //    the first 2 pages of the underlying file into the buffer cache
    // 3) When we then change the offset with LOOP_SET_STATUS64, those pages
    //    are not invalidated from the cache.
    // 4) When we try to mount an ext4 filesystem on the loop device, the ext4
    //    code will try to find a superblock by reading 4k at offset 0; but,
    //    because we still have the old pages at offset 0 lying in the cache,
    //    those pages will be returned directly. However, those pages contain
    //    the data at offset 0 in the underlying file, not at the offset that
    //    we configured
    // 5) the ext4 driver fails to find a superblock in the (wrong) data, and
    //    fails to mount the filesystem.
    //
    // To work around this, explicitly flush the block device, which will flush
    // the buffer cache and make sure we actually read the data at the correct
    // offset.
    return Failed::MakeError(PStringLog()
                             << "Failed to flush buffers on the loop device.");
  }

  // Direct-IO requires the loop device to have the same block size as the
  // underlying filesystem.
  if (ioctl(device_fd.get(), LOOP_SET_BLOCK_SIZE, 4096) == -1) {
    PLOG(WARNING) << "Failed to LOOP_SET_BLOCK_SIZE";
  } else {
    if (ioctl(device_fd.get(), LOOP_SET_DIRECT_IO, 1) == -1) {
      PLOG(WARNING) << "Failed to LOOP_SET_DIRECT_IO";
      // TODO Eventually we'll want to fail on this; right now we can't because
      // not all devices have the necessary kernel patches.
    }
  }

  Status readAheadStatus = configureReadAhead(device);
  if (!readAheadStatus.Ok()) {
    return Failed::MakeError(StringLog() << readAheadStatus.ErrorMessage());
  }
  return StatusOr<LoopbackDeviceUniqueFd>(std::move(device_fd));
}

template <typename T>
void DestroyLoopDevice(const std::string& path, T extra) {
  unique_fd fd(open(path.c_str(), O_RDWR | O_CLOEXEC));
  if (fd.get() == -1) {
    if (errno != ENOENT) {
      PLOG(WARNING) << "Failed to open " << path;
    }
    return;
  }

  struct loop_info64 li;
  if (ioctl(fd.get(), LOOP_GET_STATUS64, &li) < 0) {
    if (errno != ENXIO) {
      PLOG(WARNING) << "Failed to LOOP_GET_STATUS64 " << path;
    }
    return;
  }

  auto id = std::string((char*)li.lo_crypt_name);
  if (android::base::StartsWith(id, kApexLoopIdPrefix)) {
    extra(path, id);

    if (ioctl(fd.get(), LOOP_CLR_FD, 0) < 0) {
      PLOG(WARNING) << "Failed to LOOP_CLR_FD " << path;
    }
  }
}

void destroyAllLoopDevices() {
  std::string root = "/dev/block/";
  auto dirp =
      std::unique_ptr<DIR, int (*)(DIR*)>(opendir(root.c_str()), closedir);
  if (!dirp) {
    PLOG(ERROR) << "Failed to open /dev/block/, can't destroy loop devices.";
    return;
  }

  // Poke through all devices looking for loop devices.
  auto log_fn = [](const std::string& path, const std::string& id) {
    LOG(DEBUG) << "Tearing down stale loop device at " << path << " named "
               << id;
  };
  struct dirent* de;
  while ((de = readdir(dirp.get()))) {
    auto test = std::string(de->d_name);
    if (!android::base::StartsWith(test, "loop")) continue;

    auto path = root + de->d_name;
    DestroyLoopDevice(path, log_fn);
  }
}

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

StatusOr<std::vector<std::string>> getApexRootSubFolders() {
  // This code would be much shorter if C++17's std::filesystem were available,
  // which is not at the time of writing this.
  auto d = std::unique_ptr<DIR, int (*)(DIR*)>(opendir(kApexRoot), closedir);
  if (!d) {
    return StatusOr<std::vector<std::string>>::MakeError(
        PStringLog() << "Can't open " << kApexRoot << " for reading.");
  }

  std::vector<std::string> ret;
  struct dirent* dp;
  while ((dp = readdir(d.get())) != NULL) {
    if (dp->d_type != DT_DIR || (strcmp(dp->d_name, ".") == 0) ||
        (strcmp(dp->d_name, "..") == 0)) {
      continue;
    }
    ret.push_back(dp->d_name);
  }

  return StatusOr<std::vector<std::string>>(std::move(ret));
}

Status mountNonFlattened(const ApexFile& apex, const std::string& mountPoint,
                         MountedApexData* apex_data) {
  const ApexManifest& manifest = apex.GetManifest();
  const std::string& full_path = apex.GetPath();
  const std::string& packageId = manifest.GetPackageId();

  LoopbackDeviceUniqueFd loopbackDevice;
  for (size_t attempts = 1;; ++attempts) {
    StatusOr<LoopbackDeviceUniqueFd> ret =
        createLoopDevice(full_path, apex.GetImageOffset(), apex.GetImageSize());
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
      !android::base::StartsWith(full_path, kApexPackageSystemDir);
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

    Status readAheadStatus = configureReadAhead(verityDev.GetDevPath());
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
  if (!android::base::StartsWith(apex.GetPath(), kApexPackageSystemDir)) {
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
    destroyAllLoopDevices();
  }

  if (error_msg.empty()) {
    return Status::Success();
  } else {
    return Status::Fail(error_msg);
  }
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

  gMountedApexes.AddMountedApex(apex.GetManifest().GetName(), false,
                                std::move(data));
  return Status::Success();
}

Status UnmountPackage(const ApexFile& apex) {
  LOG(VERBOSE) << "Unmounting " << apex.GetManifest().GetPackageId();

  const ApexManifest& manifest = apex.GetManifest();

  const MountedApexData* data = nullptr;
  bool latest = false;

  gMountedApexes.ForallMountedApexes(manifest.GetName(),
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
  gMountedApexes.RemoveMountedApex(manifest.GetName(), apex.GetPath());

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
    DestroyLoopDevice(loop, log_fn);
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
  return StringPrintf("%s/%s", kApexRoot, manifest.GetPackageId().c_str());
}

std::string GetActiveMountPoint(const ApexManifest& manifest) {
  return StringPrintf("%s/%s", kApexRoot, manifest.GetName().c_str());
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
    uint64_t new_version = manifest.GetVersion();
    bool version_found_active = false;
    gMountedApexes.ForallMountedApexes(
        manifest.GetName(), [&](const MountedApexData& data, bool latest) {
          StatusOr<ApexFile> otherApex = ApexFile::Open(data.full_path);
          if (!otherApex.Ok()) {
            return;
          }
          found_other_version = true;
          if (otherApex->GetManifest().GetVersion() == new_version) {
            version_found_mounted = true;
            version_found_active = latest;
          }
          if (otherApex->GetManifest().GetVersion() > new_version) {
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
    gMountedApexes.SetLatest(manifest.GetName(), full_path);
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
    gMountedApexes.RemoveMountedApex(apexFile->GetManifest().GetName(),
                                     full_path);
  }

  return st;
}

std::vector<std::pair<std::string, uint64_t>> getActivePackages() {
  std::vector<std::pair<std::string, uint64_t>> ret;
  gMountedApexes.ForallMountedApexes([&](const std::string& package,
                                         const MountedApexData& data,
                                         bool latest) {
    if (!latest) {
      return;
    }

    StatusOr<ApexFile> apexFile = ApexFile::Open(data.full_path);
    if (!apexFile.Ok()) {
      // TODO: Fail?
      return;
    }

    ret.emplace_back(package, apexFile->GetManifest().GetVersion());
  });

  return ret;
}

void unmountAndDetachExistingImages() {
  // TODO: this procedure should probably not be needed anymore when apexd
  // becomes an actual daemon. Remove if that's the case.
  LOG(INFO) << "Scanning " << kApexRoot
            << " looking for packages already mounted.";
  StatusOr<std::vector<std::string>> folders_status = getApexRootSubFolders();
  if (!folders_status.Ok()) {
    LOG(ERROR) << folders_status.ErrorMessage();
    return;
  }

  // Sort the folders. This way, the "latest" folder will appear before any
  // versioned folder, so we'll unmount the bind-mount first.
  std::vector<std::string>& folders = *folders_status;
  std::sort(folders.begin(), folders.end());

  for (const std::string& folder : folders) {
    std::string full_path = std::string(kApexRoot).append("/").append(folder);
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

  destroyAllLoopDevices();
}

void scanPackagesDirAndActivate(const char* apex_package_dir) {
  LOG(INFO) << "Scanning " << apex_package_dir << " looking for APEX packages.";
  auto d =
      std::unique_ptr<DIR, int (*)(DIR*)>(opendir(apex_package_dir), closedir);

  if (!d) {
    PLOG(WARNING) << "Package directory " << apex_package_dir
                  << " not found, nothing to do.";
    return;
  }
  const bool scanSystemApexes =
      android::base::StartsWith(apex_package_dir, kApexPackageSystemDir);
  struct dirent* dp;
  while ((dp = readdir(d.get())) != NULL) {
    const std::string name(dp->d_name);
    if (name == "." || name == "..") {
      continue;
    }
    const bool isApexFile =
        dp->d_type == DT_REG && EndsWith(name, kApexPackageSuffix);
    if (isApexFile || (dp->d_type == DT_DIR && scanSystemApexes)) {
      LOG(INFO) << "Found " << name;

      Status res = activatePackage(std::string(apex_package_dir) + "/" + name);
      if (!res.Ok()) {
        LOG(ERROR) << res.ErrorMessage();
      }
    }
  }
}

Status verifyPackages(const std::vector<std::string>& paths) {
  LOG(DEBUG) << "verifyPackages() for " << android::base::Join(paths, ',');

  if (paths.empty()) {
    return Status::Fail("Empty set of inputs");
  }

  // Note: assume that sessions do not have thousands of paths, so that it is
  //       OK to keep the ApexFile/ApexManifests in memory.

  // Open all to check they can be opened and have valid manifests etc.
  for (const std::string& path : paths) {
    StatusOr<ApexFile> apex_file = ApexFile::Open(path);
    if (!apex_file.Ok()) {
      return apex_file.ErrorStatus();
    }
    StatusOr<ApexVerityData> verity_or = apex_file->VerifyApexVerity(
        {kApexKeySystemDirectory, kApexKeyProductDirectory});
    if (!verity_or.Ok()) {
      return verity_or.ErrorStatus();
    }
  }

  return Status::Success();
}

Status preinstallPackages(const std::vector<std::string>& paths) {
  LOG(DEBUG) << "preinstallPackages() for " << android::base::Join(paths, ',');

  if (paths.empty()) {
    return Status::Fail("Empty set of inputs");
  }

  // Note: assume that sessions do not have thousands of paths, so that it is
  //       OK to keep the ApexFile/ApexManifests in memory.

  // 1) Open all APEXes, check whether they have hooks.
  bool has_preInstallHooks = false;
  std::vector<ApexFile> apex_files;
  for (const std::string& path : paths) {
    StatusOr<ApexFile> apex_file = ApexFile::Open(path);
    if (!apex_file.Ok()) {
      return apex_file.ErrorStatus();
    }
    if (!apex_file->GetManifest().GetPreInstallHook().empty()) {
      has_preInstallHooks = true;
    }
    apex_files.emplace_back(std::move(*apex_file));
  }

  // 2) If we found hooks, run pre-install.
  if (has_preInstallHooks) {
    Status preinstall_status = StagePreInstall(apex_files);
    if (!preinstall_status.Ok()) {
      return preinstall_status;
    }
  }

  return Status::Success();
}

Status stagePackages(const std::vector<std::string>& tmp_paths) {
  LOG(DEBUG) << "stagePackages() for " << android::base::Join(tmp_paths, ',');

  // Note: this function is temporary. As such the code is not optimized, e.g.,
  //       it will open ApexFiles multiple times.

  // 1) Verify all packages.
  Status verify_status = verifyPackages(tmp_paths);
  if (!verify_status.Ok()) {
    return verify_status;
  }
  if (tmp_paths.empty()) {
    return Status::Fail("Empty set of inputs");
  }

  // 2) Now stage all of them.

  auto path_fn = [](const ApexFile& apex_file) {
    return StringPrintf("%s/%s%s", kApexPackageDataDir,
                        apex_file.GetManifest().GetPackageId().c_str(),
                        kApexPackageSuffix);
  };

  // Ensure the APEX gets removed on failure.
  std::vector<std::string> staged;
  auto deleter = [&staged]() {
    for (const std::string& staged_path : staged) {
      if (TEMP_FAILURE_RETRY(unlink(staged_path.c_str())) != 0) {
        PLOG(ERROR) << "Unable to unlink " << staged_path;
      }
    }
  };
  auto scope_guard = android::base::make_scope_guard(deleter);

  for (const std::string& path : tmp_paths) {
    StatusOr<ApexFile> apex_file = ApexFile::Open(path);
    if (!apex_file.Ok()) {
      return apex_file.ErrorStatus();
    }
    std::string dest_path = path_fn(*apex_file);

    if (rename(apex_file->GetPath().c_str(), dest_path.c_str()) != 0) {
      // TODO: Get correct binder error status.
      return Status::Fail(PStringLog()
                          << "Unable to rename " << apex_file->GetPath()
                          << " to " << dest_path);
    }
    staged.push_back(dest_path);

    // TODO(b/112669193) remove this. Move the file from packageTmpPath to
    // destPath using file descriptor.
    if (selinux_android_restorecon(dest_path.c_str(), 0) < 0) {
      return Status::Fail(PStringLog() << "Failed to restorecon " << dest_path);
    }
    LOG(DEBUG) << "Success renaming " << apex_file->GetPath() << " to "
               << dest_path;
  }

  // 3) Run preinstall, if necessary.
  Status preinstall_status = preinstallPackages(staged);
  if (!preinstall_status.Ok()) {
    return preinstall_status;
  }

  scope_guard.Disable();  // Accept the state.
  return Status::Success();
}

void onStart() {
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
  if (!android::base::SetProperty(kApexStatusSysprop, kApexStatusReady)) {
    PLOG(ERROR) << "Failed to set " << kApexStatusSysprop << " to "
                << kApexStatusReady;
  }
}

}  // namespace apex
}  // namespace android
