// HLSL to SPIR-V with DXC in process. The generator writes FXC-era HLSL (vector || and ?:), so the compiler runs with
// -HV 2018; the register shifts put every resource in set 0 as the backend's single push descriptor set expects.
#include "vulkan/vk_shader.h"

#ifdef _WIN32
// MinGW-w64's dxcapi.h (GUIDs without MS extensions; the COM interfaces are those of the DXC release's dxcompiler.dll,
// tools/fetch_windows_deps.sh); ATL's CComPtr is not there, a minimal one stands in
#include <windows.h>
#include <dxcapi.h>
template <class T>
struct CComPtr {
    T* p = nullptr;
    CComPtr() = default;
    CComPtr(const CComPtr&) = delete;
    CComPtr& operator=(const CComPtr&) = delete;
    ~CComPtr() {
        if (p) p->Release();
    }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};
#else
#include <dxc/dxcapi.h>
#endif

#include <cstring>
#include <mutex>
#include <unordered_map>

#include "core/log.h"
#include "gpu/shaders.h"

namespace vk_shader {

namespace {

struct Dxc {
    CComPtr<IDxcUtils> utils;
    CComPtr<IDxcCompiler3> compiler;
    bool ok = false;
    Dxc() {
        ok = SUCCEEDED(DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&utils))) &&
             SUCCEEDED(DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler)));
        if (!ok) LOGE("vulkan: DXC is not available (libdxcompiler)");
    }
};

std::mutex g_lock;
std::unordered_map<uint64_t, std::vector<uint32_t>> g_cache;

std::wstring Wide(const char* s) { return std::wstring(s, s + std::strlen(s)); }

}  // namespace

const std::vector<uint32_t>& Compile(const std::string& hlsl, Stage stage, const char* entry) {
    static Dxc dxc;
    static const std::vector<uint32_t> kEmpty;
    const char* profile = stage == Stage::Vertex ? "vs_6_0" : stage == Stage::Pixel ? "ps_6_0" : "gs_6_0";
    uint64_t key = gpu::Hash(hlsl.data(), hlsl.size(), gpu::Hash(profile, 6, gpu::Hash(entry, std::strlen(entry))));
    std::lock_guard<std::mutex> lock(g_lock);
    auto it = g_cache.find(key);
    if (it != g_cache.end()) return it->second;
    std::vector<uint32_t>& out = g_cache[key];
    if (!dxc.ok) return out;

    std::wstring wentry = Wide(entry), wprofile = Wide(profile);
    const wchar_t* args[] = {L"-E", wentry.c_str(), L"-T", wprofile.c_str(), L"-spirv", L"-HV", L"2018",
                             L"-fspv-target-env=vulkan1.3", L"-fvk-b-shift", L"0", L"0", L"-fvk-t-shift", L"2", L"0",
                             L"-fvk-s-shift", L"6", L"0", L"-O3"};
    DxcBuffer src{hlsl.data(), hlsl.size(), DXC_CP_UTF8};
    CComPtr<IDxcResult> result;
    HRESULT hr = dxc.compiler->Compile(&src, args, UINT32(std::size(args)), nullptr, IID_PPV_ARGS(&result));
    HRESULT status = E_FAIL;
    if (SUCCEEDED(hr)) result->GetStatus(&status);
    if (FAILED(hr) || FAILED(status)) {
        CComPtr<IDxcBlobUtf8> errors;
        if (result) result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);
        LOGE("vulkan: %s compile failed: %s", profile, errors ? errors->GetStringPointer() : "?");
        LOGE("%s", hlsl.c_str());
        return out;
    }
    CComPtr<IDxcBlob> spirv;
    result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&spirv), nullptr);
    const uint32_t* words = static_cast<const uint32_t*>(spirv->GetBufferPointer());
    out.assign(words, words + spirv->GetBufferSize() / 4);
    return out;
}

}  // namespace vk_shader
