#include "rhi/ShaderCompiler.h"

#include "core/Log.h"

#include <vector>

namespace tg::rhi {
namespace {

bool IsShaderFile(const std::filesystem::path& path) {
    const std::filesystem::path ext = path.extension();
    return ext == L".hlsl" || ext == L".hlsli";
}

}  // namespace

ShaderCompiler::~ShaderCompiler() {
    Destroy();
}

bool ShaderCompiler::Create(const std::filesystem::path& shaderRoot) {
    m_root = shaderRoot;

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
