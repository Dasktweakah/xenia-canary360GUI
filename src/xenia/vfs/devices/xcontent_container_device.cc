/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/vfs/devices/xcontent_container_device.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/vfs/devices/host_path_device.h"

namespace xe {
namespace vfs {

XContentContainerDevice::XContentContainerDevice(
    const std::string_view mount_path, const size_t data_file_count)
    : Device(mount_path),
      name_("XContent"),
      data_file_count_(data_file_count),
      files_total_size_(0) {}

XContentContainerDevice::~XContentContainerDevice() {}

bool XContentContainerDevice::Initialize() {
  if (LoadHostFiles(data_file_count_) != Result::kSuccess) {
    XELOGE("Error loading XContent host files.");
    return false;
  }

  if (Read() != Result::kSuccess) {
    return false;
  }

  LoadExtraFiles();
  return true;
}

Entry* XContentContainerDevice::ResolvePath(const std::string_view path) {
  // The filesystem will have stripped our prefix off already, so the path will
  // be in the form:
  // some\PATH.foo
  XELOGFS("StfsContainerDevice::ResolvePath({})", path);
  Entry* entry = root_entry_->ResolvePath(path);
  if (extra_files_) {
    // A file in the extra files folder replaces the package's; a folder there
    // is used only when the package has none by that name, so the package's
    // own folders keep all of their files.
    Entry* extra = extra_files_->ResolvePath(path);
    if (extra && (!entry || !(extra->attributes() & kFileAttributeDirectory))) {
      XELOGFS("StfsContainerDevice::ResolvePath({}): from the extra files",
              path);
      return extra;
    }
  }
  return entry;
}

void XContentContainerDevice::SetExtraFiles(
    const std::filesystem::path& folder) {
  extra_files_folder_ = folder;
}

void XContentContainerDevice::LoadExtraFiles() {
  if (extra_files_folder_.empty()) {
    return;
  }
  auto extra =
      std::make_unique<HostPathDevice>(mount_path(), extra_files_folder_, true);
  if (!extra->Initialize()) {
    XELOGE("XContent: couldn't read the extra files in {}",
           xe::path_to_utf8(extra_files_folder_));
    return;
  }
  XELOGI("XContent: {} adds its extra files from {}", mount_path(),
         xe::path_to_utf8(extra_files_folder_));
  GraftExtraFiles(root_entry_.get(), extra->ResolvePath(""));
  extra_files_ = std::move(extra);
}

void XContentContainerDevice::GraftExtraFiles(Entry* into, Entry* from) {
  if (!into || !from) {
    return;
  }
  auto& extra_children = from->children_;
  for (auto it = extra_children.begin(); it != extra_children.end();) {
    Entry* own = into->GetChild((*it)->name());
    if (!own) {
      // New to the package: becomes part of its folder.
      (*it)->parent_ = into;
      into->children_.push_back(std::move(*it));
      it = extra_children.erase(it);
      continue;
    }
    const bool own_dir = own->attributes() & kFileAttributeDirectory;
    const bool extra_dir = (*it)->attributes() & kFileAttributeDirectory;
    if (own_dir && extra_dir) {
      GraftExtraFiles(own, it->get());
    } else if (!own_dir && !extra_dir) {
      // The same file: the extra one replaces the package's, in folder
      // listings too (a game may take a file's size from its folder).
      for (auto& child : into->children_) {
        if (child.get() == own) {
          (*it)->parent_ = into;
          child = std::move(*it);
          break;
        }
      }
      it = extra_children.erase(it);
      continue;
    }
    ++it;
  }
}

void XContentContainerDevice::Dump(StringBuffer* string_buffer) {
  auto global_lock = global_critical_region_.Acquire();
  root_entry_->Dump(string_buffer, 0);
}

}  // namespace vfs
}  // namespace xe
