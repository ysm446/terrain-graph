# 確認用シーンを撮影する。アプリをウィンドウを出さずに（最小化ではなく非表示で）起動し、
# グラフの評価が終わってから 1 枚撮って終わる。ログの警告・エラーを拾って表示する。
# 手順は docs/design/scene-authoring.md。
#
# 使い方（data/ の中でも外でもよい）:
#   python tools/scene/shoot.py Test/albura-qa/far.tgscene Test/albura-qa/pass.tgscene --prefix v3
#     → Test/albura-qa/v3_far.png、v3_pass.png（ログは .err）
#   python tools/scene/shoot.py Scenes/albura/albura.tgscene --save        （Debug で開いて保存し直す確認）
#   python tools/scene/shoot.py Scenes/albura/albura.tgscene --bake Models/Arve/Arve_Var1.tgmodel ...
#
# 既定は Release（build/bin/Release）。--debug で Debug（デバッグレイヤーの警告を見るとき）。
# 撮影はグラフの評価が止まるまで待つ（評価中は撮らない）ので、--frame は普通は要らない。
import argparse
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from tgscene import find_root, to_root_relative  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def run(exe, arguments, log_base):
    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 0  # SW_HIDE。最小化（SW_MINIMIZE）では描画が止まる
    with open(log_base + ".err", "w", encoding="utf-8") as err, open(log_base + ".out", "w", encoding="utf-8") as out:
        code = subprocess.call([exe] + arguments, stdout=out, stderr=err, startupinfo=startup)
    with open(log_base + ".err", encoding="utf-8", errors="replace") as f:
        problems = [line.rstrip() for line in f if "[warn" in line or "[error" in line]
    return code, problems


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("scenes", nargs="+")
    parser.add_argument("--prefix", default="shot", help="画像の名前の頭（v1 → v1_<シーン名>.png）")
    parser.add_argument("--debug", action="store_true", help="Debug ビルドで起動する")
    parser.add_argument("--save", action="store_true", help="撮らずに、開いて同じ場所へ保存し直す")
    parser.add_argument("--bake", nargs="+", default=[], help="インポスターを焼き直すモデル（ルート相対）")
    parser.add_argument("--frame", type=int, default=0, help="最低この数のフレームを描いてから撮る")
    parser.add_argument("--root")
    args = parser.parse_args()
    root = args.root or find_root()
    exe = os.path.join(REPO, "build", "bin", "Debug" if args.debug or args.save else "Release", "terrain_graph.exe")
    if not os.path.exists(exe):
        raise SystemExit(f"{exe} がありません。先にビルドする（cmake --build --preset x64-release）")

    failed = False
    for scene in args.scenes:
        path = os.path.join(root, to_root_relative(root, scene))
        folder, name = os.path.split(os.path.splitext(path)[0])
        arguments = ["--project", path]
        if args.save:
            arguments += ["--save-project", path, "--screenshot-frame", str(max(args.frame, 30))]
            log_base = os.path.join(folder, f"{name}_save")
            target = "保存"
        elif args.bake:
            arguments += ["--screenshot-frame", str(max(args.frame, 5))]
            for model in args.bake:
                arguments += ["--bake-impostors", os.path.join(root, to_root_relative(root, model))]
            log_base = os.path.join(folder, f"{name}_bake")
            target = f"インポスター {len(args.bake)} モデル"
        else:
            image = os.path.join(folder, f"{args.prefix}_{name}.png")
            arguments += ["--screenshot", image]
            if args.frame:
                arguments += ["--screenshot-frame", str(args.frame)]
            log_base = os.path.splitext(image)[0]
            target = os.path.relpath(image, root)
        code, problems = run(exe, arguments, log_base)
        failed |= code != 0 or bool(problems)
        print(f"{scene}: 終了コード {code}、{target}、警告・エラー {len(problems)} 件")
        for line in problems[:10]:
            print("    " + line)
    sys.exit(1 if failed else 0)


main()
