# crosspoint-jp

[CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader) の日本語フォーク。
ESP32-C3 (X3) ベースのE Ink端末で縦書き・ルビ付き日本語 ePub を読むための拡張を行っています。

## 本家からの主な追加機能

### 縦書きレイアウト
- 縦書き本文レイアウトエンジン（[matcha-reader](https://github.com/eszter007/matcha-reader) 由来）
- ルビ（furigana）対応
- 禁則処理（Kinsoku）
- 句読点・括弧・長音符の縦組み配置最適化

### 日本語フォント対応
- Noto Sans CJK JP / Noto Serif CJK JP / GenEi Koburi Mincho のダウンロード・選択
- SDカードフォント自動フォールバック
- CJK glyph prewarm による高速ページ描画
- フォントキャッシュのライフサイクル管理

### 安定性・メモリ最適化
- フレームバッファ貸出（`FrameBufferLoan`）によるビルド中ヒープ確保
- 超低ヒープ時（4-6KB）のglyph drop回避機構
- アダプティブメモリマージン設定
- 48 slot overflow ring による glyph churn 低減
- チャプターインクリメンタルビルド

### その他
- 一覧画面の日本語ファイル名高速表示
- 画像のみ titlepage の自動スキップ
- 外字 (gaiji) PNG の低ヒープフォールバック
- cross-session partial resume
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
matcha-reader 由来のコード (`lib/Epub/Epub/VerticalParsedText.*` 等) は MIT ライセンスです。

## 謝辞

- [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader) — 本家プロジェクト
- [matcha-reader](https://github.com/eszter007/matcha-reader) — 縦書きレイアウトエンジン
- [zrn-ns/crosspoint-jp](https://github.com/zrn-ns/crosspoint-jp) — 先駆的な日本語フォーク
