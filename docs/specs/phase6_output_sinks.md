# Phase 6 実装仕様: 出力抽象化 (OutputSink) + Window Preview + Syphon

設計: Fable / 実装: Sonnet。設計指示書 §3.8「出力抽象化は用意する」に対応。
全体制約は phase3 仕様の 9 項目を厳守（ただし本フェーズは Output 層が対象
なので、Metal API は LifeCore/Output/*.mm 内で自由に使ってよい —
MetalInternal.h を include して CommandGraph::Impl::commandBuffer や
ResourcePool::Impl::texture() に触れる。これが .mm に Metal を閉じ込める
という制約の正しい使い方である）。

## 目的

LifeRealtime の絵をライブで見られるようにする。headless 既定は不変
（sink 未登録なら一切のオーバーヘッドなし）。

```bash
./build/LifeRealtime --scene Presets/coupled_life_basic.json \
    --preview               # ウィンドウ表示
    --syphon LifeSim        # Syphon サーバとして公開 (Resolume/MadMapper へ)
```

## 新規/変更ファイル

```
新規:
  LifeCore/Output/OutputSink.h            (pure C++ interface)
  LifeCore/Output/WindowPreviewSink.h/.mm
  LifeCore/Output/SyphonSink.h/.mm
  Shaders/Render/Present.metal            (RGBA16F → BGRA8 sRGB 変換 kernel)
  Apps/SyphonCheck/main.mm                (検証用ミニ Syphon クライアント)
  external/Syphon/                        (Syphon-Framework を vendor)
変更:
  LifeCore/Sim/SceneRunner.h/.cpp         (addOutputSink / pumpOutputs)
  Apps/LifeRealtime/main.cpp              (--preview / --syphon フラグ)
  CMakeLists.txt
```

## 1. OutputSink interface (LifeCore/Output/OutputSink.h, pure C++)

```cpp
class OutputSink {
public:
    virtual ~OutputSink() = default;
    virtual bool start(MetalContext& metal, ResourcePool& pool,
                       uint32_t width, uint32_t height, std::string& outError) = 0;
    // フレームの command buffer が開いている間に呼ばれる（beginFrame 後、
    // endFrame 前）。実装はこの command buffer 上に blit/compute/present を
    // encode する。frame は RGBA16F の最終 renderTarget。
    virtual void publish(CommandGraph& graph, TextureHandle frame) = 0;
    // 毎ループ 1 回、フレーム外で呼ばれる（イベントポンプ等）。
    virtual void pump(bool& shouldQuit) { (void)shouldQuit; }
    virtual void stop() {}
    virtual const char* name() const = 0;
};
```

## 2. SceneRunner への統合

- `void addOutputSink(std::unique_ptr<OutputSink> sink)` — create 後・run 前に
  アプリが呼ぶ。SceneRunner が所有。start() は addOutputSink 内で呼び、
  失敗したら stderr に警告してそのまま破棄（アプリは止めない）。
- step() 内: composite encode の**直後、readback の前**に
  `for (sink) sink->publish(*graph_, renderTarget_)`。
- `void pumpOutputs(bool& shouldQuit)` — 全 sink の pump を呼ぶ。
  LifeRealtime がループ末尾で呼び、true なら graceful 終了。
- dtor で全 sink の stop()。

## 3. Present.metal

```metal
struct PresentParams { uint width; uint height; float exposure; };
kernel void presentToBGRA(texture2d<float, access::read> src [[texture(0)]],
                          texture2d<float, access::write> dst [[texture(1)]],
                          constant PresentParams& p [[buffer(0)]],
                          uint2 gid [[thread_position_in_grid]]);
```

- linear RGBA16F → sRGB エンコード（ImageWriter.mm の linearToSRGB8 と同じ
  カーブ）→ dst（BGRA8Unorm。compute の texture write は RGBA 意味順なので
  スウィズルは自動）。
- dst サイズ ≠ src サイズの場合は nearest 拡縮（gid → src 座標へスケール）。

## 4. WindowPreviewSink (.mm)

- start(): `[NSApplication sharedApplication]` +
  `setActivationPolicy:NSApplicationActivationPolicyRegular` +
  `activateIgnoringOtherApps:YES`。NSWindow
  (titled|closable|miniaturizable|resizable、コンテンツサイズ = シーン解像度
  × previewScale 既定 0.5)。contentView.wantsLayer=YES、layer に CAMetalLayer:
  device = 我々の MTLDevice、pixelFormat = MTLPixelFormatBGRA8Unorm、
  colorspace = sRGB、drawableSize = シーン解像度、
  framebufferOnly = NO（compute write のため必須）。
- publish(): `[layer nextDrawable]`（nil ならスキップ）→ presentToBGRA を
  生 encoder で encode（PipelineCache::Impl::pipeline("presentToBGRA") を
  MetalInternal 経由で取得、src は pool impl の texture(handle)、dst は
  drawable.texture 直接）→ `[cb presentDrawable:drawable]`。
  drawable への参照はフレーム内で完結させる（保持しない）。
- pump(): `nextEventMatchingMask:NSEventMaskAny untilDate:[NSDate distantPast]
  inMode:NSDefaultRunLoopMode dequeue:YES` を空になるまで回して
  `[NSApp sendEvent:]`。ウィンドウが閉じられたら shouldQuit=true
  （NSWindowDelegate の windowWillClose か、[window isVisible] 監視）。
- ウィンドウタイトル: "LifeRealtime — <scene名>"。
- 全て main thread 前提（LifeRealtime のループは main thread なので OK）。

## 5. SyphonSink (.mm)

- vendor: `git clone --depth 1 https://github.com/Syphon/Syphon-Framework
  external/Syphon`（.git は削除）。
- CMake: **Metal サーバ経路に必要な .m のみ**を静的にコンパイルする
  （SyphonMetalServer / SyphonServerBase / SyphonPrivate / サーバディレクトリ
  や messaging 等の依存 — 実際の依存は #import を辿って特定せよ。OpenGL 系
  ファイルは含めない）。**Syphon は非 ARC**なので、これらのファイルには
  `-fno-objc-arc` を付ける（set_source_files_properties）。リンク:
  IOSurface.framework 追加。静的組み込み時のクラス名一意化マクロ
  （SyphonBuildMacros.h / SyphonPrivate.h に定義がある）を確認し、必要な
  define を設定すること。
- SyphonSink::start(): `[[SyphonMetalServer alloc] initWithName:@"<name>"
  device:dev options:nil]`。
- publish(): まず RGBA16F テクスチャを直接
  `publishFrameTexture:onCommandBuffer:imageRegion:flipped:NO` で公開する。
  SyphonCheck クライアント（下記）で受信フレームが取れない/真っ黒の場合のみ、
  presentToBGRA で BGRA8 中間テクスチャ（pool から確保）へ変換してそれを
  公開する方式に切り替える。
- stop(): [server stop]。

## 6. Apps/SyphonCheck (検証用クライアント)

vendored SyphonClient (Metal) を使う小さな CLI:
`SyphonServerDirectory` で該当名のサーバを探し、クライアントを作成、
最大 5 秒待って新フレームのテクスチャを取得。取得できたら
`syphon_check: got WxH frame` を stdout に出して exit 0、できなければ
exit 1。CMake ターゲット名 `SyphonCheck`。
（同一プロセス内ではなく別プロセスで動かすこと — 本物の IPC 検証になる）

## 7. LifeRealtime 変更

- `--preview` (flag), `--preview-scale` (float, 既定 0.5),
  `--syphon <name>`（文字列、空なら無効）。
- runner 生成後: フラグに応じて sink を addOutputSink。
- ループ末尾で `runner->pumpOutputs(quit)`; quit なら break。
- status 行は不変。

## 8. 完了条件（エージェント自身で確認）

1. ビルド エラー 0。
2. 回帰: `LifeOfflineRender --scene Presets/coupled_life_basic.json
   --frames 60` エラーなし（sink なし経路が不変であること）。
   `LifeRealtime --scene Presets/default.json --duration 4`（sink なし）
   exit 0・エラー行 0。
3. `LifeRealtime --scene Presets/default.json --preview --duration 6` が
   exit 0・エラー行 0（ウィンドウ経路の実走）。実行中に
   `screencapture -x /tmp/preview_shot.png` を撮り、ウィンドウが実際に
   表示されているか確認できれば尚可（GUI セッション前提。sandbox で失敗
   するコマンドは dangerouslyDisableSandbox）。
4. Syphon e2e: `LifeRealtime --scene Presets/default.json --syphon LifeSim
   --duration 10` をバックグラウンド起動 → 別プロセスで `./build/SyphonCheck
   LifeSim` が exit 0（フレーム受信）。
5. どうしても Syphon が成立しない場合のフォールバック: OutputSink 抽象 +
   WindowPreview は完成させ、SyphonSink は CMake オプション
   `LIFE_WITH_SYPHON=OFF` 既定で除外し、何がどう失敗したかを正確に報告する
   （30 分相当以上は粘らない）。

## 制約の注意

- OutputSink.h は pure C++（Metal 型を出さない）。
- WindowPreview/Syphon の Metal 操作は各 .mm 内のみ。
- sink なしの経路にコストゼロ（分岐 1 個以外）を保つ。
- 既存のプリセット・モジュール・シェーダは一切変更しない
  （Present.metal の追加は可）。
