#include "graph/RoadMesh.h"

#include <algorithm>
#include <cmath>

namespace tg::graph {
namespace {
using namespace DirectX;

// 隣り合う区間の向きの内積がこれ未満の角は断る（約 75° より急。断面が重なって裏返る）。
constexpr float kMinCornerDot = 0.25f;
// 頂点数の上限（1 本の道路）。
constexpr size_t kMaxRoadVertices = 4u * 1024u * 1024u;
}  // namespace

uint32_t RoadMeshStride(const RoadMeshSettings& settings) {
    const float width = std::clamp(settings.widthMeters, kRoadMinWidthMeters, kRoadMaxWidthMeters);
    return static_cast<uint32_t>(std::max(1, static_cast<int>(std::ceil(width)))) + 1;
}

bool BuildRoadMesh(const RoadPathSettings& road, const RoadProfileCurve& centerline,
                   const RoadMeshSettings& settings, renderer::MeshData& out, std::string* error) {
    const auto fail = [&](const char* message) { if (error) *error = message; return false; };
    out.vertices.clear();
    out.indices.clear();
    const auto& points = centerline.points;
    const size_t rows = points.size();
    if (rows < 2) return fail("中心線の点が足りません");
    const float width = std::clamp(settings.widthMeters, kRoadMinWidthMeters, kRoadMaxWidthMeters);
    // 幅方向は約 1 m ごと。
    const int columns = std::max(1, static_cast<int>(std::ceil(width)));
    const size_t stride = static_cast<size_t>(columns) + 1;
    if (rows * stride > kMaxRoadVertices) return fail("道路が長すぎます（頂点が多すぎる）");

    // 区間の水平な向き。
    std::vector<XMFLOAT2> directions(rows - 1);
    for (size_t i = 0; i + 1 < rows; ++i) {
        const float dx = points[i + 1].x - points[i].x;
        const float dz = points[i + 1].z - points[i].z;
        const float length = std::hypot(dx, dz);
        if (length < 1e-5f) return fail("中心線に長さ 0 の区間があります");
        directions[i] = {dx / length, dz / length};
    }

    out.vertices.resize(rows * stride);
    for (size_t i = 0; i < rows; ++i) {
        // 頂点の向きは前後の区間の平均。角では断面を 1/cos だけ広げて幅を保つ（マイター）。
        const XMFLOAT2 before = directions[i == 0 ? 0 : i - 1];
        const XMFLOAT2 after = directions[i + 1 == rows ? rows - 2 : i];
        const float turn = before.x * after.x + before.y * after.y;
        if (turn < kMinCornerDot) return fail("中心線の角が急すぎます（曲線にするか点を足してください）");
        XMFLOAT2 forward{before.x + after.x, before.y + after.y};
        const float length = std::hypot(forward.x, forward.y);
        forward = {forward.x / length, forward.y / length};
        const float miter = 1.0f / std::max(forward.x * after.x + forward.y * after.y, 0.5f);
        // 進行方向に向かって右（Road Path と同じ。右手系 Y-up で (-dz, 0, dx)）。
        const XMFLOAT3 right{-forward.y, 0.0f, forward.x};
        const float distance = centerline.arcLengths[i];
        const float bank = road.bankEnabled ? EvaluateBankAngleRadians(road, centerline, distance) : 0.0f;
        // 正のバンクで左（-right）が上がる。横位置 l（右が正）の点は right*cos - up*sin だけずれる。
        const float c = std::cos(bank);
        const float s = std::sin(bank);
        for (int k = 0; k <= columns; ++k) {
            const float lateral = -0.5f * width + width * static_cast<float>(k) / static_cast<float>(columns);
            const float horizontal = lateral * c * miter;
            renderer::MeshVertex& v = out.vertices[i * stride + static_cast<size_t>(k)];
            v.position = {points[i].x + right.x * horizontal,
                          points[i].y - lateral * s + settings.surfaceOffsetMeters,
                          points[i].z + right.z * horizontal};
            v.normal = {0.0f, 0.0f, 0.0f};
            // 接線は U（左 → 右）の向き。従法線 cross(N, T) が進行方向（+V）になるので w = +1。
            v.tangent = {right.x * c, -s, right.z * c, 1.0f};
            v.uv = {lateral + 0.5f * width, distance};
        }
    }

    // 三角形と、面の法線の積み上げ。表は上（+Y）。
    out.indices.reserve((rows - 1) * static_cast<size_t>(columns) * 6);
    for (size_t i = 0; i + 1 < rows; ++i) {
        for (int k = 0; k < columns; ++k) {
            const uint32_t a = static_cast<uint32_t>(i * stride + static_cast<size_t>(k));
            const uint32_t b = a + 1;                                 // 右隣
            const uint32_t d = static_cast<uint32_t>(a + stride);     // 前
            const uint32_t e = d + 1;
            const XMVECTOR pa = XMLoadFloat3(&out.vertices[a].position);
            const XMVECTOR across = XMVectorSubtract(XMLoadFloat3(&out.vertices[b].position), pa);
            const XMVECTOR along = XMVectorSubtract(XMLoadFloat3(&out.vertices[d].position), pa);
            const XMVECTOR face = XMVector3Cross(across, along);
            if (XMVectorGetY(face) <= 1e-7f) return fail("路面が裏返る所があります（バンク角が大きすぎるか、角が急すぎます）");
            for (const uint32_t index : {a, b, d, e}) {
                XMFLOAT3& n = out.vertices[index].normal;
                XMStoreFloat3(&n, XMVectorAdd(XMLoadFloat3(&n), face));
            }
            out.indices.insert(out.indices.end(), {a, d, b, b, d, e});
        }
    }
    for (renderer::MeshVertex& v : out.vertices) {
        XMStoreFloat3(&v.normal, XMVector3Normalize(XMLoadFloat3(&v.normal)));
        // 接線を法線と直交させる（バンクで傾いた面でも U の向きのまま）。
        const XMVECTOR n = XMLoadFloat3(&v.normal);
        XMVECTOR t = XMVectorSet(v.tangent.x, v.tangent.y, v.tangent.z, 0.0f);
        t = XMVector3Normalize(XMVectorSubtract(t, XMVectorScale(n, XMVectorGetX(XMVector3Dot(n, t)))));
        v.tangent = {XMVectorGetX(t), XMVectorGetY(t), XMVectorGetZ(t), 1.0f};
    }
    return true;
}

std::vector<RoadSectionPoint> ShoulderSectionPoints(const RoadShoulderSettings& settings) {
    std::vector<RoadSectionPoint> points{{0.0f, 0.0f}};
    if (settings.shape == RoadShoulderShape::Section) {
        points.insert(points.end(), settings.section.begin(), settings.section.end());
        return points;
    }
    // 勾配の形: 段差があれば面取りの点、外側の端。
    const float width = std::clamp(settings.widthMeters, kShoulderMinWidthMeters, kShoulderMaxWidthMeters);
    const float drop = settings.crossSlopePercent * 0.01f;
    const float step = std::max(settings.stepHeightMeters, 0.0f);
    if (step > 0.0f) {
        const float stepLateral = std::min(std::max(settings.stepWidthMeters, 0.005f), width * 0.5f);
        points.push_back({stepLateral, -(step + drop * stepLateral)});
    }
    points.push_back({width, -(step + drop * width)});
    return points;
}

bool ValidateShoulderSection(const std::vector<RoadSectionPoint>& section, std::string* error) {
    const auto fail = [&](const char* message) { if (error) *error = message; return false; };
    if (section.empty()) return fail("断面の点がありません");
    if (section.size() > kShoulderMaxSectionPoints) return fail("断面の点が多すぎます");
    RoadSectionPoint previous{0.0f, 0.0f};
    float previousDx = 0.0f, previousDy = 0.0f, previousLength = 0.0f;
    for (const RoadSectionPoint& point : section) {
        if (!std::isfinite(point.acrossMeters) || !std::isfinite(point.heightMeters)) return fail("断面の点が不正です");
        if (std::abs(point.heightMeters) > kShoulderMaxSectionHeight) return fail("断面の高さが大きすぎます");
        const float dx = point.acrossMeters - previous.acrossMeters;
        const float dy = point.heightMeters - previous.heightMeters;
        if (dx < -1e-5f) return fail("断面の点は外へ向かう順に並べてください（外への距離を減らさない）");
        const float length = std::hypot(dx, dy);
        if (length < 0.005f) return fail("断面の点が近すぎます（5 mm 未満）");
        // 縦の面で折り返す（上がってすぐ下がる）と面が重なる。
        if (previousLength > 0.0f && (dx * previousDx + dy * previousDy) < -0.95f * length * previousLength)
            return fail("断面が折り返しています");
        previous = point;
        previousDx = dx;
        previousDy = dy;
        previousLength = length;
    }
    if (section.back().acrossMeters < kShoulderMinWidthMeters) return fail("断面の外側の端が内側の端に近すぎます");
    if (section.back().acrossMeters > kShoulderMaxWidthMeters) return fail("断面の幅が広すぎます");
    return true;
}

float ShoulderSectionLength(const RoadShoulderSettings& settings) {
    const std::vector<RoadSectionPoint> points = ShoulderSectionPoints(settings);
    float length = 0.0f;
    for (size_t i = 1; i < points.size(); ++i)
        length += std::hypot(points[i].acrossMeters - points[i - 1].acrossMeters,
                             points[i].heightMeters - points[i - 1].heightMeters);
    return length;
}

std::vector<RoadShoulderSpan> ShoulderSpans(const RoadShoulderSettings& settings, float lengthMeters) {
    const float length = std::max(lengthMeters, 0.0f);
    std::vector<RoadShoulderSpan> spans{{0.0f, 0.0f, settings.material, settings.uvRepeatMeters, settings.boundaryPath,
                                         settings.boundaryUid}};
    std::vector<RoadShoulderSwitch> switches = settings.switches;
    // NaN を先に 0 へ正してから並べる（NaN が混じると並べ替えの順序が壊れる）。
    for (RoadShoulderSwitch& change : switches) {
        if (!std::isfinite(change.atMeters)) change.atMeters = 0.0f;
        if (!std::isfinite(change.transitionMeters)) change.transitionMeters = 0.0f;
    }
    // 同じ位置は後に足したものを使うため、安定に並べてから前のものを捨てる。
    std::stable_sort(switches.begin(), switches.end(),
                     [](const RoadShoulderSwitch& a, const RoadShoulderSwitch& b) { return a.atMeters < b.atMeters; });
    for (const RoadShoulderSwitch& change : switches) {
        const float at = std::clamp(change.atMeters, 0.0f, length);
        RoadShoulderSpan span{at, std::max(change.transitionMeters, 0.0f),
                              change.material, change.uvRepeatMeters, change.boundaryPath, change.boundaryUid};
        if (spans.back().startMeters >= at) {
            // 同じ位置（か 0 の位置）の切り替えは前の区間を置き換える。最初の区間は移行を持たない。
            const bool first = spans.size() == 1;
            spans.back() = span;
            if (first) spans.back().startMeters = 0.0f, spans.back().transitionMeters = 0.0f;
            continue;
        }
        spans.push_back(std::move(span));
    }
    // 移行距離は前後の区間の長さまで（移行の半分ずつが区間の半分を超えず、隣の移行と重ならない）。
    for (size_t i = 1; i < spans.size(); ++i) {
        const float before = spans[i].startMeters - spans[i - 1].startMeters;
        const float after = (i + 1 < spans.size() ? spans[i + 1].startMeters : length) - spans[i].startMeters;
        spans[i].transitionMeters = std::min({spans[i].transitionMeters, before, after, kShoulderMaxTransitionMeters});
    }
    return spans;
}

std::vector<RoadSectionPoint> ShoulderSectionTemplate(RoadSectionTemplate kind) {
    switch (kind) {
    case RoadSectionTemplate::Sidewalk:
        // 縁石 15 cm で上がり、2 m の歩道（道路へ向かって 2% 下がる）。
        return {{0.0f, 0.15f}, {2.0f, 0.19f}};
    case RoadSectionTemplate::Gutter:
        // 10 cm の縁の外に幅 30 cm・深さ 30 cm の U 字の側溝、その外に 60 cm の土の路肩。
        return {{0.1f, 0.0f}, {0.1f, -0.3f}, {0.4f, -0.3f}, {0.4f, 0.0f}, {1.0f, -0.02f}};
    case RoadSectionTemplate::SoilShoulder:
        // 50 cm ほぼ平らに続き、外の 1 m で 30 cm 下がる。
        return {{0.5f, -0.02f}, {1.5f, -0.3f}};
    }
    return {{1.5f, -0.06f}};
}

bool BuildRoadShoulder(const renderer::MeshData& source, uint32_t sourceStride, uint32_t edgeColumn,
                       uint32_t innerColumn, const RoadShoulderSettings& settings, renderer::MeshData& out,
                       uint32_t& outStride, std::string* error) {
    const auto fail = [&](const char* message) { if (error) *error = message; return false; };
    out.vertices.clear();
    out.indices.clear();
    outStride = 0;
    if (sourceStride < 2 || source.vertices.size() < static_cast<size_t>(sourceStride) * 2 ||
        source.vertices.size() % sourceStride != 0)
        return fail("路肩の元になる面がありません");
    if (edgeColumn >= sourceStride || innerColumn >= sourceStride || edgeColumn == innerColumn)
        return fail("路肩の端の列が不正です");
    if (settings.shape == RoadShoulderShape::Section && !ValidateShoulderSection(settings.section, error)) return false;
    const size_t rows = source.vertices.size() / sourceStride;
    const std::vector<RoadSectionPoint> points = ShoulderSectionPoints(settings);

    // 列。断面の区間を約 1 m ごとに割る。角（向きが約 20° より大きく変わる点）は同じ点の列を 2 つ
    // 重ね、そこで面をつながない（法線を分けて稜線を立てる）。u は断面に沿った長さ。
    struct Column {
        float across = 0, height = 0, u = 0;
        bool joinNext = true;  // 次の列との間に面を張るか
        float tangentAcross = 1, tangentHeight = 0;  // 断面に沿う向き（接線）
    };
    std::vector<Column> columns{{0.0f, 0.0f, 0.0f}};
    float arc = 0.0f;
    for (size_t i = 1; i < points.size(); ++i) {
        const float dx = points[i].acrossMeters - points[i - 1].acrossMeters;
        const float dy = points[i].heightMeters - points[i - 1].heightMeters;
        const float length = std::hypot(dx, dy);
        if (length < 1e-6f) continue;
        if (i > 1) {
            // 前の区間との角。
            const float px = points[i - 1].acrossMeters - points[i - 2].acrossMeters;
            const float py = points[i - 1].heightMeters - points[i - 2].heightMeters;
            const float plen = std::hypot(px, py);
            if (plen > 1e-6f && (px * dx + py * dy) < std::cos(XMConvertToRadians(20.0f)) * plen * length) {
                columns.back().joinNext = false;
                columns.push_back(columns.back());
                columns.back().joinNext = true;
            }
        }
        columns.back().tangentAcross = dx / length;
        columns.back().tangentHeight = dy / length;
        // 1 m をわずかに超えるだけ（勾配で斜めの長さが伸びた分）なら割らない。
        const int steps = std::max(1, static_cast<int>(std::ceil(length - 0.05f)));
        for (int step = 1; step <= steps; ++step) {
            const float t = static_cast<float>(step) / static_cast<float>(steps);
            columns.push_back({points[i - 1].acrossMeters + dx * t, points[i - 1].heightMeters + dy * t,
                               arc + length * t, true, dx / length, dy / length});
        }
        arc += length;
    }
    const uint32_t stride = static_cast<uint32_t>(columns.size());
    if (stride < 2) return fail("路肩の断面に幅がありません");
    if (rows * stride > kMaxRoadVertices) return fail("路肩の頂点が多すぎます");

    // 外向きは、端から内側の列を引いた水平成分。内側の帯の端が縦の面（縁石の外など）なら、
    // 水平に離れた列までさらに内へ辿る。
    const int inward = innerColumn > edgeColumn ? 1 : -1;
    std::vector<XMFLOAT2> outwards(rows);
    for (size_t row = 0; row < rows; ++row) {
        const renderer::MeshVertex& edge = source.vertices[row * sourceStride + edgeColumn];
        float ox = 0.0f, oz = 0.0f, length = 0.0f;
        for (int column = static_cast<int>(innerColumn); column >= 0 && column < static_cast<int>(sourceStride);
             column += inward) {
            const renderer::MeshVertex& inner = source.vertices[row * sourceStride + column];
            ox = edge.position.x - inner.position.x;
            oz = edge.position.z - inner.position.z;
            length = std::hypot(ox, oz);
            if (length >= 1e-4f) break;
        }
        if (length < 1e-4f) return fail("端の幅が 0 の行があります");
        outwards[row] = {ox / length, oz / length};
    }

    out.vertices.resize(rows * stride);
    for (size_t row = 0; row < rows; ++row) {
        const renderer::MeshVertex& edge = source.vertices[row * sourceStride + edgeColumn];
        const float ox = outwards[row].x, oz = outwards[row].y;
        for (uint32_t column = 0; column < stride; ++column) {
            const Column& c = columns[column];
            renderer::MeshVertex& v = out.vertices[row * stride + column];
            v.position = edge.position;
            if (column > 0) {
                v.position.x += ox * c.across;
                v.position.z += oz * c.across;
                v.position.y += c.height;
            }
            v.normal = {0.0f, 0.0f, 0.0f};
            v.tangent = {ox * c.tangentAcross, c.tangentHeight, oz * c.tangentAcross, 1.0f};
            v.uv = {c.u, edge.uv.y};
        }
    }
    // 三角形。左右どちらの端かで列の並びの向きが変わるので、平らな面なら法線が上を向く並びにする。
    // 面は断面の区間の向きから決まる向き（外への距離 da、高さ dh に対し 外向き × (-dh) + 上 × da。
    // 縁石の立ち上がりは道路側を向く）を向くはずで、逆を向けばカーブの内側で裏返っている。
    out.indices.reserve((rows - 1) * (stride - 1) * 6);
    for (size_t row = 0; row + 1 < rows; ++row) {
        const XMVECTOR along = XMVectorSubtract(XMLoadFloat3(&out.vertices[(row + 1) * stride].position),
                                                XMLoadFloat3(&out.vertices[row * stride].position));
        const XMVECTOR outward = XMVectorSet(outwards[row].x, 0.0f, outwards[row].y, 0.0f);
        const bool flip = XMVectorGetY(XMVector3Cross(along, outward)) < 0.0f;
        for (uint32_t column = 0; column + 1 < stride; ++column) {
            if (!columns[column].joinNext) continue;
            const float da = columns[column + 1].across - columns[column].across;
            const float dh = columns[column + 1].height - columns[column].height;
            const XMVECTOR expected = XMVectorSet(outwards[row].x * -dh, da, outwards[row].y * -dh, 0.0f);
            const uint32_t a = static_cast<uint32_t>(row * stride + column);
            const uint32_t tris[2][3] = {{a, a + stride, a + 1}, {a + 1, a + stride, a + stride + 1}};
            for (const auto& tri : tris) {
                uint32_t x = tri[0], y = tri[1], z = tri[2];
                if (flip) std::swap(y, z);
                const XMVECTOR px = XMLoadFloat3(&out.vertices[x].position);
                const XMVECTOR face = XMVector3Cross(XMVectorSubtract(XMLoadFloat3(&out.vertices[y].position), px),
                                                     XMVectorSubtract(XMLoadFloat3(&out.vertices[z].position), px));
                if (XMVectorGetX(XMVector3Dot(face, expected)) <= 1e-9f)
                    return fail("幅に対してカーブが急すぎて路肩が裏返ります");
                for (const uint32_t index : {x, y, z}) {
                    XMFLOAT3& n = out.vertices[index].normal;
                    XMStoreFloat3(&n, XMVectorAdd(XMLoadFloat3(&n), face));
                }
                out.indices.insert(out.indices.end(), {x, y, z});
            }
        }
    }
    // 法線と接線。接線は U（断面に沿って外へ）の向き。従法線 cross(N, T) が道のりの増える向き（+V）に
    // なるよう w を決める（左の路肩では外向きが左なので w = -1 になる）。
    for (size_t row = 0; row < rows; ++row) {
        const size_t next = row + 1 < rows ? row + 1 : row;
        const size_t prev = row + 1 < rows ? row : row - 1;
        for (uint32_t column = 0; column < stride; ++column) {
            renderer::MeshVertex& v = out.vertices[row * stride + column];
            const XMVECTOR n = XMVector3Normalize(XMLoadFloat3(&v.normal));
            XMStoreFloat3(&v.normal, n);
            XMVECTOR t = XMVectorSet(v.tangent.x, v.tangent.y, v.tangent.z, 0.0f);
            t = XMVectorSubtract(t, XMVectorScale(n, XMVectorGetX(XMVector3Dot(n, t))));
            if (XMVectorGetX(XMVector3LengthSq(t)) < 1e-10f) t = XMVectorSet(outwards[row].x, 0.0f, outwards[row].y, 0.0f);
            t = XMVector3Normalize(t);
            const XMVECTOR along = XMVectorSubtract(XMLoadFloat3(&out.vertices[next * stride + column].position),
                                                    XMLoadFloat3(&out.vertices[prev * stride + column].position));
            const float w = XMVectorGetX(XMVector3Dot(XMVector3Cross(n, t), along)) < 0.0f ? -1.0f : 1.0f;
            v.tangent = {XMVectorGetX(t), XMVectorGetY(t), XMVectorGetZ(t), w};
        }
    }
    outStride = stride;
    return true;
}

}  // namespace tg::graph
