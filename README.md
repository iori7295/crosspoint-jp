# crosspoint-jp

[CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader) の日本語フォーク。
[CrossPoint v1.5.0](https://github.com/crosspoint-reader/crosspoint-reader) ベースに縦書きエンジンを
[matcha-reader v98](https://github.com/eszter007/matcha-reader)（MIT）に統合。
ESP32-C3 (X3) で縦書き・ルビ付き日本語 ePub を安定的に読むための拡張を行っています。

## 本家・matcha からの追加機能

### 縦書きレイアウト（matcha v98 統合）
- 縦書き本文レイアウトエンジン + ルビ（furigana）対応
- 禁則処理（Kinsoku）、句読点・括弧・長音符の縦組み配置
- 回転記号描画を GfxRenderer の専用関数に統一（`drawCharVerticalRotatedInCell`）
- フォント実測 ascender ベースの baseline tracking
- `VSECTION_FILE_VERSION = 98`（全キャッシュ互換）

### 日本語フォント対応
- Noto Sans CJK JP / Noto Serif CJK JP / GenEi Koburi Mincho のダウンロード・選択
- SDカードフォント自動フォールバック（フォールバックチェーン）
- CJK glyph prewarm + 48 slot overflow ring
- フォントキャッシュのライフサイクル管理（`unloadFonts`）
- フォント配信: `raw.githubusercontent.com`（リダイレクト無し）
- フォントファイルはリポジトリにコミット済み

### ビルドパイプライン（独自改良）
- `.part` 一時ファイル分離 — ビルド中も既存キャッシュを読み取り可能
- 永続パイプライン（`BuildState`）— Expat parser / layout / sink を呼び出し間で保持
- 適応的グリフ成長（`currentSeedGlyphReserve` / `currentGlyphLinearStep` / `currentGlyphMinHeadroom`）
- チャンク予算のヒープ適応（`currentChunkCharBudget` 等）
- frontier 検出 — glyph drop 時に当該ページを破棄し直前までを有効キャッシュ化
- フロンティア再試行（`frontierRetryCount`、最大5回）

### 安定性・メモリ最適化
- フレームバッファ貸出（`FrameBufferLoan`）によるビルド中ヒープ確保
- 8KB token heap guard
- 超低ヒープ時（4-6KB）のglyph drop回避機構
- 3段階グリフ成長フォールバック（doubling → linear → half-margin → zero-margin → drop）
- ヒープ断片化ガード（`buildTickHeapGate` / `BACKGROUND_BUILD_MIN_MAX_ALLOC`）

### その他
- 一覧画面の日本語ファイル名高速表示（UTF-8 sanitize）
- 画像のみ titlepage の自動スキップ
- empty spine の自動スキップ
- 外字 (gaiji) PNG の低ヒープフォールバック
- cross-session partial resume
- WebDAV 対応ファイル転送（WebServer 起動時にフォントアンロード）
- Wi-Fi/OPDS/WebServer 遷移時の自動キャッシュクリア

## ビルド

```bash
git clone --recursive https://github.com/iori7295/crosspoint-jp.git
cd crosspoint-jp
platformio run -e default
```

書き込み:

```bash
platformio run -e default -t upload
```

## ライセンス

本家 CrossPoint Reader に従い GPL-3.0 です。
matcha-reader 由来のコード（`lib/Epub/Epub/VerticalParsedText.*`、`lib/Epub/Epub/Kinsoku.*`、
`lib/GfxRenderer/GfxRenderer.cpp` の `drawCharVerticalRotatedInCell` 等）は MIT ライセンスです。

## 謝辞

- [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader) — 本家プロジェクト
- [matcha-reader](https://github.com/eszter007/matcha-reader) — 縦書きレイアウトエンジン (v98)
- [zrn-ns/crosspoint-jp](https://github.com/zrn-ns/crosspoint-jp) — 先駆的な日本語フォーク
