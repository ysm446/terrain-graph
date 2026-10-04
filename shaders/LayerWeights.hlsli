// Surface ごとの重み（合成結果のどの画素を、どの Surface がどれだけ塗ったか）。
//
// 合成結果の色とは別に、R32_UINT の 1 枚へ「上位 3 つの Surface の ID と重み」を詰めて持つ。
// 地形の描画が近景マテリアルを貼り直すときに、どの素材をどの割合で混ぜるかをここから読む
// （docs/design/near-material.md）。
//
//   bit  0〜 5: ID 0（一番重い Surface）
//   bit  6〜11: ID 1
//   bit 12〜17: ID 2
//   bit 18〜24: ID 0 の重み（0〜127）
//   bit 25〜31: ID 1 の重み（0〜127）
//   ID 2 の重みは 1 − 残り（合計はいつも 1）。
//
// ID は 1〜63（地形グラフの Surface を下から数えた番号。C++ 側の MaterialEvaluator と揃える）。
// 0 は「Surface なし」。

#ifndef TG_LAYER_WEIGHTS_HLSLI
#define TG_LAYER_WEIGHTS_HLSLI

static const uint kLayerIdMask = 63u;
static const float kLayerWeightSteps = 127.0f;

struct LayerWeights
{
    uint3 ids;
    float3 weights;  // 重い順。合計 1
};

LayerWeights DecodeLayerWeights(uint packed)
{
    LayerWeights result;
    result.ids = uint3(packed & kLayerIdMask, (packed >> 6) & kLayerIdMask, (packed >> 12) & kLayerIdMask);
    const float w0 = float((packed >> 18) & 127u) / kLayerWeightSteps;
    const float w1 = float((packed >> 25) & 127u) / kLayerWeightSteps;
    result.weights = float3(w0, w1, saturate(1.0f - w0 - w1));
    return result;
}

uint EncodeLayerWeights(LayerWeights value)
{
    // 重い順に並べ直す（3 つなので比較 3 回）。
    if (value.weights.y > value.weights.x)
    {
        value.weights.xy = value.weights.yx;
        value.ids.xy = value.ids.yx;
    }
    if (value.weights.z > value.weights.y)
    {
        value.weights.yz = value.weights.zy;
        value.ids.yz = value.ids.zy;
    }
    if (value.weights.y > value.weights.x)
    {
        value.weights.xy = value.weights.yx;
        value.ids.xy = value.ids.yx;
    }
    const float total = max(value.weights.x + value.weights.y + value.weights.z, 1e-6f);
    const uint q0 = min(uint(value.weights.x / total * kLayerWeightSteps + 0.5f), 127u);
    const uint q1 = min(uint(value.weights.y / total * kLayerWeightSteps + 0.5f), 127u - q0);
    return (value.ids.x & kLayerIdMask) | ((value.ids.y & kLayerIdMask) << 6) |
           ((value.ids.z & kLayerIdMask) << 12) | (q0 << 18) | (q1 << 25);
}

// 1 つの Surface だけが塗っている状態。
uint SingleLayerWeights(uint id)
{
    return (id & kLayerIdMask) | (127u << 18);
}

// Surface id を重み weight で上に重ねる。それまでの重みは (1 − weight) 倍になる。
// 4 つ目が入るときは、一番軽いものを落として残りを合計 1 へ直す。
uint BlendLayerWeights(uint packed, uint id, float weight)
{
    LayerWeights value = DecodeLayerWeights(packed);
    value.weights *= 1.0f - weight;
    if (value.ids.x == id)
    {
        value.weights.x += weight;
    }
    else if (value.ids.y == id)
    {
        value.weights.y += weight;
    }
    else if (value.ids.z == id)
    {
        value.weights.z += weight;
    }
    else if (weight > value.weights.z)
    {
        // 重い順に並んでいるので、落とすのは z。
        value.ids.z = id;
        value.weights.z = weight;
    }
    return EncodeLayerWeights(value);
}

#endif  // TG_LAYER_WEIGHTS_HLSLI
