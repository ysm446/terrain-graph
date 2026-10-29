// 遠隔操作（起動中のアプリを MCP サーバなどから操作する口）の要求の処理。
// パイプの受け口は RemoteServer、要求と返事の形式は docs/reference/remote-control.md。
//
// 方針（docs/plan/plan.md の「LLM がアプリを動かしてノードを組める仕組み」）:
// - **UI と同じ処理を通す。** ノードの作成・接続は NodeGraph の CreateNode / CheckLink / CreateLink、
//   設定は保存と同じ書き出し・読み込み（io::WriteNodeJson / io::ReadNodeJson）、パスは Path の編集操作。
//   検証（循環・型・範囲）も UI と同じになる。
// - **決まった操作だけを受ける。** 知らない操作・キー・範囲外の値は失敗として返す。
// - **1 つの操作列は、まとめて成功するか、何も変えないか。** 失敗したらグラフを操作列の前へ戻す。
// - 編集はフレームの中（UI の描画の後）で行い、アンドゥの 1 段になる。保存はしない（未保存のまま）。
#include "app/Application.h"

#include "app/ApplicationUiHelpers.h"
#include "core/Log.h"
#include "core/PathUtf8.h"
#include "io/ProjectIo.h"

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <DirectXMath.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace tg {
using nlohmann::json;

namespace {

using Clock = std::chrono::steady_clock;

constexpr int kProtocolVersion = 1;
// 評価が落ち着いたとみなすまでに続くフレーム数（--evaluate-report と同じ）。
constexpr uint32_t kSettledFrames = 10;
// フレームの中で進める要求が、いつまでも始まらないとき（ウィジェットを掴みっぱなしなど）の上限。
constexpr double kJobTimeoutSeconds = 600.0;
constexpr float kPi = 3.14159265358979f;

// --- 要求の値の読み取り（例外を投げない） ------------------------------------

const json* Member(const json& object, const char* key) {
    if (!object.is_object()) return nullptr;
    const auto it = object.find(key);
    return it != object.end() ? &*it : nullptr;
}

std::optional<double> NumberOf(const json& object, const char* key) {
    const json* value = Member(object, key);
    if (value == nullptr || !value->is_number()) return std::nullopt;
    return value->get<double>();
}

std::optional<std::string> StringOf(const json& object, const char* key) {
    const json* value = Member(object, key);
    if (value == nullptr || !value->is_string()) return std::nullopt;
    return value->get<std::string>();
}

bool BoolOf(const json& object, const char* key, bool fallback) {
    const json* value = Member(object, key);
    return (value != nullptr && value->is_boolean()) ? value->get<bool>() : fallback;
}

double SecondsSince(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}

template <typename... Args>
std::string Printf(const char* format, Args... args) {
    char buffer[2048];
    std::snprintf(buffer, sizeof(buffer), format, args...);
    return buffer;
}

// [x, y] / [x, y, z] の数の配列。
bool ReadVector(const json* value, size_t size, float* out) {
    if (value == nullptr || !value->is_array() || value->size() != size) return false;
    for (size_t i = 0; i < size; ++i) {
        if (!(*value)[i].is_number()) return false;
        out[i] = (*value)[i].get<float>();
    }
    return true;
}

// get_graph の短い形に載せるメモの長さ（文字数）。
constexpr size_t kNoteSummaryLength = 40;

// UTF-8 の文字列を、文字の途中で切らずに maxChars 文字までにする。切ったら "…" を付ける。
std::string TruncateUtf8(const std::string& text, size_t maxChars) {
    size_t chars = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        // 続きのバイト（10xxxxxx）は文字の頭ではない。
        if ((static_cast<unsigned char>(text[i]) & 0xC0) == 0x80) continue;
        if (chars++ == maxChars) return text.substr(0, i) + "…";
    }
    return text;
}

const char* ComponentName(int component) {
    return component == 1 ? "cloud" : "terrain";
}

// "terrain" / "cloud"（0 / 1 でもよい）。読めなければ -1。
int ParseComponent(const json& value) {
    if (value.is_number_integer()) {
        const int component = value.get<int>();
        return (component == 0 || component == 1) ? component : -1;
    }
    if (value.is_string()) {
        const std::string name = value.get<std::string>();
        if (name == "terrain") return 0;
        if (name == "cloud") return 1;
    }
    return -1;
}

const char* KindName(const graph::Node& node) {
    if (const auto* missing = std::get_if<graph::MissingNodeSettings>(&node.settings)) return missing->kindName.c_str();
    const graph::NodeDefinition* definition = graph::FindNodeDefinition(node.kind);
    return definition != nullptr ? definition->name : "";
}

// ピン名の比べ方（io::NormalizeGraphJson と同じく、大文字小文字・空白・'_'・'-' を無視する）。
std::string NormalizePinName(const std::string& name) {
    std::string normalized;
    for (const char c : name) {
        if (c == ' ' || c == '_' || c == '-') continue;
        normalized += (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    }
    return normalized;
}

// JSON Pointer（"/layer/height/scale"）を "." 区切りのパスへ。
std::string DottedPath(const std::string& pointer) {
    std::string path = pointer.empty() ? pointer : pointer.substr(1);
    std::replace(path.begin(), path.end(), '/', '.');
    return path;
}

bool NearlyEqual(const json& a, const json& b) {
    if (a.is_number() && b.is_number()) {
        const double x = a.get<double>(), y = b.get<double>();
        return std::abs(x - y) <= 1.0e-5 * std::max({1.0, std::abs(x), std::abs(y)});
    }
    return a == b;
}

// 出力用に設定を縮める。経路探索の内部点（waypoints）は導出したもので数千個になり、LLM に渡すと
// 文脈を埋めるので、数（waypointCount）に置き換える。読むだけの表示で、set_params には効かない
// （set_params は縮める前の設定へ重ねる）。
void CompactSettings(json& value) {
    if (value.is_object()) {
        if (const auto waypoints = value.find("waypoints"); waypoints != value.end() && waypoints->is_array()) {
            value["waypointCount"] = waypoints->size() / 2;
            value.erase("waypoints");
        }
        for (auto& [key, child] : value.items()) CompactSettings(child);
    } else if (value.is_array()) {
        for (json& child : value) CompactSettings(child);
    }
}

// 操作の結果のパスの ID 配列。
json IdArray(const std::vector<graph::PathElementId>& ids) {
    json array = json::array();
    for (const graph::PathElementId id : ids) array.push_back(id);
    return array;
}

const char* const kPathCurveNames[] = {"line", "quadratic", "cubic", "clothoid"};
const char* const kPathRouteNames[] = {"none", "road", "flow", "trail"};

template <size_t N>
std::optional<uint32_t> EnumIndex(const char* const (&names)[N], const std::string& value) {
    for (size_t i = 0; i < N; ++i)
        if (value == names[i]) return static_cast<uint32_t>(i);
    return std::nullopt;
}

template <size_t N>
std::string EnumList(const char* const (&names)[N]) {
    std::string list;
    for (size_t i = 0; i < N; ++i) list += (i ? ", " : "") + std::string(names[i]);
    return list;
}

}  // namespace

// 要求の中身。Application の内部を読み書きするので friend にしてある（Application.h の遠隔操作の節）。
class RemoteCommands {
public:
    using Job = Application::RemoteJob;
    using Phase = Application::RemotePhase;

    explicit RemoteCommands(Application& app) : m_app(app) {}

    // 1 つの要求を進める。終わったら真。
    bool Step(Job& job, Phase phase);

private:
    // apply_ops の 1 回ぶんの状態。
    struct OpContext {
        std::map<std::string, graph::GraphId> keys;            // 操作列の中で付けた名前 → ノード ID
        std::map<graph::GraphId, std::set<std::string>> touched;  // 設定を変えたノードと、変えたキー
        std::vector<std::pair<graph::GraphId, bool>> selects;  // 最後に選ぶノードと、プレビューも移すか
        json warnings = json::array();
        int autoPlaced = 0;
        bool arrangeAll = false, arrangeOverlaps = false;
        // 文書（グラフ）を変える操作があったか。選択と並べ直しの要求だけなら、未保存にもアンドゥにもしない
        // （並べ直しは次のフレームにグラフのパネルが行い、そちらで段を積む）。
        bool changed = false;
    };

    // --- 1 回で済む要求（フレームの外） ---
    json Status();
    json Graph(const json& params, std::string& error);
    json NodeKinds(const json& params);
    json Assets(const json& params);
    json Log(const json& params);
    json Camera(const json& params, bool set, std::string& error);
    json SampleTerrain(const json& params, std::string& error);
    // --- フレームの中 ---
    bool NodeDetails(Job& job);
    bool DescribeKind(Job& job);
    bool ApplyOps(Job& job);
    // --- フレームをまたぐ ---
    bool Evaluation(Job& job, Phase phase);
    bool Screenshot(Job& job, Phase phase);
    bool RecomputeRoutes(Job& job, Phase phase);
    bool History(Job& job, Phase phase, int direction);
    bool Save(Job& job, Phase phase);

    // --- 操作 ---
    bool RunOp(const json& op, OpContext& context, json& result, std::string& error);
    bool OpCreateNode(const json& op, OpContext& context, json& result, std::string& error);
    bool OpDeleteNode(const json& op, OpContext& context, json& result, std::string& error);
    bool OpSetParams(const json& op, OpContext& context, json& result, std::string& error);
    bool OpConnect(const json& op, OpContext& context, json& result, std::string& error);
    bool OpDisconnect(const json& op, OpContext& context, json& result, std::string& error);
    bool OpAddPath(const json& op, OpContext& context, json& result, std::string& error);
    bool OpSetPathPoints(const json& op, OpContext& context, json& result, std::string& error);
    bool OpSetPathStyle(const json& op, OpContext& context, json& result, std::string& error);
    bool OpDeletePathElements(const json& op, OpContext& context, json& result, std::string& error);
    // 設定の JSON（保存と同じ形）を上書きで当てる。読まれなかったキー・使えない値は失敗。
    bool ApplySettings(graph::Node& node, const json& patch, OpContext& context, std::string& error);
    // 設定を変えたノードの値が UI の範囲の内側か（フレームの中で、見えないウィンドウで行を描いて調べる）。
    std::string CheckRanges(const OpContext& context);
    // 鎖の設定（ファイルの edges[] と同じキー）を読む。
    bool ReadEdgeStyle(const json& style, graph::PathEdgeStyle& out, std::string& error);

    // --- 参照の解決 ---
    graph::Node* ResolveNode(const json* reference, const OpContext& context, std::string& error);
    const graph::Pin* ResolvePin(const graph::Node& node, graph::PinKind kind, const json* reference,
                                 std::string& error);
    // [ノード, ピン] か ノード だけ（最初のピン）。
    const graph::Pin* ResolvePinReference(const json* reference, graph::PinKind kind, const OpContext& context,
                                          std::string& error);
    graph::PathSettings* PathOf(graph::Node& node, std::string& error);

    // --- 出力 ---
    // compact なら経路の内部点を数に縮め（CompactSettings）、パスの点とエッジを鎖の要約に置き換える
    // （CompactPath。region があればその範囲の点とエッジだけは残す）。
    json NodeSummary(const graph::Node& node, bool settings, bool compact = true,
                     const std::optional<std::array<float, 4>>& region = std::nullopt) const;
    // パスの点とエッジ（数千になる）を、鎖ごとの要約と、範囲の中の点とエッジに置き換える。
    void CompactPath(json& settings, const graph::PathSettings& path,
                     const std::optional<std::array<float, 4>>& region) const;
    // ピンを指す名前（同じ名前のピンが複数あれば "Shape 2" のように番号を付ける。1 始まり）。
    std::string PinName(const graph::Node& node, const graph::Pin& pin) const;
    json PinReference(graph::GraphId pinId) const;
    json CameraJson() const;
    json TerrainJson() const;
    json EvaluationReport(bool timedOut, bool full);

    template <typename F>
    void InProbeWindow(F&& body);

    Application& m_app;
};

// --- Application 側の入口 -----------------------------------------------------

bool Application::RemoteControlWanted() const {
    if (m_options.remoteControl) return true;
    // 開発用の書き出し（スクリーンショット・レポートなど）では、起動引数で頼まれたときだけ開く。
    return m_settings.Remote().enabled && !Headless();
}

void Application::UpdateRemoteServer() {
    const bool wanted = RemoteControlWanted();
    const std::string name = m_options.remotePipe.empty() ? m_settings.Remote().pipeName : m_options.remotePipe;
    const std::wstring pipe(name.begin(), name.end());  // ASCII だけ（設定と起動引数で確かめてある）
    if (m_remote.Running() && (!wanted || pipe != m_remotePipe)) {
        FailRemoteJobs(ui::Tr("遠隔操作の口を閉じました", "The remote control pipe was closed"));
        m_remote.Stop();
        m_remotePipe.clear();
        TG_LOG_INFO(ui::Tr("遠隔操作の口を閉じました", "Closed the remote control pipe"));
    }
    if (!wanted) {
        m_remoteError.clear();
        m_remoteAttemptedPipe.clear();
        return;
    }
    if (m_remote.Running() || pipe == m_remoteAttemptedPipe) return;
    m_remoteAttemptedPipe = pipe;
    if (m_remote.Start(pipe, m_window.Handle())) {
        m_remotePipe = pipe;
        m_remoteError.clear();
        m_remoteSettledFrames = 0;
        TG_LOG_INFO(ui::Tr("遠隔操作の口を開きました: \\\\.\\pipe\\%s", "Opened the remote control pipe: \\\\.\\pipe\\%s"),
                    name.c_str());
    } else {
        m_remoteError = ui::Tr("開けなかった（同じ名前のパイプを別のアプリが開いている）",
                               "Could not open (another app has a pipe with the same name)");
        TG_LOG_WARN(ui::Tr("遠隔操作の口を開けませんでした: \\\\.\\pipe\\%s（別のアプリが同じ名前で開いている）",
                           "Could not open the remote control pipe: \\\\.\\pipe\\%s (another app uses the same name)"),
                    name.c_str());
    }
}

namespace {

// 返事の 1 行。壊れた UTF-8 が混ざっていても例外を出さない（置換文字にする）。
std::string ReplyLine(const json& id, const json* result, const std::string& error, const json& errorData) {
    json reply;
    reply["id"] = id;
    if (result != nullptr) {
        reply["result"] = *result;
    } else {
        reply["error"] = {{"message", error}};
        if (!errorData.is_null()) reply["error"]["data"] = errorData;
    }
    return reply.dump(-1, ' ', false, json::error_handler_t::replace);
}

// apply_ops の操作列を「追加 2・接続 3」のように数える（出てきた順）。
std::string SummarizeOps(const json& params) {
    const json* ops = Member(params, "ops");
    if (ops == nullptr || !ops->is_array()) return {};
    std::vector<std::pair<std::string, int>> counts;
    for (const json& op : *ops) {
        const std::string name = StringOf(op, "op").value_or("?");
        std::string word = name;
        if (name == "create_node") word = ui::Tr("追加", "add");
        else if (name == "delete_node") word = ui::Tr("削除", "delete");
        else if (name == "set_params") word = ui::Tr("設定", "set");
        else if (name == "connect") word = ui::Tr("接続", "connect");
        else if (name == "disconnect") word = ui::Tr("切断", "disconnect");
        else if (name == "set_bypass") word = ui::Tr("バイパス", "bypass");
        else if (name == "set_note") word = ui::Tr("メモ", "note");
        else if (name == "move_node") word = ui::Tr("移動", "move");
        else if (name == "add_path" || name == "set_path_points" || name == "set_path_style" ||
                 name == "delete_path_elements") word = ui::Tr("パス", "path");
        else if (name == "select") word = ui::Tr("選択", "select");
        else if (name == "arrange") word = ui::Tr("整列", "arrange");
        const auto it = std::find_if(counts.begin(), counts.end(), [&](const auto& entry) { return entry.first == word; });
        if (it != counts.end()) ++it->second;
        else counts.emplace_back(word, 1);
    }
    std::string text;
    for (const auto& [word, count] : counts) {
        if (!text.empty()) text += ui::Tr("・", ", ");
        text += count > 1 ? Printf("%s %d", word.c_str(), count) : word;
    }
    return text;
}

// ステータスバーに出す「何をしているか」。知らない操作は名前のまま。
std::string RemoteJobLabel(const std::string& method, const json& params) {
    if (method == "get_status") return ui::Tr("状態を確認", "Checking status");
    if (method == "get_graph") return ui::Tr("グラフを読む", "Reading graph");
    if (method == "get_node") {
        const std::optional<double> node = NumberOf(params, "node");
        return node ? Printf(ui::Tr("ノード #%d の設定を読む", "Reading node #%d"), static_cast<int>(*node))
                    : std::string(ui::Tr("ノードの設定を読む", "Reading node"));
    }
    if (method == "list_node_kinds") return ui::Tr("ノードの種類の一覧を読む", "Listing node kinds");
    if (method == "describe_node_kind")
        return Printf(ui::Tr("ノードの種類 %s を調べる", "Describing node kind %s"),
                      StringOf(params, "kind").value_or("?").c_str());
    if (method == "apply_ops") {
        const std::string summary = SummarizeOps(params);
        return summary.empty() ? std::string(ui::Tr("グラフを編集", "Editing graph"))
                               : Printf(ui::Tr("グラフを編集（%s）", "Editing graph (%s)"), summary.c_str());
    }
    if (method == "get_evaluation")
        return BoolOf(params, "wait", true) ? ui::Tr("評価を待つ", "Waiting for evaluation")
                                            : ui::Tr("評価の結果を読む", "Reading evaluation");
    if (method == "screenshot") return ui::Tr("スクリーンショットを撮る", "Taking screenshot");
    if (method == "get_camera") return ui::Tr("カメラを読む", "Reading camera");
    if (method == "set_camera") return ui::Tr("カメラを動かす", "Moving camera");
    if (method == "sample_terrain") {
        const json* points = Member(params, "points");
        const int count = (points != nullptr && points->is_array()) ? static_cast<int>(points->size()) : 0;
        return Printf(ui::Tr("地形の標高を調べる（%d 点）", "Sampling terrain (%d points)"), count);
    }
    if (method == "recompute_routes") return ui::Tr("経路を計算", "Computing routes");
    if (method == "undo") return ui::Tr("元に戻す", "Undo");
    if (method == "redo") return ui::Tr("やり直す", "Redo");
    if (method == "save_scene") return ui::Tr("シーンを保存", "Saving scene");
    if (method == "list_assets") return ui::Tr("アセットの一覧を読む", "Listing assets");
    if (method == "get_log") return ui::Tr("ログを読む", "Reading log");
    return method;
}

// ステータスバーのホバーに並べる、終わった依頼の数。
constexpr size_t kRemoteActivityLimit = 12;

}  // namespace

void Application::FailRemoteJobs(const char* reason) {
    if (!m_remote.Running()) return;
    for (RemoteServer::Request& request : m_remote.TakeRequests()) {
        const json message = json::parse(request.line, nullptr, false);
        const json id = (message.is_object() && message.contains("id")) ? message["id"] : json();
        m_remote.Reply(request.ticket, ReplyLine(id, nullptr, reason, json()));
    }
    for (const RemoteJob& job : m_remoteJobs) m_remote.Reply(job.ticket, ReplyLine(job.id, nullptr, reason, json()));
    m_remoteJobs.clear();
}

void Application::ProcessRemoteJobs(RemotePhase phase) {
    if (!m_remote.Running()) return;
    // 届いた要求を取り込む。形が読めないものはその場で断る。
    for (RemoteServer::Request& request : m_remote.TakeRequests()) {
        const json message = json::parse(request.line, nullptr, false);
        const json id = (message.is_object() && message.contains("id")) ? message["id"] : json();
        const json* method = Member(message, "method");
        if (message.is_discarded() || !message.is_object() || method == nullptr || !method->is_string()) {
            m_remote.Reply(request.ticket,
                           ReplyLine(id, nullptr, "要求は {\"id\": …, \"method\": \"…\", \"params\": {…}} の JSON で送ってください", json()));
            continue;
        }
        RemoteJob job;
        job.ticket = request.ticket;
        job.id = id;
        job.method = method->get<std::string>();
        const json* params = Member(message, "params");
        job.params = (params != nullptr && params->is_object()) ? *params : json::object();
        job.label = RemoteJobLabel(job.method, job.params);
        m_remoteJobs.push_back(std::move(job));
    }
    RemoteCommands commands(*this);
    for (RemoteJob& job : m_remoteJobs) {
        if (job.finished) continue;
        if (commands.Step(job, phase)) {
            job.finished = true;
        } else if (SecondsSince(job.start) > kJobTimeoutSeconds) {
            job.error = "時間内に処理を始められなかった（ウィジェットを掴んだままになっていないか確かめてください）";
            job.finished = true;
        }
    }
    for (auto it = m_remoteJobs.begin(); it != m_remoteJobs.end();) {
        if (!it->finished) {
            ++it;
            continue;
        }
        const json* errorData = Member(it->scratch, "errorData");
        m_remote.Reply(it->ticket, it->error.empty() ? ReplyLine(it->id, &it->result, {}, json())
                                                     : ReplyLine(it->id, nullptr, it->error, errorData ? *errorData : json()));
        m_remoteActivity.push_back({std::time(nullptr), std::chrono::steady_clock::now(), it->label, it->error});
        if (m_remoteActivity.size() > kRemoteActivityLimit) m_remoteActivity.pop_front();
        it = m_remoteJobs.erase(it);
    }
}

// --- 振り分け -------------------------------------------------------------------

bool RemoteCommands::Step(Job& job, Phase phase) {
    const std::string& method = job.method;
    // フレームの外で 1 回で済むもの。
    const auto once = [&](auto&& body) {
        if (phase != Phase::BeforeFrame) return false;
        std::string error;
        json result = body(error);
        if (!error.empty()) job.error = std::move(error);
        else job.result = std::move(result);
        return true;
    };
    if (method == "get_status") return once([&](std::string&) { return Status(); });
    if (method == "get_graph") return once([&](std::string& error) { return Graph(job.params, error); });
    if (method == "list_node_kinds") return once([&](std::string&) { return NodeKinds(job.params); });
    if (method == "list_assets") return once([&](std::string&) { return Assets(job.params); });
    if (method == "get_log") return once([&](std::string&) { return Log(job.params); });
    if (method == "get_camera") return once([&](std::string& error) { return Camera(job.params, false, error); });
    if (method == "set_camera") return once([&](std::string& error) { return Camera(job.params, true, error); });
    if (method == "sample_terrain") return once([&](std::string& error) { return SampleTerrain(job.params, error); });
    if (method == "get_node") return phase == Phase::InFrame && NodeDetails(job);
    if (method == "describe_node_kind") return phase == Phase::InFrame && DescribeKind(job);
    if (method == "apply_ops") return phase == Phase::InFrame && ApplyOps(job);
    if (method == "get_evaluation") return Evaluation(job, phase);
    if (method == "screenshot") return Screenshot(job, phase);
    if (method == "recompute_routes") return RecomputeRoutes(job, phase);
    if (method == "undo") return History(job, phase, -1);
    if (method == "redo") return History(job, phase, 1);
    if (method == "save_scene") return Save(job, phase);
    job.error = "知らない method です: " + method +
                "（get_status, get_graph, get_node, list_node_kinds, describe_node_kind, apply_ops, "
                "get_evaluation, screenshot, get_camera, set_camera, sample_terrain, recompute_routes, "
                "undo, redo, save_scene, list_assets, get_log）";
    return true;
}

// --- 出力の部品 -------------------------------------------------------------------

std::string RemoteCommands::PinName(const graph::Node& node, const graph::Pin& pin) const {
    const std::vector<graph::Pin>& pins = pin.kind == graph::PinKind::Input ? node.inputs : node.outputs;
    int same = 0, index = 0;
    for (const graph::Pin& other : pins) {
        if (other.label != pin.label) continue;
        ++same;
        if (other.id == pin.id) index = same;
    }
    return same > 1 ? pin.label + " " + std::to_string(index) : pin.label;
}

json RemoteCommands::PinReference(graph::GraphId pinId) const {
    const graph::Pin* pin = m_app.m_graph.FindPin(pinId);
    const graph::Node* node = pin ? m_app.m_graph.FindNode(pin->nodeId) : nullptr;
    if (pin == nullptr || node == nullptr) return json();
    return json::array({node->id, PinName(*node, *pin)});
}

void RemoteCommands::CompactPath(json& settings, const graph::PathSettings& path,
                                 const std::optional<std::array<float, 4>>& region) const {
    // 点とエッジの配列を持つオブジェクト（Path は "path"、Road Path も同じ形）を探す。
    json* target = nullptr;
    for (auto& [key, value] : settings.items()) {
        if (value.is_object() && value.contains("points") && value.contains("edges") && value.contains("nextId")) target = &value;
    }
    if (target == nullptr) return;
    const float size = m_app.m_renderer.PlaneSize();
    const std::vector<graph::PathStrand> strands = graph::BuildPathStrands(path);
    constexpr size_t kMaxStrands = 200;
    json summary = json::array();
    for (size_t i = 0; i < strands.size() && i < kMaxStrands; ++i) {
        const graph::PathStrand& strand = strands[i];
        const graph::PathPoint* first = strand.points.empty() ? nullptr : path.FindPoint(strand.points.front());
        const graph::PathPoint* last = strand.points.empty() ? nullptr : path.FindPoint(strand.points.back());
        const graph::PathEdge* edge = strand.edges.empty() ? nullptr : path.FindEdge(strand.edges.front());
        if (first == nullptr || last == nullptr || edge == nullptr) continue;
        double length = 0.0;
        for (size_t j = 1; j < strand.points.size(); ++j) {
            const graph::PathPoint* a = path.FindPoint(strand.points[j - 1]);
            const graph::PathPoint* b = path.FindPoint(strand.points[j]);
            if (a && b) length += std::hypot(b->u - a->u, b->v - a->v) * size;
        }
        json item = {{"start", {{"id", first->id}, {"uv", {first->u, first->v}}}},
                     {"end", {{"id", last->id}, {"uv", {last->u, last->v}}}},
                     {"points", strand.points.size()}, {"firstEdge", edge->id}, {"edges", strand.edges.size()},
                     {"lengthMeters", length}, {"curve", kPathCurveNames[static_cast<size_t>(edge->curve) % 4]},
                     {"widthMeters", edge->overrideValues ? edge->widthMeters : first->widthMeters}};
        if (strand.closed) item["closed"] = true;
        if (edge->route != graph::PathRoute::None) item["route"] = kPathRouteNames[static_cast<size_t>(edge->route) % 4];
        summary.push_back(std::move(item));
    }
    json compacted = {{"pointCount", path.points.size()}, {"edgeCount", path.edges.size()},
                      {"strandCount", strands.size()}, {"strands", std::move(summary)},
                      {"note", "点とエッジは鎖の要約に置き換えてある。範囲の点とエッジは get_node の path_region、"
                               "全部は full_settings で見る"}};
    for (const char* key : {"defaultWidth", "defaultFeather", "defaultIntensity", "nextId"})
        if (target->contains(key)) compacted[key] = (*target)[key];
    if (region) {
        // 範囲の中の点と、その点に付いているエッジ（保存と同じ形）。
        const auto& [u0, v0, u1, v1] = *region;
        std::set<graph::PathElementId> inside;
        json points = json::array(), edges = json::array();
        for (const json& point : (*target)["points"]) {
            const float u = point.value("u", -1.0f), v = point.value("v", -1.0f);
            if (u < std::min(u0, u1) || u > std::max(u0, u1) || v < std::min(v0, v1) || v > std::max(v0, v1)) continue;
            inside.insert(point.value("id", 0));
            points.push_back(point);
        }
        for (const json& edge : (*target)["edges"])
            if (inside.contains(edge.value("from", 0)) || inside.contains(edge.value("to", 0))) edges.push_back(edge);
        compacted["regionPoints"] = std::move(points);
        compacted["regionEdges"] = std::move(edges);
    }
    *target = std::move(compacted);
}

json RemoteCommands::NodeSummary(const graph::Node& node, bool settings, bool compact,
                                 const std::optional<std::array<float, 4>>& region) const {
    json item;
    item["id"] = node.id;
    item["kind"] = KindName(node);
    if (const graph::NodeDefinition* definition = graph::FindNodeDefinition(node.kind);
        definition != nullptr && node.kind != graph::NodeKind::Missing) {
        item["title"] = definition->title;
    }
    item["component"] = ComponentName(node.component);
    if (const auto* layer = std::get_if<graph::LayerNodeSettings>(&node.settings)) item["name"] = layer->layer.name;
    item["position"] = {node.posX, node.posY};
    if (graph::IsBypassed(node)) item["bypass"] = true;
    if (!node.note.empty()) item["note"] = node.note;
    json inputs = json::array(), outputs = json::array();
    for (const graph::Pin& pin : node.inputs) {
        json entry = {{"name", PinName(node, pin)}, {"type", io::ValueTypeName(pin.valueType)}};
        for (const graph::Link& link : m_app.m_graph.Links()) {
            if (link.endPin == pin.id) {
                entry["from"] = PinReference(link.startPin);
                break;
            }
        }
        inputs.push_back(std::move(entry));
    }
    for (const graph::Pin& pin : node.outputs) {
        outputs.push_back({{"name", PinName(node, pin)}, {"type", io::ValueTypeName(pin.valueType)}});
    }
    item["inputs"] = std::move(inputs);
    item["outputs"] = std::move(outputs);
    if (settings) {
        item["settings"] = io::WriteNodeJson(node);
        if (compact) {
            CompactSettings(item["settings"]);
            if (const graph::PathSettings* path = graph::EditablePathSettings(node)) CompactPath(item["settings"], *path, region);
        }
    }
    if (const graph::PathSettings* path = graph::EditablePathSettings(node)) {
        item["pathStats"] = {{"points", path->points.size()}, {"edges", path->edges.size()},
                             {"strands", graph::BuildPathStrands(*path).size()}};
    }
    return item;
}

json RemoteCommands::CameraJson() const {
    const renderer::Camera& camera = m_app.m_renderer.GetCamera();
    const renderer::CameraState state = camera.State();
    const DirectX::XMFLOAT3 eye = camera.Position();
    const float size = std::max(m_app.m_renderer.PlaneSize(), 1.0e-3f);
    return {
        {"target", {state.target.x, state.target.y, state.target.z}},
        {"targetUv", {state.target.x / size + 0.5f, state.target.z / size + 0.5f}},
        {"position", {eye.x, eye.y, eye.z}},
        {"distance", state.distance},
        // 回し続けると角度が溜まるので、-180〜180 度へ畳んで出す。
        {"yawDegrees", RadiansToDegrees(std::remainder(state.yaw, 2.0f * kPi))},
        {"pitchDegrees", RadiansToDegrees(state.pitch)},
        {"fovDegrees", RadiansToDegrees(state.fovY)},
    };
}

json RemoteCommands::TerrainJson() const {
    json terrain;
    terrain["sizeMeters"] = m_app.m_renderer.PlaneSize();
    terrain["heightMeters"] = m_app.m_renderer.DisplacementScale();
    terrain["baseElevationMeters"] = m_app.TerrainBaseElevation();
    for (const graph::Node& node : m_app.m_graph.Nodes()) {
        if (node.kind != graph::NodeKind::Output) continue;
        if (const graph::TerrainScale* scale = m_app.m_graph.FindChainScale(node.id); scale && scale->hasLocation)
            terrain["location"] = {{"latitude", scale->latitude}, {"longitude", scale->longitude}};
        break;
    }
    terrain["heightfieldReady"] = m_app.m_renderer.Evaluator().Heightfield().IsValid();
    return terrain;
}

template <typename F>
void RemoteCommands::InProbeWindow(F&& body) {
    // 画面の外のウィンドウで描く（RunPropertyProbes と同じ。透明にすると ImGui が中身ごと飛ばす）。
    ImGui::SetNextWindowPos(ImVec2(-20000.0f, -20000.0f));
    ImGui::SetNextWindowSize(ImVec2(720.0f, 540.0f));
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                                        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBringToFrontOnFocus |
                                        ImGuiWindowFlags_NoBackground;
    if (ImGui::Begin("##remoteProbe", nullptr, kFlags)) body();
    ImGui::End();
}

// --- 1 回で済む要求 -------------------------------------------------------------

json RemoteCommands::Status() {
    Application& app = m_app;
    json status;
    status["app"] = {{"name", "terrain-graph"}, {"version", TG_APP_VERSION}, {"protocol", kProtocolVersion}};
    status["scene"] = app.m_projectPath.empty() ? json() : json(ToUtf8Portable(app.m_projectPath));
    status["root"] = ToUtf8Portable(app.m_workspace.Root());
    if (app.m_componentPreview >= 0) status["previewingGraph"] = ToUtf8Portable(app.m_componentPreviewPath);
    status["editing"] = app.m_editComponent == 1 ? "cloud" : app.m_editComponent == 0 ? "terrain" : "all";
    status["unsaved"] = app.UnsavedItemNames();
    status["undo"] = {{"undo", app.m_undoHistory.UndoCount()}, {"redo", app.m_undoHistory.RedoCount()}};
    status["selectedNode"] = app.m_selectedGraphNode;
    status["previewNode"] = app.m_previewGraphNode;
    status["evaluation"] = {{"busy", !app.m_remoteEvaluationIdle},
                            {"settled", app.m_remoteSettledFrames >= kSettledFrames}};
    status["terrain"] = TerrainJson();
    status["counts"] = {{"nodes", app.m_graph.Nodes().size()}, {"links", app.m_graph.Links().size()}};
    status["camera"] = CameraJson();
    status["logSequence"] = app.m_remoteLogSequence;
    return status;
}

json RemoteCommands::Graph(const json& params, std::string& error) {
    int component = -1;  // 全部
    if (const json* value = Member(params, "component"); value != nullptr && !(value->is_string() && value->get<std::string>() == "all")) {
        component = ParseComponent(*value);
        if (component < 0) {
            error = "component は \"terrain\" / \"cloud\" / \"all\" のどれか";
            return {};
        }
    }
    const bool settings = BoolOf(params, "include_settings", false);
    std::set<graph::GraphId> only;
    if (const json* nodes = Member(params, "nodes"); nodes != nullptr && nodes->is_array()) {
        for (const json& id : *nodes) if (id.is_number_integer()) only.insert(id.get<int>());
    }
    // 既定は短い形（ノードは id・種類・名前・メモの先頭、リンクは 1 行の配列）。百数十ノードのグラフでも
    // LLM が一度に読める大きさにする。ピンの型・位置・つながりの詳細は detail（か get_node）で見る。
    const bool detail = BoolOf(params, "detail", false) || settings;
    std::set<graph::GraphId> included;
    json nodes = json::array();
    for (const graph::Node& node : m_app.m_graph.Nodes()) {
        if (component >= 0 && node.component != component) continue;
        if (!only.empty() && !only.contains(node.id)) continue;
        included.insert(node.id);
        if (detail) {
            nodes.push_back(NodeSummary(node, settings));
            continue;
        }
        json item = {{"id", node.id}, {"kind", KindName(node)}};
        if (component < 0) item["component"] = ComponentName(node.component);
        if (const auto* layer = std::get_if<graph::LayerNodeSettings>(&node.settings)) item["name"] = layer->layer.name;
        if (graph::IsBypassed(node)) item["bypass"] = true;
        if (!node.note.empty()) item["note"] = TruncateUtf8(node.note, kNoteSummaryLength);
        nodes.push_back(std::move(item));
    }
    json links = json::array();
    for (const graph::Link& link : m_app.m_graph.Links()) {
        const graph::Pin* start = m_app.m_graph.FindPin(link.startPin);
        const graph::Pin* end = m_app.m_graph.FindPin(link.endPin);
        if (start == nullptr || end == nullptr) continue;
        if (!included.contains(start->nodeId) && !included.contains(end->nodeId)) continue;
        if (detail) {
            links.push_back({{"id", link.id}, {"from", PinReference(link.startPin)}, {"to", PinReference(link.endPin)}});
        } else {
            // [出力ノード, 出力ピン, 入力ノード, 入力ピン]
            json from = PinReference(link.startPin), to = PinReference(link.endPin);
            if (from.is_null() || to.is_null()) continue;
            links.push_back(json::array({from[0], from[1], to[0], to[1]}));
        }
    }
    json groups = json::array();
    for (const graph::NodeGroup& group : m_app.m_graph.Groups()) {
        if (component >= 0 && group.component != component) continue;
        groups.push_back({{"id", group.id}, {"name", group.name}, {"component", ComponentName(group.component)},
                          {"rect", {group.x, group.y, group.width, group.height}}, {"collapsed", group.collapsed}});
    }
    const std::vector<graph::GraphId>& cycle = m_app.GraphCycleNodes();
    json result = {{"nodes", std::move(nodes)}, {"links", std::move(links)}, {"groups", std::move(groups)}};
    if (!detail) {
        result["format"] = "短い形。links は [出力ノード, 出力ピン, 入力ノード, 入力ピン]、note は先頭だけ。"
                           "ピンの型・位置・メモの全文は detail: true か get_node で見る";
    }
    if (!cycle.empty()) result["cycleNodes"] = cycle;
    return result;
}

json RemoteCommands::NodeKinds(const json& params) {
    int component = -1;
    if (const json* value = Member(params, "component")) component = ParseComponent(*value);
    const bool defaults = BoolOf(params, "include_defaults", false);
    json kinds = json::array();
    const json catalog = io::NodeCatalog();
    for (const json& entry : catalog["nodes"]) {
        const std::string kind = entry["kind"].get<std::string>();
        const graph::NodeDefinition* definition = graph::FindNodeDefinitionByName(kind);
        if (definition == nullptr) continue;
        json graphs = json::array();
        if (NodeKindOffered(definition->kind, 0)) graphs.push_back("terrain");
        if (NodeKindOffered(definition->kind, 1)) graphs.push_back("cloud");
        if (component >= 0 && !NodeKindOffered(definition->kind, component)) continue;
        json item = {{"kind", kind}, {"title", entry["title"]}, {"description", entry["description"]},
                     {"graphs", std::move(graphs)}, {"inputs", entry["inputs"]}, {"outputs", entry["outputs"]}};
        if (entry.contains("variadicInputs")) item["variadicInputs"] = true;
        if (defaults) {
            item["defaults"] = entry["defaults"];
            if (entry.contains("enums")) item["enums"] = entry["enums"];
        }
        kinds.push_back(std::move(item));
    }
    return {{"kinds", std::move(kinds)}, {"valueTypes", catalog["valueTypes"]}};
}

json RemoteCommands::Assets(const json& params) {
    const std::string kind = StringOf(params, "kind").value_or("all");
    const std::filesystem::path& root = m_app.m_workspace.Root();
    const auto relative = [&root](const std::filesystem::path& path) {
        if (path.empty()) return json();
        const std::filesystem::path inside = path.lexically_relative(root);
        return json(ToUtf8Portable(inside.empty() || inside.native().starts_with(L"..") ? path : inside));
    };
    json result = json::object();
    if (kind == "all" || kind == "material") {
        json materials = json::array();
        for (const compositor::MaterialAsset& asset : m_app.m_materialLibrary.Entries()) {
            json item = {{"id", asset.id}, {"name", asset.name}, {"path", relative(asset.assetPath)}};
            if (asset.layerMaterial) item["layered"] = true;
            if (!asset.layerError.empty()) item["error"] = asset.layerError;
            materials.push_back(std::move(item));
        }
        result["materials"] = std::move(materials);
    }
    if (kind == "all" || kind == "model") {
        json models = json::array();
        for (const renderer::ModelAsset& model : m_app.m_models) {
            json item = {{"id", model.id}, {"name", model.name}, {"path", relative(model.assetPath)}};
            if (!model.error.empty()) item["error"] = model.error;
            models.push_back(std::move(item));
        }
        result["models"] = std::move(models);
    }
    if (kind == "all" || kind == "texture") {
        json textures = json::array();
        for (const compositor::LibraryTexture& texture : m_app.m_textureLibrary.Entries()) {
            json item = {{"id", texture.id}, {"name", texture.name}, {"path", relative(texture.path)}};
            if (texture.missing) item["missing"] = true;
            textures.push_back(std::move(item));
        }
        result["textures"] = std::move(textures);
    }
    result["note"] = "読み込み済みのアセットだけ。ノードの設定では参照をこの id で書く";
    return result;
}

json RemoteCommands::Log(const json& params) {
    const uint64_t since = static_cast<uint64_t>(std::max(0.0, NumberOf(params, "since").value_or(0.0)));
    const std::string level = StringOf(params, "level").value_or("info");
    json entries = json::array();
    for (const Application::RemoteLogEntry& entry : m_app.m_remoteLog) {
        if (entry.sequence <= since) continue;
        if (level == "warn" && entry.level == LogLevel::Info) continue;
        if (level == "error" && entry.level != LogLevel::Error) continue;
        entries.push_back({{"sequence", entry.sequence},
                           {"level", entry.level == LogLevel::Error ? "error" : entry.level == LogLevel::Warn ? "warn" : "info"},
                           {"message", entry.text}});
    }
    return {{"entries", std::move(entries)}, {"sequence", m_app.m_remoteLogSequence}};
}

json RemoteCommands::Camera(const json& params, bool set, std::string& error) {
    renderer::Camera& camera = m_app.m_renderer.GetCamera();
    if (set) {
        renderer::CameraState state = camera.State();
        const float size = m_app.m_renderer.PlaneSize();
        if (const json* bookmark = Member(params, "bookmark")) {
            const int index = bookmark->is_number_integer() ? bookmark->get<int>() : -1;
            const auto& bookmarks = m_app.m_renderer.GetCameraBookmarks();
            if (index < 0 || index >= renderer::kCameraBookmarkCount || !bookmarks[static_cast<size_t>(index)]) {
                error = "bookmark は保存済みのブックマークの番号（0〜9）";
                return {};
            }
            state = *bookmarks[static_cast<size_t>(index)];
        }
        if (BoolOf(params, "frame_terrain", false)) {
            // 地形全体が収まる所（A キーと同じ）。向きは今のまま。
            camera.Frame(DirectX::XMFLOAT3{0.0f, 0.0f, 0.0f}, size * 0.75f);
            state = camera.State();
        }
        float values[3] = {};
        const auto worldAt = [&](const float* uv, float above) { return m_app.PathWorldPosition(uv[0], uv[1], above); };
        if (ReadVector(Member(params, "target_uv"), 2, values)) state.target = worldAt(values, 0.0f);
        else if (ReadVector(Member(params, "target"), 3, values)) state.target = {values[0], values[1], values[2]};
        // 目の位置から向きと距離を決める（eye_uv は地形の上 eye_height_m の所）。
        std::optional<DirectX::XMFLOAT3> eye;
        if (ReadVector(Member(params, "eye_uv"), 2, values)) {
            eye = worldAt(values, static_cast<float>(NumberOf(params, "eye_height_m").value_or(100.0)));
        } else if (ReadVector(Member(params, "eye"), 3, values)) {
            eye = DirectX::XMFLOAT3{values[0], values[1], values[2]};
        }
        if (eye) {
            const float dx = eye->x - state.target.x, dy = eye->y - state.target.y, dz = eye->z - state.target.z;
            const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (distance < 1.0e-3f) {
                error = "目の位置が注視点と同じです";
                return {};
            }
            state.distance = distance;
            state.pitch = std::asin(std::clamp(dy / distance, -1.0f, 1.0f));
            state.yaw = std::atan2(dx, dz);
        }
        if (const auto distance = NumberOf(params, "distance")) state.distance = static_cast<float>(*distance);
        if (const auto yaw = NumberOf(params, "yaw_degrees")) state.yaw = static_cast<float>(*yaw) * kPi / 180.0f;
        if (const auto pitch = NumberOf(params, "pitch_degrees")) state.pitch = static_cast<float>(*pitch) * kPi / 180.0f;
        if (const auto fov = NumberOf(params, "fov_degrees")) state.fovY = static_cast<float>(*fov) * kPi / 180.0f;
        camera.SetState(state);
    }
    return CameraJson();
}

json RemoteCommands::SampleTerrain(const json& params, std::string& error) {
    const compositor::CpuHeightfield& field = m_app.m_renderer.Evaluator().Heightfield();
    if (!field.IsValid()) {
        error = "地形のハイトがまだ評価されていません（get_evaluation で待ってから）";
        return {};
    }
    const json* points = Member(params, "points");
    if (points == nullptr || !points->is_array() || points->empty() || points->size() > 10000) {
        error = "points に [[u, v], …]（1〜10000 個、地形の正規化 UV 0〜1）を渡してください";
        return {};
    }
    const float size = m_app.m_renderer.PlaneSize();
    const float scale = m_app.m_renderer.DisplacementScale();
    const float base = m_app.TerrainBaseElevation();
    const float step = 1.0f / static_cast<float>(field.resolution);
    json samples = json::array();
    for (const json& point : *points) {
        float uv[2] = {};
        if (!ReadVector(&point, 2, uv)) {
            error = "points の要素は [u, v]";
            return {};
        }
        const float height = field.Sample(uv[0], uv[1]);
        // 傾斜は縮小ハイトの中心差分（評価器の 512² の写しなので、細かい凹凸は均される）。
        const float du = (field.Sample(uv[0] + step, uv[1]) - field.Sample(uv[0] - step, uv[1])) * scale / (2.0f * step * size);
        const float dv = (field.Sample(uv[0], uv[1] + step) - field.Sample(uv[0], uv[1] - step)) * scale / (2.0f * step * size);
        const DirectX::XMFLOAT3 world = m_app.PathWorldPosition(uv[0], uv[1], 0.0f);
        samples.push_back({{"uv", {uv[0], uv[1]}},
                           {"elevationMeters", base + height * scale},
                           {"slopeDegrees", RadiansToDegrees(std::atan(std::sqrt(du * du + dv * dv)))},
                           {"world", {world.x, world.y, world.z}}});
    }
    return {{"samples", std::move(samples)}, {"heightfieldResolution", field.resolution}};
}

// --- フレームの中 ---------------------------------------------------------------

bool RemoteCommands::NodeDetails(Job& job) {
    std::string error;
    OpContext context;
    graph::Node* found = ResolveNode(Member(job.params, "node"), context, error);
    if (found == nullptr) {
        job.error = error;
        return true;
    }
    graph::Node node = *found;
    std::optional<std::array<float, 4>> region;
    if (float values[4] = {}; ReadVector(Member(job.params, "path_region"), 4, values))
        region = std::array<float, 4>{values[0], values[1], values[2], values[3]};
    json result = NodeSummary(node, true, !BoolOf(job.params, "full_settings", false), region);
    if (std::binary_search(m_app.GraphCycleNodes().begin(), m_app.GraphCycleNodes().end(), node.id))
        result["errors"].push_back("接続が循環している");
    if (const auto road = m_app.m_roadNodeStatus.find(node.id); road != m_app.m_roadNodeStatus.end()) {
        if (!road->second.error.empty()) result["errors"].push_back(road->second.error);
        if (!road->second.materialError.empty()) result["errors"].push_back(road->second.materialError);
        result["stats"] = {{"lengthMeters", road->second.lengthMeters}, {"vertices", road->second.vertices},
                           {"triangles", road->second.triangles}};
    }
    if (const graph::PathSettings* path = graph::EditablePathSettings(node))
        result["staleRoutes"] = graph::CountStalePathRoutes(*path);
    // UI の行（範囲・単位・説明）。カタログの parameters と同じ取り方で、今の値も添える。
    if (BoolOf(job.params, "parameters", true)) {
        json parameters = json::object();
        const json settings = result["settings"];
        InProbeWindow([&] {
            const std::vector<ui::PropertyRecord> records = m_app.RecordNodeProperties(node);
            const json baseFlat = io::WriteNodeJson(node).flatten();
            for (const ui::PropertyRecord& record : records) {
                const std::string path = m_app.FindPropertyPath(node, record, baseFlat);
                if (path.empty() || parameters.contains(path)) continue;
                json item = m_app.DescribeProperty(node, record, path);
                item.erase("default");  // UI のリセットの値。今の値と紛らわしいので出さない
                std::string pointer = "/" + path;
                std::replace(pointer.begin(), pointer.end(), '.', '/');
                if (settings.contains(json::json_pointer(pointer))) item["value"] = settings.at(json::json_pointer(pointer));
                parameters[path] = std::move(item);
            }
        });
        result["parameters"] = std::move(parameters);
    }
    job.result = std::move(result);
    return true;
}

bool RemoteCommands::DescribeKind(Job& job) {
    const std::string kind = StringOf(job.params, "kind").value_or("");
    const graph::NodeDefinition* definition = graph::FindNodeDefinitionByName(kind);
    if (definition == nullptr || definition->kind == graph::NodeKind::Missing) {
        job.error = "知らないノードの種類です: " + kind + "（list_node_kinds で一覧を見られる）";
        return true;
    }
    if (!m_app.m_catalogParameters.contains(definition->name)) {
        // 種類ごとに 1 つだけ置いたノードを描く。今のグラフの ID と混ざらないよう、空のグラフに差し替える
        // （RunPropertyProbes と同じ）。
        InProbeWindow([&] {
            graph::NodeGraph saved = std::move(m_app.m_graph);
            m_app.m_graph = graph::NodeGraph();
            graph::NodeGraph scratch;
            if (const graph::Node* node = scratch.FindNode(scratch.CreateNode(definition->kind)))
                m_app.m_catalogParameters[definition->name] = m_app.ProbeNodeParameters(*node);
            m_app.m_graph = std::move(saved);
        });
    }
    json catalog = m_app.BuildNodeCatalog();
    for (json& entry : catalog["nodes"]) {
        if (entry["kind"] != kind) continue;
        json graphs = json::array();
        if (NodeKindOffered(definition->kind, 0)) graphs.push_back("terrain");
        if (NodeKindOffered(definition->kind, 1)) graphs.push_back("cloud");
        entry["graphs"] = std::move(graphs);
        job.result = std::move(entry);
        return true;
    }
    job.error = "カタログに種類が見つかりません: " + kind;
    return true;
}

bool RemoteCommands::ApplyOps(Job& job) {
    const json* ops = Member(job.params, "ops");
    if (ops == nullptr || !ops->is_array() || ops->empty()) {
        job.error = "ops に操作の配列を渡してください";
        return true;
    }
    Application& app = m_app;
    // 失敗したら操作列の前へ戻すための控え。グラフは複製しても数百 KB で済む（アンドゥと同じ考え方）。
    const graph::NodeGraph savedGraph = app.m_graph;
    const graph::GraphId savedSelected = app.m_selectedGraphNode;
    const graph::GraphId savedPreview = app.m_previewGraphNode, savedPreviewPin = app.m_previewGraphPin;
    const size_t savedToPlace = app.m_graphNodesToPlace.size();

    OpContext context;
    json results = json::array();
    std::string error;
    size_t failed = 0;
    for (size_t i = 0; i < ops->size(); ++i) {
        json result;
        if (!RunOp((*ops)[i], context, result, error)) {
            failed = i;
            break;
        }
        results.push_back(std::move(result));
    }
    bool rangeError = false;
    if (error.empty()) {
        error = CheckRanges(context);
        rangeError = !error.empty();
    }
    if (!error.empty()) {
        app.m_graph = savedGraph;
        app.m_selectedGraphNode = savedSelected;
        app.m_previewGraphNode = savedPreview;
        app.m_previewGraphPin = savedPreviewPin;
        app.m_graphNodesToPlace.resize(savedToPlace);
        json data = {{"applied", false}, {"note", "グラフは操作列の前の状態のまま（何も変えていない）"}};
        if (!rangeError) {
            data["failedOp"] = failed;
            const json& op = (*ops)[failed];
            if (const json* name = Member(op, "op")) data["op"] = *name;
        }
        job.error = rangeError ? error : Printf("ops[%zu]: %s", failed, error.c_str());
        job.scratch["errorData"] = std::move(data);
        return true;
    }

    // 確定。1 つの要求をアンドゥの 1 段にする（このフレームの終わりで畳まれる）。
    if (context.changed) {
        app.m_graph.MarkDirty();
        app.MarkDocumentChanged(true);
        app.m_remoteLogMark = app.m_remoteLogSequence;
    }
    for (const auto& [id, preview] : context.selects) {
        const graph::Node* node = app.m_graph.FindNode(id);
        if (node == nullptr) continue;
        if (app.m_sceneComponents.is_array() && node->component != app.m_editComponent) app.OpenComponentEditor(node->component);
        app.m_pendingSelectGraphNode = id;
        app.m_pendingSelectReplace = true;
        if (preview) app.SetPreviewGraphNode(id);
    }
    if (context.arrangeAll) app.m_pendingGraphArrange = true;
    else if (context.arrangeOverlaps) app.m_pendingGraphSpread = true;
    if (context.changed) {
        TG_LOG_INFO(ui::Tr("遠隔操作: %zu 個の操作を実行しました（Ctrl+Z で戻せる）", "Remote control: applied %zu operations (Ctrl+Z to undo)"),
                    ops->size());
    }
    json created = json::object();
    for (const auto& [key, id] : context.keys) created[key] = id;
    // undoStep: この要求でアンドゥの段が増えたか（選択だけなら偽。arrange は次のフレームに別の段を積む）。
    job.result = {{"applied", true}, {"undoStep", context.changed}, {"results", std::move(results)},
                  {"created", std::move(created)}, {"warnings", std::move(context.warnings)}};
    return true;
}

std::string RemoteCommands::CheckRanges(const OpContext& context) {
    std::string error;
    if (context.touched.empty()) return error;
    InProbeWindow([&] {
        for (const auto& [id, paths] : context.touched) {
            const graph::Node* original = m_app.m_graph.FindNode(id);
            if (original == nullptr) continue;
            graph::Node node = *original;
            const std::vector<ui::PropertyRecord> records = m_app.RecordNodeProperties(node);
            json baseFlat;
            for (const ui::PropertyRecord& record : records) {
                if (record.type != ui::PropertyRecord::Type::Float && record.type != ui::PropertyRecord::Type::Int) continue;
                const double tolerance = 1.0e-5 * std::max(1.0, std::max(std::abs(record.minValue), std::abs(record.maxValue)));
                if (record.shown >= record.minValue - tolerance && record.shown <= record.maxValue + tolerance) continue;
                if (baseFlat.is_null()) baseFlat = io::WriteNodeJson(node).flatten();
                const std::string path = m_app.FindPropertyPath(node, record, baseFlat);
                // 操作で変えたキーだけを咎める（もとから範囲の外にある値は触らない）。
                const bool changed = std::any_of(paths.begin(), paths.end(), [&](const std::string& touched) {
                    return touched == path || touched.starts_with(path + ".") || path.starts_with(touched + ".");
                });
                if (path.empty() || !changed) continue;
                error = Printf("ノード %d の「%s」（%s）の値 %g は UI の範囲 %g〜%g の外です",
                               id, record.label.c_str(), path.c_str(), record.shown, record.minValue, record.maxValue);
                return;
            }
        }
    });
    return error;
}

// --- 操作 -----------------------------------------------------------------------

bool RemoteCommands::RunOp(const json& op, OpContext& context, json& result, std::string& error) {
    const std::string name = StringOf(op, "op").value_or("");
    result = {{"op", name}};
    if (name != "select" && name != "arrange") context.changed = true;
    if (name == "create_node") return OpCreateNode(op, context, result, error);
    if (name == "delete_node") return OpDeleteNode(op, context, result, error);
    if (name == "set_params") return OpSetParams(op, context, result, error);
    if (name == "connect") return OpConnect(op, context, result, error);
    if (name == "disconnect") return OpDisconnect(op, context, result, error);
    if (name == "add_path") return OpAddPath(op, context, result, error);
    if (name == "set_path_points") return OpSetPathPoints(op, context, result, error);
    if (name == "set_path_style") return OpSetPathStyle(op, context, result, error);
    if (name == "delete_path_elements") return OpDeletePathElements(op, context, result, error);
    if (name == "set_bypass" || name == "set_note" || name == "move_node" || name == "select") {
        graph::Node* node = ResolveNode(Member(op, "node"), context, error);
        if (node == nullptr) return false;
        result["node"] = node->id;
        if (name == "set_bypass") {
            const json* bypass = Member(op, "bypass");
            if (bypass == nullptr || !bypass->is_boolean()) {
                error = "bypass に true / false を渡してください";
                return false;
            }
            graph::SetBypassed(*node, bypass->get<bool>());
        } else if (name == "set_note") {
            const auto note = StringOf(op, "note");
            if (!note) {
                error = "note に文字列を渡してください";
                return false;
            }
            node->note = *note;
        } else if (name == "move_node") {
            float position[2] = {};
            if (!ReadVector(Member(op, "position"), 2, position)) {
                error = "position に [x, y]（キャンバス座標）を渡してください";
                return false;
            }
            node->posX = position[0];
            node->posY = position[1];
            node->positionValid = true;
            m_app.m_graphNodesToPlace.push_back(node->id);
        } else {
            context.selects.emplace_back(node->id, BoolOf(op, "preview", false));
        }
        return true;
    }
    if (name == "arrange") {
        const std::string mode = StringOf(op, "mode").value_or("overlaps");
        if (mode == "all") context.arrangeAll = true;
        else if (mode == "overlaps") context.arrangeOverlaps = true;
        else {
            error = "arrange の mode は \"overlaps\"（重なりを解消）か \"all\"（全体を左から右へ並べ直す）";
            return false;
        }
        return true;
    }
    error = "知らない op です: \"" + name +
            "\"（create_node, delete_node, set_params, connect, disconnect, set_bypass, set_note, move_node, "
            "add_path, set_path_points, set_path_style, delete_path_elements, select, arrange）";
    return false;
}

graph::Node* RemoteCommands::ResolveNode(const json* reference, const OpContext& context, std::string& error) {
    if (reference == nullptr) {
        error = "node にノード ID（数）か、この操作列で付けた key（文字列）を渡してください";
        return nullptr;
    }
    graph::GraphId id = 0;
    if (reference->is_number_integer()) {
        id = reference->get<int>();
    } else if (reference->is_string()) {
        const auto found = context.keys.find(reference->get<std::string>());
        if (found == context.keys.end()) {
            error = "key \"" + reference->get<std::string>() + "\" はこの操作列でまだ作っていません";
            return nullptr;
        }
        id = found->second;
    }
    graph::Node* node = m_app.m_graph.FindMutableNode(id);
    if (node == nullptr) error = "ノード " + reference->dump() + " がありません";
    return node;
}

const graph::Pin* RemoteCommands::ResolvePin(const graph::Node& node, graph::PinKind kind, const json* reference,
                                              std::string& error) {
    const bool input = kind == graph::PinKind::Input;
    const std::vector<graph::Pin>& pins = input ? node.inputs : node.outputs;
    const auto available = [&] {
        std::string names;
        for (const graph::Pin& pin : pins) names += (names.empty() ? "" : ", ") + PinName(node, pin);
        return names.empty() ? std::string("なし") : names;
    };
    const auto fail = [&](const std::string& what) {
        error = Printf("ノード %d（%s）に%sピン %s がありません（あるのは: %s）", node.id, KindName(node),
                       input ? "入力" : "出力", what.c_str(), available().c_str());
        return nullptr;
    };
    if (pins.empty()) return fail("");
    if (reference == nullptr || reference->is_null()) return &pins.front();
    if (reference->is_number_integer()) {
        const int index = reference->get<int>();
        if (index >= 0 && static_cast<size_t>(index) < pins.size()) return &pins[static_cast<size_t>(index)];
        return fail(reference->dump());
    }
    if (!reference->is_string()) return fail(reference->dump());
    const std::string name = NormalizePinName(reference->get<std::string>());
    const auto connected = [&](const graph::Pin& pin) {
        return std::any_of(m_app.m_graph.Links().begin(), m_app.m_graph.Links().end(),
                           [&](const graph::Link& link) { return link.endPin == pin.id; });
    };
    std::vector<const graph::Pin*> matches;
    for (const graph::Pin& pin : pins)
        if (NormalizePinName(pin.label) == name) matches.push_back(&pin);
    if (matches.size() == 1) return matches.front();
    if (matches.size() > 1) {
        // 同じ名前が並ぶ可変入力（Cloud Merge / Model Merge）。空いている最初のピン。
        for (const graph::Pin* pin : matches)
            if (!input || !connected(*pin)) return pin;
        return fail(reference->dump() + "（空きが無い。\"" + reference->get<std::string>() + " 2\" のように番号で指す）");
    }
    // "Shape 2" のような番号付きの名前（1 始まり）。
    const size_t last = name.find_last_not_of("0123456789");
    if (last != std::string::npos && last + 1 < name.size()) {
        const std::string base = name.substr(0, last + 1);
        const int number = std::atoi(name.c_str() + last + 1);
        int same = 0;
        for (const graph::Pin& pin : pins)
            if (NormalizePinName(pin.label) == base && ++same == number) return &pin;
    }
    return fail(reference->dump());
}

const graph::Pin* RemoteCommands::ResolvePinReference(const json* reference, graph::PinKind kind,
                                                       const OpContext& context, std::string& error) {
    if (reference == nullptr) {
        error = Printf("%s に [ノード, ピン名] を渡してください", kind == graph::PinKind::Input ? "to" : "from");
        return nullptr;
    }
    const json* nodeReference = reference;
    const json* pinReference = nullptr;
    if (reference->is_array()) {
        if (reference->empty() || reference->size() > 2) {
            error = "ピンは [ノード, ピン名]（ピン名を省くと最初のピン）で書いてください";
            return nullptr;
        }
        nodeReference = &(*reference)[0];
        if (reference->size() == 2) pinReference = &(*reference)[1];
    }
    graph::Node* node = ResolveNode(nodeReference, context, error);
    return node != nullptr ? ResolvePin(*node, kind, pinReference, error) : nullptr;
}

graph::PathSettings* RemoteCommands::PathOf(graph::Node& node, std::string& error) {
    graph::PathSettings* path = graph::EditablePathSettings(node);
    if (path == nullptr) error = Printf("ノード %d（%s）はパスを持たない（path / roadPath だけ）", node.id, KindName(node));
    return path;
}

bool RemoteCommands::OpCreateNode(const json& op, OpContext& context, json& result, std::string& error) {
    Application& app = m_app;
    const std::string kind = StringOf(op, "kind").value_or("");
    const graph::NodeDefinition* definition = graph::FindNodeDefinitionByName(kind);
    if (definition == nullptr || definition->kind == graph::NodeKind::Missing) {
        error = "知らないノードの種類です: \"" + kind + "\"（list_node_kinds で一覧を見られる）";
        return false;
    }
    int component = NodeKindOffered(definition->kind, 0) ? 0 : 1;
    if (const json* value = Member(op, "component")) {
        component = ParseComponent(*value);
        if (component < 0) {
            error = "component は \"terrain\" か \"cloud\"";
            return false;
        }
    }
    if (!NodeKindOffered(definition->kind, component)) {
        error = Printf("%s は%sグラフに置けない種類です", kind.c_str(), component == 1 ? "雲" : "地形");
        return false;
    }
    if (definition->kind == graph::NodeKind::CloudOutput && app.m_graph.CompileCloud().hasOutput) {
        error = "Cloud Output は 1 つだけ置ける（もうある）";
        return false;
    }
    std::string key;
    if (const json* value = Member(op, "key")) {
        if (!value->is_string() || value->get<std::string>().empty()) {
            error = "key は空でない文字列";
            return false;
        }
        key = value->get<std::string>();
        if (context.keys.contains(key)) {
            error = "key \"" + key + "\" はこの操作列で使用済み";
            return false;
        }
    }
    // 位置。省けば、そのグラフのノードの右へ縦に並べる（あとで arrange で整えられる）。
    float position[2] = {};
    const bool placed = ReadVector(Member(op, "position"), 2, position);
    if (!placed) {
        float right = 0.0f, top = 0.0f;
        bool any = false;
        for (const graph::Node& other : app.m_graph.Nodes()) {
            if (other.component != component || !other.positionValid) continue;
            right = any ? std::max(right, other.posX) : other.posX;
            top = any ? std::min(top, other.posY) : other.posY;
            any = true;
        }
        position[0] = any ? right + 320.0f : 0.0f;
        position[1] = top + static_cast<float>(context.autoPlaced++) * 160.0f;
    }

    const graph::GraphId id = app.m_graph.CreateNode(definition->kind);
    graph::Node* node = app.m_graph.FindMutableNode(id);
    if (node == nullptr) {
        error = "ノードを作れませんでした";
        return false;
    }
    ApplyNewNodeDefaults(*node, app.m_graph.Nodes().size());
    node->component = component;
    node->posX = position[0];
    node->posY = position[1];
    node->positionValid = true;
    app.m_graphNodesToPlace.push_back(id);
    if (!key.empty()) context.keys[key] = id;
    result["node"] = id;
    if (!key.empty()) result["key"] = key;

    if (const auto name = StringOf(op, "name")) {
        if (auto* layer = std::get_if<graph::LayerNodeSettings>(&node->settings)) layer->layer.name = *name;
        else context.warnings.push_back(Printf("ノード %d（%s）は名前を持たない種類なので name を使わなかった", id, kind.c_str()));
    }
    if (const auto note = StringOf(op, "note")) node->note = *note;
    if (const json* settings = Member(op, "settings")) {
        if (!ApplySettings(*node, *settings, context, error)) return false;
    }
    if (BoolOf(op, "bypass", false)) graph::SetBypassed(*node, true);
    return true;
}

bool RemoteCommands::OpDeleteNode(const json& op, OpContext& context, json& result, std::string& error) {
    graph::Node* node = ResolveNode(Member(op, "node"), context, error);
    if (node == nullptr) return false;
    const graph::GraphId id = node->id;
    if (!m_app.m_graph.DeleteNode(id)) {
        error = Printf("ノード %d を消せませんでした", id);
        return false;
    }
    // UI でノードを消したときと同じ後始末。
    m_app.m_graphNodeSizes.erase(id);
    if (m_app.m_previewGraphNode == id) {
        m_app.m_previewGraphNode = 0;
        m_app.m_previewGraphPin = 0;
    }
    if (m_app.m_selectedGraphNode == id) m_app.m_selectedGraphNode = 0;
    context.touched.erase(id);
    result["node"] = id;
    return true;
}

bool RemoteCommands::OpSetParams(const json& op, OpContext& context, json& result, std::string& error) {
    graph::Node* node = ResolveNode(Member(op, "node"), context, error);
    if (node == nullptr) return false;
    result["node"] = node->id;
    const json* settings = Member(op, "settings");
    if (settings == nullptr) {
        error = "settings に設定（get_node の settings と同じ形。変えるキーだけでよい）を渡してください";
        return false;
    }
    return ApplySettings(*node, *settings, context, error);
}

bool RemoteCommands::ApplySettings(graph::Node& node, const json& patch, OpContext& context, std::string& error) {
    if (!patch.is_object()) {
        error = "settings はオブジェクト";
        return false;
    }
    // 今の設定へ上書きで重ねて（JSON Merge Patch。配列は丸ごと置き換え、null は既定値へ戻す）、
    // 保存と同じ読み込みを通す。
    json merged = io::WriteNodeJson(node);
    merged.merge_patch(patch);
    graph::Node read;
    if (!io::ReadNodeJson(node, merged, read)) {
        error = Printf("ノード %d の設定を読めませんでした", node.id);
        return false;
    }
    // 渡した値が読み込みでそのまま残ったか。残らない値は、知らないキー・使えない列挙名・範囲外で丸めた値。
    const json written = io::WriteNodeJson(read);
    const json writtenFlat = written.flatten();
    std::set<std::string>& touched = context.touched[node.id];
    // 一時オブジェクトの items() を回すと、ループの間に元が消える。変数に受ける。
    const json patchFlat = patch.flatten();
    for (const auto& [pointer, value] : patchFlat.items()) {
        if (value.is_null()) continue;  // 既定値へ戻す指定
        const std::string path = DottedPath(pointer);
        touched.insert(path);
        const auto found = writtenFlat.find(pointer);
        if (found != writtenFlat.end()) {
            if (NearlyEqual(*found, value)) continue;
            error = Printf("ノード %d の %s に %s は使えません（読み込むと %s になる）", node.id, path.c_str(),
                           value.dump().c_str(), found->dump().c_str());
            return false;
        }
        // 書き戻しに無いキー。既定値のときは書かないキーもあるので、外して読んでも同じなら「知らないキー」。
        json without = merged;
        const json::json_pointer at(pointer);
        if (at.empty() || !without.contains(at.parent_pointer())) continue;
        json& parent = without[at.parent_pointer()];
        if (parent.is_object()) parent.erase(at.back());
        graph::Node other;
        if (io::ReadNodeJson(node, without, other) && io::WriteNodeJson(other) == written) {
            error = Printf("ノード %d（%s）の設定に %s というキーはありません（get_node か describe_node_kind で確かめる）",
                           node.id, KindName(node), path.c_str());
            return false;
        }
    }
    node.settings = read.settings;
    return true;
}

bool RemoteCommands::OpConnect(const json& op, OpContext& context, json& result, std::string& error) {
    const graph::Pin* start = ResolvePinReference(Member(op, "from"), graph::PinKind::Output, context, error);
    if (start == nullptr) return false;
    const graph::Pin* end = ResolvePinReference(Member(op, "to"), graph::PinKind::Input, context, error);
    if (end == nullptr) return false;
    const graph::GraphId startId = start->id, endId = end->id;
    switch (m_app.m_graph.CheckLink(startId, endId)) {
    case graph::LinkCheck::Ok: break;
    case graph::LinkCheck::Cycle: error = "繋ぐと接続が循環するため繋げません"; return false;
    case graph::LinkCheck::TypeMismatch:
        error = Printf("ピンの型が違うため繋げません（%s → %s）", io::ValueTypeName(start->valueType),
                       io::ValueTypeName(end->valueType));
        return false;
    case graph::LinkCheck::SameNode: error = "同じノードの中では繋げません"; return false;
    default: error = "このピンどうしは繋げません"; return false;
    }
    // 入力にもう繋がっていれば置き換わる（UI と同じ）。
    for (const graph::Link& link : m_app.m_graph.Links()) {
        if (link.endPin == endId) {
            result["replaced"] = PinReference(link.startPin);
            break;
        }
    }
    if (!m_app.m_graph.CreateLink(startId, endId)) {
        error = "繋げませんでした";
        return false;
    }
    result["from"] = PinReference(startId);
    result["to"] = PinReference(endId);
    return true;
}

bool RemoteCommands::OpDisconnect(const json& op, OpContext& context, json& result, std::string& error) {
    graph::GraphId linkId = 0;
    if (const json* requested = Member(op, "link"); requested != nullptr && requested->is_number_integer()) {
        linkId = requested->get<int>();
    } else {
        const graph::Pin* end = ResolvePinReference(Member(op, "to"), graph::PinKind::Input, context, error);
        if (end == nullptr) return false;
        for (const graph::Link& link : m_app.m_graph.Links())
            if (link.endPin == end->id) linkId = link.id;
        if (linkId == 0) {
            error = "その入力には何も繋がっていません";
            return false;
        }
    }
    const graph::Link* found = nullptr;
    for (const graph::Link& link : m_app.m_graph.Links())
        if (link.id == linkId) found = &link;
    if (found == nullptr) {
        error = Printf("リンク %d がありません", linkId);
        return false;
    }
    result["from"] = PinReference(found->startPin);
    result["to"] = PinReference(found->endPin);
    if (!m_app.m_graph.DeleteLink(linkId)) {
        error = Printf("リンク %d を外せませんでした", linkId);
        return false;
    }
    return true;
}

bool RemoteCommands::ReadEdgeStyle(const json& style, graph::PathEdgeStyle& out, std::string& error) {
    if (!style.is_object()) {
        error = "style はオブジェクト";
        return false;
    }
    static const std::set<std::string> kKeys = {"curve", "rounding", "clothoidRatio", "overrideValues", "width",
                                                "feather", "intensity", "meander", "meanderWavelength", "route",
                                                "maxGrade", "ridgeWeight", "avoidWeight"};
    for (const auto& [key, value] : style.items()) {
        if (!kKeys.contains(key)) {
            error = "style に " + key + " というキーはありません（curve, rounding, clothoidRatio, width, feather, intensity, "
                    "meander, meanderWavelength, route, maxGrade, ridgeWeight, avoidWeight）";
            return false;
        }
    }
    if (const auto curve = StringOf(style, "curve")) {
        const auto index = EnumIndex(kPathCurveNames, *curve);
        if (!index) {
            error = "curve は " + EnumList(kPathCurveNames) + " のどれか";
            return false;
        }
        out.curve = static_cast<graph::PathCurve>(*index);
    }
    if (const auto route = StringOf(style, "route")) {
        const auto index = EnumIndex(kPathRouteNames, *route);
        if (!index) {
            error = "route は " + EnumList(kPathRouteNames) + " のどれか";
            return false;
        }
        out.route = static_cast<graph::PathRoute>(*index);
    }
    // 範囲はファイルの読み込み（io::ReadPath）と同じ。外なら失敗にする（丸めない）。
    const auto number = [&](const char* key, float& target, float low, float high) {
        const auto value = NumberOf(style, key);
        if (!value) return true;
        if (*value < low || *value > high) {
            error = Printf("style.%s は %g〜%g", key, low, high);
            return false;
        }
        target = static_cast<float>(*value);
        return true;
    };
    if (!number("rounding", out.rounding, 0.0f, 1.0f) || !number("clothoidRatio", out.clothoidRatio, 0.0f, 1.0f) ||
        !number("meander", out.meanderMeters, 0.0f, 100.0f) ||
        !number("meanderWavelength", out.meanderWavelengthMeters, 1.0f, 1000.0f) ||
        !number("maxGrade", out.maxGradePercent, 0.1f, 100.0f) || !number("ridgeWeight", out.ridgeWeight, 0.0f, 10.0f) ||
        !number("avoidWeight", out.avoidWeight, 0.0f, 100.0f)) {
        return false;
    }
    // 幅を書けば、鎖の上で幅を一定にする（エッジの幅の上書き）。
    const bool values = style.contains("width") || style.contains("feather") || style.contains("intensity");
    out.overrideValues = BoolOf(style, "overrideValues", values ? true : out.overrideValues);
    if (!number("width", out.widthMeters, 0.0f, 100000.0f) || !number("feather", out.featherMeters, 0.0f, 100000.0f) ||
        !number("intensity", out.intensity, 0.0f, 1.0f)) {
        return false;
    }
    return true;
}

bool RemoteCommands::OpAddPath(const json& op, OpContext& context, json& result, std::string& error) {
    graph::Node* node = ResolveNode(Member(op, "node"), context, error);
    if (node == nullptr) return false;
    graph::PathSettings* path = PathOf(*node, error);
    if (path == nullptr) return false;
    result["node"] = node->id;
    const json* points = Member(op, "points");
    if (points == nullptr || !points->is_array() || points->empty() || points->size() > 4096) {
        error = "points に [[u, v], …] か [{\"u\", \"v\", \"width\", …}, …]（1〜4096 点、地形の正規化 UV 0〜1）を渡してください";
        return false;
    }
    graph::PathEdgeStyle style;
    bool styled = false;
    if (const json* value = Member(op, "style")) {
        if (!ReadEdgeStyle(*value, style, error)) return false;
        styled = true;
    }
    // 既存の点から伸ばす / 既存の点で閉じる。
    graph::PathElementId previous = 0;
    if (const json* from = Member(op, "connect_from")) {
        previous = from->is_number_integer() ? from->get<int>() : 0;
        if (path->FindPoint(previous) == nullptr) {
            error = "connect_from の点 " + from->dump() + " がこのパスにありません";
            return false;
        }
    }
    graph::PathElementId closeTo = 0;
    if (const json* to = Member(op, "connect_to")) {
        closeTo = to->is_number_integer() ? to->get<int>() : 0;
        if (path->FindPoint(closeTo) == nullptr) {
            error = "connect_to の点 " + to->dump() + " がこのパスにありません";
            return false;
        }
    }
    const graph::PathElementId start = previous;
    std::vector<graph::PathElementId> created;
    std::vector<std::pair<graph::PathElementId, graph::PathElementId>> joined;
    for (const json& item : *points) {
        float uv[2] = {};
        const bool pair = ReadVector(&item, 2, uv);
        if (!pair) {
            const auto u = NumberOf(item, "u"), v = NumberOf(item, "v");
            if (!u || !v) {
                error = "points の要素は [u, v] か {\"u\": …, \"v\": …}";
                return false;
            }
            uv[0] = static_cast<float>(*u);
            uv[1] = static_cast<float>(*v);
        }
        if (!(uv[0] >= 0.0f && uv[0] <= 1.0f && uv[1] >= 0.0f && uv[1] <= 1.0f)) {
            error = Printf("点 (%g, %g) が地形の外です（u, v は 0〜1）", uv[0], uv[1]);
            return false;
        }
        const graph::PathElementId id = graph::AddPathPoint(*path, uv[0], uv[1], previous);
        graph::PathPoint* point = path->FindPoint(id);
        if (id == 0 || point == nullptr) {
            error = "点を置けませんでした";
            return false;
        }
        if (!pair) {
            point->widthMeters = static_cast<float>(NumberOf(item, "width").value_or(point->widthMeters));
            point->featherMeters = static_cast<float>(NumberOf(item, "feather").value_or(point->featherMeters));
            point->intensity = std::clamp(static_cast<float>(NumberOf(item, "intensity").value_or(point->intensity)), 0.0f, 1.0f);
            point->heightOffsetMeters = static_cast<float>(NumberOf(item, "heightOffset").value_or(point->heightOffsetMeters));
        }
        if (previous != 0) joined.emplace_back(previous, id);
        created.push_back(id);
        previous = id;
    }
    if (closeTo != 0) {
        graph::ConnectPathPoints(*path, previous, closeTo);
        joined.emplace_back(previous, closeTo);
    } else if (BoolOf(op, "closed", false)) {
        const graph::PathElementId first = start != 0 ? start : created.front();
        if (created.size() + (start != 0 ? 1 : 0) < 3) {
            error = "閉じた輪には 3 点以上が要ります";
            return false;
        }
        graph::ConnectPathPoints(*path, previous, first);
        joined.emplace_back(previous, first);
    }
    std::vector<graph::PathElementId> edges;
    for (const auto& [a, b] : joined) {
        const graph::PathEdge* edge = path->FindEdgeBetween(a, b);
        if (edge == nullptr) continue;
        edges.push_back(edge->id);
        if (styled) {
            for (graph::PathEdge& target : path->edges)
                if (target.id == edge->id) graph::ApplyPathEdgeStyle(target, style);
        }
    }
    result["points"] = IdArray(created);
    result["edges"] = IdArray(edges);
    if (styled && style.route != graph::PathRoute::None)
        context.warnings.push_back(Printf("ノード %d: 経路探索の鎖は recompute_routes で経路を計算する", node->id));
    return true;
}

bool RemoteCommands::OpSetPathPoints(const json& op, OpContext& context, json& result, std::string& error) {
    graph::Node* node = ResolveNode(Member(op, "node"), context, error);
    if (node == nullptr) return false;
    graph::PathSettings* path = PathOf(*node, error);
    if (path == nullptr) return false;
    result["node"] = node->id;
    const json* points = Member(op, "points");
    if (points == nullptr || !points->is_array() || points->empty()) {
        error = "points に [{\"id\": 点の ID, \"u\", \"v\", \"width\", \"feather\", \"intensity\", \"heightOffset\"}, …] を渡してください";
        return false;
    }
    for (const json& item : *points) {
        const json* id = Member(item, "id");
        graph::PathPoint* point = (id != nullptr && id->is_number_integer()) ? path->FindPoint(id->get<int>()) : nullptr;
        if (point == nullptr) {
            error = "点 " + (id ? id->dump() : std::string("（id なし）")) + " がこのパスにありません";
            return false;
        }
        const auto u = NumberOf(item, "u"), v = NumberOf(item, "v");
        if ((u && (*u < 0.0 || *u > 1.0)) || (v && (*v < 0.0 || *v > 1.0))) {
            error = "u, v は 0〜1";
            return false;
        }
        if (u) point->u = static_cast<float>(*u);
        if (v) point->v = static_cast<float>(*v);
        point->widthMeters = static_cast<float>(NumberOf(item, "width").value_or(point->widthMeters));
        point->featherMeters = static_cast<float>(NumberOf(item, "feather").value_or(point->featherMeters));
        point->intensity = std::clamp(static_cast<float>(NumberOf(item, "intensity").value_or(point->intensity)), 0.0f, 1.0f);
        point->heightOffsetMeters = static_cast<float>(NumberOf(item, "heightOffset").value_or(point->heightOffsetMeters));
    }
    return true;
}

bool RemoteCommands::OpSetPathStyle(const json& op, OpContext& context, json& result, std::string& error) {
    graph::Node* node = ResolveNode(Member(op, "node"), context, error);
    if (node == nullptr) return false;
    graph::PathSettings* path = PathOf(*node, error);
    if (path == nullptr) return false;
    result["node"] = node->id;
    const json* style = Member(op, "style");
    const json* edges = Member(op, "edges");
    if (style == nullptr || edges == nullptr || !(edges->is_array() || (edges->is_string() && *edges == "all"))) {
        error = "edges（エッジの ID の配列か \"all\"）と style を渡してください";
        return false;
    }
    std::vector<graph::PathElementId> targets;
    if (edges->is_string()) {
        for (const graph::PathEdge& edge : path->edges) targets.push_back(edge.id);
    } else {
        for (const json& id : *edges) {
            if (!id.is_number_integer() || path->FindEdge(id.get<int>()) == nullptr) {
                error = "エッジ " + id.dump() + " がこのパスにありません";
                return false;
            }
            targets.push_back(id.get<int>());
        }
    }
    size_t changed = 0;
    bool routed = false;
    for (graph::PathEdge& edge : path->edges) {
        if (std::find(targets.begin(), targets.end(), edge.id) == targets.end()) continue;
        // エッジごとに今の設定から読み、書いたキーだけを変える。
        graph::PathEdgeStyle edgeStyle = graph::GetPathEdgeStyle(edge);
        if (!ReadEdgeStyle(*style, edgeStyle, error)) return false;
        if (graph::ApplyPathEdgeStyle(edge, edgeStyle)) ++changed;
        routed |= edgeStyle.route != graph::PathRoute::None;
    }
    result["changedEdges"] = changed;
    if (routed) context.warnings.push_back(Printf("ノード %d: 経路探索の鎖は recompute_routes で経路を計算する", node->id));
    return true;
}

bool RemoteCommands::OpDeletePathElements(const json& op, OpContext& context, json& result, std::string& error) {
    graph::Node* node = ResolveNode(Member(op, "node"), context, error);
    if (node == nullptr) return false;
    graph::PathSettings* path = PathOf(*node, error);
    if (path == nullptr) return false;
    result["node"] = node->id;
    const auto ids = [&](const char* key, std::vector<graph::PathElementId>& out) {
        const json* list = Member(op, key);
        if (list == nullptr) return true;
        if (!list->is_array()) return false;
        for (const json& id : *list) {
            if (!id.is_number_integer()) return false;
            out.push_back(id.get<int>());
        }
        return true;
    };
    std::vector<graph::PathElementId> points, edges;
    if (!ids("points", points) || !ids("edges", edges) || (points.empty() && edges.empty())) {
        error = "points / edges に消す点・エッジの ID の配列を渡してください";
        return false;
    }
    // エッジを先に消す（点を消すと付いているエッジも消えることがある）。
    for (const graph::PathElementId id : edges) {
        if (!graph::DeletePathEdge(*path, id)) {
            error = Printf("エッジ %d がこのパスにありません", id);
            return false;
        }
    }
    for (const graph::PathElementId id : points) {
        if (!graph::DeletePathPoint(*path, id)) {
            error = Printf("点 %d がこのパスにありません", id);
            return false;
        }
    }
    return true;
}

// --- フレームをまたぐ要求 -------------------------------------------------------

json RemoteCommands::EvaluationReport(bool timedOut, bool full) {
    // 直近の apply_ops より後に出た警告とエラー。
    std::vector<std::pair<LogLevel, std::string>> logEntries;
    for (const Application::RemoteLogEntry& entry : m_app.m_remoteLog) {
        if (entry.sequence > m_app.m_remoteLogMark && entry.level != LogLevel::Info)
            logEntries.emplace_back(entry.level, entry.text);
    }
    json report = m_app.BuildEvaluationReport(timedOut, true, logEntries);
    if (!full) {
        // エラーか統計を持つノードだけ（全ノードは get_graph で見られる）。
        json nodes = json::array();
        for (json& node : report["nodes"])
            if (node.contains("errors") || node.contains("stats")) nodes.push_back(std::move(node));
        report["nodes"] = std::move(nodes);
    }
    return report;
}

bool RemoteCommands::Evaluation(Job& job, Phase phase) {
    if (phase != Phase::AfterFrame) return false;
    const bool wait = BoolOf(job.params, "wait", true);
    const double timeout = std::clamp(NumberOf(job.params, "timeout_seconds").value_or(120.0), 1.0, 590.0);
    const bool settled = m_app.m_remoteSettledFrames >= kSettledFrames;
    const bool timedOut = wait && !settled && SecondsSince(job.start) > timeout;
    if (wait && !settled && !timedOut) return false;
    job.result = EvaluationReport(timedOut, BoolOf(job.params, "full", false));
    job.result["settled"] = settled;
    return true;
}

bool RemoteCommands::Screenshot(Job& job, Phase phase) {
    Application& app = m_app;
    if (job.stage == 0) {
        if (phase != Phase::BeforeFrame) return false;
        const std::string target = StringOf(job.params, "target").value_or("viewport");
        const auto path = StringOf(job.params, "path");
        if (target != "viewport" && target != "window") {
            job.error = "target は \"viewport\"（UI なしの描画結果）か \"window\"（UI 込みのウィンドウ全体）";
            return true;
        }
        if (!path || path->empty() || !FromUtf8(*path).is_absolute() || FromUtf8(*path).extension() != L".png") {
            job.error = "path に書き出す PNG の絶対パスを渡してください";
            return true;
        }
        std::error_code ignored;
        std::filesystem::create_directories(FromUtf8(*path).parent_path(), ignored);
        job.scratch["target"] = target;
        job.stage = 1;
        return false;
    }
    const std::filesystem::path path = FromUtf8(StringOf(job.params, "path").value_or(""));
    const bool window = job.scratch["target"] == "window";
    if (job.stage == 1) {
        // 評価が落ち着くのを待ってから撮る（非同期の評価の途中だと前の絵が写る）。
        if (phase != Phase::AfterFrame) return false;
        const double timeout = std::clamp(NumberOf(job.params, "timeout_seconds").value_or(120.0), 1.0, 590.0);
        const bool settled = m_app.m_remoteSettledFrames >= kSettledFrames;
        if (BoolOf(job.params, "wait", true) && !settled && SecondsSince(job.start) <= timeout) return false;
        job.scratch["settled"] = settled;
        if (window) {
            job.stage = 2;
            return false;
        }
        const uint32_t maxSize = static_cast<uint32_t>(std::clamp(NumberOf(job.params, "max_size").value_or(1600.0), 0.0, 16384.0));
        app.m_device.WaitForGpu();
        if (!app.m_renderer.SaveOutputToPng(app.m_device, path, maxSize)) {
            job.error = "ビューポートを書き出せませんでした: " + ToUtf8Portable(path);
            return true;
        }
        uint32_t width = app.m_renderer.Width(), height = app.m_renderer.Height();
        if (maxSize > 0 && std::max(width, height) > maxSize) {
            const double scale = static_cast<double>(maxSize) / std::max(width, height);
            width = static_cast<uint32_t>(std::lround(width * scale));
            height = static_cast<uint32_t>(std::lround(height * scale));
        }
        job.result = {{"path", ToUtf8Portable(path)}, {"target", "viewport"}, {"width", width}, {"height", height},
                      {"settled", job.scratch["settled"]}, {"camera", CameraJson()}};
        return true;
    }
    if (job.stage == 2) {
        // ウィンドウ全体はバックバッファの読み戻し。このフレームの EndFrame で撮られる。
        if (phase != Phase::InFrame) return false;
        const uint64_t ticket = job.ticket;
        Application* owner = &app;
        app.m_device.RequestBackBufferCapture(path, [owner, ticket](bool success, const std::filesystem::path&,
                                                                    uint32_t width, uint32_t height) {
            for (Application::RemoteJob& pending : owner->m_remoteJobs) {
                if (pending.ticket != ticket) continue;
                pending.scratch["captured"] = success;
                pending.scratch["width"] = width;
                pending.scratch["height"] = height;
            }
        });
        job.stage = 3;
        return false;
    }
    if (phase != Phase::AfterFrame || !job.scratch.contains("captured")) return false;
    if (!job.scratch["captured"].get<bool>()) {
        job.error = "ウィンドウを書き出せませんでした: " + ToUtf8Portable(path);
        return true;
    }
    job.result = {{"path", ToUtf8Portable(path)}, {"target", "window"}, {"width", job.scratch["width"]},
                  {"height", job.scratch["height"]}, {"settled", job.scratch["settled"]}};
    return true;
}

bool RemoteCommands::RecomputeRoutes(Job& job, Phase phase) {
    // 経路探索用の地形の焼き直しは GPU 待機を伴うので、フレームの外で。
    if (phase != Phase::BeforeFrame) return false;
    std::string error;
    OpContext context;
    graph::Node* node = ResolveNode(Member(job.params, "node"), context, error);
    graph::PathSettings* path = node ? PathOf(*node, error) : nullptr;
    if (path == nullptr) {
        job.error = error;
        return true;
    }
    const bool force = BoolOf(job.params, "force", false);
    const size_t staleBefore = graph::CountStalePathRoutes(*path);
    if (!force && staleBefore == 0) {
        job.result = {{"node", node->id}, {"recomputed", false}, {"staleRoutes", 0},
                      {"note", "計算し直す鎖が無い（全部の鎖を計算し直すなら force: true）"}};
        return true;
    }
    if (!m_app.BakePathRouteTerrain(*node)) {
        job.error = Printf("ノード %d の Base に地形が繋がっていないため、経路を探せません", node->id);
        return true;
    }
    m_app.m_remoteLogMark = m_app.m_remoteLogSequence;
    m_app.RecomputePathRoutes(*node, force, nullptr);
    job.result = {{"node", node->id}, {"recomputed", true}, {"staleRoutes", graph::CountStalePathRoutes(*path)},
                  {"staleBefore", staleBefore}};
    return true;
}

bool RemoteCommands::History(Job& job, Phase phase, int direction) {
    // 戻す / やり直すは ProcessPendingFileWork（このフレームの前）が処理する。1 段ずつ待って進める。
    if (phase != Phase::BeforeFrame) return false;
    Application& app = m_app;
    if (app.m_pendingHistoryStep != 0) return false;
    if (job.stage == 0) {
        job.scratch["remaining"] = std::clamp(static_cast<int>(NumberOf(job.params, "steps").value_or(1.0)), 1, 100);
        job.scratch["done"] = 0;
        job.stage = 1;
    }
    int remaining = job.scratch["remaining"].get<int>();
    const bool possible = direction < 0 ? app.m_undoHistory.CanUndo() : app.m_undoHistory.CanRedo();
    if (remaining > 0 && possible) {
        app.m_pendingHistoryStep = direction;
        job.scratch["remaining"] = remaining - 1;
        job.scratch["done"] = job.scratch["done"].get<int>() + 1;
        return false;
    }
    job.result = {{direction < 0 ? "undone" : "redone", job.scratch["done"]},
                  {"undo", app.m_undoHistory.UndoCount()}, {"redo", app.m_undoHistory.RedoCount()}};
    return true;
}

bool RemoteCommands::Save(Job& job, Phase phase) {
    if (phase != Phase::BeforeFrame) return false;
    Application& app = m_app;
    if (job.stage == 0) {
        if (app.m_componentPreview >= 0 || app.m_projectPath.extension() != L".tgscene") {
            job.error = "まだ保存先の無いシーン（か一時プレビュー中）です。初めての保存は UI で行ってください";
            return true;
        }
        app.RequestSaveProject(false);
        job.stage = 1;
        return false;
    }
    // 保存は ProcessPendingFileWork（フレームの前）が行う。
    if (!app.m_pendingProjectSave.empty()) return false;
    app.RefreshSceneDirty();
    job.result = {{"scene", ToUtf8Portable(app.m_projectPath)}, {"unsaved", app.UnsavedItemNames()}};
    return true;
}

}  // namespace tg
