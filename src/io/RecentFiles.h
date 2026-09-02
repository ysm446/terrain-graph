#pragma once

#include <filesystem>
#include <vector>

namespace tg::io {

// 最近開いたプロジェクトの履歴。
//
// プロジェクトの中身ではなくアプリ側の状態なので、`.tgproj` には入れず、
// **`%LOCALAPPDATA%/terrain-graph/recent.json`** に置く。
// 作業ディレクトリに置くと、exe の場所を変えるたびに履歴が分かれてしまう。
class RecentFiles {
public:
    // 保持する件数。多すぎるとメニューが縦に伸びて選びにくい。
    static constexpr size_t kMaxEntries = 10;

    // 起動時に 1 回読む。ファイルが無ければ空のまま。
    void Load();

    // 先頭へ入れる。すでにあれば先頭へ引き上げる（重複は作らない）。
    // 追加のたびに書き出すので、異常終了しても履歴は残る。
    void Add(const std::filesystem::path& path);
    // 開けなくなったものを外す。
    void Remove(const std::filesystem::path& path);
    void Clear();

    // 新しい順。
    const std::vector<std::filesystem::path>& Entries() const { return m_entries; }

private:
    bool Save() const;

    std::vector<std::filesystem::path> m_entries;
};

}  // namespace tg::io
