#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

// メッシュの足跡（MeshFootprintStore）。
//
// 道路などのメッシュはアプリ（CPU）が組み立て、合成の評価器は知らない。Mask Mesh
// （と、この先の地形の均し）がメッシュの形を読めるように、三角形を地形平面の正規化 UV と
// 正規化ハイトへ写したものをここへ置き、評価器が op から引く。ペイントマスクと同じく
// **ID は変わらないのに中身が変わる**ので、中身のハッシュが変わったときだけ世代が進む。
// 評価器はハッシュを op のハッシュへ混ぜて焼き直しを決め、アプリは世代でスタックを
// 改版する。
//
// UI / D3D12 には依存しない（単体テストから使う）。
namespace tg::compositor {

// 頂点。u / v は地形平面の正規化座標（0〜1。地形の外へはみ出してもよい）、
// height は正規化ハイト（0.5 がワールドの 0 m。CpuHeightfield と同じ換算）。
struct MeshFootprintVertex {
    float u = 0.0f;
    float v = 0.0f;
    float height = 0.5f;
};

// 1 つのキーに属する三角形の集まり（鎖の路面と路肩をまとめたもの）。
struct MeshFootprint {
    std::vector<MeshFootprintVertex> vertices;
    std::vector<uint32_t> indices;  // 3 つで 1 枚
    // 中身のハッシュ。Set が計算する（座標は少し丸めて、浮動小数の揺れで世代が進まないようにする）。
    uint64_t hash = 0;

    size_t TriangleCount() const { return indices.size() / 3; }
};

// 足跡のハッシュ。頂点は 1/65536（UV）と 1/4096（ハイト）へ丸めてから混ぜる。
uint64_t HashMeshFootprint(const MeshFootprint& footprint);

class MeshFootprintStore {
public:
    // 足跡を置く / 置き換える。中身（ハッシュ）が前と同じなら何もせず偽を返す。
    // 変わったら世代を進めて真を返す。
    bool Set(uint32_t key, MeshFootprint footprint);
    // 取り除く。あれば世代を進めて真を返す。
    bool Remove(uint32_t key);
    // 無ければ nullptr。
    const MeshFootprint* Find(uint32_t key) const;
    // 中身の世代。足跡を置く / 置き換える / 取り除くたびに進む。
    uint64_t Revision() const { return m_revision; }
    size_t Count() const { return m_entries.size(); }
    std::vector<uint32_t> Keys() const;

private:
    std::unordered_map<uint32_t, MeshFootprint> m_entries;
    uint64_t m_revision = 1;
};

}  // namespace tg::compositor
