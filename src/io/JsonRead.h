#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <limits>
#include <string>

// --- JSON の読み取り（例外を投げない） ------------------------------------
//
// 型が食い違っていたら既定値に落とす。手で編集されたファイルでも落ちないようにする。
// nlohmann::json の value() は型違いで例外を投げるので、ユーザーが編集するファイルの
// 読み取りには使わず、こちらを通す。

namespace tg::io {

inline const nlohmann::json* FindMember(const nlohmann::json& node, const char* key) {
    const auto it = node.find(key);
    return (it != node.end()) ? &(*it) : nullptr;
}

inline float ReadFloat(const nlohmann::json& node, const char* key, float fallback) {
    const nlohmann::json* member = FindMember(node, key);
    return (member != nullptr && member->is_number()) ? member->get<float>() : fallback;
}

inline double ReadDouble(const nlohmann::json& node, const char* key, double fallback) {
    const nlohmann::json* member = FindMember(node, key);
    return (member != nullptr && member->is_number()) ? member->get<double>() : fallback;
}

// int に収まらない値は黙って切り詰めず、既定値に落とす。
inline int ReadInt(const nlohmann::json& node, const char* key, int fallback) {
    const nlohmann::json* member = FindMember(node, key);
    if (member == nullptr || !member->is_number_integer()) {
        return fallback;
    }
    constexpr int64_t kMin = std::numeric_limits<int>::min();
    constexpr int64_t kMax = std::numeric_limits<int>::max();
    if (member->is_number_unsigned()) {
        const uint64_t value = member->get<uint64_t>();
        return (value > static_cast<uint64_t>(kMax)) ? fallback : static_cast<int>(value);
    }
    const int64_t value = member->get<int64_t>();
    return (value < kMin || value > kMax) ? fallback : static_cast<int>(value);
}

inline uint32_t ReadUInt(const nlohmann::json& node, const char* key, uint32_t fallback) {
    const nlohmann::json* member = FindMember(node, key);
    if (member == nullptr || !member->is_number_integer()) {
        return fallback;
    }
    const int64_t value = member->get<int64_t>();
    return (value < 0) ? fallback : static_cast<uint32_t>(value);
}

inline bool ReadBool(const nlohmann::json& node, const char* key, bool fallback) {
    const nlohmann::json* member = FindMember(node, key);
    return (member != nullptr && member->is_boolean()) ? member->get<bool>() : fallback;
}

inline std::string ReadString(const nlohmann::json& node, const char* key,
                              const std::string& fallback = {}) {
    const nlohmann::json* member = FindMember(node, key);
    return (member != nullptr && member->is_string()) ? member->get<std::string>() : fallback;
}

}  // namespace tg::io
