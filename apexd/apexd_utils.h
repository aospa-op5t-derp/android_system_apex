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

#ifndef ANDROID_APEXD_APEXD_UTILS_H_
#define ANDROID_APEXD_APEXD_UTILS_H_

#include <filesystem>
#include <string>
#include <vector>

#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <android-base/logging.h>
#include <cutils/android_reboot.h>

#include "string_log.h"

namespace android {
namespace apex {

inline int WaitChild(pid_t pid) {
  int status;
  pid_t got_pid = TEMP_FAILURE_RETRY(waitpid(pid, &status, 0));

  if (got_pid != pid) {
    PLOG(WARNING) << "waitpid failed: wanted " << pid << ", got " << got_pid;
    return 1;
  }

  if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
    return 0;
  } else {
    return status;
  }
}

inline int ForkAndRun(const std::vector<std::string>& args,
                      std::string* error_msg) {
  std::vector<const char*> argv;
  argv.resize(args.size() + 1, nullptr);
  std::transform(args.begin(), args.end(), argv.begin(),
                 [](const std::string& in) { return in.c_str(); });

  // 3) Fork.
  pid_t pid = fork();
  if (pid == -1) {
    // Fork failed.
    *error_msg = PStringLog() << "Unable to fork";
    return -1;
  }

  if (pid == 0) {
    execv(argv[0], const_cast<char**>(argv.data()));
    PLOG(ERROR) << "execv failed";
    _exit(1);
  }

  int rc = WaitChild(pid);
  if (rc != 0) {
    *error_msg = StringLog() << "Failed run: status=" << rc;
  }
  return rc;
}

template <typename FilterFn>
StatusOr<std::vector<std::string>> ReadDir(const std::string& path,
                                           FilterFn fn) {
  // TODO use C++17's std::filesystem instead
  auto d = std::unique_ptr<DIR, int (*)(DIR*)>(opendir(path.c_str()), closedir);
  if (!d) {
    return StatusOr<std::vector<std::string>>::MakeError(
        PStringLog() << "Can't open " << path << " for reading");
  }

  std::vector<std::string> ret;
  struct dirent* dp;
  while ((dp = readdir(d.get())) != NULL) {
    if ((strcmp(dp->d_name, ".") == 0) || (strcmp(dp->d_name, "..") == 0)) {
      continue;
    }
    if (!fn(dp->d_type, dp->d_name)) {
      continue;
    }
    ret.push_back(path + "/" + dp->d_name);
  }

  return StatusOr<std::vector<std::string>>(std::move(ret));
}

inline Status createDirIfNeeded(const std::string& path, mode_t mode) {
  struct stat stat_data;

  if (stat(path.c_str(), &stat_data) != 0) {
    if (errno == ENOENT) {
      if (mkdir(path.c_str(), mode) != 0) {
        return Status::Fail(PStringLog() << "Could not mkdir " << path);
      }
    } else {
      return Status::Fail(PStringLog() << "Could not stat " << path);
    }
  } else {
    if (!S_ISDIR(stat_data.st_mode)) {
      return Status::Fail(path + " exists and is not a directory.");
    }
  }

  // Need to manually call chmod because mkdir will create a folder with
  // permissions mode & ~umask.
  if (chmod(path.c_str(), mode) != 0) {
    return Status::Fail(PStringLog() << "Could not chmod " << path);
  }

  return Status::Success();
}

inline Status DeleteDirContent(const std::string& path) {
  auto files = ReadDir(path, [](auto _, auto __) { return true; });
  if (!files.Ok()) {
    return Status::Fail(StringLog() << "Failed to delete " << path << " : "
                                    << files.ErrorMessage());
  }
  for (const std::string& file : *files) {
    if (unlink(file.c_str()) != 0) {
      return Status::Fail(PStringLog() << "Failed to delete " << file);
    }
  }
  return Status::Success();
}

inline StatusOr<bool> PathExists(const std::string& path) {
  namespace fs = std::filesystem;

  std::error_code ec;
  if (!fs::exists(fs::path(path), ec)) {
    if (ec) {
      return StatusOr<bool>::MakeError(StringLog() << "Failed to access "
                                                   << path << " : " << ec);
    } else {
      return StatusOr<bool>(false);
    }
  }
  return StatusOr<bool>(true);
}

inline void Reboot() {
  LOG(INFO) << "Rebooting device";
  if (android_reboot(ANDROID_RB_RESTART2, 0, nullptr) != 0) {
    LOG(ERROR) << "Failed to reboot device";
  }
}

}  // namespace apex
}  // namespace android

#endif  // ANDROID_APEXD_APEXD_UTILS_H_
