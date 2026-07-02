/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <unordered_map>
#include <string>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <rex/math.h>
#include <rex/memory/utils.h>
#include <rex/platform.h>
#include <rex/string.h>

#if REX_PLATFORM_ANDROID
#include <string.h>

#include <dlfcn.h>
#include <sys/ioctl.h>

#include <linux/ashmem.h>

// TODO(tomc): Android or maybe na. idk
// #include "xenia/base/main_android.h"
#endif

namespace rex {
namespace memory {

// Convert filesystem path to valid shm_open name (must start with /, no other slashes)
static std::string MakeShmName(const std::filesystem::path& path) {
  std::string name = path.string();
  for (char& c : name) {
    if (c == '/')
      c = '_';
  }
  if (name.empty() || name[0] != '/') {
    name.insert(name.begin(), '/');
  }
  return name;
}

#if REX_PLATFORM_ANDROID
// May be null if no dynamically loaded functions are required.
static void* libandroid_;
// API 26+.
static int (*android_ASharedMemory_create_)(const char* name, size_t size);

void AndroidInitialize() {
  if (rex::GetAndroidApiLevel() >= 26) {
    libandroid_ = dlopen("libandroid.so", RTLD_NOW);
    assert_not_null(libandroid_);
    if (libandroid_) {
      android_ASharedMemory_create_ = reinterpret_cast<decltype(android_ASharedMemory_create_)>(
          dlsym(libandroid_, "ASharedMemory_create"));
      assert_not_null(android_ASharedMemory_create_);
    }
  }
}

void AndroidShutdown() {
  android_ASharedMemory_create_ = nullptr;
  if (libandroid_) {
    dlclose(libandroid_);
    libandroid_ = nullptr;
  }
}
#endif

size_t page_size() {
  return getpagesize();
}
size_t allocation_granularity() {
  return page_size();
}

uint32_t ToPosixProtectFlags(PageAccess access) {
  switch (access) {
    case PageAccess::kNoAccess:
      return PROT_NONE;
    case PageAccess::kReadOnly:
      return PROT_READ;
    case PageAccess::kReadWrite:
      return PROT_READ | PROT_WRITE;
    case PageAccess::kExecuteReadOnly:
      return PROT_READ | PROT_EXEC;
    case PageAccess::kExecuteReadWrite:
      return PROT_READ | PROT_WRITE | PROT_EXEC;
    default:
      assert_unhandled_case(access);
      return PROT_NONE;
  }
}

bool IsWritableExecutableMemorySupported() {
  return true;
}

// TODO(tomc): this needs to go somewhere else. we should utilize the platform namespace more.
#if REX_PLATFORM_LINUX
namespace {

struct LinuxMapEntry {
  uintptr_t start = 0;
  uintptr_t end = 0;
  char perms[5] = {};
};

// Parse a line from /proc/self/maps into a LinuxMapEntry
static bool ParseProcMapsLine(const std::string& line, LinuxMapEntry& out) {
  out = LinuxMapEntry{};
  unsigned long long start = 0, end = 0;
  char perms[5] = {};
  const int matched = std::sscanf(line.c_str(), "%llx-%llx %4s", &start, &end, perms);
  if (matched < 3)
    return false;
  out.start = static_cast<uintptr_t>(start);
  out.end = static_cast<uintptr_t>(end);
  std::memcpy(out.perms, perms, sizeof(out.perms));
  return out.start < out.end;
}

// Find the mapping entry in /proc/self/maps that contains the given address.
//
// ASYNC-SIGNAL-SAFE + allocation-free by construction: this runs inside the
// SIGSEGV handler (MMIOHandler::ExceptionCallback -> QueryProtect) where
// ifstream/std::string are not permitted -- and their cost is not academic: a
// 23-minute Ayn Thor session locked up with a guest thread burning 78% of the
// process's CPU re-running an ifstream/getline version of this parse on every
// write-watch fault (the file has thousands of lines mid-session). Raw
// open/read syscalls, fixed stack buffer, hand-rolled hex parse.
static bool FindEntryForAddress(void* address, LinuxMapEntry& out_entry) {
  const uintptr_t addr = reinterpret_cast<uintptr_t>(address);
  int fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    return false;
  bool found = false;
  char buf[4096];
  char line[192];
  size_t line_len = 0;
  bool line_overflow = false;
  for (;;) {
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n <= 0)
      break;
    for (ssize_t i = 0; i < n && !found; ++i) {
      char c = buf[i];
      if (c != '\n') {
        if (line_len < sizeof(line) - 1) {
          line[line_len++] = c;
        } else {
          line_overflow = true;  // only the tail (pathname) can overflow
        }
        continue;
      }
      line[line_len] = '\0';
      // Parse "start-end perms ..." without sscanf (keeps this handler-safe on
      // libcs whose sscanf allocates).
      const char* p = line;
      uintptr_t start = 0, end = 0;
      bool ok = false;
      while (*p) {
        unsigned d;
        if (*p >= '0' && *p <= '9') d = unsigned(*p - '0');
        else if (*p >= 'a' && *p <= 'f') d = unsigned(*p - 'a' + 10);
        else break;
        start = (start << 4) | d;
        ++p;
        ok = true;
      }
      if (ok && *p == '-') {
        ++p;
        ok = false;
        while (*p) {
          unsigned d;
          if (*p >= '0' && *p <= '9') d = unsigned(*p - '0');
          else if (*p >= 'a' && *p <= 'f') d = unsigned(*p - 'a' + 10);
          else break;
          end = (end << 4) | d;
          ++p;
          ok = true;
        }
      }
      if (ok && *p == ' ' && start < end && addr >= start && addr < end) {
        out_entry = LinuxMapEntry{};
        out_entry.start = start;
        out_entry.end = end;
        for (int k = 0; k < 4 && p[1 + k] && p[1 + k] != ' '; ++k) {
          out_entry.perms[k] = p[1 + k];
        }
        found = true;
      }
      line_len = 0;
      line_overflow = false;
      (void)line_overflow;
    }
    if (found)
      break;
  }
  close(fd);
  return found;
}

// Fast per-page protection shadow.
//
// Parsing /proc/self/maps in QueryProtect (and in Protect when the old access is
// requested) is a severe hot-path cost: the guest's write-watch mechanism faults
// on every write to a watched page, and MMIOHandler::ExceptionCallback then calls
// QueryProtect, re-reading and re-parsing the entire maps file each time. On a
// busy title this dominates the CPU and the game crawls.
//
// All protection changes within the guest region funnel through Protect()/
// DeallocFixed(), so we record the per-page access here and answer QueryProtect /
// old-access lookups in O(1). Any page we have not recorded falls back to the
// /proc/self/maps scan, so correctness is unchanged - the shadow is purely a
// fast path for the pages the fault loop actually touches.
std::mutex g_prot_shadow_mutex;
std::unordered_map<uintptr_t, PageAccess> g_prot_shadow;

// Cap for EAGER shadow population (AllocFixed): above this, pages memoize
// lazily on first fault instead, so a multi-GB reservation doesn't turn into
// millions of map entries at boot.
constexpr size_t kShadowEagerMaxBytes = 4 * 1024 * 1024;

static void ShadowSet(void* base_address, size_t length, PageAccess access) {
  const size_t ps = page_size();
  if (!base_address || !length || !ps) {
    return;
  }
  uintptr_t a = reinterpret_cast<uintptr_t>(base_address) & ~(static_cast<uintptr_t>(ps) - 1);
  const uintptr_t end = reinterpret_cast<uintptr_t>(base_address) + length;
  std::lock_guard<std::mutex> lock(g_prot_shadow_mutex);
  for (; a < end; a += ps) {
    g_prot_shadow[a] = access;
  }
}

static void ShadowErase(void* base_address, size_t length) {
  const size_t ps = page_size();
  if (!base_address || !length || !ps) {
    return;
  }
  uintptr_t a = reinterpret_cast<uintptr_t>(base_address) & ~(static_cast<uintptr_t>(ps) - 1);
  const uintptr_t end = reinterpret_cast<uintptr_t>(base_address) + length;
  std::lock_guard<std::mutex> lock(g_prot_shadow_mutex);
  for (; a < end; a += ps) {
    g_prot_shadow.erase(a);
  }
}

// Returns true and sets access_out if the page containing base_address is in the
// shadow.
static bool ShadowQuery(void* base_address, PageAccess& access_out) {
  const size_t ps = page_size();
  if (!ps) {
    return false;
  }
  const uintptr_t page =
      reinterpret_cast<uintptr_t>(base_address) & ~(static_cast<uintptr_t>(ps) - 1);
  std::lock_guard<std::mutex> lock(g_prot_shadow_mutex);
  auto it = g_prot_shadow.find(page);
  if (it == g_prot_shadow.end()) {
    return false;
  }
  access_out = it->second;
  return true;
}

// Check if [base, base+length) is fully covered by existing mappings (no gaps)
static bool IsRangeFullyMapped(void* base_address, size_t length) {
  if (!base_address || length == 0)
    return false;

  const uintptr_t begin = reinterpret_cast<uintptr_t>(base_address);
  const uintptr_t end = begin + length;
  if (end < begin) {  // overflow check
    return false;
  }

  std::ifstream maps("/proc/self/maps");
  if (!maps.is_open())
    return false;

  uintptr_t cursor = begin;
  std::string line;
  while (std::getline(maps, line)) {
    LinuxMapEntry e;
    if (!ParseProcMapsLine(line, e))
      continue;
    if (e.end <= cursor)
      continue;
    if (e.start > cursor)
      return false;  // gap found
    cursor = e.end;
    if (cursor >= end)
      return true;
  }
  return cursor >= end;
}

// Convert /proc/self/maps permission chars to PageAccess
static PageAccess PermsToPageAccess(const char perms[5]) {
  const bool r = perms[0] == 'r';
  const bool w = perms[1] == 'w';
  const bool x = perms[2] == 'x';

  if (!r && !w && !x)
    return PageAccess::kNoAccess;
  if (x)
    return w ? PageAccess::kExecuteReadWrite : PageAccess::kExecuteReadOnly;
  return w ? PageAccess::kReadWrite : PageAccess::kReadOnly;
}

}  // namespace
#endif  // REX_PLATFORM_LINUX

void* AllocFixed(void* base_address, size_t length, AllocationType allocation_type,
                 PageAccess access) {
  // Emulates Windows VirtualAlloc behavior:
  // - Reserve: create PROT_NONE mapping to hold address space
  // - Commit on existing reservation: mprotect to enable access (EEXIST path)
  // - New allocation: mmap with MAP_FIXED_NOREPLACE (never silently replace)
  const uint32_t prot_requested = ToPosixProtectFlags(access);

  // Determine initial protection based on allocation type
  int prot_initial = 0;
  switch (allocation_type) {
    case AllocationType::kReserve:
      prot_initial = PROT_NONE;
      break;
    case AllocationType::kCommit:
    case AllocationType::kReserveCommit:
    default:
      prot_initial = static_cast<int>(prot_requested);
      break;
  }

  // Build flags - always use MAP_FIXED_NOREPLACE for fixed addresses
  int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#if defined(MAP_FIXED_NOREPLACE)
  if (base_address) {
    flags |= MAP_FIXED_NOREPLACE;
  }
#else
  if (base_address) {
    flags |= MAP_FIXED;
  }
#endif

  void* result = mmap(base_address, length, prot_initial, flags, -1, 0);
  if (result != MAP_FAILED) {
#if REX_PLATFORM_LINUX
    // Record in the protection shadow so the write-watch fault path never has
    // to fall back to parsing /proc/self/maps for these pages. Reservations
    // are PROT_NONE regardless of the requested access. Bounded: shadowing a
    // multi-GB reservation page-by-page would put millions of entries in the
    // map; huge ranges memoize lazily instead (QueryProtect records a page the
    // first time it faults), which caps the per-page maps-parse cost at once.
    if (length <= kShadowEagerMaxBytes) {
      ShadowSet(result, length,
                allocation_type == AllocationType::kReserve ? PageAccess::kNoAccess : access);
    }
#endif
    return result;
  }
#if defined(MAP_FIXED_NOREPLACE) && REX_PLATFORM_LINUX
  // Handle EEXIST: address already has a mapping (e.g., from prior Reserve)
  // This is the "commit on existing reservation" path
  if (errno == EEXIST && base_address &&
      (allocation_type == AllocationType::kCommit ||
       allocation_type == AllocationType::kReserveCommit)) {
    // Verify the entire range is mapped before using mprotect
    if (IsRangeFullyMapped(base_address, length)) {
      if (mprotect(base_address, length, static_cast<int>(prot_requested)) == 0) {
        if (length <= kShadowEagerMaxBytes) {
          ShadowSet(base_address, length, access);
        }
        return base_address;
      }
    }
  }
#endif

  return nullptr;
}

bool DeallocFixed(void* base_address, size_t length, DeallocationType deallocation_type) {
  switch (deallocation_type) {
    case DeallocationType::kDecommit: {
      // Decommit: remove access first, then release physical pages
      if (mprotect(base_address, length, PROT_NONE) != 0) {
        return false;
      }
#if defined(MADV_DONTNEED)
      (void)madvise(base_address, length, MADV_DONTNEED);
#endif
#if REX_PLATFORM_LINUX
      ShadowSet(base_address, length, PageAccess::kNoAccess);
#endif
      return true;
    }
    case DeallocationType::kRelease: {
      bool ok = munmap(base_address, length) == 0;
#if REX_PLATFORM_LINUX
      if (ok) {
        ShadowErase(base_address, length);
      }
#endif
      return ok;
    }
    default:
      // how we get here? :(
      assert_always();
      return false;
  }
}

bool Protect(void* base_address, size_t length, PageAccess access, PageAccess* out_old_access) {
  if (out_old_access) {
    *out_old_access = PageAccess::kNoAccess;
  }

#if REX_PLATFORM_LINUX
  // NOTE(tomc): we may want to look at doing this differently. it should work for now
  //             but there is a TOCTOU window between reading and changing.
  //             This really shouldn't be an issue since VirtualProtect on Windows isn't truly
  //             atomic in a mutli-threaded process either, but it's something to be aware of.
  // Query old access before changing, if the caller needs it. Prefer the fast
  // shadow; fall back to /proc/self/maps for pages we have not recorded.
  if (out_old_access) {
    PageAccess old_access;
    if (ShadowQuery(base_address, old_access)) {
      *out_old_access = old_access;
    } else {
      LinuxMapEntry e;
      if (FindEntryForAddress(base_address, e)) {
        *out_old_access = PermsToPageAccess(e.perms);
      }
    }
  }
#endif

  uint32_t prot = ToPosixProtectFlags(access);
  if (mprotect(base_address, length, prot) != 0) {
    return false;
  }
#if REX_PLATFORM_LINUX
  ShadowSet(base_address, length, access);
#endif
  return true;
}

bool QueryProtect(void* base_address, size_t& length, PageAccess& access_out) {
#if !REX_PLATFORM_LINUX
  access_out = PageAccess::kNoAccess;
  length = 0;
  return false;
#else
  access_out = PageAccess::kNoAccess;
  length = 0;

  // Fast path: per-page protection shadow (avoids parsing /proc/self/maps on the
  // MMIO/write-watch fault hot path). Returns a single page's worth - the only
  // caller (MMIOHandler::ExceptionCallback) queries exactly one page.
  PageAccess shadow_access;
  if (ShadowQuery(base_address, shadow_access)) {
    access_out = shadow_access;
    length = page_size();
    return true;
  }

  LinuxMapEntry e;
  if (!FindEntryForAddress(base_address, e)) {
    return false;
  }

  const uintptr_t addr = reinterpret_cast<uintptr_t>(base_address);
  length = static_cast<size_t>(e.end - addr);
  access_out = PermsToPageAccess(e.perms);

  // Memoize: a page that fell through to the maps parse pays for it AT MOST
  // once. All subsequent protection changes in the guest region funnel through
  // Protect()/AllocFixed()/DeallocFixed(), which keep the shadow current, so
  // the recorded value stays coherent. Without this, a page class that misses
  // the shadow (observed live on the Ayn Thor) re-parses the entire maps file
  // on EVERY write-watch fault -- thousands per frame -- and the game locks up.
  ShadowSet(base_address, page_size(), access_out);

  return true;
#endif
}

FileMappingHandle CreateFileMappingHandle(const std::filesystem::path& path, size_t length,
                                          PageAccess access, bool commit) {
#if REX_PLATFORM_ANDROID
  // TODO(Triang3l): Check if memfd can be used instead on API 30+.
  if (android_ASharedMemory_create_) {
    int sharedmem_fd = android_ASharedMemory_create_(path.c_str(), length);
    return sharedmem_fd >= 0 ? static_cast<FileMappingHandle>(sharedmem_fd)
                             : kFileMappingHandleInvalid;
  }

  // Use /dev/ashmem on API versions below 26, which added ASharedMemory.
  // /dev/ashmem was disabled on API 29 for apps targeting it.
  // https://chromium.googlesource.com/chromium/src/+/master/third_party/ashmem/ashmem-dev.c
  int ashmem_fd = open("/" ASHMEM_NAME_DEF, O_RDWR);
  if (ashmem_fd < 0) {
    return kFileMappingHandleInvalid;
  }
  char ashmem_name[ASHMEM_NAME_LEN];
  strlcpy(ashmem_name, path.c_str(), rex::countof(ashmem_name));
  if (ioctl(ashmem_fd, ASHMEM_SET_NAME, ashmem_name) < 0 ||
      ioctl(ashmem_fd, ASHMEM_SET_SIZE, length) < 0) {
    close(ashmem_fd);
    return kFileMappingHandleInvalid;
  }
  return static_cast<FileMappingHandle>(ashmem_fd);
#else
  int oflag;
  switch (access) {
    case PageAccess::kNoAccess:
      oflag = 0;
      break;
    case PageAccess::kReadOnly:
    case PageAccess::kExecuteReadOnly:
      oflag = O_RDONLY;
      break;
    case PageAccess::kReadWrite:
    case PageAccess::kExecuteReadWrite:
      oflag = O_RDWR;
      break;
    default:
      assert_always();
      return kFileMappingHandleInvalid;
  }
  oflag |= O_CREAT;
  auto full_path = MakeShmName(path);
  int ret = shm_open(full_path.c_str(), oflag, 0777);
  if (ret < 0) {
    return kFileMappingHandleInvalid;
  }
  if (ftruncate64(ret, static_cast<off_t>(length)) != 0) {
    close(ret);
    shm_unlink(full_path.c_str());
    return kFileMappingHandleInvalid;
  }
  return static_cast<FileMappingHandle>(ret);
#endif
}

void CloseFileMappingHandle(FileMappingHandle handle, const std::filesystem::path& path) {
  close(static_cast<int>(handle));
#if !REX_PLATFORM_ANDROID
  auto full_path = MakeShmName(path);
  shm_unlink(full_path.c_str());
#endif
}

void* MapFileView(FileMappingHandle handle, void* base_address, size_t length, PageAccess access,
                  size_t file_offset) {
  // file_offset must be page-aligned
  const size_t page = page_size();
  if (file_offset % page != 0) {
    return nullptr;
  }

  int flags = MAP_SHARED;

  // For file views, we need MAP_FIXED to replace existing reservations.
  // The emulator reserves address space first, then maps file views into it.
  // MAP_FIXED_NOREPLACE would fail with EEXIST in this case.
  if (base_address) {
    flags |= MAP_FIXED;
  }

  uint32_t prot = ToPosixProtectFlags(access);
  void* result = mmap64(base_address, length, prot, flags, static_cast<int>(handle),
                        static_cast<off_t>(file_offset));
  if (result == MAP_FAILED) {
    return nullptr;
  }

  // Verify we got the address we asked for
  if (base_address && result != base_address) {
    munmap(result, length);
    return nullptr;
  }

  return result;
}

bool UnmapFileView(FileMappingHandle handle, void* base_address, size_t length) {
  return munmap(base_address, length) == 0;
}

}  // namespace memory
}  // namespace rex
