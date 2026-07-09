# Metal Life Simulation Core 設計指示書 v1.0

## 使い方

この文書は、Metalで生命シミュレーション基盤を実装するAI、エンジニア、研究者に渡すための設計指示書である。

目的は、単体のVJエフェクトを作ることではない。  
目的は、Lenia、Slime Mold Simulation、Particle Life、Reaction Diffusion、Boids、Cellular Automata、Fluidなどを、共通のMetal実行基盤上で扱えるようにすることである。

この文書では、細かいMetal shaderの実装までは確定しない。  
ただし、アーキテクチャ、分割方針、実装対象、推奨ライブラリ、参照すべき論文、最初に作るべき機能、後回しにすべき機能は明確に定義する。

---

# 0. 結論

一つの巨大アプリにすべての生命シミュレーションを詰め込むべきではない。

作るべきものは以下である。

```text
LifeCore
  Metal上でField系とParticle系を実行する共通基盤

LifeRealtime
  VJ用のリアルタイム実行アプリ

LifeOfflineRender
  作品出力用の高解像度オフラインレンダラー

LifeBench
  GPU負荷、解像度、粒子数、pass単位の性能測定

LifePresetLab
  seed探索、parameter sweep、thumbnail生成

LifeCaptureReplay
  OSC入力とAudioFeatureStateの記録再生
```

最初に完成させるべきシミュレーションは以下である。

```text
Reaction Diffusion
Lenia
Cellular Automata
Slime Mold
Particle Life
Boids
```

最初から入れるべきではないものは以下である。

```text
MPM
SPH
PBD
Soft Body
Cloth
Artificial Chemistry
Ecosystem
High Level Multi Agent Cognition
GUI Editor
Node Editor
Timeline Editor
```

理由は単純である。  
最初から範囲を広げると、基盤が完成する前に実装が散らかる。  
まずFieldとParticleの相互作用に特化したMetal基盤を完成させる。

---

# 1. プロジェクトの目的

## 1.1 最終目的

音に反応する生命シミュレーション群をMetalで実装し、以下の2つの実行モードで使えるようにする。

```text
Realtime Mode
  VJ用
  60fps目標
  低レイテンシ
  OSC入力で即時反応
  UIなし
  Headless起動
  将来的にSyphon、NDI、Window出力へ拡張可能

Offline Render Mode
  作品出力用
  固定タイムステップ
  高解像度
  高粒子数
  同じseedと入力記録から再現可能
  image sequenceまたはmovieとして保存
```

## 1.2 このプロジェクトで重要な思想

このプロジェクトで重要なのは、見た目のエフェクトではなく、生命ルールが音によって変化することである。

悪い設計。

```text
kickで明るくする
snareで画面を白くする
fftで色を変えるだけ
```

良い設計。

```text
kickでbirth rateやgrowth rateが変わる
snareでkernelやinteraction matrixが切り替わる
hihatでsensor jitterやmicro noiseが増える
beatでorganism presetがmorphする
fftでforce fieldやspecies matrixが変形する
```

VJとしての価値は、単なる反応ではなく、システムの内部ルールが音で変わることにある。

---

# 2. 入力仕様

## 2.1 OSC入力

以下のOSC addressを受信する。

```text
/kick    float 0..1
/snare   float 0..1
/hihat   float 0..1
/perc    float 0..1
/beat    float 0..1
/fft     float[128]
```

`/fft` は以下の2形式を受けられる設計にする。

```text
方式A
  128個のfloat argumentとして送る

方式B
  blobとして送る
```

初期実装では方式Aだけでもよい。  
ただし、将来の互換性を考えるとblob形式も受けられる設計が望ましい。

## 2.2 AudioFeatureState

OSCを各シミュレーションへ直接渡してはいけない。  
必ず中間状態に正規化する。

```cpp
struct AudioFeatureState {
    float kick;
    float snare;
    float hihat;
    float perc;
    float beat;

    float fft[128];

    float rms;
    float low;
    float mid;
    float high;
    float centroid;
    float flux;

    float time;
    float deltaTime;

    uint32_t frameIndex;
};
```

## 2.3 派生特徴量

`/fft` から以下を毎フレーム計算する。

```text
low
  fft[0..15] の平均または合計

mid
  fft[16..63] の平均または合計

high
  fft[64..127] の平均または合計

rms
  全体エネルギー

centroid
  spectral centroid

flux
  前フレームとの差分
```

## 2.4 入力スムージング

すべての入力に以下を持たせる。

```text
raw
smoothed
peak
decay
trigger
hold
```

生のOSC値をそのままGPUへ入れると、動きが安っぽくなる。  
特に`/kick`、`/snare`、`/beat`は、triggerとdecayを分けて扱う必要がある。

---

# 3. 推奨技術スタック

## 3.1 言語

推奨。

```text
Core
  C++20

Metal Bridge
  Objective C++

Shader
  Metal Shading Language

CLI
  C++ CLI

GUI
  初期版では作らない
```

Swiftは初期のcoreには使わない。  
Swiftは将来のGUIや管理ツールには使ってよいが、シミュレーションエンジンの中核はC++とObjective C++で構成する。

理由。

```text
Metal resource layoutを明示的に管理しやすい
C++のSIMD、buffer、memory layoutを扱いやすい
外部ライブラリと接続しやすい
RealtimeとOfflineで同じcoreを使いやすい
```

## 3.2 OSC

第一候補。

```text
oscpack
```

理由。

```text
C++で扱いやすい
UDPベースのOSC受信に十分
小さく、依存が少ない
headlessアプリと相性がよい
```

参照。

```text
https://github.com/RossBencina/oscpack
```

Swift側ツールを作る場合の候補。

```text
orchetect swift osc
```

参照。

```text
https://github.com/orchetect/swift-osc
```

## 3.3 CLI

推奨。

```text
CLI11
```

用途。

```text
LifeRealtime
LifeOfflineRender
LifeBench
LifePresetLab
LifeCaptureReplay
```

参照。

```text
https://github.com/CLIUtils/CLI11
```

## 3.4 JSON

推奨。

```text
nlohmann/json
```

用途。

```text
Preset
Scene
Audio Mapping
Capture metadata
Render metadata
Benchmark result
```

参照。

```text
https://github.com/nlohmann/json
```

## 3.5 画像書き出し

初期出力。

```text
PNG
EXR
Raw float dump
```

推奨。

```text
TinyEXR
libpng
```

理由。

```text
EXRはRGBA16FloatやRGBA32Floatの保存に向く
PNGはpreview用に十分
Raw float dumpはdebug用に便利
```

参照。

```text
https://github.com/syoyo/tinyexr
https://www.libpng.org/pub/png/libpng.html
```

## 3.6 動画書き出し

初期では必須ではない。  
最初はimage sequenceを優先する。

将来的な候補。

```text
AVFoundation
AVAssetWriter
CVMetalTextureCache
```

参照。

```text
https://developer.apple.com/documentation/avfoundation/avassetwriter
https://developer.apple.com/documentation/corevideo/cvmetaltexturecache
```

## 3.7 GPU補助ライブラリ

候補。

```text
Metal Performance Shaders
MPSGraph
Accelerate vDSP
```

注意。

```text
MPSは便利だが、すべてをMPSに任せない
Leniaの大きいradial kernelは自前computeまたはFFT方式が必要
小さいblurや一部のfilterにはMPSを使ってよい
FFTはAccelerate vDSPまたはMPSGraph FFTを検討
```

参照。

```text
https://developer.apple.com/documentation/metalperformanceshaders
https://developer.apple.com/documentation/accelerate/vdsp/fft
```

## 3.8 ライブ出力

初期では実装しなくてよい。  
ただし、出力抽象化は用意する。

将来候補。

```text
Syphon
NDI
Blackmagic DeckLink
Window Preview
```

参照。

```text
https://github.com/Syphon/Syphon-Framework
https://ndi.video/for-developers/ndi-sdk/
```

---

# 4. アプリ構成

## 4.1 LifeCore

共通ライブラリ。  
すべての実行アプリはこの上に作る。

役割。

```text
Metal実行基盤
Resource管理
Field2D
ParticleSet2D
SpatialHashGrid
AudioFeatureState
ParameterBus
RenderTarget
Composite
FrameRecorder
Preset
Scene
Benchmark hook
```

## 4.2 LifeRealtime

VJ用リアルタイム実行アプリ。

要件。

```text
headlessで起動
OSC受信
60fps目標
低レイテンシ
GPU timing出力
scene json読み込み
任意でpreview frame dump
将来的にSyphonやNDI出力へ接続
```

起動例。

```text
LifeRealtime
  scene Presets/default.json
  width 1920
  height 1080
  fps 60
  osc port 9000
  seed 1234
```

## 4.3 LifeOfflineRender

作品出力用の高解像度レンダラー。

要件。

```text
固定dt
固定seed
OSC記録再生
高解像度
substep指定
frame sequence保存
metadata保存
途中再開可能
```

起動例。

```text
LifeOfflineRender
  scene Presets/shot001.json
  width 4096
  height 4096
  fps 30
  frames 3600
  substeps 4
  seed 1234
  audio capture input.jsonl
  output renders/shot001
```

## 4.4 LifeBench

性能測定用。

測るもの。

```text
module別GPU time
pass別GPU time
resolution別GPU time
particle count別GPU time
spatial hash build cost
splat cost
readback cost
memory usage
```

## 4.5 LifePresetLab

探索用。  
初期では後回しでよいが、早めに作る価値は高い。

役割。

```text
parameter sweep
seed sweep
audio mapping探索
thumbnail生成
短いpreview動画生成
良い状態だけ保存
```

## 4.6 LifeCaptureReplay

OSCとAudioFeatureStateを記録再生する。

役割。

```text
OSC記録
AudioFeatureState記録
ParameterBus記録
同じ入力を再生
RealtimeとOfflineで同じ結果を確認
```

ライブ中に良い状態が出ても、再現できなければ作品制作に使いにくい。  
CaptureReplayは早めに作るべきである。

---

# 5. リポジトリ構成

```text
LifeSimSuite

  LifeCore

    Metal
      MetalContext
      CommandGraph
      PipelineCache
      ResourcePool
      TexturePool
      BufferPool
      GPUTimer

    Audio
      OSCReceiver
      AudioFeatureState
      FeatureSmoother
      BeatState
      FFTFeatures

    Params
      ParameterBus
      AudioMapping
      EnvelopeFollower
      Preset
      Scene

    Field
      Field2D
      PingPongTexture
      KernelTexture
      Convolution
      Diffusion
      Advection
      Gradient
      Laplacian
      Boundary

    Particle
      ParticleSet2D
      SpeciesMatrix
      Integrator
      ParticleSplat
      TrailDeposit
      FieldSampler

    Spatial
      SpatialHashGrid
      CellSort
      CellRange
      NeighborIterator

    Render
      RenderTarget
      CompositePass
      ColorMap
      PostFX

    IO
      FrameRecorder
      ImageSequenceWriter
      VideoWriter
      CaptureReplay

    Math
      Random
      Noise
      Hash
      Easing
      Color

  Modules

    FieldModules
      ReactionDiffusion
      Lenia
      CellularAutomata

    ParticleModules
      SlimeMold
      ParticleLife
      Boids

    CoupledModules
      FieldParticleCoupler
      FlowFieldCoupler

  Apps
    LifeRealtime
    LifeOfflineRender
    LifeBench
    LifePresetLab
    LifeCaptureReplay

  Shaders

    Common
      CommonTypes.metal
      Random.metal
      Noise.metal
      Sampling.metal

    Field
      ReactionDiffusion.metal
      Lenia.metal
      CellularAutomata.metal
      FieldOps.metal

    Particle
      SlimeMold.metal
      ParticleLife.metal
      Boids.metal
      ParticleSplat.metal

    Spatial
      SpatialHash.metal
      PrefixSum.metal
      Sort.metal

    Render
      Composite.metal
      ColorMap.metal
      PostFX.metal

  Presets
    default.json
    lenia_basic.json
    slime_basic.json
    particle_life_basic.json
    coupled_life_basic.json

  Docs
    architecture.md
    simulation_notes.md
    references.md
```

---

# 6. Core設計

## 6.1 MetalContext

役割。

```text
MTLDevice
MTLCommandQueue
MTLLibrary
PipelineCache
ResourcePool
GPU timing
Debug labels
```

要件。

```text
Headlessで起動できる
WindowやCAMetalLayerに依存しない
複数command bufferを扱える
Offlineでは固定stepで実行できる
```

## 6.2 CommandGraph

各moduleのcompute passを順序付きで実行する。

基本フロー。

```text
Begin Frame

Update Audio Uniforms

Encode Simulation Passes

Encode Coupling Passes

Encode Splat Passes

Encode Composite Pass

Readback or Output

End Frame
```

重要なルール。

```text
各simulationが勝手にcommand bufferをcommitしない
command bufferの所有はrunner側に集約する
simulationはencodeだけを行う
```

## 6.3 PipelineCache

同じMetal compute pipelineを毎回作らない。

機能。

```text
function nameからpipeline取得
compile option管理
debug label付与
hot reloadは後回し
```

## 6.4 ResourcePool

Metal textureとbufferを各moduleが勝手に作らない。

必要なもの。

```text
TexturePool
BufferPool
TransientResource
PersistentResource
FrameResource
```

理由。

```text
GPU memoryを管理しやすくする
OfflineとRealtimeでresource policyを切り替える
module追加時にresource leakを防ぐ
```

## 6.5 共通Uniform

Metal shaderへ渡す共通uniform。

```cpp
struct AudioUniforms {
    float kick;
    float snare;
    float hihat;
    float perc;
    float beat;

    float rms;
    float low;
    float mid;
    float high;
    float centroid;
    float flux;

    float time;
    float deltaTime;

    float fft[128];

    uint32_t frameIndex;
};
```

---

# 7. Field2D Core

## 7.1 対象

```text
Reaction Diffusion
Lenia
Cellular Automata
SmoothLife
Fluid Density
Fluid Velocity
Slime Trail Field
Nutrient Field
Force Field
Mask Field
```

## 7.2 Field2DDesc

```cpp
struct Field2DDesc {
    uint32_t width;
    uint32_t height;
    PixelFormat format;
    uint32_t channels;
    bool pingPong;
    BoundaryMode boundary;
};
```

## 7.3 Pixel Format

推奨。

```text
R16Float
  軽量scalar field

R32Float
  高精度scalar field

RG16Float
  velocity field

RGBA16Float
  visual output

RGBA32Float
  debugまたはoffline high precision
```

基本方針。

```text
Scalar fieldはR16FloatまたはR32Float
Velocity fieldはRG16Float
Visual outputはRGBA16Float
Debug precise fieldはR32FloatまたはRGBA32Float
```

## 7.4 Field Operation

必須。

```text
clear
copy
swap
diffuse
decay
blur
laplacian
gradient
divergence
advection
convolution
normalize
inject impulse
apply mask
```

## 7.5 Boundary

最初に実装するboundary。

```text
Wrap
Clamp
Mirror
Zero
```

Lenia、Particle Life、Slime MoldではWrapが重要。  
VJ用途では画面端で死ぬより、toroidal worldの方が綺麗に見える。

---

# 8. Particle Core

## 8.1 対象

```text
Slime Mold Agents
Particle Life
Boids
SPH 将来
Multi Agent 将来
Artificial Chemistry 将来
```

## 8.2 ParticleSet2D

AoSではなくSoA寄りにする。

```text
positionBuffer
velocityBuffer
speciesBuffer
attributeBuffer
randomStateBuffer
```

推奨属性。

```cpp
struct ParticleAttributes {
    float age;
    float life;
    float mass;
    float radius;
    uint32_t flags;
};
```

## 8.3 共通operation

```text
integrate
apply damping
apply boundary
reset dead particles
sample field
splat to texture
deposit trail
apply force field
apply attraction
species based interaction
```

## 8.4 Particle output

粒子系も最終的には必ずtextureへ変換する。

```text
ParticleBuffer
  ↓
ParticleSplat
  ↓
RGBA16Float Texture
  ↓
Composite
```

これを守らないと、Field系とParticle系の合成が破綻する。

---

# 9. Spatial Hash Grid

## 9.1 対象

```text
Particle Life
Boids
SPH
Artificial Chemistry
Multi Agent local communication
Differential Growth repulsion
```

## 9.2 禁止する実装

以下は禁止。

```text
for each particle
  for each particle
    interact
```

粒子数が増えた瞬間に破綻する。

## 9.3 採用する実装

初期版は2D uniform gridでよい。

```text
position to cell id
sort by cell id
build cell ranges
iterate neighbor cells
```

基本設定。

```text
cellSize = interactionRadius
neighbor cells = 3 x 3
boundary = Wrap
```

## 9.4 必要なMetal pass

```text
Compute cell id
Sort by cell id
Build cell range
Neighbor interaction
```

Sortは初期版では簡易実装でもよい。  
ただし、100k以上のParticle Lifeを狙うならGPU sortが必要になる。

---

# 10. Field Particle Coupling

このプロジェクトの核である。

## 10.1 接続パターン

```text
Particle reads Field
  Slimeがtrailを読む
  Particle LifeがLenia densityを読む
  Boidsがfluid velocityを読む

Particle writes Field
  Slimeがtrailをdepositする
  Particleがdensityをsplatする
  Boidsがfluidへforceを加える

Field modulates Particle rules
  Reaction Diffusionがspecies attractionを変える
  Leniaがparticle birth rateを変える
  FFT fieldがlocal forceを作る

Particle modulates Field rules
  Particle densityがLenia growthを変える
  Slime trailがReaction Diffusion feedを変える
```

## 10.2 初期で必ず作るCoupler

```text
FieldSampler
  particle positionからfield値を読む

TrailDeposit
  particle positionへfield値を書き込む

ParticleSplat
  particleをRGBA16Floatへ描画する

FieldToForce
  field gradientをparticle forceに変換する

AudioToField
  fft[128]から2D force fieldまたはmask fieldを作る
```

## 10.3 最初に作るCoupled Scene

```text
Lenia field
  ↓
Slime attractor
  ↓
Slime trail
  ↓
Reaction Diffusion feed map
  ↓
Particle Life force modulation
  ↓
Composite
```

目的は、個別demoではなく、一つの生命系として見える状態を作ること。

---

# 11. Simulation Module Interface

単一の万能Simulation classだけでは足りない。  
Field系とParticle系で必要resourceが違うため、interfaceを分ける。

## 11.1 共通interface

```cpp
struct SimulationContext {
    MetalContext* metal;
    ResourcePool* resources;
    ParameterBus* params;
    CommandGraph* graph;
    uint32_t frameIndex;
};

class SimulationModule {
public:
    virtual void setup(SimulationContext& ctx) = 0;
    virtual void reset(uint32_t seed) = 0;
    virtual void updateCPU(const AudioFeatureState& audio) = 0;
    virtual void encode(SimulationContext& ctx) = 0;
    virtual TextureHandle outputTexture() const = 0;
    virtual ~SimulationModule() {}
};
```

## 11.2 FieldModule

```cpp
class FieldModule : public SimulationModule {
public:
    virtual Field2DHandle outputField() const = 0;
};
```

## 11.3 ParticleModule

```cpp
class ParticleModule : public SimulationModule {
public:
    virtual ParticleSetHandle particles() const = 0;
};
```

## 11.4 CoupledModule

```cpp
class CoupledModule : public SimulationModule {
public:
    virtual void bindFieldInput(Field2DHandle field) = 0;
    virtual void bindParticleInput(ParticleSetHandle particles) = 0;
};
```

## 11.5 Module分類

```text
Reaction Diffusion
  FieldModule

Lenia
  FieldModule

Cellular Automata
  FieldModule

Slime Mold
  FieldModule + ParticleModule + CoupledModule

Particle Life
  ParticleModule

Boids
  ParticleModule
```

---

# 12. 実装するシミュレーション

## 12.1 Reaction Diffusion

基盤検証用として最初に作る。

モデル。

```text
Gray Scott model
2 channel field
Ping Pong
Laplacian
Feed Kill
```

処理。

```text
state A
  read

reaction diffusion compute

state B
  write

swap
```

音マッピング。

```text
kick
  feed増加

snare
  kill perturbation

hihat
  diffusion noise

beat
  preset morph

fft
  local feed map
```

参照。

```text
Alan Turing
The Chemical Basis of Morphogenesis
1952
https://www.dna.caltech.edu/courses/cs191/paperscs191/turing.pdf
```

## 12.2 Lenia

Field2D Coreの本命。

基本処理。

```text
state field
  ↓
kernel convolution
  ↓
growth function
  ↓
state update
  ↓
swap
```

初期仕様。

```text
single channel Lenia
single radial kernel
direct convolution
resolution 512 or 1024
wrap boundary
```

次段階。

```text
multi kernel
multi channel
FFT convolution
organism preset
audio morph
```

必要部品。

```text
KernelTexture
KernelParams
MultiKernelSupport
GrowthFunctionParams
WrapBoundary
```

音マッピング。

```text
kick
  growth rate

snare
  kernel switch

hihat
  noise injection

perc
  local disturbance

beat
  organism preset morph

fft
  kernel shape modulation
```

参照。

```text
Bert Wang Chak Chan
Lenia Biology of Artificial Life
2018
https://arxiv.org/abs/1812.05433

Bert Wang Chak Chan
Lenia and Expanded Universe
2020
https://arxiv.org/abs/2005.03742

Stephan Rafler
Generalization of Conway's Game of Life to a continuous domain SmoothLife
2011
https://arxiv.org/abs/1111.1567
```

## 12.3 Cellular Automata

Field2Dの軽量検証用。

初期ルール。

```text
Game of Life
Brian's Brain
Seeds
Cyclic Cellular Automata
```

用途。

```text
高速pattern layer
mask生成
glitch source
transition source
```

音マッピング。

```text
kick
  birth threshold

snare
  rule switch

hihat
  random cell injection

beat
  rule morph

fft
  spatial mask
```

## 12.4 Slime Mold Simulation

Slime MoldはParticle CoreとField2D Coreの両方を使う。

構造。

```text
agents particle buffer
  ↓
sense trail field
  ↓
turn
  ↓
move
  ↓
deposit trail
  ↓
trail diffusion
  ↓
trail decay
```

初期パラメータ。

```text
sensor angle
sensor distance
turn speed
move speed
deposit amount
trail decay
trail diffusion
```

初期粒子数。

```text
100k
500k
1Mはベンチ後
```

音マッピング。

```text
kick
  deposit amount

snare
  sensor angle jump

hihat
  jitter

perc
  random turn impulse

beat
  phase reset

fft
  attractor field
```

参照。

```text
Jeff Jones
Characteristics of Pattern Formation and Evolution in Approximations of Physarum Transport Networks
Artificial Life
2010
https://pubmed.ncbi.nlm.nih.gov/20067403/
```

## 12.5 Particle Life

Particle CoreとSpatial Hashの本命。

構造。

```text
particles
species
species interaction matrix
neighbor search
force integration
particle splat
```

初期仕様。

```text
species count 8
particles 100k
wrap boundary
spatial hash enabled
RGBA16Float output
```

次段階。

```text
species count 16
species count 32
particles 500k
particles 1M
field coupling
audio controlled interaction matrix
```

音マッピング。

```text
kick
  attraction matrix boost

snare
  matrix shuffle

hihat
  micro jitter

perc
  local repulsion

beat
  color remap

fft
  species interaction modulation
```

参照。

```text
Tom Mohr Particle Life Framework
https://github.com/tom-mohr/particle-life
```

## 12.6 Boids

Particle CoreとSpatial Hashを共用する。

基本ルール。

```text
separation
alignment
cohesion
```

拡張。

```text
multi species
predator
leader
obstacle
flow field
audio reactive flocking
```

音マッピング。

```text
kick
  separation burst

snare
  predator spawn

hihat
  local jitter

perc
  direction impulse

beat
  leader switch

fft
  flow field
```

参照。

```text
Craig Reynolds
Flocks, Herds, and Schools
1987
https://www.cs.toronto.edu/~dt/siggraph97-course/cwr87/
```

---

# 13. CompositeとRendering

## 13.1 出力統一

すべてのsimulation outputはRGBA16Float textureへ集約する。

```text
Field系
  fieldからcolor mapを通してRGBA16Floatへ

Particle系
  particle splatでRGBA16Floatへ

Composite
  全layerを合成
```

## 13.2 Composite Pass

必要機能。

```text
add
screen
multiply
max
alpha blend
feedback
bloom用prepass
color map
tone map
```

## 13.3 Post FX

初期は最小限でよい。

```text
feedback
blur
bloom
vignette
color remap
temporal trail
```

注意。  
Post FXに頼りすぎない。  
生命シミュレーションそのものの構造が弱いと、エフェクトだけが派手な薄い映像になる。

---

# 14. PresetとScene

## 14.1 Scene JSON例

```json
{
  "seed": 1234,
  "resolution": [1920, 1080],
  "modules": [
    {
      "type": "ReactionDiffusion",
      "name": "rd0",
      "enabled": true,
      "params": {
        "feed": 0.037,
        "kill": 0.061
      }
    },
    {
      "type": "Lenia",
      "name": "lenia0",
      "enabled": true,
      "params": {
        "dt": 0.1,
        "growthMu": 0.15,
        "growthSigma": 0.015
      }
    },
    {
      "type": "SlimeMold",
      "name": "slime0",
      "enabled": true,
      "params": {
        "agentCount": 200000,
        "sensorAngle": 0.785,
        "sensorDistance": 12.0
      }
    }
  ],
  "connections": [
    {
      "from": "lenia0.field",
      "to": "slime0.attractorField",
      "mode": "sample"
    }
  ],
  "audioMappings": [
    {
      "source": "kick",
      "target": "lenia0.growthRate",
      "scale": 0.5,
      "smoothing": 0.2
    }
  ]
}
```

## 14.2 ParameterBus

流れ。

```text
OSC
  ↓
AudioFeatureState
  ↓
AudioMapping
  ↓
ParameterBus
  ↓
SimulationUniforms
  ↓
Metal Shader
```

各simulationはOSCアドレスを知らないこと。

---

# 15. Audio Mapping設計

## 15.1 基本マッピング

```text
kick
  birth
  explosion
  growth rate
  field injection

snare
  rule switch
  kernel switch
  matrix shuffle
  topology break

hihat
  jitter
  diffusion noise
  trail shimmer
  fine particle emission

perc
  local impulse
  collision
  direction change

beat
  preset morph
  phase reset
  scene transition

fft
  force field
  kernel shape
  species matrix
  color palette
  spatial mask
```

## 15.2 FFTの使い方

`fft[128]` は単純に色へ入れない。  
以下の用途へ使う。

```text
2D force field生成
Lenia kernel shape modulation
Particle Life species matrix modulation
Slime Mold attractor field
Reaction Diffusion local feed map
Color palette selection
```

## 15.3 AudioToField

FFTを2D fieldへ変換するmoduleを作る。

入力。

```text
fft[128]
time
beat phase
seed
```

出力。

```text
R16Float mask field
RG16Float force field
RGBA16Float color control field
```

---

# 16. Realtime Mode仕様

## 16.1 要件

```text
headless起動
OSC受信
60fps目標
GPU timing
stdout status
optional frame dump
optional Syphon or NDI later
```

## 16.2 Realtimeで優先するsimulation

```text
Reaction Diffusion
Lenia low resolution
Slime Mold
Particle Life
Boids
Cellular Automata
Post FX
```

## 16.3 Realtimeで避けること

```text
巨大kernelのdirect convolutionを4Kで回す
readbackを毎フレーム行う
大量EXR保存を同時に行う
MPMや高精度SPHを初期から入れる
```

---

# 17. Offline Render仕様

## 17.1 要件

```text
固定dt
固定seed
OSC記録再生
高解像度
substep設定
frame sequence保存
metadata保存
途中再開可能
```

## 17.2 出力

```text
frame_000000.exr
frame_000001.exr
metadata.json
audio_features.jsonl
scene_snapshot.json
benchmark.json
```

## 17.3 Realtimeとの関係

RealtimeとOfflineでsimulation codeを分けない。  
分けるのはrunnerだけ。

```text
Realtime
  小さい解像度
  少ないsubstep
  少ない粒子数
  即時反応

Offline
  高解像度
  多いsubstep
  多い粒子数
  固定dt
  frame export
```

---

# 18. Benchmark仕様

LifeBenchで必ず測定する。

## 18.1 最低ベンチ

```text
ReactionDiffusion
  1920 x 1080

Lenia direct convolution
  512 x 512
  1024 x 1024

SlimeMold
  100k agents
  500k agents
  1M agents

ParticleLife
  100k particles
  500k particles
  1M particles

Boids
  100k particles
  500k particles

Composite
  4 layers
  8 layers
```

## 18.2 測るもの

```text
GPU time per pass
CPU time per frame
resource allocation count
texture memory
buffer memory
readback cost
spatial hash build cost
splat cost
composite cost
```

## 18.3 合格基準の目安

初期目標。

```text
Reaction Diffusion 1080p
  60fpsを狙う

Lenia 1024
  direct convolutionで実用速度を確認

Slime Mold 100k
  60fpsを狙う

Particle Life 100k
  spatial hash込みで実用速度を確認

Composite 4 layers
  負荷が支配的にならない
```

---

# 19. 実装フェーズ

## Phase 1 Core最小構成

作るもの。

```text
MetalContext
PipelineCache
CommandGraph
ResourcePool
PingPongTexture
Field2D
RenderTarget
FrameRecorder
OSCReceiver
AudioFeatureState
ParameterBus
LifeRealtime
LifeOfflineRender
LifeBench
```

実装するsimulation。

```text
Reaction Diffusion
```

完了条件。

```text
LifeRealtimeがheadlessで起動する
OSCを受信できる
AudioFeatureStateが更新される
Metal compute passが動く
RGBA16Float render targetが生成される
PNG previewを書き出せる
Reaction Diffusionが動く
```

## Phase 2 Field系完成

作るもの。

```text
Convolution
KernelTexture
Laplacian
FieldComposite
BoundaryMode
AudioToField
```

実装するsimulation。

```text
Lenia
Cellular Automata
```

完了条件。

```text
Field2Dが安定している
Leniaが動く
Cellular Automataが動く
Field outputをCompositeできる
Offline Renderで同じseedから同じ結果が出る
```

## Phase 3 Particle系導入

作るもの。

```text
ParticleSet2D
ParticleSplat
TrailDeposit
FieldSampler
```

実装するsimulation。

```text
Slime Mold
```

完了条件。

```text
ParticleSet2Dが動く
Slime Moldが動く
ParticleがFieldを読む
ParticleがFieldへ書く
Trail diffusionとdecayが動く
```

## Phase 4 Spatial Hash導入

作るもの。

```text
SpatialHashGrid
CellSort
CellRange
NeighborIterator
SpeciesMatrix
```

実装するsimulation。

```text
Particle Life
Boids
```

完了条件。

```text
SpatialHashGridが動く
Particle Lifeが100k以上で動く
Boidsが100k以上で動く
Particle系をtextureへsplatできる
```

## Phase 5 Coupling

作るもの。

```text
FieldToForce
ParticleDensityField
FieldDrivenRuleModulation
AudioDrivenRuleMorph
```

作るシーン。

```text
Reaction Diffusion → Lenia
Lenia → Slime Mold
Slime Mold → Particle Life
Particle Life → Composite
Audio → 全module
```

完了条件。

```text
Lenia fieldがSlimeを誘導する
Slime trailがReaction Diffusionを変える
Particle LifeがFieldを読む
Audioが複数moduleのルールを同時に変える
RealtimeとOfflineで同じSceneを実行できる
```

---

# 20. 後回しにするもの

以下は初期版では実装しない。

```text
MPM
SPH
PBD
Soft Body
Cloth
Artificial Chemistry
Ecosystem
Multi Agent cognition
GUI editor
Timeline editor
Node editor
```

ただし、将来入れる前提で論文と設計だけは把握しておく。

---

# 21. 将来拡張用の参照資料

## 21.1 Fluid

参照。

```text
Jos Stam
Stable Fluids
1999
https://pages.cs.wisc.edu/~chaol/data/cs777/stam-stable_fluids.pdf

Mark Harris
Fast Fluid Dynamics Simulation on the GPU
GPU Gems Chapter 38
https://developer.nvidia.com/gpugems/gpugems/part-vi-beyond-triangles/chapter-38-fast-fluid-dynamics-simulation-gpu
```

導入タイミング。

```text
Field2DとCouplingが安定してから
```

## 21.2 SPH

参照。

```text
Müller et al.
Particle Based Fluid Simulation for Interactive Applications
2003
https://matthias-research.github.io/pages/publications/sca03.pdf

Monaghan
Smoothed Particle Hydrodynamics
Annual Review of Astronomy and Astrophysics
1992
```

導入タイミング。

```text
SpatialHashGridが安定してから
```

## 21.3 PBD

参照。

```text
Müller et al.
Position Based Dynamics
2007
https://www.sciencedirect.com/science/article/abs/pii/S1047320307000065
```

導入タイミング。

```text
Particle CoreとConstraint Solverを分離できる見通しが立ってから
```

## 21.4 MPM

参照。

```text
Stomakhin et al.
A Material Point Method for Snow Simulation
SIGGRAPH 2013
https://disneyanimation.com/publications/a-material-point-method-for-snow-simulation/
```

導入タイミング。

```text
第2世代以降
Realtime基盤に無理に混ぜない
```

---

# 22. 実装AIへの明確な指示

以下を守ること。

```text
1. UIを作らない
2. まずheadless CLIで動かす
3. Metal computeを中心にする
4. 各simulationがOSCを直接読まない
5. 各simulationが勝手にtextureやbufferを作らない
6. ResourcePool経由でGPU resourceを作る
7. 全simulation outputはRGBA16Float textureへ集約する
8. Particle系は最終的にsplatしてtextureへ出す
9. RealtimeとOfflineでsimulation codeを分けない
10. runnerだけを分ける
11. shader共通化をやりすぎない
12. まずReaction Diffusion、Lenia、Slime Mold、Particle Lifeを完成させる
13. BoidsはParticle LifeのSpatialHashが安定してから追加する
14. MPM、SPH、PBDは後回し
15. すべてseedで再現可能にする
16. すべてのpassにGPU timingを付ける
17. まず動くMVPを作る
18. 抽象化は成功パターンが3つ出てから強める
```

---

# 23. 最初に作るべきScene

## 23.1 Scene A Field Basic

内容。

```text
Reaction Diffusion
Lenia
Cellular Automata
Composite
Audio mapping
```

目的。

```text
Field2D Coreの検証
Ping Pong Textureの検証
Offline reproducibilityの検証
```

## 23.2 Scene B Slime Basic

内容。

```text
Slime Mold
Trail Field
Trail Diffusion
Trail Decay
Particle Splat
Audio mapping
```

目的。

```text
Particle reads Field
Particle writes Field
の検証
```

## 23.3 Scene C Particle Life Basic

内容。

```text
Particle Life
Species Matrix
Spatial Hash
Particle Splat
Audio mapping
```

目的。

```text
Spatial Hash Gridの検証
Particle Coreの性能検証
```

## 23.4 Scene D Coupled Life

内容。

```text
Lenia field
  ↓
Slime attractor
  ↓
Slime trail
  ↓
Reaction Diffusion feed map
  ↓
Particle Life force modulation
  ↓
Composite
```

目的。

```text
個別simulationではなく、一つの生命系として見える状態を作る
```

---

# 24. 判断基準

このプロジェクトの成否は、「何種類のシミュレーションが入ったか」では決まらない。

成否は以下で決まる。

```text
FieldとParticleが自然に接続できるか
音で見た目ではなくルールが変わるか
RealtimeとOfflineが同じsceneを共有できるか
seedと入力記録から再現できるか
GPU負荷が測定できるか
新しいsimulationを足してもcoreが壊れないか
```

最初のMVPはこれで十分。

```text
LifeCore
LifeRealtime
LifeOfflineRender
LifeBench

Reaction Diffusion
Lenia
Slime Mold
Particle Life

OSC input
AudioFeatureState
ParameterBus
Composite
Frame export
```

BoidsはParticle LifeのSpatial Hashが安定してから追加する。  
FluidはField2DとCouplingが安定してから追加する。  
MPM、SPH、PBDはこの基盤の第2世代でよい。

---

# 25. 最後の注意

最初から全部入りを狙うと失敗する。

本当に作るべきものは、FieldとParticleの相互作用に特化したMetal生命シミュレーション基盤である。

この基盤ができれば、Lenia、Slime Mold、Particle Lifeは自然に乗る。  
逆に、この基盤なしで個別実装を始めると、3つ目あたりからresource管理、OSC mapping、録画、合成、presetが全部バラバラになり、後から直すコストが爆発する。

最初にやるべきことは、派手なsimulationではない。

```text
MetalContext
Field2D
ParticleSet2D
ParameterBus
FrameRecorder
```

この5つを退屈なくらい堅く作ること。  
そこが固ければ、生命シミュレーションは後から増やせる。

---

# 26. 参照リンク一覧

```text
Metal Documentation
https://developer.apple.com/documentation/metal

Metal Performance Shaders
https://developer.apple.com/documentation/metalperformanceshaders

Accelerate vDSP FFT
https://developer.apple.com/documentation/accelerate/vdsp/fft

Open Sound Control
https://www.cnmat.berkeley.edu/opensoundcontrol

oscpack
https://github.com/RossBencina/oscpack

CLI11
https://github.com/CLIUtils/CLI11

nlohmann/json
https://github.com/nlohmann/json

TinyEXR
https://github.com/syoyo/tinyexr

libpng
https://www.libpng.org/pub/png/libpng.html

AVAssetWriter
https://developer.apple.com/documentation/avfoundation/avassetwriter

CVMetalTextureCache
https://developer.apple.com/documentation/corevideo/cvmetaltexturecache

Syphon Framework
https://github.com/Syphon/Syphon-Framework

NDI SDK
https://ndi.video/for-developers/ndi-sdk/

Turing
The Chemical Basis of Morphogenesis
https://www.dna.caltech.edu/courses/cs191/paperscs191/turing.pdf

Lenia
Biology of Artificial Life
https://arxiv.org/abs/1812.05433

Lenia Expanded Universe
https://arxiv.org/abs/2005.03742

SmoothLife
https://arxiv.org/abs/1111.1567

Jeff Jones Physarum
https://pubmed.ncbi.nlm.nih.gov/20067403/

Particle Life Framework
https://github.com/tom-mohr/particle-life

Craig Reynolds Boids
https://www.cs.toronto.edu/~dt/siggraph97-course/cwr87/

Stable Fluids
https://pages.cs.wisc.edu/~chaol/data/cs777/stam-stable_fluids.pdf

GPU Gems Fluid
https://developer.nvidia.com/gpugems/gpugems/part-vi-beyond-triangles/chapter-38-fast-fluid-dynamics-simulation-gpu

SPH Interactive Applications
https://matthias-research.github.io/pages/publications/sca03.pdf

Position Based Dynamics
https://www.sciencedirect.com/science/article/abs/pii/S1047320307000065

MPM Snow Simulation
https://disneyanimation.com/publications/a-material-point-method-for-snow-simulation/
```
