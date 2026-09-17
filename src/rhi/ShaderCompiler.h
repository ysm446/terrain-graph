#pragma once

#include "rhi/Common.h"

#include <directx-dxc/dxcapi.h>

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace tg::rhi {

// DXC を使った HLSL のランタイムコンパイラ。
// シェーダはソースツリーの shaders/ を直接参照し、更新を検出して再コンパイルできる。
class ShaderCompiler {
public:
    ShaderCompiler() = default;
    ~ShaderCompiler();

    ShaderCompiler(const ShaderCompiler&) = delete;
    ShaderCompiler& operator=(const ShaderCompiler&) = delete;

    // cacheDirectory を渡すと、コンパイル済み DXIL をそこへ置いて次回以降の起動で再利用する。
    // 空なら毎回コンパイルする。
    bool Create(const std::filesystem::path& shaderRoot,
                const std::filesystem::path& cacheDirectory = {});
    void Destroy();

    // shaderRoot からの相対パスを指定する。失敗時は nullptr を返し、エラーはログへ出す。
    ComPtr<IDxcBlob> Compile(const std::wstring& relativePath, const wchar_t* entryPoint,
                             const wchar_t* targetProfile);

    // 監視対象のシェーダファイルに更新があれば true を返す。初回呼び出しは常に false。
    bool PollChanges();

    const std::filesystem::path& Root() const { return m_root; }

private:
    void ScanTimestamps(std::unordered_map<std::wstring, std::filesystem::file_time_type>& out) const;
    // ソースと #include 先の内容、コンパイル引数から決まるキャッシュファイルの場所。
    // 読めないファイルがあれば空を返す（そのときはキャッシュを使わない）。
    std::filesystem::path CachePath(const std::filesystem::path& source, const wchar_t* entryPoint,
                                    const wchar_t* targetProfile,
                                    const std::vector<const wchar_t*>& args) const;

    ComPtr<IDxcUtils> m_utils;
    ComPtr<IDxcCompiler3> m_compiler;
    ComPtr<IDxcIncludeHandler> m_includeHandler;
    std::filesystem::path m_root;
    std::filesystem::path m_cacheDirectory;
    std::unordered_map<std::wstring, std::filesystem::file_time_type> m_timestamps;
    bool m_scanned = false;
};

}  // namespace tg::rhi
