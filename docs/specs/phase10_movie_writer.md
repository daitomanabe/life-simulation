# Phase 10 実装仕様: MovieWriter (AVAssetWriter) — 直接動画書き出し

設計: Fable / 実装: Sonnet。設計指示書 §3.6（動画書き出し: AVFoundation /
AVAssetWriter）に対応。現状のオフライン出力は PNG/EXR 連番のみで、
納品・確認には ffmpeg 変換が要る — ProRes/HEVC の .mov を直接吐けるようにする。
全体制約は phase3 仕様の 9 項目（AVFoundation は LifeCore/IO の .mm 内のみ）。

## 新規/変更ファイル

```
新規:
  LifeCore/IO/MovieWriter.h        (pure C++ ヘッダ)
  LifeCore/IO/MovieWriter.mm
変更:
  Apps/LifeOfflineRender/main.cpp  (--movie / --codec / --quality フラグ)
  CMakeLists.txt                   (AVFoundation, CoreMedia, CoreVideo,
                                    VideoToolbox をリンク)
```

## 1. MovieWriter (LifeCore/IO/)

```cpp
enum class MovieCodec { H264, HEVC, ProRes422, ProRes4444 };

struct MovieWriterDesc {
    std::string path;          // .mov
    uint32_t width = 0, height = 0;
    double fps = 30.0;
    MovieCodec codec = MovieCodec::ProRes422;
    float quality = 0.9f;      // H264/HEVC のみ (0..1 -> AVVideoQualityKey)
    float exposure = 1.0f;     // linear -> sRGB 変換時の露出 (PNG と同じ)
};

class MovieWriter {
public:
    ~MovieWriter();
    bool open(const MovieWriterDesc& desc, std::string& outError);
    // FrameRecorder::halfPixels() の RGBA16F 生値を 1 フレーム追加。
    // 内部で linear->sRGB 8bit BGRA へ変換 (ImageWriter と同じカーブ) し
    // CVPixelBuffer へ。呼び出しはフレーム順・単一スレッド前提。
    bool appendFrame(const uint16_t* halfRGBA, std::string& outError);
    // 全フレーム追加後に必ず呼ぶ (finishWriting を同期待ち)。
    bool finish(std::string& outError);
    uint64_t framesWritten() const;
};
```

実装メモ:
- AVAssetWriter + AVAssetWriterInput (AVVideoCodecKey:
  h264 / hevc / AppleProRes422 / AppleProRes4444) +
  AVAssetWriterInputPixelBufferAdaptor
  (kCVPixelFormatType_32BGRA、sRGB カラータグ:
  kCVImageBufferColorPrimaries_ITU_R_709_2 /
  TransferFunction_ITU_R_709_2 / YCbCrMatrix_ITU_R_709_2 を attachment で)。
- presentationTime = CMTimeMake(frameIndex, fps を timescale に —
  非整数 fps に備え CMTimeMakeWithSeconds(frame / fps, 600) でよい)。
- `isReadyForMoreMediaData` が NO の間は 1ms スリープで待つ
  (offline なので単純ブロッキングで可)。
- 変換は CPU で: half→float (arm64 __fp16)、linearToSRGB は
  ImageWriter.mm と同一式。行パディング (CVPixelBufferGetBytesPerRow)
  に注意。
- expectsMediaDataInRealTime = NO。

## 2. LifeOfflineRender 統合

- 新フラグ: `--movie out.mov`（指定時のみ有効）、
  `--codec prores422|prores4444|h264|hevc`（既定 prores422）、
  `--quality 0.9`。
- `--movie` は `--format` (png/exr) と併用可: movie 指定時は毎フレーム
  readback が必要になるので needReadback 判定に `|| movieWriter` を追加。
  PNG/EXR を書かないフレームでも movie へは全フレーム追加する。
  `--resume` とは併用不可（movie は追記再開できない → 指定されたら
  エラーで exit 1、明確なメッセージ）。
- 終了時 finish() を呼び、`[life] movie: 900 frames -> out.mov (123.4 MB)`
  を stderr に出す。metadata.json に movie パス/コーデックを追記。

## 3. 完了条件（エージェント自身で確認）

1. ビルド エラー 0。
2. `LifeOfflineRender --scene Presets/fluid_basic.json --frames 90 --fps 30
   --movie /tmp/fluid_test.mov --codec prores422 --format png` が exit 0。
   PNG も並行して出ていること（併用確認）。
3. 生成 .mov の妥当性検証:
   `mdls -name kMDItemDurationSeconds -name kMDItemPixelWidth /tmp/fluid_test.mov`
   が duration ≈ 3.0 秒 (±0.1)・width 1280 を返す。かつ
   `afinfo` ではなく **AVFoundation で開き直す検証**として、
   swift スクリプト or 小さな objc ワンショット
   (`osascript -l JavaScript` は不可; `swift -e` も CommandLineTools に
   ないため、検証用 .mm を一時コンパイルして AVAsset の
   duration/naturalSize を print → 検証後削除、手順を報告に残す) で
   duration と naturalSize を確認。mdls が Spotlight 無効環境で空を
   返す場合は AVAsset 検証のみで可。
4. h264 でも 60f 書けて exit 0・ファイルサイズ > 100KB。
5. `--movie` + `--resume` 併用が明確なエラーで exit 1。
6. 既存経路の回帰: `--movie` なしの fluid_basic 60f が従来どおり動く
   （エラー行 0）。

コーデック画質の目視確認はレビュー側で行う。
