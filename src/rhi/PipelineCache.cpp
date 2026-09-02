#include "rhi/PipelineCache.h"

#include "core/Log.h"
#include "rhi/ShaderCompiler.h"

namespace tg::rhi {
namespace {

D3D12_STATIC_SAMPLER_DESC MakeStaticSampler(UINT shaderRegister, D3D12_FILTER filter,
                                            D3D12_TEXTURE_ADDRESS_MODE addressU,
                                            D3D12_TEXTURE_ADDRESS_MODE addressV,
                                            UINT maxAnisotropy = 1) {
    D3D12_STATIC_SAMPLER_DESC desc = {};
    desc.Filter = filter;
    desc.AddressU = addressU;
    desc.AddressV = addressV;
    desc.AddressW = addressU;
    desc.MaxAnisotropy = maxAnisotropy;
    desc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    desc.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    desc.MinLOD = 0.0f;
    desc.MaxLOD = D3D12_FLOAT32_MAX;
    desc.ShaderRegister = shaderRegister;
    desc.RegisterSpace = 0;
    desc.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    return desc;
}

const D3D12_INPUT_ELEMENT_DESC kMeshStandardLayout[] = {
    {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT,
     D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT,
     D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"TANGENT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT,
     D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT,
     D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
};

}  // namespace

std::wstring GraphicsPipelineDesc::MakeKey() const {
    std::wstring key = shaderPath;
    key += L"#";
    key += vertexEntry;
    key += L"#";
    key += pixelEntry;
    key += L"#";
    key += hullEntry;
    key += L"#";
    key += domainEntry;
    key += L"#";
    key += std::to_wstring(static_cast<int>(rtvFormat));
    key += L"#";
    key += std::to_wstring(static_cast<int>(rtvFormat1));
    key += L"#";
    key += std::to_wstring(static_cast<int>(dsvFormat));
    key += L"#";
    key += std::to_wstring(static_cast<int>(layout));
    key += L"#";
    key += std::to_wstring(static_cast<int>(cullMode));
    key += L"#";
    key += std::to_wstring(static_cast<int>(fillMode));
    key += depthTest ? L"#t" : L"#f";
    key += depthWrite ? L"t" : L"f";
    key += lineTopology ? L"t" : L"f";
    key += alphaBlend ? L"t" : L"f";
    return key;
}

PipelineCache::~PipelineCache() {
    Destroy();
}

bool PipelineCache::Create(ID3D12Device* device, ShaderCompiler* compiler) {
    if (device == nullptr) {
        return false;
    }
    m_device = device;
    m_compiler = compiler;
    return CreateGlobalRootSignature();
}

void PipelineCache::Destroy() {
    m_computePipelines.clear();
    m_graphicsPipelines.clear();
    m_rootSignature.Reset();
    m_device = nullptr;
    m_compiler = nullptr;
}

bool PipelineCache::CreateGlobalRootSignature() {
    D3D12_ROOT_PARAMETER1 params[2] = {};

    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;
    params[0].Constants.RegisterSpace = 0;
    params[0].Constants.Num32BitValues = kRootConstantCount;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[1].Descriptor.ShaderRegister = 1;
    params[1].Descriptor.RegisterSpace = 0;
    params[1].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    const D3D12_STATIC_SAMPLER_DESC samplers[] = {
        MakeStaticSampler(0, D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                          D3D12_TEXTURE_ADDRESS_MODE_CLAMP),
        MakeStaticSampler(1, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                          D3D12_TEXTURE_ADDRESS_MODE_CLAMP),
        MakeStaticSampler(2, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_WRAP,
                          D3D12_TEXTURE_ADDRESS_MODE_WRAP),
        MakeStaticSampler(3, D3D12_FILTER_ANISOTROPIC, D3D12_TEXTURE_ADDRESS_MODE_WRAP,
                          D3D12_TEXTURE_ADDRESS_MODE_WRAP, 16),
        // equirectangular 用。経度方向はラップ、天頂方向はクランプ。
        MakeStaticSampler(4, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_WRAP,
                          D3D12_TEXTURE_ADDRESS_MODE_CLAMP),
        // タイルしない合成結果（平面 + UV スケール 1）用。
        MakeStaticSampler(5, D3D12_FILTER_ANISOTROPIC, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                          D3D12_TEXTURE_ADDRESS_MODE_CLAMP, 16),
    };

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc = {};
    desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    desc.Desc_1_1.NumParameters = _countof(params);
    desc.Desc_1_1.pParameters = params;
    desc.Desc_1_1.NumStaticSamplers = _countof(samplers);
    desc.Desc_1_1.pStaticSamplers = samplers;
    // 入力レイアウトを使うグラフィックス PSO には
    // ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT が必須。無いと PSO 作成が E_INVALIDARG で落ちる。
    // **SAMPLER_HEAP_DIRECTLY_INDEXED は付けない。**
    // サンプラは上の静的サンプラ（s0..s4）で足りており、
    // シェーダは SamplerDescriptorHeap[] を使っていない。
    // 付けるとサンプラヒープの SetDescriptorHeaps が必須になり、
    // ルートシグネチャを設定するたびに検証エラーが出る。
    desc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
                          D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;

    ComPtr<ID3DBlob> blob;
    ComPtr<ID3DBlob> errorBlob;
    const HRESULT hr = D3D12SerializeVersionedRootSignature(&desc, &blob, &errorBlob);
    if (FAILED(hr)) {
        if (errorBlob) {
            TG_LOG_ERROR("ルートシグネチャのシリアライズに失敗: %s",
                         static_cast<const char*>(errorBlob->GetBufferPointer()));
        }
        return false;
    }

    if (!TG_CHECK_HR(m_device->CreateRootSignature(0, blob->GetBufferPointer(),
                                                   blob->GetBufferSize(),
                                                   IID_PPV_ARGS(&m_rootSignature)))) {
        return false;
    }
    m_rootSignature->SetName(L"GlobalRootSignature");
    return true;
}

ID3D12PipelineState* PipelineCache::GetCompute(const std::wstring& relativePath,
                                               const std::wstring& entryPoint) {
    const std::wstring key = relativePath + L"#" + entryPoint;
    if (const auto it = m_computePipelines.find(key); it != m_computePipelines.end()) {
        return it->second.Get();
    }

    if (m_compiler == nullptr || !m_rootSignature) {
        return nullptr;
    }

    // 失敗した組み合わせも記録する。そうしないと毎フレーム再コンパイルしてしまう。
    // ホットリロード時は InvalidateAll() でまとめて捨てるため、再挑戦はそこで行われる。
    ComPtr<IDxcBlob> bytecode = m_compiler->Compile(relativePath, entryPoint.c_str(), L"cs_6_6");
    if (!bytecode) {
        m_computePipelines.emplace(key, nullptr);
        return nullptr;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
    desc.pRootSignature = m_rootSignature.Get();
    desc.CS.pShaderBytecode = bytecode->GetBufferPointer();
    desc.CS.BytecodeLength = bytecode->GetBufferSize();

    ComPtr<ID3D12PipelineState> pipeline;
    if (!TG_CHECK_HR(m_device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pipeline)))) {
        m_computePipelines.emplace(key, nullptr);
        return nullptr;
    }
    pipeline->SetName(key.c_str());

    const auto [it, insertedNew] = m_computePipelines.emplace(key, std::move(pipeline));
    (void)insertedNew;
    return it->second.Get();
}

ID3D12PipelineState* PipelineCache::GetGraphics(const GraphicsPipelineDesc& desc) {
    const std::wstring key = desc.MakeKey();
    if (const auto it = m_graphicsPipelines.find(key); it != m_graphicsPipelines.end()) {
        return it->second.Get();
    }

    if (m_compiler == nullptr || !m_rootSignature) {
        return nullptr;
    }

    // コンピュートと同じく、失敗も記録して毎フレームの再コンパイルを防ぐ。
    ComPtr<IDxcBlob> vertexBlob =
        m_compiler->Compile(desc.shaderPath, desc.vertexEntry.c_str(), L"vs_6_6");
    if (!vertexBlob) {
        m_graphicsPipelines.emplace(key, nullptr);
        return nullptr;
    }
    // ピクセルシェーダは省略できる。深度だけを描くパスで使う。
    ComPtr<IDxcBlob> pixelBlob;
    if (!desc.pixelEntry.empty()) {
        pixelBlob = m_compiler->Compile(desc.shaderPath, desc.pixelEntry.c_str(), L"ps_6_6");
        if (!pixelBlob) {
            m_graphicsPipelines.emplace(key, nullptr);
            return nullptr;
        }
    }

    // テセレーションはハルとドメインの両方がそろっているときだけ使う。
    const bool useTessellation = !desc.hullEntry.empty() && !desc.domainEntry.empty();
    ComPtr<IDxcBlob> hullBlob;
    ComPtr<IDxcBlob> domainBlob;
    if (useTessellation) {
        hullBlob = m_compiler->Compile(desc.shaderPath, desc.hullEntry.c_str(), L"hs_6_6");
        domainBlob = m_compiler->Compile(desc.shaderPath, desc.domainEntry.c_str(), L"ds_6_6");
        if (!hullBlob || !domainBlob) {
            m_graphicsPipelines.emplace(key, nullptr);
            return nullptr;
        }
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = m_rootSignature.Get();
    psoDesc.VS.pShaderBytecode = vertexBlob->GetBufferPointer();
    psoDesc.VS.BytecodeLength = vertexBlob->GetBufferSize();
    if (pixelBlob) {
        psoDesc.PS.pShaderBytecode = pixelBlob->GetBufferPointer();
        psoDesc.PS.BytecodeLength = pixelBlob->GetBufferSize();
    }
    if (useTessellation) {
        psoDesc.HS.pShaderBytecode = hullBlob->GetBufferPointer();
        psoDesc.HS.BytecodeLength = hullBlob->GetBufferSize();
        psoDesc.DS.pShaderBytecode = domainBlob->GetBufferPointer();
        psoDesc.DS.BytecodeLength = domainBlob->GetBufferSize();
    }

    psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    if (desc.alphaBlend) {
        D3D12_RENDER_TARGET_BLEND_DESC& blend = psoDesc.BlendState.RenderTarget[0];
        blend.BlendEnable = TRUE;
        blend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
        blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        blend.BlendOp = D3D12_BLEND_OP_ADD;
        blend.SrcBlendAlpha = D3D12_BLEND_ONE;
        blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    }
    psoDesc.SampleMask = UINT_MAX;

    psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    psoDesc.RasterizerState.CullMode = desc.cullMode;
    psoDesc.RasterizerState.FillMode = desc.fillMode;
    // 右手系なので、外向きの面は画面上で反時計回りに見える。
    // 詳細は docs/design/rendering.md の「座標系」を参照。
    psoDesc.RasterizerState.FrontCounterClockwise = TRUE;

    psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    psoDesc.DepthStencilState.DepthEnable = desc.depthTest ? TRUE : FALSE;
    psoDesc.DepthStencilState.DepthWriteMask =
        desc.depthWrite ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    psoDesc.DepthStencilState.StencilEnable = FALSE;

    if (desc.layout == VertexLayout::MeshStandard) {
        psoDesc.InputLayout.pInputElementDescs = kMeshStandardLayout;
        psoDesc.InputLayout.NumElements = _countof(kMeshStandardLayout);
    }

    // テセレーションを使うときは、IA へパッチとして渡す。
    psoDesc.PrimitiveTopologyType = useTessellation ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH
                                    : desc.lineTopology
                                        ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE
                                        : D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.RTVFormats[0] = desc.rtvFormat;
    if (desc.rtvFormat1 != DXGI_FORMAT_UNKNOWN) {
        // RTV1 を使うなら RTV0 も必須。歯抜けの MRT は E_INVALIDARG になる。
        if (desc.rtvFormat == DXGI_FORMAT_UNKNOWN) {
            TG_LOG_ERROR("グラフィックス PSO: RTV0 が UNKNOWN のまま RTV1 は指定できません");
            m_graphicsPipelines.emplace(key, nullptr);
            return nullptr;
        }
        psoDesc.NumRenderTargets = 2;
        psoDesc.RTVFormats[1] = desc.rtvFormat1;
    } else {
        psoDesc.NumRenderTargets = (desc.rtvFormat != DXGI_FORMAT_UNKNOWN) ? 1 : 0;
    }
    psoDesc.DSVFormat = desc.dsvFormat;
    psoDesc.SampleDesc.Count = 1;

    ComPtr<ID3D12PipelineState> pipeline;
    if (!TG_CHECK_HR(m_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pipeline)))) {
        m_graphicsPipelines.emplace(key, nullptr);
        return nullptr;
    }
    pipeline->SetName(key.c_str());

    const auto [it, insertedNew] = m_graphicsPipelines.emplace(key, std::move(pipeline));
    (void)insertedNew;
    return it->second.Get();
}

size_t PipelineCache::PipelineCount() const {
    size_t count = 0;
    for (const auto& [key, pipeline] : m_computePipelines) {
        if (pipeline) {
            ++count;
        }
    }
    for (const auto& [key, pipeline] : m_graphicsPipelines) {
        if (pipeline) {
            ++count;
        }
    }
    return count;
}

void PipelineCache::InvalidateAll() {
    m_computePipelines.clear();
    m_graphicsPipelines.clear();
}

}  // namespace tg::rhi
