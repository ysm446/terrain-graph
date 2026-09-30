#pragma once

#include <nlohmann/json.hpp>

namespace tg::io {

// グラフの JSON（nodes / links）を、保存するときと同じ正規の形に直す。
// - ノードに inputs / outputs が無ければ、定義のピンの数だけ新しい ID を振る。
// - リンクの "from": [ノード ID, ピン名 か 番号] / "to": [...] を start / end のピン ID に直す。
//   ピン名は定義のラベルと、大文字小文字・空白・'_'・'-' を無視して比べる。番号は 0 始まり。
// - id の無いリンクに ID を振る。
// 人や LLM が ID を振らずに書いたグラフを読めるようにするため（保存は常に正規の形）。
// 解決できないリンクは理由を警告して捨てる。nodes が無ければ何もしない。
void NormalizeGraphJson(nlohmann::json& graph);

}  // namespace tg::io
