# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

SCVP(Simple Comparison Video Player): 두 영상을 프레임 단위로 정확히 맞춰 비교하는 플레이어(동기 재생, 오프셋, 분할/와이프/전환/차이, 확대 시 픽셀 값 표시). C++17, Qt6 Widgets, FFmpeg, OpenGL 3.3 core(macOS 4.1), 오디오 없음. 사용자 문서([README.md](README.md))는 한국어입니다. 반응 속도와 GPU 활용이 설계상 최우선입니다.

## 빌드와 실행

```bash
source .deps/env.sh             # root 권한이 없는 머신에서만: scripts/fetch-ubuntu-deps.sh 가 푼 의존성
cmake --preset dev              # Ninja, RelWithDebInfo, build/dev (그 외: debug, release, asan, tsan)
cmake --build --preset dev
build/dev/src/scvp a.mp4 b.mp4 --view diff --offset -3
```

- API 하한은 **Qt 6.4, FFmpeg 6.1**(Ubuntu 24.04)이고, CI의 Linux 빌드가 정확히 이 버전으로 확인합니다. FFmpeg 버전 차이는 모두 `engine/FfmpegCompat.h`(FFmpeg의 유일한 include 지점, 버전 비교는 `AV_VERSION_INT`)에서 처리합니다. `FFMPEG_ROOT=<prefix>` 를 주면 pkg-config 대신 그 경로를 씁니다([cmake/ScvpFFmpeg.cmake](cmake/ScvpFFmpeg.cmake)).
- `release` 프리셋은 테스트를 빌드하지 않습니다. 패키지: build/release 에서 `cpack`, 또는 [scripts/package-linux.sh](scripts/package-linux.sh) 로 `dist/` 에 독립 실행 번들 생성.
- 성능 측정: `build/dev/tools/scvp-bench <video> --scenario seq|realtime|random|backstep|cached --hw cpu|gpu --json`(두 영상은 `--with <B> --scenario lockstep`), `QT_QPA_PLATFORM=offscreen build/dev/tests/render/bench_render`(ctest 아님).

## 테스트

```bash
tests/fixtures/gen_fixtures.sh          # 또는: cmake --build --preset dev --target fixtures
ctest --preset dev                      # 전체 (8 jobs)
ctest --preset dev -R tst_engine.backwardStepping
build/dev/tests/engine/tst_engine backwardStepping   # 같은 테스트를 QtTest로 직접 실행
scripts/xvfb-run.sh ctest --preset dev  # 전체를 별도 Xvfb + Mesa에서 실행 (CI와 같은 방식)
```

- fixture(`tests/fixtures/out/` 에 생성, `manifest.tsv` 와 ffmpeg `framemd5` 기준값 포함)를 만들려면 x264/x265/vpx/svtav1/aom이 들어간 ffmpeg CLI가 필요합니다. fixture가 필요한 테스트는 없으면 `QSKIP` 하므로(`SCVP_REQUIRE_FIXTURES()`), fixture 없이 전부 통과해도 검증된 것은 거의 없습니다.
- [tests/CMakeLists.txt](tests/CMakeLists.txt) 의 `scvp_add_test(name src... LIBS ... FUNCTIONS f1 f2)`: `FUNCTIONS` 를 주면 QtTest 함수마다 `name.fn` 이라는 별도 ctest 항목이 됩니다(느린 fixture 테스트를 병렬로 돌리는 방법). 이런 테스트에 함수를 추가하면 목록에도 넣어야 합니다. 안 넣으면 ctest가 실행하지 않습니다.
- 모든 테스트는 `QT_QPA_PLATFORM=offscreen` 으로 돌지만, GL 테스트는 접근 가능한 X 디스플레이(GLX)가 있어야 하고 없으면 skip합니다. Linux에서는 렌더 GL 테스트마다 `.mesa` 변형(`__GLX_VENDOR_LIBRARY_NAME=mesa LIBGL_ALWAYS_SOFTWARE=1`)이 있어 셰이더 계산을 두 드라이버에서 확인합니다. 하드웨어 디코딩·CUDA 테스트는 해당 하드웨어가 없으면 skip합니다.
- E2E([tests/e2e/tst_app_grab.cpp](tests/e2e/tst_app_grab.cpp))는 실제 `scvp` 를 `scripts/xvfb-run.sh` 아래에서 띄우고, 공개 옵션(`--frame/--play/--view/--zoom/...`)과 [src/main.cpp](src/main.cpp) 에 정의된 숨은 플래그 `--debug-grab/--debug-actions/--debug-hover/--debug-export/...` 로 조작합니다(`--debug-grab` 은 `debug-state:` 줄로 명령줄이 정한 상태도 출력). 테스트를 위해 사용자 데스크톱에 창을 띄우지 말고 이 플래그나 xvfb-run을 쓰세요.
- 공개 명령줄 옵션은 [src/ui/CommandLine.cpp](src/ui/CommandLine.cpp) 의 표 하나에서 파서와 `--help` 를 함께 만듭니다. 옵션을 추가할 때는 표에 넣고 `readLaunchOptions`/`applyLaunchOptions` 와 [tests/ui/tst_commandline.cpp](tests/ui/tst_commandline.cpp), README 사용법 표를 함께 고치세요.
- 정확성 기준은 **비트 단위 일치**입니다: 디코딩한 프레임은 순차·랜덤·역방향 모두 `ffmpeg -f framemd5` 와 같아야 하고, 하드웨어 디코딩도 마찬가지입니다(`frameMd5AsSoftware` 가 NV12/P010을 다시 묶어 비교). GL 출력은 CPU 기준값과 비교합니다.
- sanitizer: `--preset asan` / `--preset tsan`. 이 머신의 커널에서는 TSan 바이너리를 `setarch -R` 로 실행해야 합니다.

## 아키텍처

세 개의 정적 라이브러리가 엄격한 계층을 이룹니다. 각 계층은 아래 계층만 링크합니다:

- `scvp::engine`(`src/engine`): FFmpeg과 스레드만 사용, **Qt 없음**. 색인, 디코딩, 캐시, 재생 제어 로직.
- `scvp::render`(`src/render`): Qt Gui/OpenGL. 프레임 업로드, RGB 변환, 비교 모드 그리기. 셰이더는 `src/render/shaders` 에 있고 Qt 리소스 `:/scvp` 로 포함됩니다.
- `scvp::ui`(`src/ui`): Qt Widgets. 창, 컨트롤, 키 처리, 설정. `scvp` 실행 파일은 그 위의 [src/main.cpp](src/main.cpp) 하나입니다.

### 프레임 번호

프레임 번호는 타임스탬프가 아니라 `FrameIndex` 의 *표시 순서 인덱스*(`pres`)입니다. `IndexScanner` 가 우선순위 낮은 스레드에서 파일 전체를 디먹싱만 하고(디코딩 없음) 불변 스냅샷을 내놓습니다. 재생을 바로 시작할 수 있게 초기 스냅샷을 먼저, 이후 주기적으로 냅니다. 색인이 끝나기 전에는 안정된 앞부분의 번호만 최종이고, 그 전에 디코딩된 프레임은 `provisional` 로 표시됩니다. 정렬은 pts 기준이고, pts가 없는 raw H.264/HEVC와 AVI는 POC(picture order count) 기준입니다. 각 `Frame` 은 `pres` 와 `dec`(디코딩 순서 패킷 인덱스)를 함께 가집니다.

소스는 `IVideoSource`/`IDecodeSession`([engine/VideoSource.h](src/engine/VideoSource.h))을 구현합니다: `FFmpegSource`(컨테이너, raw 엘리멘터리 스트림), `RawVideoSource`(.yuv/Bayer), `ImageSequenceSource`. `openVideoSource()` 가 그중 하나를 고릅니다. `SourceCaps::randomAccess` 인 소스는 여러 워커가 병렬로 디코딩합니다.

### 디코딩 파이프라인 (UI를 절대 막지 않음)

```
PlaybackController (UI 스레드, 타임라인 위치)
  -> 슬롯별 PlaybackIntent -> Engine::request()
  -> planPrefetch() -> WantList (우선순위 구간 + 그에 맞는 CachePolicy)
  -> WantMailbox (최신 요청만 유효; 새 generation이 진행 중인 디코딩을 취소)
  -> DecodeLane 스레드 -> FrameCache::offer()   (RAM 계층 + VRAM 계층)
  -> 프레임 콜백 -> EngineBridge (합쳐진 queued Qt 시그널) -> Player tick -> VideoView
```

- `Engine` 은 슬롯 두 개(A = 0, B = 1)를 가집니다. 슬롯마다 소스, 풀, 캐시, 메일박스, 레인이 따로 있고, 메모리 예산 하나를 프레임 크기 비율로 나눠 씁니다.
- `PrefetchPlanner` 와 `FrameCache` 의 `CachePolicy` 는 같은 비율(정지: 마지막 이동 방향 60/40, 재생: 80/20)을 써서 캐시가 계획한 프레임을 정확히 유지합니다. 바꿀 때는 둘을 함께 바꾸세요.
- `DecodeLane` 은 현재 디코더 위치에서 계속 디코딩할지, 키 패킷으로 seek할지 중 패킷이 덜 드는 쪽을 고릅니다. 목표 이전 프레임은 `AVDISCARD_NONREF` 로 디코딩합니다.
- `PlaybackController` 는 `PlaybackBackend` 인터페이스(앱에서는 `EngineBackend`, 테스트에서는 가짜 구현) 위의 순수 로직이고 절대 기다리지 않습니다. **Lockstep**: 두 영상의 프레임이 모두 디코딩돼야 그 위치를 표시합니다. 아니면 재생이 멈췄다가 그 지점에서 다시 기준을 잡습니다(밀린 프레임을 몰아 보여주지 않음). 키를 누르고 있을 때의 반복은 `positionShown()` 을 기다립니다.
- `TimelineMapper` 는 공용 타임라인 위치를 각 영상의 프레임으로 바꿉니다(B = A + offset, 프레임 또는 타임스탬프 동기). 타임라인 범위를 벗어난 영상은 첫/끝 프레임을 *HOLD* 합니다.
- `FramePacer`/`VsyncClock` 은 버퍼 swap 마다 vsync 격자에 맞춰 tick을 냅니다. swap이 대기하지 않으면 타이머로 대신합니다.

### GPU 경로

- `CudaApi` 는 CUDA 드라이버 API를 실행 중에 로드합니다(`libcuda.so.1`/`nvcuda.dll`). 그래서 CUDA 툴킷이 필요 없고, NVIDIA가 아닌 머신에서는 없다고만 보고합니다.
- `HwDevice` 는 하드웨어 장치 컨텍스트를 프로세스 전체에서 공유합니다. GPU 우선 디코딩은 규격상 비트 정확한 코덱에만 쓰고, 그 외에는 소프트웨어로 디코딩합니다. 장치가 실제로 규격대로 디코딩하는지는 (장치, 코덱, 픽셀 형식)마다 처음 한 번 키 프레임을 소프트웨어 결과와 비교해 확인하고(`FFmpegDecodeSession::verifyHardware`, 결과는 `HwDevices::verdict`), 다르면 그 조합은 프로세스가 끝날 때까지 소프트웨어로 디코딩합니다. 확인 전의 하드웨어 프레임은 표시하지 않습니다(예: GitHub macOS VM의 VideoToolbox는 8비트 HEVC 색차가 다름).
- CUDA에서는 프레임이 VRAM에 머물 수 있고(`FrameLocation::Cuda`, RAM으로 스필되는 VRAM 캐시 계층), 렌더는 이를 `CudaInteropSource` 로 zero-copy 매핑합니다. 나머지는 모두 `CpuUploadSource`(텍스처 더블 버퍼)를 거칩니다.
- 내보내기(`ui::ExportJob`): 재생 파이프라인과 별개입니다. `Engine::source()` 로 슬롯의 소스를 공유받아(색인 재스캔 없음) 영상마다 디코드 스레드를 두고, 자체 GL 컨텍스트 스레드가 `Renderer` + `render::OffscreenView`(오버레이 레이어, PBO 비동기 리드백)로 그린 뒤, 인코드 스레드가 `engine::VideoEncoder`(BT.709 4:2:0은 자체 고정소수점 변환, 무손실은 RGB 그대로)로 씁니다. 단계 사이는 작은 bounded queue로 잇습니다.
- 렌더: `TextureSource` -> `RgbConverter`(`convert.frag`: 픽셀 값 오버레이용 원본 코드값도 보존, GPU Bayer 디모자이크) -> 표시 패스(`display.frag`: 비교 모드, 숫자, 커서 픽셀 테두리) -> 비동기 `PixelReadback`. 셰이더가 읽지 못하는 형식은 swscale로 먼저 변환합니다.

## 작업 규칙

- `dev` 브랜치에서 작업하고, 구현 단위마다 커밋 하나(기능 + 테스트, 그 커밋만으로 빌드·테스트 통과). Conventional Commits에 `feat(engine)`, `perf(render)`, `test(ui)`, `build`, `ci` 같은 scope를 씁니다. 요청 없이 push하거나 `main` 에 merge하지 않습니다.
- 코드 주석은 영어로, *왜* 그런지와 불변 조건을 설명합니다. 코드를 추가할 때 같은 밀도를 유지하세요.
