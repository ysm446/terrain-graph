#pragma once

#include "compositor/MaterialEvaluator.h"
#include "compositor/PaintMask.h"
#include "compositor/TextureLibrary.h"
#include "renderer/Camera.h"
#include "renderer/Environment.h"
#include "renderer/SkyLibrary.h"
#include "renderer/Mesh.h"
#include "rhi/Device.h"
#include "rhi/PipelineCache.h"

#include <DirectXMath.h>

namespace tg::renderer {

// ビューポートに何を出すか。シェーダの TG_VIEW_* と一致させること。
//
// チャンネルを覗く表示（ベースカラー〜ハイト）は「中身をそのまま見る」ためのもので、
// 露出もトーンマップも掛けない。**形を見る表示（ワイヤーフレーム / クレイ）は
// これに含まれない**（クレイは陰影を付けるので、シェーディングと同じ扱い）。
enum class DebugView : uint32_t {
    Shaded = 0,
    BaseColor = 1,
    // 陰影に使う向き（法線マップを当てたあと）を、カメラ空間とワールド空間で見る。
    // カメラ空間は「画面に対してどちらを向いているか」が読める（正面が水色）。
    NormalView = 2,
    NormalWorld = 3,
    Roughness = 4,
    Metallic = 5,
    AmbientOcclusion = 6,
    Height = 7,
    // **地形の大きな高さを引いた「その場の起伏」だけ**の表示。
    // 素材のハイトマップをそのまま貼ったように見える（Height だと
    // 標高差 600m の傾きに埋もれて、素材の凹凸が見えないため）。
    HeightLocal = 8,
    // 形だけを見る表示。ラスタライザをワイヤーフレームにする。
    Wireframe = 9,
    // **テクスチャを貼らない単色の陰影**（粘土模型のような見え方）。メッシュの確認用。
    // 形（ディスプレイスメント）はそのままで、色 / 法線 / サーフェスだけを
    // 合成結果から切り離し、単色マテリアルと**面の向き**（画面微分で起こした法線）
    // で陰影を付ける。法線マップに隠れない、実際のポリゴンの形が見える。
    Clay = 10,
};

enum class TonemapMode : uint32_t {
    None = 0,
    Reinhard = 1,
    Aces = 2,
};

// 物理カメラの露出設定。
//   EV100    = log2(N^2 / t) - log2(ISO / 100)
//   exposure = 1 / (1.2 * 2^EV100)
struct ExposureSettings {
    bool useManualEv = false;
    float manualEv100 = 15.0f;
    float aperture = 16.0f;              // N（F 値）
    float shutterSpeed = 1.0f / 250.0f;  // t（秒）
    float iso = 100.0f;

    float Ev100() const;
    float Exposure() const;
};

struct LightSettings {
    float azimuth = 0.9f;               // 方位角（ラジアン）
    float elevation = 0.9f;             // 仰角（ラジアン）
    float illuminance = 100000.0f;      // lux。晴天の直射日光がおよそ 100000
    DirectX::XMFLOAT3 color = {1.0f, 0.98f, 0.95f};

    // サーフェスから光源へ向かう正規化ベクトル。
    DirectX::XMFLOAT3 Direction() const;
};

// 1 フレームぶんの描画の量。ビューポートの右上に出す。
//
// **投入した量（IA が読む量）を数える。** テセレーションを入れると実際に
// 出る三角形はこれより多いが、CPU 側では分からないのでパッチ数と上限を添える。
struct RenderStats {
    uint32_t drawCalls = 0;
    // 投入した頂点とインデックス。インデックス付き描画では
    // 「頂点 = インデックス数」（IA がその回数だけ頂点を読む）。
    uint64_t vertices = 0;
    uint64_t triangles = 0;
    // テセレーションのパッチ数。使っていなければ 0。
    uint64_t patches = 0;
    bool tessellation = false;
    float tessellationFactor = 1.0f;
};

// 絞りの形。ボケの形になる。
enum class ApertureShape : uint32_t {
    Circle = 0,
    Triangle = 1,
    Hexagon = 2,
    Octagon = 3,
};

// 被写界深度。**ビューポートの見え方だけの設定**で、合成結果には一切効かない。
//
// レンズの値は増やさない。焦点距離はカメラの画角から、F 値は露出の絞りから取る。
// 被写界深度のためだけに同じ意味の値をもう一組持つと、どちらが効いているのか
// 分からなくなる（露出とレンズが食い違った絵になる）。
struct DofSettings {
    bool enabled = false;
    // **注視点までの距離をピント面にする。** 軌道カメラなので、見ているものが
    // 常に注視点にある。手で合わせ直す手間をなくすため既定でオン。
    bool focusOnTarget = true;
    // 手動のピント距離（メートル。ワールドの 1 単位を 1m とみなす）。
    float focusDistance = 3.2f;
    // 画面上のボケ半径の上限。現実の式のままだと極端なボケと負荷になるので、
    // 表示のための頭打ちとして持つ。
    float maxBlurPixels = 24.0f;
    // **ミニチュアの縮尺（1 : この値）。** 1 で実物大。
    //
    // 錯乱円は距離に反比例するので、実寸のまま 2km の地形を撮ると
    // ほぼ全部が無限遠の扱いになり、絞りを開けても 1 画素もボケない
    // （既定の 29mm・F16 でピント 2000m のとき、1000m の所で 0.0006 画素）。
    // ここを 1000 にすると「2km の地形を 2m の模型として撮る」計算になり、
    // 素材（2m 角）を撮ったときと同じくらいボケる。
    //
    // **UI の単位は現実のカメラのまま**にしてある（焦点距離 mm / F 値 / m）。
    // 縮めるのはシーンの距離だけで、レンズの側は触らない。
    float miniatureScale = 1.0f;
    // **ボケ量に掛ける誇張の倍率。** 1 で現実どおり。
    //
    // スケールの補正ではない。2m 角の地面を 3.2m から広角で撮れば、
    // **現実のカメラでも全域にピントが合う**（29mm・F1.8 で錯乱円が 1 画素に届かない）。
    // 物理的に正しくても素材の見せ方としては物足りないことがあるので、
    // 物理の関係を保ったまま量だけ持ち上げるための係数として持つ。
    // 倍率を使わずにボケを出したいなら、現実と同じく望遠へ寄せればよい
    // （100mm・F1.8 なら倍率 1 のまま半径 10 画素ほどになる）。
    float blurScale = 1.0f;
    ApertureShape shape = ApertureShape::Circle;
    // 多角形のボケの向き（度）。円のときは効かない。
    float rotationDegrees = 0.0f;
};

struct MaterialSettings {
    // 合成結果を使わないときの単色マテリアル。**18% グレー**にしてある
    // （MaterialLayer::baseColor と同じ基準）。ルックデブで使う
    // グレーボールと同じ明るさで、露出とライトの確認がしやすい。
    DirectX::XMFLOAT3 baseColor = {0.18f, 0.18f, 0.18f};
    float roughness = 0.35f;
    float metallic = 0.0f;
};

// シーンを HDR で描き、露出とトーンマップを通して表示用テクスチャへ書き出す。
// プレビュー設定の既定値。**メンバ初期化子・UI の既定値マーカー・
// プロジェクト読み込みのフォールバックの 3 か所で必ずこれを使う。**
// 数値を直接書くと、片方だけ変えたときに「既定値マーカーが点いたまま」
// 「読み込みで別の値に化ける」という食い違いが起きる。
struct PreviewDefaults {
    TonemapMode tonemap = TonemapMode::Aces;
    bool useMaterialTextures = true;
    float displacementScale = 0.0f;
    // 平面の一辺（m）。Megascans のサーフェスが 2m 角で作られていることに合わせた既定。
    float planeSize = 2.0f;
    bool tessellationEnabled = false;
    float tessellationFactor = 8.0f;
    uint32_t materialResolution = 1024;
    // 平面メッシュの分割数。**形の細かさの上限はここで決まる。**
    // 地形の一辺 ÷ 分割数 が 1 マスの大きさ（2048m を 256 分割で 8m）。
    uint32_t meshSubdivisions = 256;
    bool showSkybox = true;
    bool skyboxBlur = false;
    bool shadowEnabled = true;
    // マスクのプレビューで、0 か 1 に張り付いた所へ斜線を引くか。
    bool maskSaturationHatch = false;
};
inline constexpr PreviewDefaults kPreviewDefaults{};

class PreviewRenderer {
public:
    // 平面メッシュを作るときの一辺（XZ 平面、原点中心、Y = 0）。
    // **これは頂点データの基準サイズで、表示される大きさではない。**
    // 実際の大きさは PlaneSize()（m）で、モデル行列の拡大で合わせる。
    // メッシュを作り直さずに 2m の素材から 2km の地形まで扱えるようにするため。
    static constexpr float kPlaneMeshSize = 2.0f;

    bool Initialize(rhi::Device& device, rhi::PipelineCache& pipelineCache);
    void Shutdown(rhi::Device& device);

    // プロジェクトが持つ設定をすべて既定へ戻す。「新規」で使う。
    //
    // **対象は io::ReadPreview が読む項目と一致させること。**
    // 片方に足し忘れると、「新規にしたのに前のプロジェクトの値が残る」
    // （こちらの漏れ）か「開いても既定に戻らない」（あちらの漏れ）になる。
    // 天球は SkyLibrary が持つので、ここでは触らない。
    void ResetSettings();

    // ビューポートに適用する天球を渡す。**毎フレーム呼んでよい。**
    // 前回と中身が違えば、必要な作り直し（環境マップの再生成か、
    // 較正倍率だけの掛け直し）を予約する。実際の生成はフレームの外で行う。
    void SetActiveSky(const SkyDefinition& sky);
    const SkyDefinition& ActiveSky() const { return m_activeSky; }
    // 環境マップやマテリアル解像度の作り直しは GPU 待機を伴うため、
    // フレームの外でまとめて処理する。
    void ProcessPendingWork(rhi::Device& device, rhi::PipelineCache& pipelineCache);

    // 表示先のサイズに合わせてレンダーターゲットを作り直す。
    bool Resize(rhi::Device& device, uint32_t width, uint32_t height);

    void Render(rhi::Device& device, rhi::PipelineCache& pipelineCache,
                ID3D12GraphicsCommandList* commandList, const compositor::MaterialStack& stack,
                const compositor::TextureLibrary& textures,
                const compositor::MaterialLibrary& materials,
                const compositor::PaintMaskStore& paintMasks);

    // ペイントのブラシパスが UV バッファを読むための準備をする。
    // フレーム内、Render より前に呼ぶこと（読むのは前フレームの内容）。
    compositor::PaintContext PrepareUvBufferForRead(ID3D12GraphicsCommandList* commandList);

    Camera& GetCamera() { return m_camera; }
    const Camera& GetCamera() const { return m_camera; }
    ExposureSettings& Exposure() { return m_exposure; }
    LightSettings& Light() { return m_light; }
    MaterialSettings& Material() { return m_material; }
    // 平面を包む球の半径（原点中心）。カメラの Frame() が使う。
    float BoundingRadius() const;
    TonemapMode& Tonemap() { return m_tonemap; }
    DebugView& Debug() { return m_debugView; }
    DebugView Debug() const { return m_debugView; }
    const Environment& GetEnvironment() const { return m_environment; }
    bool& ShowSkybox() { return m_showSkybox; }
    // 背景だけをぼかす。**IBL の寄与は変えない。**
    // プリフィルタ済みキューブの粗いミップを引くだけなので、追加のパスは要らない。
    bool& SkyboxBlur() { return m_skyboxBlur; }
    // ディレクショナルライトの影を落とすか。落とさないとシャドウパスも走らない。
    bool& ShadowEnabled() { return m_shadowEnabled; }
    DofSettings& Dof() { return m_dof; }
    const DofSettings& Dof() const { return m_dof; }
    // 実際にピント面として使う距離。注視点に合わせる設定ならカメラの距離。
    float FocusDistance() const;
    // テセレーション（画面上の辺の長さに応じた分割）を使うか。
    bool& TessellationEnabled() { return m_tessellationEnabled; }
    // 1 辺あたりの分割の上限。
    float& TessellationFactor() { return m_tessellationFactor; }
    bool& UseMaterialTextures() { return m_useMaterialTextures; }
    // ハイトを形状に反映する量（0 で反映しない）。頂点シェーダで押し出す。
    // **単位は m。** ハイト 0〜1 の全幅がこの高さに対応する。
    float& DisplacementScale() { return m_displacementScale; }
    float DisplacementScale() const { return m_displacementScale; }
    // 平面の一辺（m）。**ジオメトリだけがメートルで、テクスチャは無次元のまま**
    // （1 UV が何 m かは決めない）。地形の実寸はここで決まる。
    float& PlaneSize() { return m_planeSize; }
    float PlaneSize() const { return m_planeSize; }
    // ハイトの範囲（height 0 / 0.5 / 1 の枠）を描くか。設定は AppSettings が持ち、
    // Application が毎フレーム写す。深度テストするためレンダラ側で描く。
    bool& ShowHeightGuide() { return m_showHeightGuide; }
    // マスクのプレビューで、0 か 1 に張り付いた所へ斜線を引くか（設定）。
    bool& MaskSaturationHatch() { return m_maskSaturationHatch; }
    bool MaskSaturationHatch() const { return m_maskSaturationHatch; }
    // いまマスクをプレビューしているか。Application が毎フレーム写す。
    bool& MaskPreviewActive() { return m_maskPreviewActive; }
    const compositor::MaterialEvaluator& Evaluator() const { return m_evaluator; }
    // 直前のフレームの描画の量。
    const RenderStats& Stats() const { return m_stats; }
    uint32_t MaterialResolution() const { return m_materialResolution; }
    void RequestMaterialResolution(uint32_t resolution) { m_requestedMaterialResolution = resolution; }
    // 平面メッシュの分割数。作り直しは GPU 待機を伴うのでフレームの外で行う
    // （`ProcessPendingWork`）。合成解像度と同じ作法。
    uint32_t MeshSubdivisions() const { return m_meshSubdivisions; }
    void RequestMeshSubdivisions(uint32_t subdivisions) {
        m_requestedMeshSubdivisions = subdivisions;
    }

    // 表示用テクスチャを PNG に書き出す。フレームの外で呼ぶこと。
    bool SaveOutputToPng(rhi::Device& device, const std::filesystem::path& path);

    bool HasOutput() const { return m_output.IsValid(); }
    D3D12_GPU_DESCRIPTOR_HANDLE OutputHandle() const { return m_output.srv.gpu; }
    uint32_t Width() const { return m_width; }
    uint32_t Height() const { return m_height; }

private:
    // 適用中の天球から環境マップを作り直す。HDRI の読み込みに失敗したら
    // 手続き的な空へ落とす（アセットの中身は書き換えない）。
    void ApplyActiveSky(rhi::Device& device, rhi::PipelineCache& pipelineCache);

    const Mesh& CurrentMesh() const;
    // ライトから見たビュー×投影。プレビューの被写体を囲む平行投影。
    DirectX::XMMATRIX LightViewProjection() const;
    void ReleaseTargets(rhi::Device& device);
    // ハイトの範囲の枠。トーンマップ後の表示用テクスチャへ、
    // シーンの深度でテストしながらラインを描く（平面のときだけ）。
    void DrawHeightGuideOverlay(rhi::Device& device, rhi::PipelineCache& pipelineCache,
                                ID3D12GraphicsCommandList* commandList);

    Mesh m_plane;

    rhi::GpuTexture m_sceneColor;  // 線形 HDR
    // 被写界深度を掛けた結果。**トーンマップはこちらを読む。**
    // 元のシーンカラーを潰さないので、掛けるかどうかを毎フレーム選べる。
    rhi::GpuTexture m_sceneColorDof;
    // メッシュ描画の 2 枚目のターゲット。xy: マテリアル UV、z: メッシュに当たったか。
    // ビューポートのカーソル位置からマテリアルの UV を引くために使う。
    rhi::GpuTexture m_materialUv;
    rhi::GpuTexture m_depth;
    rhi::GpuTexture m_output;  // トーンマップ後の表示用
    // ディレクショナルライトから見た深度。ビューポートの大きさとは無関係に固定。
    rhi::GpuTexture m_shadowMap;

    Camera m_camera;
    ExposureSettings m_exposure;
    LightSettings m_light;
    MaterialSettings m_material;
    Environment m_environment;
    compositor::MaterialEvaluator m_evaluator;
    // ビューポートに適用している天球の中身。**Environment の元になっているもの。**
    // 既定値は Environment::Initialize が作る環境と一致させてあるので、
    // 起動直後は作り直しが要らない。
    SkyDefinition m_activeSky;
    TonemapMode m_tonemap = kPreviewDefaults.tonemap;
    DebugView m_debugView = DebugView::Shaded;
    bool m_skyLuminanceRebuildRequested = false;
    float m_displacementScale = kPreviewDefaults.displacementScale;
    float m_planeSize = kPreviewDefaults.planeSize;
    uint32_t m_materialResolution = kPreviewDefaults.materialResolution;
    uint32_t m_requestedMaterialResolution = kPreviewDefaults.materialResolution;
    uint32_t m_meshSubdivisions = kPreviewDefaults.meshSubdivisions;
    uint32_t m_requestedMeshSubdivisions = kPreviewDefaults.meshSubdivisions;
    bool m_useMaterialTextures = kPreviewDefaults.useMaterialTextures;
    bool m_showSkybox = kPreviewDefaults.showSkybox;
    bool m_skyboxBlur = kPreviewDefaults.skyboxBlur;
    bool m_shadowEnabled = kPreviewDefaults.shadowEnabled;
    DofSettings m_dof;
    RenderStats m_stats;
    bool m_tessellationEnabled = kPreviewDefaults.tessellationEnabled;
    float m_tessellationFactor = kPreviewDefaults.tessellationFactor;
    bool m_showHeightGuide = false;
    bool m_maskSaturationHatch = kPreviewDefaults.maskSaturationHatch;
    bool m_maskPreviewActive = false;
    bool m_skyRebuildRequested = false;
    // Environment がいま持っている HDRI。較正倍率だけを掛け直せるかの判断に使う。
    std::filesystem::path m_loadedHdriPath;

    uint32_t m_width = 0;
    uint32_t m_height = 0;
};

}  // namespace tg::renderer
