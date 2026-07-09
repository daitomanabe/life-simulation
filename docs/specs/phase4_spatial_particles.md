# Phase 4 実装仕様: Spatial Hash Grid + Particle Life + Boids

設計: Fable / 実装: Sonnet。設計指示書 §9, §12.5, §12.6, §19 Phase 4 に対応。
前提: Phase 3 (ParticleSet2D / ParticleSplat / SlimeMold) がマージ済み。
参照: `docs/specs/phase3_particle_core.md` の全体制約 9 項目（同様に厳守）、
`Modules/ParticleModules/SlimeMold.{h,cpp}`, `LifeCore/Particle/ParticleSet2D.h`。

## 決定性の設計（このフェーズの核心）

- 全粒子ペア O(n²) は禁止（§9.2）。2D uniform grid + counting sort。
- atomic scatter の書き込み順は非決定的だが、**per-cell 挿入ソート**で
  正準順序（粒子 index 昇順）に直す → 近傍反復順が決定的 → float 加算順も
  決定的 → 同一 seed でビット一致。
- 粒子間相互作用シムは positions/velocities を **ping-pong**
  (`ParticleSet2DDesc::pingPongPosVel = true`)。read 側から読み write 側へ
  書く。パス内の in-place 更新は禁止（レース = 非決定）。

## 新規ファイル

```
LifeCore/Spatial/SpatialHashGrid.h/.cpp
Shaders/Spatial/SpatialHash.metal
Modules/ParticleModules/ParticleLife.h/.cpp
Modules/ParticleModules/Boids.h/.cpp
Shaders/Particle/ParticleLife.metal
Shaders/Particle/Boids.metal
Presets/particle_life_basic.json
Presets/boids_basic.json
```

既存ファイルへの追記可: `Shaders/Particle/ParticleSplat.metal`（RGB スプラット
ヘルパー）、CMakeLists.txt、Modules/RegisterModules.cpp。

## 1. SpatialHashGrid (LifeCore/Spatial/)

世界 = シーン解像度の px 空間 (toroidal)。`cellsX = ceil(width / cellSize)`,
`cellsY = ceil(height / cellSize)`, `numCells = cellsX * cellsY`。

```cpp
struct SpatialHashDesc {
    uint32_t maxParticles = 0;
    uint32_t width = 0, height = 0; // world in px
    float cellSize = 24.0f;         // == interactionRadius (§9.3)
    std::string label;
};

class SpatialHashGrid {
public:
    bool create(ResourcePool& pool, const SpatialHashDesc& desc);
    void destroy();
    // positions: float2 バッファ (read 側)。count 粒子分の全パスを encode。
    void encodeBuild(CommandGraph& graph, const std::string& labelPrefix,
                     BufferHandle positions, uint32_t count);
    BufferHandle cellStart() const;      // uint × numCells (exclusive start)
    BufferHandle cellCount() const;      // uint × numCells
    BufferHandle sortedIndices() const;  // uint × maxParticles
    uint32_t cellsX() const; uint32_t cellsY() const;
    uint32_t numCells() const; float cellSize() const;
};
```

内部バッファ (全て GPUPrivate): cellOf (uint×maxParticles), cellCount,
scanA, scanB (uint×numCells), cellCursor, sortedIndices。

### カーネル (Shaders/Spatial/SpatialHash.metal)

uniform (スカラーのみ):

```c
struct GridBuildParams {
    uint particleCount; uint numCells; uint cellsX; uint cellsY;
    float cellSize; float worldW; float worldH;
    uint scanStride;   // scan パスごとに更新
};
```

1. `hashClearCells(cellCount atomic_uint*, params)` — dispatch1D(numCells)、0 に。
2. `hashCount(positions const float2*, cellOf uint*, cellCount atomic_uint*,
   params)` — dispatch1D(count): pos を [0,W)×[0,H) に wrap →
   `cx = min(uint(px/cellSize), cellsX-1)` → cell = cy*cellsX+cx →
   cellOf[i]=cell, atomic++。
3. `hashScanInclusive(src const uint*, dst uint*, params)` —
   dispatch1D(numCells): `dst[i] = src[i] + (i >= stride ? src[i-stride] : 0)`。
   C++ 側で stride = 1,2,4,... と ping-pong しながら ceil(log2(numCells)) 回。
   初回の src は cellCount。
4. `hashFinalize(inclusive const uint*, cellCount const uint*,
   cellStart uint*, cellCursor atomic_uint*, params)` — dispatch1D(numCells):
   `start = inclusive[i] - cellCount[i]`（exclusive化）; cellStart[i]=start;
   cursor[i]=start。
5. `hashScatter(cellOf const uint*, cellCursor atomic_uint*,
   sortedIndices uint*, params)` — dispatch1D(count):
   `slot = atomic_fetch_add(&cursor[cellOf[i]], 1); sorted[slot] = i;`
6. `hashSortCells(cellStart const uint*, cellCount const uint*,
   sortedIndices uint*, params)` — dispatch1D(numCells): 自セル範囲を
   **挿入ソート（index 昇順）**。count > 256 のセルはソートをスキップして良い
   （病的密集時のみ決定性が緩む。コメントで明記）。

C++ scanA/scanB の最終結果がどちらに載るかを管理し hashFinalize に渡すこと。

### 消費側の近傍ループ（PL/Boids の .metal にこのパターンを直書き）

```metal
float2 pi = posR[i];
int2 cc = int2(pi / cellSize);            // wrap 済み座標から
for (int dy = -1; dy <= 1; ++dy)
  for (int dx = -1; dx <= 1; ++dx) {
    int cx = (cc.x + dx + int(cellsX)) % int(cellsX);
    int cy = (cc.y + dy + int(cellsY)) % int(cellsY);
    uint cell = uint(cy) * cellsX + uint(cx);
    uint start = cellStart[cell], n = cellCount[cell];
    for (uint k = start; k < start + n; ++k) {
      uint j = sorted[k];
      if (j == i) continue;
      float2 d = posR[j] - pi;
      d -= float2(worldW, worldH) * round(d / float2(worldW, worldH)); // 最小像
      ...
    }
  }
```

## 2. Particle Life (§12.5)

### モジュール構成
- ParticleSet2D (pingPong=true), SpatialHashGrid (cellSize=rMax)
- interactionMatrix: `K*K` float の **Shared** バッファ。CPU 生成:
  `SplitMix64(deriveSeed(seed_, 0x4D545831 + shuffleCount_))` で [-1,1]。
- speciesColor: `K*4` float Shared。cosine palette:
  `c = 0.5 + 0.5*cos(6.2832*(s/K + float3(0.0,0.33,0.67)))`。
- RGB density: uint × (w*h*3)（atomic、固定小数点 256）。

### PLParams (C++/MSL ミラー、この順)

```c
uint  particleCount; uint speciesCount;
float dt; float rMax; float beta;
float forceScale; float friction; float maxSpeed;
float jitter; float forceBoost;   // kick → 引力ブースト
uint  seed; uint frameIndex;
float worldW; float worldH;
float cellSize; uint cellsX; uint cellsY;
```

### カーネル (Shaders/Particle/ParticleLife.metal)

1. `plInit(posW, velW, species, random, PLParams)`: pos 一様ランダム、
   `species[i] = min(uint(rand01(...) * K), K-1)`、vel = 0。
2. `plStep(posR, velR, posW, velW, species, random, cellStart, cellCount,
   sorted, matrix const float*, PLParams, AudioUniforms)`:
   - 近傍ループで力を合算（標準 Particle Life 力）:
     `rn = r / rMax`;
     `rn < beta` → `f = rn/beta - 1`（普遍斥力、負）;
     `beta <= rn < 1` → `a = matrix[si*K+sj] * (1 + forceBoost)`,
     `f = a * (1 - fabs(2*rn - 1 - beta) / (1 - beta))`;
     `force += normalize(d) * f`（r < 1e-5 はスキップ）。
   - `vel = velR[i] + force * forceScale * dt`;
     `vel *= exp(-friction * dt)`; 長さを maxSpeed に clamp;
     jitter > 0 なら ランダム単位ベクトル × jitter × dt を加算。
   - `pos = wrap(pi + vel * dt)`; posW/velW へ書く。
3. スプラット: ParticleSplat.metal に追記する共有ヘルパーを使用:
   - `splatAddRGB(device atomic_uint* rgb, uint cellIdx, float3 c)`
     （3 チャンネル atomic add, scale 256）
   - `plSplatAccum(posR, species, speciesColor const float*, rgb, PLParams)`
   - `splatResolveRGB(rgb device atomic_uint*, target read_write,
     SplatResolveRGBParams {uint width; uint height; float gain;})`
     — 読んで target += rgb/256*gain、自セルを 0 クリア。**共有カーネル**
     （Boids も使う）なので ParticleSplat.metal に置く。

### encode 順
grid.encodeBuild(posR) → plStep → set.swap() → (初回 splatClear) →
plSplatAccum → splatResolveRGB(output_ へ; output_ は毎フレーム
clearTexture で黒クリアしてから)。ColorMapPass は使わない（スプラットが
そのまま絵）。outputTexture() = output_ (RGBA16F)。

### updateCPU での matrix shuffle（リプレイ決定性を保つ設計）
- scene param `shufflePulse`（audioMappings で snare.trigger をマップ）。
- encode 時に `param(ctx, "shufflePulse", 0)` を読み、**0.5 を上向きに横切った
  瞬間**に shuffleCount_++ して matrix を CPU 再生成 →
  `ResourcePool::bufferContents` に memcpy。
- 母数は seed_ と shuffleCount_ のみ → capture リプレイで完全再現。

### プリセット Presets/particle_life_basic.json (Scene C §23.3)
1280×720, seed 5150: particleCount 100000, species 6, rMax 24, beta 0.3,
forceScale 60, friction 4, maxSpeed 160, jitter 0, splatGain 1.0,
blend "add", opacity 1.0。
audioMappings: kick→pl0.forceBoost (scale 1.2, sm 0.08),
snare.trigger→pl0.shufflePulse (scale 1.0, sm 0.0),
hihat→pl0.jitter (scale 30, sm 0.05)。

## 3. Boids (§12.6)

- ParticleSet2D pingPong=true、species 未使用 (0)。grid cellSize = radius。

### BoidsParams (この順)

```c
uint  particleCount;
float dt; float radius;
float sepWeight; float aliWeight; float cohWeight;
float minSpeed; float maxSpeed;
float jitter; float sepBoost;     // kick → separation burst
float impulse;                    // perc → 方向インパルス
float scatterPulse;               // snare.trigger → 散開
uint  seed; uint frameIndex;
float worldW; float worldH;
float cellSize; uint cellsX; uint cellsY;
```

### カーネル (Shaders/Particle/Boids.metal)

1. `boidsInit`: pos 一様、vel = ランダム方向 × (minSpeed+maxSpeed)/2。
2. `boidsStep(posR, velR, posW, velW, random, grid…, BoidsParams,
   AudioUniforms)`:
   - 近傍 (r < radius): sep += -d/r * (1 - r/radius)（近いほど強く離れる）,
     aliSum += velR[j], cohSum += minImage(pos[j]), count++
   - acc = sep*sepWeight*(1+sepBoost) + (aliSum/count - vel)*aliWeight
     + (cohSum/count - pos)*cohWeight
   - impulse > 0: rand < impulse*0.03 → acc += ランダム方向 × maxSpeed*4
   - scatterPulse > 0: rand < scatterPulse*0.08 → vel = ランダム方向 × maxSpeed
   - vel += acc*dt; jitter; 速度を [minSpeed, maxSpeed] に clamp
     （len<1e-4 なら ランダム方向 × minSpeed で再点火）; pos wrap。
3. `boidsSplatAccum`: 色 = heading から
   `0.5+0.5*cos(6.2832*(atan2(v.y,v.x)/6.2832 + float3(0.0,0.33,0.67)))` →
   splatAddRGB。resolve は共有 splatResolveRGB。

### プリセット Presets/boids_basic.json
1280×720, seed 3030: particleCount 100000, radius 14, sepWeight 60,
aliWeight 25, cohWeight 18, minSpeed 40, maxSpeed 140, jitter 0,
splatGain 1.0。
audioMappings: kick→boids0.sepBoost (1.5, sm 0.06),
perc→boids0.impulse (1.0, sm 0.03),
snare.trigger→boids0.scatterPulse (1.0, sm 0.0),
hihat→boids0.jitter (25, sm 0.05)。

## 4. 統合

- CMakeLists.txt: SpatialHashGrid.cpp, ParticleLife.cpp, Boids.cpp を追加
  (`# Spatial` コメントセクションを新設)。
- RegisterModules.cpp: "ParticleLife", "Boids" を登録。

## 5. 完了条件（エージェント自身で確認）

1. ビルドがエラー 0。
2. `LifeOfflineRender --scene Presets/particle_life_basic.json --frames 180
   --output /tmp/pl_smoke --format png` 正常終了、最終 PNG が 40KB 以上
   （クラスタ模様が出る）、stderr にパスエラーなし。
3. 同 2 回実行で最終フレーム md5 一致（per-cell ソートによる決定性の証明）。
4. `LifeOfflineRender --scene Presets/boids_basic.json --frames 180 ...`
   同様に非単色 + md5 一致。
5. `LifeBench --scene Presets/particle_life_basic.json --sizes 1280x720
   --frames 60 --warmup 10` が完走し、gpu/frame を報告に含める。

チューニング（見た目の良さ）はレビュー側で行うので、数値の微調整に
時間をかけないこと。
