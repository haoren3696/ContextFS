// ntstatus_map.h must come first: it sets up WIN32_NO_STATUS before
// <windows.h> so STATUS_xxx macros aren't double-defined when
// <ntstatus.h> arrives later via <winfsp/winfsp.h>.
#include "platform/windows/ntstatus_map.h"
#include "platform/windows/path_translation.h"
#include "platform.h"
#include "daemon.h"
#include "bootstrap.h"
#include "object_store.h"
#include "working_tree.h"

#include <winfsp/winfsp.h>
#include <sddl.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace cas {
namespace {

// Mirror the FUSE-side helper. `Daemon::branch()` can return null if the
// branch was deleted while a file handle was still open; fall back to
// main so dereferencing is always safe.
std::shared_ptr<BranchContext> branch_for_fh(Daemon* d, uint32_t branch_id) {
    auto br = d->branch(branch_id);
    return br ? br : d->main_branch();
}

// WorkingTree never stores the root "/" itself — Bootstrap only ingests
// its children — so callbacks that look up a path must synthesize the
// root. Mirrors fuse_adapter.cpp's `if (strcmp(path,"/")==0)` branch
// in cas_getattr; without it, the Windows kernel can't open the
// volume root and Z:\ appears empty.
std::optional<WorkingTreeEntry> lookup_with_root(WorkingTree& wt,
                                                  const std::string& path) {
    if (path == "/") return WorkingTreeEntry{EntryKind::Tree, ZERO_HASH, 040755};
    return wt.lookup(path);
}

HANDLE g_stop_event = nullptr;
BOOL WINAPI ctrl_handler(DWORD t) {
    if (t == CTRL_C_EVENT || t == CTRL_CLOSE_EVENT || t == CTRL_BREAK_EVENT) {
        if (g_stop_event) SetEvent(g_stop_event);
        return TRUE;
    }
    return FALSE;
}

uint32_t fnv_hash(const std::string& s) {
    uint32_t h = 2166136261u;
    for (unsigned char c : s) { h ^= c; h *= 16777619u; }
    return h ? h : 1u;
}

struct PathContext {
    std::string path;
    uint64_t    fh = 0;
    bool        is_dir = false;
};

void fill_file_info(Daemon& d, const WorkingTreeEntry& e,
                    FSP_FSCTL_FILE_INFO* info) {
    std::memset(info, 0, sizeof(*info));
    if (e.kind == EntryKind::Tree) {
        info->FileAttributes = FILE_ATTRIBUTE_DIRECTORY;
        return;
    }
    info->FileAttributes = FILE_ATTRIBUTE_NORMAL;
    if (e.hash != ZERO_HASH) {
        std::vector<uint8_t> blob;
        if (d.store().read_blob(e.hash, blob)) {
            info->FileSize = blob.size();
            info->AllocationSize =
                (info->FileSize + 4095) & ~(uint64_t)4095;
        }
    }
}

// ───── Callbacks ─────

static NTSTATUS cb_get_volume_info(FSP_FILE_SYSTEM*, FSP_FSCTL_VOLUME_INFO* info) {
    std::memset(info, 0, sizeof(*info));
    info->TotalSize = 1ull << 40;
    info->FreeSize  = 1ull << 39;
    const wchar_t* label = L"agentvfs";
    info->VolumeLabelLength = (UINT16)(std::wcslen(label) * sizeof(wchar_t));
    std::wcsncpy(info->VolumeLabel, label,
                 sizeof(info->VolumeLabel)/sizeof(wchar_t) - 1);
    return STATUS_SUCCESS;
}

static NTSTATUS cb_get_security_by_name(FSP_FILE_SYSTEM* fs, PWSTR file_name,
        PUINT32 p_attrs, PSECURITY_DESCRIPTOR sd_buf, SIZE_T* p_sd_size) {
    auto* d = static_cast<Daemon*>(fs->UserContext);
    std::string path = win::narrow(file_name);
    if (auto* bs = d->bootstrap()) bs->ensure_path(path);
    auto e = lookup_with_root(d->working_tree(), path);
    if (!e) return STATUS_OBJECT_NAME_NOT_FOUND;
    if (p_attrs)
        *p_attrs = (e->kind == EntryKind::Tree)
            ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;

    // Use the same SDDL pattern as WinFsp's memfs sample: Owner=BA,
    // Group=BA, Protected DACL granting Full-Access to SYSTEM, Admins,
    // and Everyone. The previous "D:P(A;;GA;;;WD)" was missing Owner
    // and Group SIDs, and the Windows kernel quietly rejected the
    // returned SD (Get-ChildItem traces showed it re-asking for "/"
    // hundreds of times instead of proceeding to open it).
    static PSECURITY_DESCRIPTOR cached_sd = nullptr;
    static SIZE_T cached_sz = 0;
    static std::once_flag sd_once;
    std::call_once(sd_once, [] {
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                L"O:BAG:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FA;;;WD)",
                SDDL_REVISION_1, &cached_sd, nullptr)) {
            std::fprintf(stderr, "cas: ConvertStringSD failed %lu\n",
                         GetLastError());
        } else {
            cached_sz = GetSecurityDescriptorLength(cached_sd);
        }
    });
    if (!cached_sd) return STATUS_UNSUCCESSFUL;
    if (p_sd_size) {
        if (*p_sd_size < cached_sz) { *p_sd_size = cached_sz; return STATUS_BUFFER_OVERFLOW; }
        *p_sd_size = cached_sz;
        if (sd_buf) std::memcpy(sd_buf, cached_sd, cached_sz);
    }
    return STATUS_SUCCESS;
}

static NTSTATUS cb_open(FSP_FILE_SYSTEM* fs, PWSTR file_name,
        UINT32, UINT32, PVOID* p_ctx, FSP_FSCTL_FILE_INFO* fi) {
    auto* d = static_cast<Daemon*>(fs->UserContext);
    std::string path = win::narrow(file_name);
    if (Daemon::is_hidden(path)) return STATUS_OBJECT_NAME_NOT_FOUND;
    if (auto* bs = d->bootstrap()) bs->ensure_path(path);

    auto br = d->main_branch();
    std::lock_guard<std::mutex> lk(br->checkpoint_mu);
    auto e = lookup_with_root(br->wt, path);
    if (!e) return STATUS_OBJECT_NAME_NOT_FOUND;

    auto* ctx = new PathContext{path, 0, e->kind == EntryKind::Tree};
    fill_file_info(*d, *e, fi);
    if (!ctx->is_dir) {
        // Get base_size by reading the blob directly. Don't take it from
        // fi->FileSize: fill_file_info silently leaves FileSize at 0 if
        // the blob read failed, and zero-base writes would then truncate
        // the file's effective view.
        uint64_t base_size = 0;
        if (e->hash != ZERO_HASH) {
            std::vector<uint8_t> blob;
            if (!d->store().read_blob(e->hash, blob))
                return STATUS_UNEXPECTED_IO_ERROR;
            base_size = blob.size();
        }
        auto state = std::make_unique<FhState>();
        state->path = path;
        state->branch_id = br->branch_id;
        d->refs().read_ref(br->name, state->pinned_commit);
        state->base_blob = e->hash;
        state->base_size = base_size;
        state->write_buf = std::make_unique<WriteBuffer>(e->hash, base_size);
        ctx->fh = d->allocate_fh(std::move(state));
    }
    *p_ctx = ctx;
    return STATUS_SUCCESS;
}

static VOID cb_close(FSP_FILE_SYSTEM* fs, PVOID file_context) {
    auto* d = static_cast<Daemon*>(fs->UserContext);
    auto* ctx = static_cast<PathContext*>(file_context);
    if (!ctx) return;
    if (ctx->fh) d->release_fh(ctx->fh);
    delete ctx;
}

static NTSTATUS cb_read(FSP_FILE_SYSTEM* fs, PVOID file_context,
        PVOID buffer, UINT64 offset, ULONG length, PULONG p_xfer) {
    auto* d = static_cast<Daemon*>(fs->UserContext);
    auto* ctx = static_cast<PathContext*>(file_context);
    if (!ctx || !ctx->fh) return STATUS_INVALID_HANDLE;
    auto s = d->get_fh(ctx->fh);
    if (!s) return STATUS_FILE_INVALID;
    auto br = branch_for_fh(d, s->branch_id);
    std::lock_guard<std::mutex> lk(br->checkpoint_mu);
    if (s->stale) return STATUS_FILE_INVALID;
    d->ensure_base_cache(*s);
    size_t n = s->write_buf->read(offset, (uint8_t*)buffer, length, s->base_cache);
    *p_xfer = (ULONG)n;
    return n == 0 ? STATUS_END_OF_FILE : STATUS_SUCCESS;
}

static NTSTATUS cb_get_file_info(FSP_FILE_SYSTEM* fs, PVOID file_context,
                                  FSP_FSCTL_FILE_INFO* info) {
    auto* d = static_cast<Daemon*>(fs->UserContext);
    auto* ctx = static_cast<PathContext*>(file_context);
    if (!ctx) return STATUS_INVALID_HANDLE;
    auto e = lookup_with_root(d->working_tree(), ctx->path);
    if (!e) return STATUS_OBJECT_NAME_NOT_FOUND;
    fill_file_info(*d, *e, info);
    if (ctx->fh) {
        if (auto s = d->get_fh(ctx->fh)) {
            uint64_t sz = s->write_buf->effective_size(s->base_size);
            info->FileSize = sz;
            info->AllocationSize = (sz + 4095) & ~(uint64_t)4095;
        }
    }
    return STATUS_SUCCESS;
}

static NTSTATUS cb_read_directory(FSP_FILE_SYSTEM* fs, PVOID file_context,
        PWSTR, PWSTR marker, PVOID buf, ULONG length, PULONG p_xfer) {
    auto* d = static_cast<Daemon*>(fs->UserContext);
    auto* ctx = static_cast<PathContext*>(file_context);
    if (!ctx) return STATUS_INVALID_HANDLE;

    std::string mk = marker ? win::narrow(marker) : "";
    if (!mk.empty() && mk.front() == '/') mk.erase(0, 1);
    for (auto& [child_path, child_entry] : d->working_tree().list_dir(ctx->path)) {
        std::string name = child_path;
        auto slash = name.rfind('/');
        if (slash != std::string::npos) name = name.substr(slash + 1);
        if (name == ".agentvfs-store") continue;
        if (!mk.empty() && name <= mk) continue;

        std::wstring wname(name.begin(), name.end());
        size_t sz = sizeof(FSP_FSCTL_DIR_INFO) + wname.size() * sizeof(wchar_t);
        std::vector<unsigned char> dbuf(sz, 0);
        auto* dir = reinterpret_cast<FSP_FSCTL_DIR_INFO*>(dbuf.data());
        dir->Size = (UINT16)sz;
        fill_file_info(*d, child_entry, &dir->FileInfo);
        std::memcpy(dir->FileNameBuf, wname.data(), wname.size() * sizeof(wchar_t));
        if (!FspFileSystemAddDirInfo(dir, buf, length, p_xfer))
            return STATUS_SUCCESS;
    }
    FspFileSystemAddDirInfo(nullptr, buf, length, p_xfer);
    return STATUS_SUCCESS;
}

static NTSTATUS cb_create(FSP_FILE_SYSTEM* fs, PWSTR file_name,
        UINT32 create_options, UINT32, UINT32, PSECURITY_DESCRIPTOR, UINT64,
        PVOID* p_ctx, FSP_FSCTL_FILE_INFO* fi) {
    auto* d = static_cast<Daemon*>(fs->UserContext);
    std::string path = win::narrow(file_name);
    if (Daemon::is_hidden(path)) return STATUS_ACCESS_DENIED;
    if (auto* bs = d->bootstrap()) bs->ensure_path(path);

    auto br = d->main_branch();
    bool is_dir = (create_options & FILE_DIRECTORY_FILE) != 0;
    std::lock_guard<std::mutex> lk(br->checkpoint_mu);

    auto* ctx = new PathContext{path, 0, is_dir};
    if (is_dir) {
        br->wt.insert(path, {EntryKind::Tree, ZERO_HASH, 040755});
        fill_file_info(*d, *br->wt.lookup(path), fi);
        *p_ctx = ctx;
        return STATUS_SUCCESS;
    }

    Hash empty = d->store().write_blob(nullptr, 0);
    br->wt.insert(path, {EntryKind::Blob, empty, 0100644});
    auto state = std::make_unique<FhState>();
    state->path = path;
    state->branch_id = br->branch_id;
    d->refs().read_ref(br->name, state->pinned_commit);
    state->base_blob = empty;
    state->base_cache_loaded = true;
    state->write_buf = std::make_unique<WriteBuffer>(empty, 0);
    ctx->fh = d->allocate_fh(std::move(state));
    fill_file_info(*d, *br->wt.lookup(path), fi);
    *p_ctx = ctx;
    return STATUS_SUCCESS;
}

static NTSTATUS cb_write(FSP_FILE_SYSTEM* fs, PVOID file_context,
        PVOID buffer, UINT64 offset, ULONG length,
        BOOLEAN write_to_eof, BOOLEAN, PULONG p_xfer,
        FSP_FSCTL_FILE_INFO* fi) {
    auto* d = static_cast<Daemon*>(fs->UserContext);
    auto* ctx = static_cast<PathContext*>(file_context);
    if (!ctx || !ctx->fh) return STATUS_INVALID_HANDLE;
    auto s = d->get_fh(ctx->fh);
    if (!s) return STATUS_FILE_INVALID;
    auto br = branch_for_fh(d, s->branch_id);
    std::lock_guard<std::mutex> lk(br->checkpoint_mu);
    if (s->stale) return STATUS_FILE_INVALID;
    if (write_to_eof) offset = s->write_buf->effective_size(s->base_size);
    s->write_buf->write(offset, (const uint8_t*)buffer, length);
    *p_xfer = length;
    if (auto e = br->wt.lookup(s->path)) fill_file_info(*d, *e, fi);
    uint64_t sz = s->write_buf->effective_size(s->base_size);
    fi->FileSize = sz;
    fi->AllocationSize = (sz + 4095) & ~(uint64_t)4095;
    return STATUS_SUCCESS;
}

static NTSTATUS cb_overwrite(FSP_FILE_SYSTEM* fs, PVOID file_context,
        UINT32, BOOLEAN, UINT64, FSP_FSCTL_FILE_INFO* fi) {
    auto* d = static_cast<Daemon*>(fs->UserContext);
    auto* ctx = static_cast<PathContext*>(file_context);
    if (!ctx || !ctx->fh) return STATUS_INVALID_HANDLE;
    auto s = d->get_fh(ctx->fh);
    if (!s) return STATUS_FILE_INVALID;
    auto br = branch_for_fh(d, s->branch_id);
    std::lock_guard<std::mutex> lk(br->checkpoint_mu);
    if (s->stale) return STATUS_FILE_INVALID;
    Hash empty = d->store().write_blob(nullptr, 0);
    br->wt.insert(s->path, {EntryKind::Blob, empty, 0100644});
    s->base_blob = empty; s->base_size = 0;
    s->base_cache_loaded = true; s->base_cache.clear();
    s->write_buf = std::make_unique<WriteBuffer>(empty, 0);
    if (auto e = br->wt.lookup(s->path)) fill_file_info(*d, *e, fi);
    return STATUS_SUCCESS;
}

static NTSTATUS cb_flush(FSP_FILE_SYSTEM* fs, PVOID file_context,
                          FSP_FSCTL_FILE_INFO* fi) {
    auto* d = static_cast<Daemon*>(fs->UserContext);
    auto* ctx = static_cast<PathContext*>(file_context);
    if (!ctx || !ctx->fh) {
        if (fi) std::memset(fi, 0, sizeof(*fi));
        return STATUS_SUCCESS;
    }
    auto s = d->get_fh(ctx->fh);
    if (!s) return STATUS_FILE_INVALID;
    auto br = branch_for_fh(d, s->branch_id);
    std::lock_guard<std::mutex> lk(br->checkpoint_mu);
    if (s->stale) return STATUS_FILE_INVALID;
    if (s->write_buf->is_dirty()) {
        d->ensure_base_cache(*s);
        auto bytes = s->write_buf->materialize(s->base_cache);
        Hash h = d->store().write_blob(bytes);
        if (h == ZERO_HASH) return STATUS_UNEXPECTED_IO_ERROR;
        auto existing = br->wt.lookup(s->path);
        uint32_t mode = existing ? existing->mode : 0100644;
        br->wt.insert(s->path, {EntryKind::Blob, h, mode});
        s->base_blob = h; s->base_size = bytes.size();
        s->base_cache = std::move(bytes);
        s->base_cache_loaded = true;
        s->write_buf->clear();
    }
    if (auto e = br->wt.lookup(s->path)) fill_file_info(*d, *e, fi);
    return STATUS_SUCCESS;
}

static NTSTATUS cb_set_file_size(FSP_FILE_SYSTEM* fs, PVOID file_context,
        UINT64 new_size, BOOLEAN, FSP_FSCTL_FILE_INFO* fi) {
    auto* d = static_cast<Daemon*>(fs->UserContext);
    auto* ctx = static_cast<PathContext*>(file_context);
    if (!ctx || !ctx->fh) return STATUS_INVALID_HANDLE;
    auto s = d->get_fh(ctx->fh);
    if (!s) return STATUS_FILE_INVALID;
    auto br = branch_for_fh(d, s->branch_id);
    std::lock_guard<std::mutex> lk(br->checkpoint_mu);
    if (s->stale) return STATUS_FILE_INVALID;
    s->write_buf->truncate(new_size);
    if (auto e = br->wt.lookup(s->path)) fill_file_info(*d, *e, fi);
    fi->FileSize = new_size;
    fi->AllocationSize = (new_size + 4095) & ~(uint64_t)4095;
    return STATUS_SUCCESS;
}

static NTSTATUS cb_set_basic_info(FSP_FILE_SYSTEM* fs, PVOID file_context,
        UINT32, UINT64, UINT64, UINT64, UINT64, FSP_FSCTL_FILE_INFO* fi) {
    auto* d = static_cast<Daemon*>(fs->UserContext);
    auto* ctx = static_cast<PathContext*>(file_context);
    if (!ctx) return STATUS_INVALID_HANDLE;
    auto e = lookup_with_root(d->working_tree(), ctx->path);
    if (!e) return STATUS_OBJECT_NAME_NOT_FOUND;
    fill_file_info(*d, *e, fi);
    return STATUS_SUCCESS;
}

static NTSTATUS cb_rename(FSP_FILE_SYSTEM* fs, PVOID file_context,
        PWSTR, PWSTR new_file_name, BOOLEAN replace_if_exists) {
    auto* d = static_cast<Daemon*>(fs->UserContext);
    auto* ctx = static_cast<PathContext*>(file_context);
    if (!ctx) return STATUS_INVALID_HANDLE;
    std::string new_path = win::narrow(new_file_name);
    if (auto* bs = d->bootstrap()) {
        bs->ensure_path(ctx->path);
        bs->ensure_path(new_path);
    }
    auto br = d->main_branch();
    std::lock_guard<std::mutex> lk(br->checkpoint_mu);
    auto src = br->wt.lookup(ctx->path);
    if (!src) return STATUS_OBJECT_NAME_NOT_FOUND;
    if (!replace_if_exists && br->wt.lookup(new_path))
        return STATUS_OBJECT_NAME_COLLISION;
    if (src->kind == EntryKind::Tree)
        br->wt.rename_dir(ctx->path, new_path);
    else
        br->wt.rename_entry(ctx->path, new_path);
    ctx->path = new_path;
    return STATUS_SUCCESS;
}

static VOID cb_cleanup(FSP_FILE_SYSTEM* fs, PVOID file_context,
                       PWSTR, ULONG flags) {
    auto* d = static_cast<Daemon*>(fs->UserContext);
    auto* ctx = static_cast<PathContext*>(file_context);
    if (!ctx) return;
    auto br = d->main_branch();
    std::lock_guard<std::mutex> lk(br->checkpoint_mu);

    if (flags & FspCleanupDelete) {
        br->wt.remove(ctx->path);
        // Mark the still-open fh stale so a subsequent Write/Flush
        // can't re-insert the just-deleted blob and resurrect the
        // path. Without this, cb_flush's unconditional wt.insert
        // would recreate the file after a delete-on-close.
        if (ctx->fh) {
            if (auto s = d->get_fh(ctx->fh)) s->stale = true;
        }
        return;
    }

    // Normal close-of-modified-file path. WinFsp invokes Cleanup
    // when the last user handle is released; with
    // PostCleanupWhenModifiedOnly=1 (set in run_filesystem) this
    // fires only for files that were written. Mirror the FUSE
    // adapter's cas_release: materialize the per-fh WriteBuffer
    // into a blob and update WorkingTree. Without this, every
    // write made through this fh is dropped on close.
    if (!ctx->fh) return;
    auto s = d->get_fh(ctx->fh);
    if (!s || s->stale || !s->write_buf || !s->write_buf->is_dirty()) return;
    d->ensure_base_cache(*s);
    auto bytes = s->write_buf->materialize(s->base_cache);
    Hash h = d->store().write_blob(bytes);
    if (h == ZERO_HASH) return;
    auto existing = br->wt.lookup(s->path);
    uint32_t mode = existing ? existing->mode : 0100644;
    br->wt.insert(s->path, {EntryKind::Blob, h, mode});
    s->base_blob = h;
    s->base_size = bytes.size();
    s->base_cache = std::move(bytes);
    s->base_cache_loaded = true;
    s->write_buf->clear();
}

static FSP_FILE_SYSTEM_INTERFACE g_iface = {
    /*GetVolumeInfo     */ cb_get_volume_info,
    /*SetVolumeLabel    */ nullptr,
    /*GetSecurityByName */ cb_get_security_by_name,
    /*Create            */ cb_create,
    /*Open              */ cb_open,
    /*Overwrite         */ cb_overwrite,
    /*Cleanup           */ cb_cleanup,
    /*Close             */ cb_close,
    /*Read              */ cb_read,
    /*Write             */ cb_write,
    /*Flush             */ cb_flush,
    /*GetFileInfo       */ cb_get_file_info,
    /*SetBasicInfo      */ cb_set_basic_info,
    /*SetFileSize       */ cb_set_file_size,
    /*CanDelete         */ nullptr,
    /*Rename            */ cb_rename,
    /*GetSecurity       */ nullptr,
    /*SetSecurity       */ nullptr,
    /*ReadDirectory     */ cb_read_directory,
};

} // namespace

int run_filesystem(Daemon& daemon, const MountOptions& opts) {
    g_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_stop_event) return 5;
    SetConsoleCtrlHandler(ctrl_handler, TRUE);

    FSP_FSCTL_VOLUME_PARAMS vp{};
    vp.SectorSize = 4096;
    vp.SectorsPerAllocationUnit = 1;
    vp.MaxComponentLength = 255;
    vp.VolumeCreationTime = GetTickCount64();
    vp.VolumeSerialNumber = fnv_hash(daemon.store_root());
    vp.FileInfoTimeout = 1000;
    // Windows expects case-insensitive lookup but case-preserving display.
    // With CaseSensitiveSearch=1, the kernel sends paths in whatever case
    // the caller used (often uppercased by shells), which wouldn't match
    // our case-preserving WorkingTree keys.
    vp.CaseSensitiveSearch = 0;
    vp.CasePreservedNames = 1;
    vp.UnicodeOnDisk = 1;
    vp.PostCleanupWhenModifiedOnly = 1;
    wcsncpy_s(vp.FileSystemName, L"agentvfs", _TRUNCATE);

    FSP_FILE_SYSTEM* fs = nullptr;
    NTSTATUS s = FspFileSystemCreate(
        // FSP_FSCTL_DISK_DEVICE_NAME is ANSI ("WinFsp.Disk") but the API
        // wants PWSTR; adjacent string-literal concatenation with L""
        // promotes it to a wide literal at compile time.
        const_cast<PWSTR>(L"" FSP_FSCTL_DISK_DEVICE_NAME), &vp, &g_iface, &fs);
    if (!NT_SUCCESS(s)) {
        std::fprintf(stderr, "FspFileSystemCreate 0x%lx\n", (unsigned long)s);
        return 5;
    }
    fs->UserContext = &daemon;

    // Translate the real-Windows mountpoint without `widen()`'s
    // virtual-path mangling. The mountpoint is a drive letter or
    // absolute Windows path; `widen()` is for the virtual filesystem
    // paths that arrive through callbacks.
    std::wstring wmount;
    if (!opts.mountpoint.empty()) {
        int n = MultiByteToWideChar(CP_UTF8, 0, opts.mountpoint.data(),
                                    (int)opts.mountpoint.size(), nullptr, 0);
        if (n > 0) {
            wmount.assign((size_t)n, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, opts.mountpoint.data(),
                                (int)opts.mountpoint.size(), wmount.data(), n);
        }
    }
    s = FspFileSystemSetMountPoint(fs, wmount.data());
    if (!NT_SUCCESS(s)) {
        std::fprintf(stderr, "SetMountPoint('%ls') 0x%lx\n",
                     wmount.c_str(), (unsigned long)s);
        FspFileSystemDelete(fs); return 3;
    }
    s = FspFileSystemStartDispatcher(fs, 0);
    if (!NT_SUCCESS(s)) {
        std::fprintf(stderr, "StartDispatcher 0x%lx\n", (unsigned long)s);
        FspFileSystemRemoveMountPoint(fs);
        FspFileSystemDelete(fs); return 5;
    }
    std::fprintf(stderr, "agentvfs: mounted on %ls\n", wmount.c_str());
    WaitForSingleObject(g_stop_event, INFINITE);
    FspFileSystemStopDispatcher(fs);
    FspFileSystemRemoveMountPoint(fs);
    FspFileSystemDelete(fs);
    CloseHandle(g_stop_event); g_stop_event = nullptr;
    return 0;
}

} // namespace cas
