// terrain_graph_mcp.exe — 起動中の terrain-graph を LLM から操作する MCP サーバ（stdio）。
//
// MCP クライアント（Claude Code など）とは標準入出力で 1 行 1 つの JSON-RPC をやりとりし、
// ツールの呼び出しを名前付きパイプ（\\.\pipe\terrain-graph）でアプリへ渡す。アプリ側の受け口は
// src/app/RemoteServer.cpp、要求の中身は src/app/ApplicationRemote.cpp。仕様は docs/reference/remote-control.md。
//
// 使い方: terrain_graph_mcp.exe [--pipe <name>]
// アプリの「設定 > 遠隔操作 > 有効」を入れておく（か、アプリを --remote-control で起動する）。
#include <Windows.h>

#include <fcntl.h>
#include <io.h>

#include <nlohmann/json.hpp>

// 撮った PNG を、LLM へ渡す大きさの JPEG に直す（このファイルだけで使う）。
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include <stb_image.h>
#include <stb_image_resize2.h>
#include <stb_image_write.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

using nlohmann::json;

constexpr const char* kServerName = "terrain-graph";
constexpr const char* kDefaultProtocolVersion = "2025-06-18";

// --- 文字列 ---------------------------------------------------------------------

std::wstring Widen(const std::string& text) {
    if (text.empty()) return {};
    const int size = ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<size_t>(size), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), size);
    return wide;
}

std::string Narrow(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string narrow(static_cast<size_t>(size), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), narrow.data(), size, nullptr, nullptr);
    return narrow;
}

std::string Base64(const std::vector<unsigned char>& data) {
    static const char kTable[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    for (size_t i = 0; i < data.size(); i += 3) {
        const uint32_t a = data[i];
        const uint32_t b = i + 1 < data.size() ? data[i + 1] : 0;
        const uint32_t c = i + 2 < data.size() ? data[i + 2] : 0;
        const uint32_t triple = (a << 16) | (b << 8) | c;
        out += kTable[(triple >> 18) & 63];
        out += kTable[(triple >> 12) & 63];
        out += i + 1 < data.size() ? kTable[(triple >> 6) & 63] : '=';
        out += i + 2 < data.size() ? kTable[triple & 63] : '=';
    }
    return out;
}

std::string Dump(const json& value, int indent = -1) {
    return value.dump(indent, ' ', false, json::error_handler_t::replace);
}

// --- アプリへのパイプ -------------------------------------------------------------

class AppPipe {
public:
    explicit AppPipe(std::wstring name) : m_name(std::move(name)) {}
    ~AppPipe() { Close(); }

    // 1 つの要求を送り、返事の JSON を返す。繋がらない / 途中で切れたら error に理由。
    std::optional<json> Call(const std::string& method, const json& params, std::string& error) {
        const json request = {{"id", ++m_nextId}, {"method", method}, {"params", params}};
        const std::string line = Dump(request) + "\n";
        // 切れていたら 1 回だけ繋ぎ直す（アプリを起動し直したときなど）。
        for (int attempt = 0; attempt < 2; ++attempt) {
            if (m_pipe == INVALID_HANDLE_VALUE && !Open(error)) return std::nullopt;
            std::string reply;
            if (Write(line) && ReadLine(reply)) {
                json parsed = json::parse(reply, nullptr, false);
                if (parsed.is_discarded()) {
                    error = "アプリの返事を読めませんでした";
                    return std::nullopt;
                }
                return parsed;
            }
            Close();
        }
        error = "アプリとの接続が切れました（アプリが終了したか、遠隔操作を切った）";
        return std::nullopt;
    }

private:
    bool Open(std::string& error) {
        const std::wstring path = L"\\\\.\\pipe\\" + m_name;
        for (int attempt = 0; attempt < 20; ++attempt) {
            m_pipe = ::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (m_pipe != INVALID_HANDLE_VALUE) return true;
            if (::GetLastError() != ERROR_PIPE_BUSY) break;
            // ほかのクライアントが繋いでいる。空くまで少し待つ。
            ::WaitNamedPipeW(path.c_str(), 500);
        }
        error = "terrain-graph に繋がりません（\\\\.\\pipe\\" + Narrow(m_name) +
                "）。アプリを起動し、設定 > 遠隔操作 > 有効 を入れてください（ほかのクライアントが繋いでいる場合は、それを閉じる）";
        return false;
    }

    void Close() {
        if (m_pipe != INVALID_HANDLE_VALUE) ::CloseHandle(m_pipe);
        m_pipe = INVALID_HANDLE_VALUE;
        m_buffer.clear();
    }

    bool Write(const std::string& data) {
        size_t written = 0;
        while (written < data.size()) {
            DWORD bytes = 0;
            if (!::WriteFile(m_pipe, data.data() + written, static_cast<DWORD>(data.size() - written), &bytes, nullptr)) return false;
            written += bytes;
        }
        return true;
    }

    bool ReadLine(std::string& line) {
        char chunk[64 * 1024];
        while (true) {
            if (const size_t newline = m_buffer.find('\n'); newline != std::string::npos) {
                line = m_buffer.substr(0, newline);
                m_buffer.erase(0, newline + 1);
                return true;
            }
            DWORD bytes = 0;
            if (!::ReadFile(m_pipe, chunk, sizeof(chunk), &bytes, nullptr) || bytes == 0) return false;
            m_buffer.append(chunk, bytes);
        }
    }

    std::wstring m_name;
    HANDLE m_pipe = INVALID_HANDLE_VALUE;
    std::string m_buffer;
    uint64_t m_nextId = 0;
};

// --- ツールの定義 ----------------------------------------------------------------

json Schema(json properties = json::object(), json required = json::array()) {
    json schema = {{"type", "object"}, {"properties", std::move(properties)}};
    if (!required.empty()) schema["required"] = std::move(required);
    return schema;
}

const json kNodeReference = {{"description", "ノード ID（数）。apply_ops の中では、同じ操作列の create_node で付けた key（文字列）も使える"},
                             {"type", json::array({"integer", "string"})}};

json Tools() {
    json tools = json::array();
    const auto add = [&](const char* name, const char* description, json schema, bool readOnly) {
        tools.push_back({{"name", name}, {"description", description}, {"inputSchema", std::move(schema)},
                         {"annotations", {{"readOnlyHint", readOnly}}}});
    };
    add("get_status",
        "起動中の terrain-graph の状態。開いているシーン、編集中のグラフ（terrain / cloud）、未保存の項目、アンドゥの段数、"
        "選択中のノード、評価が落ち着いているか、地形の実寸（一辺 sizeMeters・標高差 heightMeters・最低標高）、カメラ。"
        "作業の最初に呼ぶ。",
        Schema(), true);
    add("get_graph",
        "ノードグラフ。既定は短い形: ノードは id・kind・名前・メモの先頭、リンクは [出力ノード, 出力ピン名, 入力ノード, "
        "入力ピン名] の配列、とグループ。detail: true でノードごとの入出力ピン（型と繋がっている先）・位置・メモの全文。"
        "設定の中身は get_node（か include_settings）で見る。",
        Schema({{"component", {{"type", "string"}, {"enum", {"terrain", "cloud", "all"}}, {"description", "どのグラフか（既定 all）"}}},
                {"detail", {{"type", "boolean"}, {"description", "詳しい形（ピンの型・位置・メモの全文）。大きくなるので nodes で絞って使う"}}},
                {"include_settings", {{"type", "boolean"}, {"description", "各ノードの設定（保存と同じ形）も入れる（詳しい形になる）。大きくなる"}}},
                {"nodes", {{"type", "array"}, {"items", {{"type", "integer"}}}, {"description", "このノードだけ"}}}}),
        true);
    add("get_node",
        "ノード 1 つの詳細。settings（保存と同じ形の設定。set_params にはこの形の一部を渡す）、parameters（設定のキーごとの"
        "表示名・型・UI の範囲 min/max・単位・説明・今の値）、入出力ピン、エラー、道路の統計。",
        Schema({{"node", {{"type", "integer"}}},
                {"parameters", {{"type", "boolean"}, {"description", "UI の行（範囲・単位）を付ける（既定 true）"}}},
                {"path_region", {{"type", "array"}, {"items", {{"type", "number"}}},
                                 {"description", "path / roadPath の点とエッジのうち、この範囲 [u0, v0, u1, v1] の中のものを"
                                                 "保存と同じ形で出す（regionPoints / regionEdges）。既定は鎖ごとの要約だけ"}}},
                {"full_settings", {{"type", "boolean"}, {"description", "パスの点とエッジ・経路探索の内部点も省かずに出す"
                                                                        "（数千になることがある）"}}}},
               json::array({"node"})),
        true);
    add("list_node_kinds",
        "置けるノードの種類の一覧（kind・表示名・説明・置けるグラフ・入出力ピンの名前と型）。リンクは同じ型どうしだけ繋がる。",
        Schema({{"component", {{"type", "string"}, {"enum", {"terrain", "cloud"}}, {"description", "このグラフに置ける種類だけ"}}},
                {"include_defaults", {{"type", "boolean"}, {"description", "既定の設定と列挙の値も入れる"}}}}),
        true);
    add("describe_node_kind",
        "ノードの種類 1 つの資料（ノードカタログの 1 項目）。既定の設定、列挙のキーと取れる値、parameters（キーごとの表示名・"
        "型・UI の範囲・単位・説明）。初めて使う種類は、これで設定のキーを確かめてから create_node / set_params する。",
        Schema({{"kind", {{"type", "string"}, {"description", "保存名（例: maskNoise, surface, path）"}}}}, json::array({"kind"})), true);

    json opSchema = {
        {"type", "object"},
        {"properties",
         {{"op", {{"type", "string"},
                  {"enum", {"create_node", "delete_node", "set_params", "connect", "disconnect", "set_bypass", "set_note",
                            "move_node", "add_path", "set_path_points", "set_path_style", "delete_path_elements", "select",
                            "arrange"}}}},
          {"node", kNodeReference}}},
        {"required", json::array({"op"})},
        {"additionalProperties", true}};
    add("apply_ops",
        "グラフを編集する操作の列を、UI と同じ処理で順に実行する。**全部成功するか、何も変えないか**のどちらか"
        "（途中で失敗したら操作列の前へ戻し、どの操作がなぜ失敗したかを返す）。1 回の呼び出しがアンドゥの 1 段になり、"
        "シーンは未保存になる（保存はしない）。\n"
        "操作（op）:\n"
        "- create_node {kind, key?, component?: terrain|cloud, position?: [x, y], name?, settings?, note?, bypass?} — key を付けると"
        "同じ操作列の後の操作から node に key（文字列）で指せる。position を省くとそのグラフの右へ並べる\n"
        "- delete_node {node}\n"
        "- set_params {node, settings} — settings は get_node の settings と同じ形で、変えるキーだけ書く（上書きで重ねる。"
        "配列は丸ごと置き換え、null は既定値へ戻す）。知らないキー・使えない列挙名・UI の範囲の外の値は失敗\n"
        "- connect {from: [node, 出力ピン名], to: [node, 入力ピン名]} — ピン名を省くと最初のピン。入力に既にある接続は置き換わる。"
        "型違い・循環は失敗\n"
        "- disconnect {to: [node, 入力ピン名]} か {link: リンク ID}\n"
        "- set_bypass {node, bypass}, set_note {node, note}, move_node {node, position: [x, y]}\n"
        "- add_path {node, points: [[u, v], …] か [{u, v, width?, feather?, intensity?, heightOffset?}, …], closed?, "
        "connect_from?: 既存の点 ID, connect_to?: 既存の点 ID, style?} — path / roadPath ノードに折れ線を足す。u, v は地形の"
        "正規化座標（0〜1、u が東 +X、v が南 +Z）。style は {curve: line|quadratic|cubic|clothoid, rounding, clothoidRatio, "
        "width, feather, intensity, meander, meanderWavelength, route: none|road|flow|trail, maxGrade, ridgeWeight, avoidWeight}。"
        "route を付けた鎖は recompute_routes で経路を計算する\n"
        "- set_path_points {node, points: [{id, u?, v?, width?, feather?, intensity?, heightOffset?}, …]}\n"
        "- set_path_style {node, edges: [エッジ ID…] か \"all\", style}\n"
        "- delete_path_elements {node, points?: [点 ID…], edges?: [エッジ ID…]}\n"
        "- select {node, preview?: false} — 操作の後、UI でそのノードを選ぶ（プロパティに出る）。preview: true でビューポートの表示もそのノードへ移す\n"
        "- arrange {mode: overlaps|all} — ノードの重なりを解消 / 全体を左から右へ並べ直す",
        Schema({{"ops", {{"type", "array"}, {"items", opSchema}, {"minItems", 1}}}}, json::array({"ops"})), false);
    add("get_evaluation",
        "評価の結果（--evaluate-report と同じ形）。既定では評価が落ち着くまで待つ。errors（循環、道路を作れない、見つからない"
        "テクスチャ、直近の apply_ops の後に出たエラーのログ）、warnings、terrain（標高の範囲）、counts（ノード・リンク・"
        "配置の数・メッシュの三角形数）、scatters（配置の候補と残った数）、エラーか統計のあるノード。",
        Schema({{"wait", {{"type", "boolean"}, {"description", "評価が落ち着くまで待つ（既定 true）"}}},
                {"timeout_seconds", {{"type", "number"}, {"description", "待つ上限（既定 120）"}}},
                {"full", {{"type", "boolean"}, {"description", "全ノードを入れる"}}}}),
        true);
    add("screenshot",
        "画面を撮って画像で返す。target: viewport（UI なしの 3D ビュー、既定）か window（UI 込みのウィンドウ全体）。"
        "既定では評価が落ち着くまで待ってから撮る。見る場所は set_camera で決める。",
        Schema({{"target", {{"type", "string"}, {"enum", {"viewport", "window"}}}},
                {"max_size", {{"type", "integer"}, {"description", "viewport の長辺の上限 px（既定 1600、0 で原寸）"}}},
                {"wait", {{"type", "boolean"}, {"description", "評価が落ち着くまで待つ（既定 true）"}}},
                {"timeout_seconds", {{"type", "number"}}},
                {"image_max_size", {{"type", "integer"}, {"description", "返す画像（JPEG）の長辺の上限 px（既定 1568）。ファイルは原寸の PNG のまま"}}},
                {"path", {{"type", "string"}, {"description", "書き出す PNG の絶対パス（省くと一時フォルダ）"}}}}),
        true);
    add("get_camera", "ビューポートのカメラ（注視点・目の位置・距離・向き・画角）。座標は m で、X が東、Y が上、Z が南、"
                      "原点は地形の中心。", Schema(), true);
    add("set_camera",
        "ビューポートのカメラを動かす（使う人の画面も動く）。target_uv（地形の上の注視点）か target [x, y, z]、"
        "eye_uv + eye_height_m（地形からの高さ）か eye [x, y, z] で目の位置から向きと距離を決める。distance / yaw_degrees / "
        "pitch_degrees / fov_degrees で直接も決められる。frame_terrain で地形全体を収める。bookmark で保存済みの視点（0〜9）。",
        Schema({{"target_uv", {{"type", "array"}, {"items", {{"type", "number"}}}}},
                {"target", {{"type", "array"}, {"items", {{"type", "number"}}}}},
                {"eye_uv", {{"type", "array"}, {"items", {{"type", "number"}}}}},
                {"eye_height_m", {{"type", "number"}}},
                {"eye", {{"type", "array"}, {"items", {{"type", "number"}}}}},
                {"distance", {{"type", "number"}}},
                {"yaw_degrees", {{"type", "number"}}},
                {"pitch_degrees", {{"type", "number"}}},
                {"fov_degrees", {{"type", "number"}}},
                {"frame_terrain", {{"type", "boolean"}}},
                {"bookmark", {{"type", "integer"}}}}),
        false);
    add("sample_terrain",
        "地形の正規化 UV（0〜1）の点の標高（m）・傾斜（度）・ワールド座標。評価器の 512² の縮小ハイトから引く。",
        Schema({{"points", {{"type", "array"}, {"items", {{"type", "array"}, {"items", {{"type", "number"}}}}},
                            {"description", "[[u, v], …]"}}}},
               json::array({"points"})),
        true);
    add("recompute_routes",
        "path / roadPath ノードの、経路探索（route）を付けた鎖の経路を計算し直す（UI の「経路を計算」と同じ）。"
        "既定は古い（両端が動いた / 未計算）鎖だけ、force で全部。Base に地形が繋がっている必要がある。",
        Schema({{"node", {{"type", "integer"}}}, {"force", {{"type", "boolean"}}}}, json::array({"node"})), false);
    add("undo", "編集を戻す（Ctrl+Z と同じ）。apply_ops 1 回が 1 段。", Schema({{"steps", {{"type", "integer"}}}}), false);
    add("redo", "戻した編集をやり直す（Ctrl+Y と同じ）。", Schema({{"steps", {{"type", "integer"}}}}), false);
    add("save_scene",
        "シーンを保存する（Ctrl+S と同じ。ファイルを上書きする）。**使う人が保存を頼んだときだけ使う。**"
        "まだ保存先の無いシーンは保存できない。",
        Schema(), false);
    add("list_assets", "読み込み済みのマテリアル・モデル・テクスチャ（id・名前・パス）。ノードの設定では参照をこの id で書く。",
        Schema({{"kind", {{"type", "string"}, {"enum", {"all", "material", "model", "texture"}}}}}), true);
    add("get_log", "アプリのログ（ステータスバーに流れる通知）。since より後の通し番号のものだけ。",
        Schema({{"since", {{"type", "integer"}}}, {"level", {{"type", "string"}, {"enum", {"info", "warn", "error"}}}}}), true);
    return tools;
}

constexpr const char* kInstructions =
    "terrain-graph（ハイトマップとマスクをノードグラフで組んで地形を作るアプリ）を、起動中のまま操作する。"
    "編集はアプリの UI と同じ処理を通り、アンドゥに積まれ、シーンは未保存になる（使う人が見て、残すかどうかを決める）。\n"
    "進め方: get_status → get_graph で今のグラフを見る → 使う種類を list_node_kinds / describe_node_kind で確かめる → "
    "apply_ops でまとめて編集 → get_evaluation でエラーと数を確かめる → screenshot で見た目を確かめる。"
    "失敗した apply_ops は何も変えないので、理由を読んで直して出し直す。\n"
    "座標: パスの点などは地形の正規化 UV（0〜1。u が東、v が南）。ワールド座標は m で原点が地形の中心、X 東・Y 上・Z 南。"
    "ノードの position はグラフの画面上の座標（評価には効かない）。\n"
    "保存（save_scene）は使う人が頼んだときだけ。";

// --- ツールの呼び出し --------------------------------------------------------------

std::filesystem::path TempScreenshotPath() {
    wchar_t buffer[MAX_PATH] = {};
    const DWORD length = ::GetTempPathW(MAX_PATH, buffer);
    std::filesystem::path directory = (length > 0 ? std::filesystem::path(buffer) : std::filesystem::path(L".")) / L"terrain-graph-mcp";
    std::error_code ignored;
    std::filesystem::create_directories(directory, ignored);
    const std::time_t now = std::time(nullptr);
    std::tm local = {};
    ::localtime_s(&local, &now);
    wchar_t name[64] = {};
    std::wcsftime(name, 64, L"shot_%Y%m%d_%H%M%S", &local);
    static int counter = 0;
    return directory / (std::wstring(name) + L"_" + std::to_wstring(++counter) + L".png");
}

// PNG を長辺 maxSize 以下に縮めた JPEG にする。原寸の PNG はウィンドウ全体だと数 MB になり、
// そのまま渡すと会話の文脈を圧迫する。読めなければ空。
std::vector<unsigned char> ToJpeg(const std::vector<unsigned char>& png, int maxSize, int& width, int& height) {
    int channels = 0;
    stbi_uc* pixels = stbi_load_from_memory(png.data(), static_cast<int>(png.size()), &width, &height, &channels, 3);
    if (pixels == nullptr) return {};
    std::vector<unsigned char> resized;
    const unsigned char* source = pixels;
    if (maxSize > 0 && std::max(width, height) > maxSize) {
        const double scale = static_cast<double>(maxSize) / std::max(width, height);
        const int newWidth = std::max(1, static_cast<int>(width * scale + 0.5));
        const int newHeight = std::max(1, static_cast<int>(height * scale + 0.5));
        resized.resize(static_cast<size_t>(newWidth) * newHeight * 3);
        stbir_resize_uint8_srgb(pixels, width, height, 0, resized.data(), newWidth, newHeight, 0, STBIR_RGB);
        source = resized.data();
        width = newWidth;
        height = newHeight;
    }
    std::vector<unsigned char> jpeg;
    stbi_write_jpg_to_func(
        [](void* context, void* data, int size) {
            auto* out = static_cast<std::vector<unsigned char>*>(context);
            out->insert(out->end(), static_cast<unsigned char*>(data), static_cast<unsigned char*>(data) + size);
        },
        &jpeg, width, height, 3, source, 85);
    stbi_image_free(pixels);
    return jpeg;
}

json TextContent(const std::string& text) {
    return {{"type", "text"}, {"text", text}};
}

json CallTool(AppPipe& pipe, const std::string& name, json arguments) {
    if (!arguments.is_object()) arguments = json::object();
    const json tools = Tools();
    bool known = false;
    for (const json& tool : tools) known |= tool["name"] == name;
    if (!known) {
        return {{"content", json::array({TextContent("知らないツールです: " + name)})}, {"isError", true}};
    }
    if (name == "screenshot" && !arguments.contains("path")) arguments["path"] = Narrow(TempScreenshotPath().wstring());

    std::string error;
    const std::optional<json> reply = pipe.Call(name, arguments, error);
    if (!reply) return {{"content", json::array({TextContent(error)})}, {"isError", true}};
    if (const auto failure = reply->find("error"); failure != reply->end()) {
        std::string text = failure->contains("message") && (*failure)["message"].is_string()
                               ? (*failure)["message"].get<std::string>() : Dump(*failure);
        if (failure->contains("data")) text += "\n" + Dump((*failure)["data"], 2);
        return {{"content", json::array({TextContent(text)})}, {"isError", true}};
    }
    const json result = reply->contains("result") ? (*reply)["result"] : json();
    json content = json::array({TextContent(Dump(result))});
    if (name == "screenshot" && result.contains("path") && result["path"].is_string()) {
        std::ifstream file(std::filesystem::path(Widen(result["path"].get<std::string>())), std::ios::binary);
        const std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        int width = 0, height = 0;
        const int maxSize = arguments.contains("image_max_size") && arguments["image_max_size"].is_number_integer()
                                ? std::clamp(arguments["image_max_size"].get<int>(), 64, 4096) : 1568;
        const std::vector<unsigned char> jpeg = bytes.empty() ? bytes : ToJpeg(bytes, maxSize, width, height);
        if (!jpeg.empty()) content.push_back({{"type", "image"}, {"data", Base64(jpeg)}, {"mimeType", "image/jpeg"}});
    }
    return {{"content", std::move(content)}, {"isError", false}};
}

void Send(const json& message) {
    const std::string line = Dump(message) + "\n";
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fflush(stdout);
}

void SendError(const json& id, int code, const std::string& message) {
    Send({{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}});
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    std::wstring pipeName = L"terrain-graph";
    for (int i = 1; i < argc; ++i) {
        const std::wstring argument = argv[i];
        if (argument == L"--pipe" && i + 1 < argc) pipeName = argv[++i];
    }
    // 改行を CRLF に変えない（1 行 1 メッセージの区切りを崩さない）。
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);

    AppPipe pipe(pipeName);
    std::string line;
    while (std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        const json message = json::parse(line, nullptr, false);
        if (message.is_discarded() || !message.is_object()) {
            SendError(nullptr, -32700, "Parse error");
            continue;
        }
        const bool request = message.contains("id");
        const json id = request ? message["id"] : json();
        const std::string method = message.contains("method") && message["method"].is_string()
                                       ? message["method"].get<std::string>() : std::string();
        const json params = message.contains("params") ? message["params"] : json::object();
        if (!request) continue;  // 通知（notifications/initialized など）には返事をしない

        if (method == "initialize") {
            const std::string version = params.contains("protocolVersion") && params["protocolVersion"].is_string()
                                            ? params["protocolVersion"].get<std::string>() : kDefaultProtocolVersion;
            Send({{"jsonrpc", "2.0"}, {"id", id},
                  {"result", {{"protocolVersion", version},
                              {"capabilities", {{"tools", json::object()}}},
                              {"serverInfo", {{"name", kServerName}, {"version", TG_APP_VERSION}}},
                              {"instructions", kInstructions}}}});
        } else if (method == "ping") {
            Send({{"jsonrpc", "2.0"}, {"id", id}, {"result", json::object()}});
        } else if (method == "tools/list") {
            Send({{"jsonrpc", "2.0"}, {"id", id}, {"result", {{"tools", Tools()}}}});
        } else if (method == "tools/call") {
            const std::string name = params.contains("name") && params["name"].is_string() ? params["name"].get<std::string>() : "";
            const json arguments = params.contains("arguments") ? params["arguments"] : json::object();
            Send({{"jsonrpc", "2.0"}, {"id", id}, {"result", CallTool(pipe, name, arguments)}});
        } else {
            SendError(id, -32601, "Method not found: " + method);
        }
    }
    return 0;
}
