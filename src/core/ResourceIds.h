#pragma once

// C++ と Windows リソーススクリプト (.rc) で共有するリソース ID。
//
// **このヘッダは rc.exe も読む。** 日本語コメントを書けるのは、
// `TerrainGraph.rc` の 1 行目で `#pragma code_page(65001)` を指定しているため
// （消すと化けて下の `#define` が失われ、アイコンが出なくなる）。
#define TG_APP_ICON 101
