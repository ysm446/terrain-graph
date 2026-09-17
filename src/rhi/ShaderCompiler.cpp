#include "rhi/ShaderCompiler.h"

#include "core/Log.h"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace tg::rhi {
namespace {

bool IsShaderFile(const std::filesystem::path& path) {
    const std::filesystem::path ext = path.extension();
    return ext == L".hlsl" || ext == L".hlsli";
}

// キャッシュの形式を変えたらここを上げる（古いファイルは使われなくなるだけで、消さなくてよい）。
constexpr uint64_t kCacheFormatVersion = 1;

uint64_t HashBytes(uint64_t hash, const void* data, size_t size) {
    // FNV-1a 64bit。
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

bool ReadWholeFile(const std::filesystem::path& path, std::string& out) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return false;
    out.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    return true;
}

// `#include "x.hlsli"` の行を拾う。インクルード先はファイルの隣と shaders/ 直下の両方を見る。
void CollectIncludes(const std::string& source, std::vector<std::string>& out) {
    size_t pos = 0;
    while (pos < source.size()) {
        size_t end = source.find('\n', pos);
        if (end == std::string::npos) end = source.size();
        const std::string_view line(source.data() + pos, end - pos);
        pos = end + 1;
        const size_t hash = line.find_first_not_of(" \t");
        if (hash == std::string_view::npos || line[hash] != '#') continue;
        const size_t keyword = line.find("include", hash + 1);
        if (keyword == std::string_view::npos) continue;
        const size_t open = line.find_first_of("\"<", keyword + 7);
        if (open == std::string_view::npos) continue;
        const size_t close = line.find_first_of("\">", open + 1);
        if (close == std::string_view::npos) continue;
        out.emplace_back(line.substr(open + 1, close - open - 1));
    }
}

}  // namespace

std::filesystem::path ShaderCompiler::CachePath(const std::filesystem::path& source,
                                                const wchar_t* entryPoint,
                                                const wchar_t* targetProfile,
                                                const std::vector<const wchar_t*>& args) const {
    if (m_cacheDirectory.empty()) return {};

    uint64_t hash = 14695981039346656037ull;
    hash = HashBytes(hash, &kCacheFormatVersion, sizeof(kCacheFormatVersion));
    const auto mix = [&hash](const std::wstring& text) {
        hash = HashBytes(hash, text.data(), text.size() * sizeof(wchar_t));
        hash = HashBytes(hash, "|", 1);
    };
    mix(entryPoint);
    mix(targetProfile);
    for (const wchar_t* arg : args) mix(arg);

    // ソースと、そこから辿れる #include 先をすべて内容ごとハッシュに混ぜる。
    // どれか 1 つでも変われば別のファイル名になり、古い結果は使われない。
    std::vector<std::filesystem::path> pending{source};
    std::unordered_set<std::wstring> visited;
    std::string content;
    while (!pending.empty()) {
        const std::filesystem::path path = std::move(pending.back());
        pending.pop_back();
        if (!visited.insert(path.lexically_normal().wstring()).second) continue;
        if (!ReadWholeFile(path, content)) return {};
        mix(path.lexically_normal().wstring());
        hash = HashBytes(hash, content.data(), content.size());
        std::vector<std::string> includes;
        CollectIncludes(content, includes);
        for (const std::string& include : includes) {
            // `<cmath>` のように実ファイルが無いものは DXC 側で処理される。飛ばしてよい。
            std::error_code ec;
            for (const std::filesystem::path candidate : {path.parent_path() / include, m_root / include}) {
                if (std::filesystem::is_regular_file(candidate, ec)) {
                    pending.push_back(candidate);
                    break;
                }
            }
        }
    }

    wchar_t name[64];
    std::swprintf(name, std::size(name), L"%016llx.dxil", static_cast<unsigned long long>(hash));
    return m_cacheDirectory / name;
}

ShaderCompiler::~ShaderCompiler() {
    Destroy();
}

bool ShaderCompiler::Create(const std::filesystem::path& shaderRoot,
                            const std::filesystem::path& cacheDirectory) {
    m_root = shaderRoot;
    m_cacheDirectory.clear();
    if (!cacheDirectory.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(cacheDirectory, ec);
        if (ec) {
            TG_LOG_WARN("シェーダキャッシュの保存先を作れません: %ls", cacheDirectory.c_str());
        } else {
            m_cacheDirectory = cacheDirectory;
        }
    }

    std::error_code ec;
    if (!std::filesystem::is_directory(m_root, ec)) {
        TG_LOG_ERROR("シェーダディレクトリが見つかりません: %ls", m_root.c_str());
        return false;
    }

    if (!TG_CHECK_HR(DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&m_utils)))) {
        return false;
    }
    if (!TG_CHECK_HR(DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&m_compiler)))) {
        return false;
    }
    if (!TG_CHECK_HR(m_utils->CreateDefaultIncludeHandler(&m_includeHandler))) {
        return false;
    }

    TG_LOG_INFO("シェーダディレクトリ: %ls", m_root.c_str());
    return true;
}

void ShaderCompiler::Destroy() {
    m_includeHandler.Reset();
    m_compiler.Reset();
    m_utils.Reset();
    m_timestamps.clear();
    m_scanned = false;
}

ComPtr<IDxcBlob> ShaderCompiler::Compile(const std::wstring& relativePath,
                                         const wchar_t* entryPoint,
                                         const wchar_t* targetProfile) {
    ComPtr<IDxcBlob> result;
    if (!m_compiler) {
        return result;
    }

    const std::filesystem::path fullPath = m_root / relativePath;
    const std::wstring fullPathStr = fullPath.wstring();

    const std::wstring includeArg = m_root.wstring();
    std::vector<const wchar_t*> extraArgs = {
        L"-I", includeArg.c_str(),
        L"-HV", L"2021",
        L"-enable-16bit-types",
    };
#if defined(TG_DEBUG)
    extraArgs.push_back(L"-Zi");
    extraArgs.push_back(L"-Qembed_debug");
    extraArgs.push_back(L"-Od");
#else
    extraArgs.push_back(L"-O3");
#endif

    // 前回のコンパイル結果がそのまま使えるなら DXC を通さない。起動時間の大半はここだった。
    const std::filesystem::path cachePath = CachePath(fullPath, entryPoint, targetProfile, extraArgs);
    if (!cachePath.empty()) {
        std::string cached;
        if (ReadWholeFile(cachePath, cached) && !cached.empty()) {
            ComPtr<IDxcBlobEncoding> blob;
            if (SUCCEEDED(m_utils->CreateBlob(cached.data(), static_cast<UINT32>(cached.size()),
                                              DXC_CP_ACP, &blob))) {
                blob.As(&result);
                return result;
            }
        }
    }

    ComPtr<IDxcBlobEncoding> sourceBlob;
    if (!TG_CHECK_HR(m_utils->LoadFile(fullPathStr.c_str(), nullptr, &sourceBlob))) {
        TG_LOG_ERROR("シェーダを読み込めません: %ls", fullPathStr.c_str());
        return result;
    }

    DxcBuffer source = {};
    source.Ptr = sourceBlob->GetBufferPointer();
    source.Size = sourceBlob->GetBufferSize();
    // シェーダソースは BOM 無し UTF-8（日本語コメントを含む）。
    // ACP のままだと CP932 環境で多バイト列が誤解釈される。
    source.Encoding = DXC_CP_UTF8;

    ComPtr<IDxcCompilerArgs> args;
    if (!TG_CHECK_HR(m_utils->BuildArguments(relativePath.c_str(), entryPoint, targetProfile,
                                             extraArgs.data(),
                                             static_cast<UINT32>(extraArgs.size()), nullptr, 0,
                                             &args))) {
        return result;
    }

    ComPtr<IDxcResult> compileResult;
    if (!TG_CHECK_HR(m_compiler->Compile(&source, args->GetArguments(), args->GetCount(),
                                         m_includeHandler.Get(),
                                         IID_PPV_ARGS(&compileResult)))) {
        return result;
    }

    ComPtr<IDxcBlobUtf8> errors;
    if (SUCCEEDED(compileResult->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr)) &&
        errors && errors->GetStringLength() > 0) {
        TG_LOG_WARN("%ls %ls:\n%s", relativePath.c_str(), entryPoint, errors->GetStringPointer());
    }

    HRESULT status = S_OK;
    compileResult->GetStatus(&status);
    if (FAILED(status)) {
        TG_LOG_ERROR("シェーダのコンパイルに失敗しました: %ls %ls", relativePath.c_str(),
                     entryPoint);
        return result;
    }

    if (!TG_CHECK_HR(compileResult->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&result), nullptr))) {
        return ComPtr<IDxcBlob>();
    }

    TG_LOG_INFO("シェーダをコンパイルしました: %ls %ls (%ls)", relativePath.c_str(), entryPoint,
                targetProfile);

    if (!cachePath.empty()) {
        // 途中で落ちても壊れたキャッシュが残らないよう、別名に書いてから差し替える。
        const std::filesystem::path temporary = cachePath.wstring() + L".tmp";
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (stream &&
            stream.write(static_cast<const char*>(result->GetBufferPointer()),
                         static_cast<std::streamsize>(result->GetBufferSize()))) {
            stream.close();
            std::error_code ec;
            std::filesystem::rename(temporary, cachePath, ec);
            if (ec) std::filesystem::remove(temporary, ec);
        }
    }
    return result;
}

void ShaderCompiler::ScanTimestamps(
    std::unordered_map<std::wstring, std::filesystem::file_time_type>& out) const {
    // 例外は使わない方針のため、イテレータの増分も error_code 版で手動で回す
    // （range-for の operator++ は I/O エラー時に例外を投げる）。
    std::error_code ec;
    std::filesystem::recursive_directory_iterator it(m_root, ec);
    const std::filesystem::recursive_directory_iterator end;
    while (!ec && it != end) {
        const std::filesystem::directory_entry& entry = *it;
        std::error_code fileEc;
        if (entry.is_regular_file(fileEc) && !fileEc && IsShaderFile(entry.path())) {
            std::error_code timeEc;
            const auto writeTime = std::filesystem::last_write_time(entry.path(), timeEc);
            if (!timeEc) {
                out.emplace(entry.path().wstring(), writeTime);
            }
        }
        it.increment(ec);
    }
}

bool ShaderCompiler::PollChanges() {
    std::unordered_map<std::wstring, std::filesystem::file_time_type> current;
    ScanTimestamps(current);

    if (!m_scanned) {
        m_timestamps = std::move(current);
        m_scanned = true;
        return false;
    }

    bool changed = current.size() != m_timestamps.size();
    if (!changed) {
        for (const auto& [path, time] : current) {
            const auto it = m_timestamps.find(path);
            if (it == m_timestamps.end() || it->second != time) {
                changed = true;
                break;
            }
        }
    }

    if (changed) {
        m_timestamps = std::move(current);
    }
    return changed;
}

}  // namespace tg::rhi
