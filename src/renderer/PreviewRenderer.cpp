#include "renderer/PreviewRenderer.h"

#include "core/ImageIo.h"
#include "core/Log.h"

#include <pix3.h>

#include <cmath>
#include <cstring>

using namespace DirectX;

namespace tg::renderer {
namespace {

constexpr DXGI_FORMAT kSceneColorFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_D32_FLOAT;
constexpr DXGI_FORMAT kOutputFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

// ガイド線の端点の最大数。シェーダの TG_OVERLAY_MAX_VERTICES と一致させること。
constexpr uint32_t kOverlayLineMaxVertices = 128;

// GPU 側の OverlayLineConstants と一致させること。
struct OverlayLineConstants {
    DirectX::XMFLOAT4X4 viewProjection;
    float color[4];
    // xyz: ワールド座標、w: 端点ごとの不透明度。
    DirectX::XMFLOAT4 positions[kOverlayLineMaxVertices];
};
// マテリアル UV バッファ。UV はタイル 1 枚ぶんに畳んであるため半精度で足りる
// （1.0 付近でも刻みは 2^-11 で、2K のペイントマスクの 1 テクセルに収まる）。
constexpr DXGI_FORMAT kMaterialUvFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

// シャドウマップの解像度。プレビューの被写体 1 個ぶんなのでこれで足りる。
constexpr uint32_t kShadowMapSize = 2048;

// 背景をぼかすときに引くプリフィルタ済みキューブのミップ。小数で指定する。
//
// **ミップ m はラフネス m / (段数 - 1) に対応する**（プリフィルタは 6 段）。
// 1.6 でおよそラフネス 0.32。空と地面の分かれ目や光の向きは残るが、
// 木立や建物の形は溶けて目に留まらなくなる、という強さ。
// 素材を見比べるときに背景が目移りの原因にならないことを優先している。
constexpr float kSkyboxBlurMip = 1.6f;
constexpr DXGI_FORMAT kShadowDsvFormat = DXGI_FORMAT_D32_FLOAT;
// プレビューのメッシュの大きさ。どれも原点中心（モデル行列は単位行列）。
// BoundingRadius() がここから包む球の半径を出すので、値を直接書かないこと。
constexpr float kSphereRadius = 1.0f;
// 平面の一辺は PreviewRenderer::kPlaneSize（ヘッダ。オーバーレイも参照する）。
constexpr float kCubeSize = 1.4f;   // 一辺の長さ

// 影を落とす範囲。**被写体を包む球の半径に対する倍率**で持つ。
// 素材の 2m 角と地形の 2km 角では 1000 倍違うので、m の固定値では片方でしか使えない
// （地形では 4.4m 角の平行投影から完全にはみ出して影が消える）。
// 係数は、従来の固定値（半径 1.41 のときに 2.2m / 6.0m）と一致するよう選んである。
// **基準より小さい被写体では従来の固定値のまま**にする（下限で止める）。
// 狭めると影の解像度は上がるが、既存のプロジェクトの見え方が変わってしまう。
constexpr float kShadowRadiusMin = 2.2f;
constexpr float kShadowDistanceMin = 6.0f;
constexpr float kShadowRadiusRatio = 2.2f / 1.41421356f;
constexpr float kShadowDistanceRatio = 6.0f / 1.41421356f;
// 自己遮蔽（シャドウアクネ）を避けるための下駄。傾きに応じてシェーダ側で増やす。
constexpr float kShadowBias = 0.0018f;
// シェーダの「影を落とさない」印。
constexpr uint32_t kNoShadowIndex = 0xFFFFFFFFu;

// GPU 側の MeshConstants と一致させること。
struct MeshConstants {
    XMFLOAT4X4 viewProjection;
    XMFLOAT4X4 model;
    XMFLOAT4X4 normalMatrix;

    XMFLOAT3 cameraPosition;
    float pad0;

    XMFLOAT3 lightDirection;
    float lightIlluminance;

    XMFLOAT3 lightColor;
    float pad1;

    XMFLOAT3 baseColor;
    float roughness;

    float metallic;
    float iblIntensity;
    uint32_t prefilteredMipCount;
    float pad2;

    uint32_t irradianceIndex;
    uint32_t prefilteredIndex;
    uint32_t brdfLutIndex;
    uint32_t useMaterialTextures;

    uint32_t materialBaseColorIndex;
    uint32_t materialNormalIndex;
    uint32_t materialSurfaceIndex;
    uint32_t materialHeightIndex;

    float materialUvScale;
    uint32_t debugView;
    float displacementScale;
    // 合成結果をクランプで読むか（平面 + UV スケール 1 のとき 1）。
    uint32_t clampMaterialUv;

    XMFLOAT4X4 lightViewProjection;

    uint32_t shadowIndex;  // 影を落とさないときは kNoShadowIndex
    float shadowTexelSize;
    float shadowBias;
    float pad5;

    XMFLOAT4X4 tessellationViewProjection;
    float viewportSize[2];
    float tessellationMaxFactor;
    float pad6;
};

// GPU 側の SkyboxConstants と一致させること。
struct SkyboxConstants {
    XMFLOAT4X4 inverseViewProjection;

    XMFLOAT3 cameraPosition;
    float intensity;

    uint32_t environmentIndex;
    float mipLevel;
    float pad0[2];
};

// GPU 側の DofConstants と一致させること。
struct DofConstants {
    uint32_t sourceIndex;
    uint32_t depthIndex;
    uint32_t outputIndex;
    uint32_t width;

    uint32_t height;
    float focalLengthMm;
    float fStop;
    float focusDistance;

    float nearZ;
    float farZ;
    float maxBlurPixels;
    float apertureRotation;

    float apertureBlades;
    float blurScale;
    float pad0[2];
};

// メッシュ 1 回ぶんの描画を数える。
void CountMeshDraw(RenderStats& stats, const Mesh& mesh, bool asPatches) {
    if (!mesh.IsValid()) {
        return;
    }
    ++stats.drawCalls;
    stats.vertices += mesh.IndexCount();
    if (asPatches) {
        // 3 制御点のパッチとして投入する。実際の三角形はドメインシェーダが決める。
        stats.patches += mesh.IndexCount() / 3;
    } else {
        stats.triangles += mesh.IndexCount() / 3;
    }
}

// 絞りの形から羽根の数へ。0 なら円。
float ApertureBladeCount(ApertureShape shape) {
    switch (shape) {
        case ApertureShape::Triangle: return 3.0f;
        case ApertureShape::Hexagon: return 6.0f;
        case ApertureShape::Octagon: return 8.0f;
        default: return 0.0f;
    }
}

struct TonemapConstants {
    uint32_t sourceIndex;
    uint32_t outputIndex;
    uint32_t width;
    uint32_t height;
    float exposure;
    uint32_t tonemapMode;
    uint32_t debugView;
};

}  // namespace

float ExposureSettings::Ev100() const {
    if (useManualEv) {
        return manualEv100;
    }
    const float safeAperture = (aperture > 0.0f) ? aperture : 1.0f;
    const float safeShutter = (shutterSpeed > 0.0f) ? shutterSpeed : 1.0f;
    const float safeIso = (iso > 0.0f) ? iso : 100.0f;
    return std::log2((safeAperture * safeAperture) / safeShutter) - std::log2(safeIso / 100.0f);
}

float ExposureSettings::Exposure() const {
    return 1.0f / (1.2f * std::pow(2.0f, Ev100()));
}

XMFLOAT3 LightSettings::Direction() const {
    const float cosElevation = std::cos(elevation);
    return XMFLOAT3{cosElevation * std::sin(azimuth), std::sin(elevation),
                    cosElevation * std::cos(azimuth)};
}

bool PreviewRenderer::Initialize(rhi::Device& device, rhi::PipelineCache& pipelineCache) {
    // ディスプレイスメントを頂点で押し出すので、プレビューのメッシュは細かく割る。
    // 数万頂点はプレビュー 1 個ぶんとしては軽い。
    if (!m_sphere.Create(device, MakeSphere(256, 128, kSphereRadius), L"SphereMesh")) {
        return false;
    }
    if (!m_plane.Create(device, MakePlane(kPlaneMeshSize, 256), L"PlaneMesh")) {
        return false;
    }
    if (!m_cube.Create(device, MakeCube(kCubeSize, 96), L"CubeMesh")) {
        return false;
    }
    if (!m_environment.Initialize(device, pipelineCache)) {
        return false;
    }
    if (!m_evaluator.Create(device, m_materialResolution)) {
        return false;
    }

    // シャドウマップ。深度として書き、SRV としても読むので TYPELESS で作る。
    rhi::TextureDesc shadowDesc;
    shadowDesc.width = kShadowMapSize;
    shadowDesc.height = kShadowMapSize;
    shadowDesc.format = DXGI_FORMAT_R32_TYPELESS;
    shadowDesc.dsvFormat = kShadowDsvFormat;
    shadowDesc.srvFormat = DXGI_FORMAT_R32_FLOAT;
    shadowDesc.allowDepthStencil = true;
    shadowDesc.createSrv = true;
    shadowDesc.clearDepth = 1.0f;
    shadowDesc.initialState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    shadowDesc.debugName = L"ShadowMap";
    if (!device.Allocator().CreateTexture2D(shadowDesc, m_shadowMap)) {
        return false;
    }
    return true;
}

// ライトから見たビュー×投影。プレビューの被写体を囲む平行投影で足りる。
XMMATRIX PreviewRenderer::LightViewProjection() const {
    // 影の範囲は被写体の大きさに追従させる（平面のサイズと変位量で変わる）。
    const float radius = BoundingRadius();
    const float shadowRadius = std::max(kShadowRadiusMin, radius * kShadowRadiusRatio);
    const float shadowDistance = std::max(kShadowDistanceMin, radius * kShadowDistanceRatio);

    const XMFLOAT3 direction = m_light.Direction();  // サーフェスから光源へ
    const XMVECTOR lightDirection = XMVector3Normalize(XMLoadFloat3(&direction));
    const XMVECTOR eye = XMVectorScale(lightDirection, shadowDistance);
    // 真上・真下からのときに上方向が縮退しないよう、軸を入れ替える。
    const XMVECTOR up = (std::abs(direction.y) > 0.99f) ? XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f)
                                                        : XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    const XMMATRIX view = XMMatrixLookAtRH(eye, XMVectorZero(), up);
    // 手前のクリップも比例させる。固定の 0.05m だと地形スケールで精度を使い切る。
    const XMMATRIX projection = XMMatrixOrthographicRH(
        shadowRadius * 2.0f, shadowRadius * 2.0f,
        std::max(0.05f, radius * 0.035f), shadowDistance + shadowRadius);
    return XMMatrixMultiply(view, projection);
}

void PreviewRenderer::Shutdown(rhi::Device& device) {
    device.DeferRelease(m_shadowMap);
    m_evaluator.Destroy(device);
    m_environment.Shutdown(device);
    m_sphere.Release(device);
    m_plane.Release(device);
    m_cube.Release(device);
    ReleaseTargets(device);
}

void PreviewRenderer::ProcessPendingWork(rhi::Device& device,
                                        rhi::PipelineCache& pipelineCache) {
    if (m_requestedMaterialResolution != m_materialResolution) {
        if (m_evaluator.Resize(device, m_requestedMaterialResolution)) {
            m_materialResolution = m_requestedMaterialResolution;
        } else {
            m_requestedMaterialResolution = m_materialResolution;
        }
    }

    if (m_skyRebuildRequested) {
        m_skyRebuildRequested = false;
        m_skyLuminanceRebuildRequested = false;
        ApplyActiveSky(device, pipelineCache);
        return;
    }

    // 較正倍率だけの変更。equirect は読み込んだままのものを使うので速い。
    if (m_skyLuminanceRebuildRequested) {
        m_skyLuminanceRebuildRequested = false;
        if (!m_loadedHdriPath.empty()) {
            m_environment.RebuildWithSkyLuminance(device, pipelineCache,
                                                  m_activeSky.skyLuminance);
        }
    }
}

void PreviewRenderer::ResetSettings() {
    const PreviewDefaults& defaults = kPreviewDefaults;
    m_shape = defaults.shape;
    m_tonemap = defaults.tonemap;
    m_useMaterialTextures = defaults.useMaterialTextures;
    m_materialUvScale = defaults.materialUvScale;
    m_displacementScale = defaults.displacementScale;
    m_planeSize = defaults.planeSize;
    m_tessellationEnabled = defaults.tessellationEnabled;
    m_tessellationFactor = defaults.tessellationFactor;
    m_showSkybox = defaults.showSkybox;
    m_skyboxBlur = defaults.skyboxBlur;
    m_shadowEnabled = defaults.shadowEnabled;
    // 解像度の作り直しは GPU 待機を伴うので、要求だけ積む。
    RequestMaterialResolution(defaults.materialResolution);

    // 各節の既定値は構造体の初期値。数値を直接書かない。
    m_camera.SetState(CameraState{});
    m_light = LightSettings{};
    m_exposure = ExposureSettings{};
    m_dof = DofSettings{};
    m_material = MaterialSettings{};

    // 表示モードはプロジェクトに保存しないが、ここでは戻す。
    // ハイトやラフネスを覗いたまま「新規」を押すと、
    // 真っ白な球が出て「何も描かれていない」ように見えるため。
    m_debugView = DebugView::Shaded;
}

void PreviewRenderer::SetActiveSky(const SkyDefinition& sky) {
    if (NeedsEnvironmentRebuild(m_activeSky, sky)) {
        m_skyRebuildRequested = true;
    } else if (NeedsLuminanceRebuild(m_activeSky, sky)) {
        m_skyLuminanceRebuildRequested = true;
    }
    // IBL の倍率は毎フレームそのまま使うので、作り直しは要らない。
    m_activeSky = sky;
}

void PreviewRenderer::ApplyActiveSky(rhi::Device& device, rhi::PipelineCache& pipelineCache) {
    if (m_activeSky.source == SkySource::Hdri && !m_activeSky.hdriPath.empty()) {
        if (m_environment.BuildFromHdrFile(device, pipelineCache, m_activeSky.hdriPath,
                                           m_activeSky.skyLuminance)) {
            m_loadedHdriPath = m_activeSky.hdriPath;
            return;
        }
        // 読み込みに失敗しても、天球アセットの中身は書き換えない（ユーザーの
        // 指定を黙って消さない）。環境だけを手続き的な空へ落とす。
        TG_LOG_WARN("HDRI の読み込みに失敗したため、手続き的な空で描きます");
    }
    m_environment.BuildFromSky(device, pipelineCache, m_activeSky.procedural);
    m_loadedHdriPath.clear();
}

float PreviewRenderer::FocusDistance() const {
    // 軌道カメラなので、注視点までの距離がそのまま「見ているものまでの距離」。
    return m_dof.focusOnTarget ? m_camera.State().distance : m_dof.focusDistance;
}

// 現在のメッシュを包む球の半径。カメラの Frame()（A キー）が使う。
//
// ディスプレイスメントは頂点を法線方向へ (height - 0.5) * scale だけ動かすので、
// 外へ出る最大量 scale * 0.5 を足す。変位量を上げたときにはみ出さないようにするため。
float PreviewRenderer::BoundingRadius() const {
    float radius = kSphereRadius;
    switch (m_shape) {
        // 平面は XZ に広がるので、対角の半分が包む球の半径になる。
        case PreviewShape::Plane: radius = m_planeSize * 0.5f * 1.41421356f; break;
        case PreviewShape::Cube:  radius = kCubeSize * 0.5f * 1.73205081f; break;
        case PreviewShape::Sphere:
        default:                  radius = kSphereRadius; break;
    }

    if (m_useMaterialTextures) {
        radius += m_displacementScale * 0.5f;
    }
    return radius;
}

const Mesh& PreviewRenderer::CurrentMesh() const {
    switch (m_shape) {
        case PreviewShape::Plane: return m_plane;
        case PreviewShape::Cube:  return m_cube;
        case PreviewShape::Sphere:
        default:                  return m_sphere;
    }
}

void PreviewRenderer::ReleaseTargets(rhi::Device& device) {
    rhi::GpuTexture* targets[] = {&m_sceneColor, &m_sceneColorDof, &m_materialUv, &m_depth,
                                  &m_output};
    for (rhi::GpuTexture* target : targets) {
        if (!target->IsValid()) {
            continue;
        }
        // ディスクリプタも含めてフレーム同期後に解放する。
        device.DeferRelease(*target);
    }
    m_width = 0;
    m_height = 0;
}

bool PreviewRenderer::SaveOutputToPng(rhi::Device& device, const std::filesystem::path& path) {
    if (!m_output.IsValid()) {
        return false;
    }

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT rowCount = 0;
    UINT64 rowSizeInBytes = 0;
    UINT64 totalBytes = 0;
    const D3D12_RESOURCE_DESC resourceDesc = m_output.resource->GetDesc();
    device.GetDevice()->GetCopyableFootprints(&resourceDesc, 0, 1, 0, &footprint, &rowCount,
                                              &rowSizeInBytes, &totalBytes);

    rhi::GpuBuffer readback;
    if (!device.Allocator().CreateReadbackBuffer(totalBytes, L"PreviewReadback", readback)) {
        return false;
    }

    const D3D12_RESOURCE_STATES previousState = m_output.state;
    const bool executed = device.ExecuteImmediate([&](ID3D12GraphicsCommandList* commandList) {
        TransitionIfNeeded(commandList, m_output, D3D12_RESOURCE_STATE_COPY_SOURCE);

        const CD3DX12_TEXTURE_COPY_LOCATION destination(readback.resource.Get(), footprint);
        const CD3DX12_TEXTURE_COPY_LOCATION source(m_output.resource.Get(), 0);
        commandList->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);

        TransitionIfNeeded(commandList, m_output, previousState);
    });
    if (!executed) {
        device.DeferRelease(readback);
        return false;
    }

    void* mapped = nullptr;
    const D3D12_RANGE readRange = {0, static_cast<SIZE_T>(totalBytes)};
    if (!TG_CHECK_HR(readback.resource->Map(0, &readRange, &mapped))) {
        device.DeferRelease(readback);
        return false;
    }

    const bool saved =
        SaveRgba8Png(path, m_width, m_height, footprint.Footprint.RowPitch,
                     static_cast<const uint8_t*>(mapped) + footprint.Offset);

    const D3D12_RANGE writtenRange = {0, 0};
    readback.resource->Unmap(0, &writtenRange);

    device.Defer(readback.resource);
    device.Defer(readback.allocation);
    return saved;
}

bool PreviewRenderer::Resize(rhi::Device& device, uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) {
        return false;
    }
    if (width == m_width && height == m_height) {
        return true;
    }

    // 作り直す前に、GPU がまだ参照しているターゲットを解放できる状態にする。
    device.WaitForGpu();
    ReleaseTargets(device);

    rhi::TextureDesc colorDesc;
    colorDesc.width = width;
    colorDesc.height = height;
    colorDesc.format = kSceneColorFormat;
    colorDesc.allowRenderTarget = true;
    colorDesc.createSrv = true;
    colorDesc.initialState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    colorDesc.debugName = L"SceneColor";
    if (!device.Allocator().CreateTexture2D(colorDesc, m_sceneColor)) {
        return false;
    }

    // 被写界深度の出力。シーンカラーと同じ形式で、コンピュートから書く。
    rhi::TextureDesc dofDesc;
    dofDesc.width = width;
    dofDesc.height = height;
    dofDesc.format = kSceneColorFormat;
    dofDesc.allowUnorderedAccess = true;
    dofDesc.createSrv = true;
    dofDesc.initialState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    dofDesc.debugName = L"SceneColorDof";
    if (!device.Allocator().CreateTexture2D(dofDesc, m_sceneColorDof)) {
        return false;
    }

    rhi::TextureDesc materialUvDesc;
    materialUvDesc.width = width;
    materialUvDesc.height = height;
    materialUvDesc.format = kMaterialUvFormat;
    materialUvDesc.allowRenderTarget = true;
    materialUvDesc.createSrv = true;
    // クリア値は実際のクリアと揃える。ずれるとデバッグレイヤーが警告する。
    materialUvDesc.clearColor[3] = 0.0f;
    materialUvDesc.initialState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    materialUvDesc.debugName = L"MaterialUv";
    if (!device.Allocator().CreateTexture2D(materialUvDesc, m_materialUv)) {
        return false;
    }

    // リサイズ直後の最初のフレームには「前フレームの UV」がまだ無い。
    // 未初期化のままブラシが読むと、被覆フラグのゴミで意図しない位置に描いてしまう。
    const float uvClearColor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    device.ExecuteImmediate([&](ID3D12GraphicsCommandList* commandList) {
        commandList->ClearRenderTargetView(m_materialUv.rtv.cpu, uvClearColor, 0, nullptr);
    });

    // **被写界深度が深度を読むので SRV も張る。** 深度として書き、SRV としても
    // 読むため TYPELESS で作る（シャドウマップと同じ作法）。
    rhi::TextureDesc depthDesc;
    depthDesc.width = width;
    depthDesc.height = height;
    depthDesc.format = DXGI_FORMAT_R32_TYPELESS;
    depthDesc.dsvFormat = kDepthFormat;
    depthDesc.srvFormat = DXGI_FORMAT_R32_FLOAT;
    depthDesc.allowDepthStencil = true;
    depthDesc.createSrv = true;
    depthDesc.clearDepth = 1.0f;
    depthDesc.initialState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    depthDesc.debugName = L"SceneDepth";
    if (!device.Allocator().CreateTexture2D(depthDesc, m_depth)) {
        return false;
    }

    rhi::TextureDesc outputDesc;
    outputDesc.width = width;
    outputDesc.height = height;
    outputDesc.format = kOutputFormat;
    outputDesc.allowUnorderedAccess = true;
    // トーンマップ（コンピュート）後に、深度テスト付きのガイド線を
    // グラフィックスパスで重ねるため、RTV としても使えるようにする。
    outputDesc.allowRenderTarget = true;
    outputDesc.createSrv = true;
    outputDesc.initialState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    outputDesc.debugName = L"PreviewOutput";
    if (!device.Allocator().CreateTexture2D(outputDesc, m_output)) {
        return false;
    }

    // RTV フラグ付きのリソースは、最初に Clear / Discard / Copy で初期化しないと
    // デバッグレイヤーが「未初期化のまま描画に使った」というエラーを出す
    // （NOT_ZEROED ヒープの規則）。中身はトーンマップが毎フレーム全画素を
    // 書き潰すので、Discard で十分。
    device.ExecuteImmediate([&](ID3D12GraphicsCommandList* commandList) {
        TransitionIfNeeded(commandList, m_output, D3D12_RESOURCE_STATE_RENDER_TARGET);
        commandList->DiscardResource(m_output.resource.Get(), nullptr);
        TransitionIfNeeded(commandList, m_output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    });

    m_width = width;
    m_height = height;
    m_camera.SetViewportSize(width, height);
    return true;
}

compositor::PaintContext PreviewRenderer::PrepareUvBufferForRead(
    ID3D12GraphicsCommandList* commandList) {
    compositor::PaintContext context;
    if (!m_materialUv.IsValid()) {
        return context;
    }

    // 読むのは前フレームの内容。ブラシは 1 フレーム前のカーソル位置に対応する
    // UV を見ることになるが、描き味に影響が出るほどの差にはならない。
    TransitionIfNeeded(commandList, m_materialUv,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    context.uvBufferSrvIndex = m_materialUv.SrvIndex();
    context.viewportWidth = m_width;
    context.viewportHeight = m_height;
    return context;
}

void PreviewRenderer::Render(rhi::Device& device, rhi::PipelineCache& pipelineCache,
                             ID3D12GraphicsCommandList* commandList,
                             const compositor::MaterialStack& stack,
                             const compositor::TextureLibrary& textures,
                             const compositor::MaterialLibrary& materials,
                             const compositor::PaintMaskStore& paintMasks) {
    if (!m_sceneColor.IsValid() || !m_output.IsValid()) {
        return;
    }

    // 軌道の距離とクリップ面を被写体の大きさへ合わせる。**毎フレーム渡してよい。**
    // 平面のサイズや変位量はいつでも変わるので、描く直前に見るのが確実。
    m_camera.SetSceneRadius(BoundingRadius());

    // 描画の量はフレームごとに数え直す。**描くところで足す**ので、
    // パスを増やしたときに数え漏らしても、増やした本人が気づきやすい。
    m_stats = RenderStats{};

    // レイヤースタックに変更があれば、メッシュを描く前に評価し直す。
    m_evaluator.EvaluateIfDirty(device, pipelineCache, commandList, stack, textures, materials,
                                paintMasks);

    rhi::GraphicsPipelineDesc meshPipelineDesc;
    meshPipelineDesc.shaderPath = L"MeshPbr.hlsl";
    meshPipelineDesc.vertexEntry = L"VsMain";
    meshPipelineDesc.pixelEntry = L"PsMain";
    meshPipelineDesc.rtvFormat = kSceneColorFormat;
    // 2 枚目にマテリアル UV を書く。ペイントのカーソル位置解決に使う。
    meshPipelineDesc.rtvFormat1 = kMaterialUvFormat;
    meshPipelineDesc.dsvFormat = kDepthFormat;
    meshPipelineDesc.layout = rhi::VertexLayout::MeshStandard;
    meshPipelineDesc.cullMode = D3D12_CULL_MODE_BACK;
    // テセレーションを使うときは、頂点シェーダを制御点の出力だけに差し替える。
    const bool useTessellation = m_tessellationEnabled;
    if (useTessellation) {
        meshPipelineDesc.vertexEntry = L"VsControl";
        meshPipelineDesc.hullEntry = L"HsMain";
        meshPipelineDesc.domainEntry = L"DsMain";
    }
    // ワイヤーフレーム表示のときだけラスタライザを切り替える。
    if (m_debugView == DebugView::Wireframe) {
        meshPipelineDesc.fillMode = D3D12_FILL_MODE_WIREFRAME;
    }

    ID3D12PipelineState* meshPipeline = pipelineCache.GetGraphics(meshPipelineDesc);
    ID3D12PipelineState* tonemapPipeline =
        pipelineCache.GetCompute(L"TonemapPass.hlsl", L"CsMain");
    if (meshPipeline == nullptr || tonemapPipeline == nullptr) {
        return;
    }

    const Mesh& mesh = CurrentMesh();
    if (!mesh.IsValid()) {
        return;
    }

    // DirectXMath は行ベクトル規約、HLSL の行列は既定で列優先。
    // XMMATRIX をそのまま積むと HLSL 側では転置として解釈され、
    // mul(matrix, vector) が意図どおりの結果になる。転置は入れない。
    const XMMATRIX view = m_camera.ViewMatrix();
    const XMMATRIX projection = m_camera.ProjectionMatrix();
    // 平面はメッシュを作り直さず、モデル行列で実サイズ（m）へ広げる。
    // **Y は拡大しない。** 高さはディスプレイスメント（m）が世界空間で足すので、
    // ここで縦に伸ばすと二重に効く。
    XMMATRIX model = XMMatrixIdentity();
    if (m_shape == PreviewShape::Plane) {
        const float scale = m_planeSize / kPlaneMeshSize;
        model = XMMatrixScaling(scale, 1.0f, scale);
    }

    const XMMATRIX viewProjection = XMMatrixMultiply(view, projection);

    MeshConstants constants = {};
    XMStoreFloat4x4(&constants.viewProjection, viewProjection);
    XMStoreFloat4x4(&constants.model, model);
    // 法線行列だけは (M^-1)^T が要るため、転置を明示する。
    XMStoreFloat4x4(&constants.normalMatrix,
                    XMMatrixTranspose(XMMatrixInverse(nullptr, model)));

    constants.cameraPosition = m_camera.Position();
    constants.lightDirection = m_light.Direction();
    constants.lightIlluminance = m_light.illuminance;
    constants.lightColor = m_light.color;
    constants.baseColor = m_material.baseColor;
    constants.roughness = m_material.roughness;
    constants.metallic = m_material.metallic;
    constants.iblIntensity = m_environment.IsReady() ? m_activeSky.iblIntensity : 0.0f;
    constants.prefilteredMipCount = m_environment.PrefilteredMipCount();
    constants.irradianceIndex = m_environment.IrradianceSrvIndex();
    constants.prefilteredIndex = m_environment.PrefilteredSrvIndex();
    constants.brdfLutIndex = m_environment.BrdfLutSrvIndex();

    const compositor::MaterialTextureSet& materialTextures = m_evaluator.Textures();
    const bool useMaterial = m_useMaterialTextures && materialTextures.IsValid();
    constants.useMaterialTextures = useMaterial ? 1u : 0u;
    constants.materialBaseColorIndex = materialTextures.baseColor.SrvIndex();
    constants.materialNormalIndex = materialTextures.normal.SrvIndex();
    constants.materialSurfaceIndex = materialTextures.surface.SrvIndex();
    constants.materialHeightIndex = materialTextures.height.SrvIndex();
    constants.materialUvScale = m_materialUvScale;
    // 平面 + UV スケール 1 は「タイルしない 1 枚絵」のプレビューとみなし、
    // 合成結果をクランプで読む。wrap だと UV 端のバイリニア補間が反対側の端と
    // 混ざり、地形の縁が反対側の高さへ引っ張られる。
    // 球はシーム（経度の 0/1）の連続性に wrap が必要なので対象外。
    constants.clampMaterialUv =
        (m_shape == PreviewShape::Plane && m_materialUvScale == 1.0f) ? 1u : 0u;
    constants.debugView = static_cast<uint32_t>(m_debugView);
    constants.displacementScale = m_displacementScale;
    // 分割量はカメラから見た見え方で決める。本描画では viewProjection と同一で、
    // シャドウパスだけが viewProjection 側を上書きして分岐する。
    XMStoreFloat4x4(&constants.tessellationViewProjection, viewProjection);
    constants.viewportSize[0] = static_cast<float>(m_width);
    constants.viewportSize[1] = static_cast<float>(m_height);
    constants.tessellationMaxFactor = m_tessellationFactor;

    // --- シャドウマップ ----------------------------------------------------
    // ライトから深度だけを描く。同じ頂点シェーダを通るので、
    // ディスプレイスメントで押し出した形がそのまま影になる。
    constants.shadowIndex = kNoShadowIndex;
    constants.shadowTexelSize = 1.0f / static_cast<float>(kShadowMapSize);
    constants.shadowBias = kShadowBias;

    if (m_shadowEnabled && m_shadowMap.IsValid()) {
        const XMMATRIX lightViewProjection = LightViewProjection();
        XMStoreFloat4x4(&constants.lightViewProjection, lightViewProjection);

        rhi::GraphicsPipelineDesc shadowPipelineDesc;
        shadowPipelineDesc.shaderPath = L"MeshPbr.hlsl";
        shadowPipelineDesc.vertexEntry = L"VsMain";
        // ピクセルシェーダは要らない。深度だけ書く。
        shadowPipelineDesc.dsvFormat = kShadowDsvFormat;
        shadowPipelineDesc.layout = rhi::VertexLayout::MeshStandard;
        // 平面のような片面のメッシュも影を落とすので、両面を描く。
        shadowPipelineDesc.cullMode = D3D12_CULL_MODE_NONE;
        // 本描画と同じ分割で描く。違う形を影にすると自己遮蔽がずれる。
        if (m_tessellationEnabled) {
            shadowPipelineDesc.vertexEntry = L"VsControl";
            shadowPipelineDesc.hullEntry = L"HsMain";
            shadowPipelineDesc.domainEntry = L"DsMain";
        }

        ID3D12PipelineState* shadowPipeline = pipelineCache.GetGraphics(shadowPipelineDesc);
        const rhi::UploadAllocation shadowCb =
            device.Upload().Allocate(sizeof(MeshConstants), 256);

        if (shadowPipeline != nullptr && shadowCb.IsValid()) {
            // ライトから見た行列で描く。ほかの値は本描画と同じ。
            MeshConstants shadowConstants = constants;
            XMStoreFloat4x4(&shadowConstants.viewProjection, lightViewProjection);
            std::memcpy(shadowCb.cpu, &shadowConstants, sizeof(shadowConstants));

            PIXBeginEvent(commandList, PIX_COLOR(220, 200, 120), "PreviewShadow");
            TransitionIfNeeded(commandList, m_shadowMap, D3D12_RESOURCE_STATE_DEPTH_WRITE);

            const D3D12_CPU_DESCRIPTOR_HANDLE shadowDsv = m_shadowMap.dsv.cpu;
            commandList->OMSetRenderTargets(0, nullptr, FALSE, &shadowDsv);
            commandList->ClearDepthStencilView(shadowDsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0,
                                               nullptr);

            const auto shadowViewport =
                CD3DX12_VIEWPORT(0.0f, 0.0f, static_cast<float>(kShadowMapSize),
                                 static_cast<float>(kShadowMapSize));
            const auto shadowScissor = CD3DX12_RECT(0, 0, static_cast<LONG>(kShadowMapSize),
                                                    static_cast<LONG>(kShadowMapSize));
            commandList->RSSetViewports(1, &shadowViewport);
            commandList->RSSetScissorRects(1, &shadowScissor);

            commandList->SetGraphicsRootSignature(pipelineCache.GlobalRootSignature());
            commandList->SetPipelineState(shadowPipeline);
            commandList->SetGraphicsRootConstantBufferView(1, shadowCb.gpuAddress);
            mesh.Draw(commandList, m_tessellationEnabled);
            CountMeshDraw(m_stats, mesh, m_tessellationEnabled);

            TransitionIfNeeded(commandList, m_shadowMap,
                               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            PIXEndEvent(commandList);

            constants.shadowIndex = m_shadowMap.SrvIndex();
        }
    }

    PIXBeginEvent(commandList, PIX_COLOR(80, 200, 120), "PreviewScene");

    TransitionIfNeeded(commandList, m_sceneColor, D3D12_RESOURCE_STATE_RENDER_TARGET);
    TransitionIfNeeded(commandList, m_materialUv, D3D12_RESOURCE_STATE_RENDER_TARGET);
    TransitionIfNeeded(commandList, m_depth, D3D12_RESOURCE_STATE_DEPTH_WRITE);

    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_sceneColor.rtv.cpu;
    const D3D12_CPU_DESCRIPTOR_HANDLE dsv = m_depth.dsv.cpu;
    const D3D12_CPU_DESCRIPTOR_HANDLE meshRtvs[] = {rtv, m_materialUv.rtv.cpu};
    commandList->OMSetRenderTargets(_countof(meshRtvs), meshRtvs, FALSE, &dsv);

    const float clearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    // UV バッファの被覆は z に入る。0 クリアで「メッシュに当たっていない」を表す。
    const float clearUv[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    commandList->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
    commandList->ClearRenderTargetView(m_materialUv.rtv.cpu, clearUv, 0, nullptr);
    commandList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    const auto viewport = CD3DX12_VIEWPORT(0.0f, 0.0f, static_cast<float>(m_width),
                                           static_cast<float>(m_height));
    const auto scissor = CD3DX12_RECT(0, 0, static_cast<LONG>(m_width),
                                      static_cast<LONG>(m_height));
    commandList->RSSetViewports(1, &viewport);
    commandList->RSSetScissorRects(1, &scissor);


    const rhi::UploadAllocation cb = device.Upload().Allocate(sizeof(MeshConstants), 256);
    if (!cb.IsValid()) {
        PIXEndEvent(commandList);
        return;
    }
    std::memcpy(cb.cpu, &constants, sizeof(constants));

    commandList->SetGraphicsRootSignature(pipelineCache.GlobalRootSignature());
    commandList->SetPipelineState(meshPipeline);
    commandList->SetGraphicsRootConstantBufferView(1, cb.gpuAddress);
    mesh.Draw(commandList, useTessellation);
    CountMeshDraw(m_stats, mesh, useTessellation);
    m_stats.tessellation = useTessellation;
    m_stats.tessellationFactor = m_tessellationFactor;

    PIXEndEvent(commandList);

    // --- スカイボックス ----------------------------------------------------
    // メッシュのあとに描く。深度は書かず、まだ何も描かれていない画素だけを埋める。
    // UV バッファには書かないので、シーンカラーだけを束ね直す。
    commandList->OMSetRenderTargets(1, &rtv, FALSE, &dsv);

    // デバッグ表示のときは背景を描かない。チャンネルの値だけを見たいため。
    if (m_showSkybox && m_environment.IsReady() && m_debugView == DebugView::Shaded) {
        rhi::GraphicsPipelineDesc skyboxPipelineDesc;
        skyboxPipelineDesc.shaderPath = L"Skybox.hlsl";
        skyboxPipelineDesc.vertexEntry = L"VsMain";
        skyboxPipelineDesc.pixelEntry = L"PsMain";
        skyboxPipelineDesc.rtvFormat = kSceneColorFormat;
        skyboxPipelineDesc.dsvFormat = kDepthFormat;
        skyboxPipelineDesc.layout = rhi::VertexLayout::None;
        skyboxPipelineDesc.cullMode = D3D12_CULL_MODE_NONE;
        skyboxPipelineDesc.depthTest = true;
        skyboxPipelineDesc.depthWrite = false;

        ID3D12PipelineState* skyboxPipeline = pipelineCache.GetGraphics(skyboxPipelineDesc);
        const rhi::UploadAllocation skyboxCb =
            device.Upload().Allocate(sizeof(SkyboxConstants), 256);

        if (skyboxPipeline != nullptr && skyboxCb.IsValid()) {
            PIXBeginEvent(commandList, PIX_COLOR(120, 160, 220), "PreviewSkybox");

            SkyboxConstants skyboxConstants = {};
            // メッシュ側と同じ理由で転置は入れない。
            XMStoreFloat4x4(&skyboxConstants.inverseViewProjection,
                            XMMatrixInverse(nullptr, XMMatrixMultiply(view, projection)));
            skyboxConstants.cameraPosition = m_camera.Position();
            skyboxConstants.intensity = m_activeSky.iblIntensity;
            if (m_skyboxBlur) {
                // プリフィルタ済みキューブはラフネス別に GGX で畳み込んである。
                // 粗いミップを引けば、ぼかしパスを足さずに背景だけを柔らかくできる。
                // ミップを落とすだけの（箱フィルタの）環境キューブより滑らか。
                skyboxConstants.environmentIndex = m_environment.PrefilteredSrvIndex();
                skyboxConstants.mipLevel = std::min(
                    kSkyboxBlurMip, static_cast<float>(m_environment.PrefilteredMipCount() - 1));
            } else {
                skyboxConstants.environmentIndex = m_environment.EnvironmentSrvIndex();
                skyboxConstants.mipLevel = 0.0f;
            }
            std::memcpy(skyboxCb.cpu, &skyboxConstants, sizeof(skyboxConstants));

            commandList->SetPipelineState(skyboxPipeline);
            commandList->SetGraphicsRootConstantBufferView(1, skyboxCb.gpuAddress);
            commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            commandList->IASetVertexBuffers(0, 0, nullptr);
            commandList->IASetIndexBuffer(nullptr);
            commandList->DrawInstanced(3, 1, 0, 0);
            // 画面全体を覆う三角形 1 枚。頂点は頂点シェーダが作る。
            ++m_stats.drawCalls;
            m_stats.vertices += 3;
            m_stats.triangles += 1;

            PIXEndEvent(commandList);
        }
    }

    TransitionIfNeeded(commandList, m_sceneColor, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    // --- 被写界深度 --------------------------------------------------------
    // **トーンマップの前に、線形 HDR のまま掛ける。** 露出後だと明るい点が
    // 飽和してから広がり、玉ボケの芯が白く潰れる。
    // デバッグ表示のときは掛けない（チャンネルの値そのものを見るための表示）。
    uint32_t tonemapSourceIndex = m_sceneColor.SrvIndex();
    ID3D12PipelineState* dofPipeline =
        (m_dof.enabled && m_debugView == DebugView::Shaded && m_sceneColorDof.IsValid())
            ? pipelineCache.GetCompute(L"DepthOfField.hlsl", L"CsMain")
            : nullptr;
    if (dofPipeline != nullptr) {
        PIXBeginEvent(commandList, PIX_COLOR(120, 160, 220), "PreviewDepthOfField");

        TransitionIfNeeded(commandList, m_depth,
                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        TransitionIfNeeded(commandList, m_sceneColorDof, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        DofConstants dofConstants = {};
        dofConstants.sourceIndex = m_sceneColor.SrvIndex();
        dofConstants.depthIndex = m_depth.SrvIndex();
        dofConstants.outputIndex = m_sceneColorDof.UavIndex();
        dofConstants.width = m_width;
        dofConstants.height = m_height;
        dofConstants.focalLengthMm = FocalLengthFromFovY(m_camera.FovY());
        dofConstants.fStop = m_exposure.aperture;
        dofConstants.focusDistance = FocusDistance();
        dofConstants.nearZ = m_camera.NearZ();
        dofConstants.farZ = m_camera.FarZ();
        dofConstants.maxBlurPixels = m_dof.maxBlurPixels;
        dofConstants.apertureRotation = m_dof.rotationDegrees * (3.14159265358979f / 180.0f);
        dofConstants.apertureBlades = ApertureBladeCount(m_dof.shape);
        dofConstants.blurScale = m_dof.blurScale;

        commandList->SetComputeRootSignature(pipelineCache.GlobalRootSignature());
        commandList->SetPipelineState(dofPipeline);
        commandList->SetComputeRoot32BitConstants(0, sizeof(dofConstants) / sizeof(uint32_t),
                                                  &dofConstants, 0);
        commandList->Dispatch(rhi::DispatchCount(m_width), rhi::DispatchCount(m_height), 1);

        TransitionIfNeeded(commandList, m_sceneColorDof,
                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        tonemapSourceIndex = m_sceneColorDof.SrvIndex();

        PIXEndEvent(commandList);
    }

    PIXBeginEvent(commandList, PIX_COLOR(200, 120, 80), "PreviewTonemap");

    TransitionIfNeeded(commandList, m_output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    const TonemapConstants tonemapConstants{
        tonemapSourceIndex,        m_output.UavIndex(),
        m_width,                   m_height,
        m_exposure.Exposure(),     static_cast<uint32_t>(m_tonemap),
        static_cast<uint32_t>(m_debugView)};

    commandList->SetComputeRootSignature(pipelineCache.GlobalRootSignature());
    commandList->SetPipelineState(tonemapPipeline);
    commandList->SetComputeRoot32BitConstants(0, sizeof(tonemapConstants) / sizeof(uint32_t),
                                              &tonemapConstants, 0);

    commandList->Dispatch(rhi::DispatchCount(m_width), rhi::DispatchCount(m_height), 1);

    PIXEndEvent(commandList);

    // ハイトの範囲の枠。シーンの深度でテストするため、ImGui ではなくここで描く。
    DrawHeightGuideOverlay(device, pipelineCache, commandList);

    // ImGui から SRV として読むため、ピクセルシェーダ可視の状態へ移す。
    TransitionIfNeeded(commandList, m_output, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

// ハイトの範囲の枠（height 0 / 0.5 / 1 の位置）。
//
// トーンマップ後の表示用テクスチャへ、露出を通さない表示色のまま描く
// （ギズモは画面上で一定の明るさに見えるべきもの）。深度は読むだけで書かない。
// ラベル（0.0 / 0.5 / 1.0 の文字）は Application 側の ImGui が重ねる。
void PreviewRenderer::DrawHeightGuideOverlay(rhi::Device& device,
                                             rhi::PipelineCache& pipelineCache,
                                             ID3D12GraphicsCommandList* commandList) {
    // 平面のときだけ。球とキューブは法線方向への押し出しなので、
    // 直方体の枠では高さの範囲を表せない。
    if (!m_showHeightGuide || m_shape != PreviewShape::Plane) {
        return;
    }

    rhi::GraphicsPipelineDesc pipelineDesc;
    pipelineDesc.shaderPath = L"OverlayLines.hlsl";
    pipelineDesc.vertexEntry = L"VsMain";
    pipelineDesc.pixelEntry = L"PsMain";
    pipelineDesc.rtvFormat = kOutputFormat;
    pipelineDesc.dsvFormat = kDepthFormat;
    pipelineDesc.layout = rhi::VertexLayout::None;
    pipelineDesc.cullMode = D3D12_CULL_MODE_NONE;
    pipelineDesc.depthTest = true;
    pipelineDesc.depthWrite = false;
    pipelineDesc.lineTopology = true;
    pipelineDesc.alphaBlend = true;

    ID3D12PipelineState* pipeline = pipelineCache.GetGraphics(pipelineDesc);
    const rhi::UploadAllocation cb =
        device.Upload().Allocate(sizeof(OverlayLineConstants), 256);
    if (pipeline == nullptr || !cb.IsValid()) {
        return;
    }

    OverlayLineConstants constants = {};
    XMStoreFloat4x4(&constants.viewProjection,
                    XMMatrixMultiply(m_camera.ViewMatrix(), m_camera.ProjectionMatrix()));
    // ImGui のギズモと同じ無彩色（表示色）。
    constants.color[0] = 150.0f / 255.0f;
    constants.color[1] = 160.0f / 255.0f;
    constants.color[2] = 175.0f / 255.0f;
    constants.color[3] = 1.0f;

    uint32_t count = 0;
    const auto addLine = [&](const XMFLOAT3& a, const XMFLOAT3& b, float alpha) {
        if (count + 2 > kOverlayLineMaxVertices) {
            return;
        }
        constants.positions[count++] = XMFLOAT4{a.x, a.y, a.z, alpha};
        constants.positions[count++] = XMFLOAT4{b.x, b.y, b.z, alpha};
    };

    const float kHalf = m_planeSize * 0.5f;
    const XMFLOAT2 corners[4] = {{-kHalf, -kHalf}, {kHalf, -kHalf}, {kHalf, kHalf},
                                 {-kHalf, kHalf}};

    // height 0 / 0.5 / 1 の矩形。0.5（基準面）だけ薄くして区別する。
    const float levels[3] = {0.0f, 0.5f, 1.0f};
    const float levelAlphas[3] = {0.78f, 0.43f, 0.78f};
    for (int level = 0; level < 3; ++level) {
        const float y = (levels[level] - 0.5f) * m_displacementScale;
        for (int i = 0; i < 4; ++i) {
            const XMFLOAT2& a = corners[i];
            const XMFLOAT2& b = corners[(i + 1) % 4];
            addLine(XMFLOAT3{a.x, y, a.y}, XMFLOAT3{b.x, y, b.y}, levelAlphas[level]);
        }
    }
    // 四隅の縦の辺（height 0 → 1）。
    for (const XMFLOAT2& corner : corners) {
        addLine(XMFLOAT3{corner.x, -0.5f * m_displacementScale, corner.y},
                XMFLOAT3{corner.x, 0.5f * m_displacementScale, corner.y}, 0.55f);
    }

    std::memcpy(cb.cpu, &constants, sizeof(constants));

    PIXBeginEvent(commandList, PIX_COLOR(160, 170, 190), "PreviewHeightGuide");

    TransitionIfNeeded(commandList, m_output, D3D12_RESOURCE_STATE_RENDER_TARGET);
    // DoF が有効なフレームでは深度が SRV になっている。DSV として束ね直す
    // （書き込みは PSO 側で無効にしてある）。
    TransitionIfNeeded(commandList, m_depth, D3D12_RESOURCE_STATE_DEPTH_WRITE);

    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_output.rtv.cpu;
    const D3D12_CPU_DESCRIPTOR_HANDLE dsv = m_depth.dsv.cpu;
    commandList->OMSetRenderTargets(1, &rtv, FALSE, &dsv);

    commandList->SetGraphicsRootSignature(pipelineCache.GlobalRootSignature());
    commandList->SetPipelineState(pipeline);
    commandList->SetGraphicsRootConstantBufferView(1, cb.gpuAddress);
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);
    commandList->IASetVertexBuffers(0, 0, nullptr);
    commandList->IASetIndexBuffer(nullptr);
    commandList->DrawInstanced(count, 1, 0, 0);
    ++m_stats.drawCalls;
    m_stats.vertices += count;

    PIXEndEvent(commandList);
}

}  // namespace tg::renderer
