# comparison-video-player

SCVP (Simple Comparison Video Player): 두 영상을 프레임 단위로 정확히 맞춰 재생·이동·확대하고, 픽셀 값까지 확인하는 크로스플랫폼(Linux / macOS / Windows) 비교 플레이어입니다. 반응 속도와 최적화를 최우선으로 설계했고, 가능한 곳은 GPU로 처리합니다.

- C++17 · Qt6 Widgets (≥ 6.4) · FFmpeg (≥ 6.1) · OpenGL 3.3 core (macOS 4.1) · 오디오 없음

## 주요 기능

| 기능 | 내용 |
|---|---|
| 형식 | FFmpeg가 읽는 거의 모든 컨테이너/코덱 (mp4, mkv, mov, avi, webm, ts, ...), raw H.264/HEVC/MPEG-2 스트림, raw YUV/RGB/Bayer(`.yuv` 등), 이미지 시퀀스(폴더 또는 번호 붙은 파일) |
| 정확한 프레임 | 프레임 번호는 디코딩 순서 태그 + 인덱스로 결정 (pts가 없는 raw/AVI는 POC로 정렬). 어느 프레임으로 이동해도 항상 같은 그림 |
| 이동 | 1프레임 앞/뒤, 짧은/긴 구간 점프(초 또는 프레임 단위로 설정), 처음/끝, 타임라인 스크럽 |
| 선로딩 캐시 | 디코딩된 프레임을 메모리에 보관(용량 설정). NVIDIA는 VRAM 캐시 + 시스템 메모리로 스필. 재생 방향·점프 대상·루프 구간을 미리 채움 |
| 구간 반복 | `[` `]` 로 A-B 구간 지정, 캐시에 들어가면 디코딩 없이 반복 |
| 2영상 비교 | 동기 재생/이동(lockstep: 두 프레임이 모두 준비돼야 표시), B 오프셋, 범위를 벗어난 영상은 첫/끝 프레임 HOLD, 좌우/위아래·와이프·A/B 전환·차이(배율) |
| 픽셀 값 | 충분히 확대하면 각 픽셀에 RGB 값(원본 비트 심도) 또는 원본 코드값(YUV 등)을 숫자로 표시, 차이 모드는 부호 있는 A−B. 커서 아래 픽셀은 두 영상 모두 테두리 표시 + 하단에 값 표시(Ctrl+C 복사) |
| 색 해석 | 영상별 YUV 행렬(BT.601/709/2020)·범위(limited/full) 재지정, 크로마 최근접(기본, 값 정확)/bilinear |
| GPU | GPU 우선 하드웨어 디코딩(NVDEC/VAAPI/D3D11VA/VideoToolbox, 비트 정확한 코덱만. 처음 한 번 소프트웨어 결과와 비교해 다르면 자동으로 소프트웨어), NVIDIA는 CUDA-GL interop zero-copy 표시. 색 변환·확대·비교·숫자·디모자이크는 모두 셰이더 |
| 전체화면 | `F`/더블클릭, 컨트롤 오버레이 자동 숨김 |

## 빌드

### Ubuntu 24.04

```bash
sudo apt install build-essential cmake ninja-build pkg-config qt6-base-dev \
    libavcodec-dev libavformat-dev libavutil-dev libswscale-dev
cmake --preset dev        # RelWithDebInfo (release 프리셋: 최적화 + 테스트 제외)
cmake --build --preset dev
build/dev/src/scvp video.mp4
```

sudo 없이 개발할 때는 `scripts/fetch-ubuntu-deps.sh` 가 패키지를 `.deps/` 에 풀어 줍니다(`source .deps/env.sh` 후 빌드).

### macOS

```bash
brew install qt ffmpeg cmake ninja pkg-config
cmake --preset dev -DCMAKE_PREFIX_PATH="$(brew --prefix qt)"
cmake --build --preset dev
open build/dev/src/scvp.app
```

### Windows

Qt 6.8 (msvc2022_64), [BtbN FFmpeg](https://github.com/BtbN/FFmpeg-Builds) shared 빌드, Visual Studio 2022 개발자 명령 프롬프트에서:

```bat
cmake --preset dev -DCMAKE_PREFIX_PATH=C:\Qt\6.8.0\msvc2022_64 -DFFMPEG_ROOT=C:\ffmpeg
cmake --build --preset dev
```

FFmpeg DLL은 빌드 시 실행 파일 옆으로 복사됩니다.

### 패키지

```bash
cmake --preset release && cmake --build --preset release
cd build/release && cpack     # Linux: tar.gz, Windows: zip (windeployqt 포함), macOS: dmg
```

CI(`.github/workflows/build.yml`)가 세 OS에서 빌드·테스트·패키지를 만듭니다.

### Linux 설치 (터미널 없이 실행, 기본 프로그램)

```bash
scripts/package-linux.sh                      # dist/scvp-<버전>-linux-x86_64/ 와 .tar.gz 생성 (Qt 포함)
dist/scvp-*-linux-x86_64/install.sh           # 현재 사용자에게 설치 (root 불필요)
```

- 프로그램은 `~/.local/opt/scvp`, 실행 링크는 `~/.local/bin/scvp` 에 설치되고, 애플리케이션 메뉴(활동 → "SCVP" 검색, 독에 고정 가능), 아이콘, 파일 형식이 등록됩니다.
- 영상 파일을 오른쪽 클릭 → **다른 애플리케이션으로 열기 → SCVP**. 두 파일을 선택해 열면 A/B 비교로 열립니다.
- 그냥 설치하면 기존 기본 프로그램은 바뀌지 않습니다. 다만 다른 프로그램이 열지 못하던 raw 형식(.h264/.h265/.yuv/.y4m/.ivf 등)은 SCVP로 열립니다.
- 기본 프로그램으로 쓰려면: 앱 메뉴 **File → Make SCVP the Default Video Player...**, 또는 `install.sh --set-default`, 또는 설정 → 앱 → 기본 앱 → 비디오. 되돌릴 때도 설정 → 기본 앱에서 고르면 됩니다.
- 삭제: `~/.local/opt/scvp/install.sh --uninstall` (SCVP를 가리키는 기본 프로그램 설정도 함께 지웁니다).
- FFmpeg 라이브러리는 시스템 것을 씁니다(Ubuntu 24.04: `sudo apt install ffmpeg`).

## 사용법

```
scvp [A] [B] [옵션]
  --offset N            B는 A 프레임 + N 을 표시
  --sync frame|time     프레임 번호로 맞춤(기본) / 시간으로 맞춤(fps가 다를 때)
  --view split|sbs|tb|wipe|flip|diff
  --raw WxH:pix_fmt[:fps[:header]]   raw 영상으로 열기 (예: 1920x1080:nv12:30)
  --cache-mb M          이번 실행의 메모리 캐시 크기
  --vram-mb M           이번 실행의 VRAM 캐시 크기 (0: 사용 안 함)
  --hwdec gpu|cpu|cuda|vaapi|d3d11va|videotoolbox
  --legend-a 글자       범례에서 A를 파일 이름 대신 이 글자로 표시 (범례가 켜짐)
  --legend-b 글자       B도 마찬가지
```

- 파일을 창의 왼쪽(위쪽) 절반에 놓으면 A, 오른쪽(아래쪽) 절반에 놓으면 B, 두 개를 함께 놓으면 A와 B로 열립니다. 폴더를 놓으면 이미지 시퀀스로 엽니다.
- raw 영상은 파일 이름에서 크기·형식·fps를 읽습니다: `clip_1920x1080_nv12.yuv`, `cam.3840x2160.p010le.60fps.raw`, `sensor_1920x1080_bayer_rggb16le.raw`. 알 수 없으면 입력 창이 뜨고, 파일 크기로 프레임 수를 바로 확인할 수 있습니다.
- 범례에는 파일 이름 대신 직접 입력한 글자(방법·설정 이름 등)를 쓸 수 있습니다: **View → Legend text...**(Ctrl+Shift+L) 또는 `--legend-a`/`--legend-b`. 비워 두면 파일 이름으로 돌아갑니다. 입력한 글자는 A ↔ B 교체 때 영상을 따라가고, 그 슬롯에 다른 파일을 열면 지워지며, 저장되지 않습니다(이번 실행에만 적용).
- 이미지 시퀀스는 폴더 전체 또는 `shot_0001.png` 처럼 번호만 다른 파일들을 자연 정렬 순서로 재생합니다(기본 30 fps).

### 키맵 (설정에서 변경 가능)

| 동작 | 키 |
|---|---|
| 재생/일시정지 | Space |
| 1프레임 뒤/앞 | `,` / `.` (마우스 뒤로/앞으로 버튼) |
| 짧은 점프 뒤/앞 | ← / → |
| 긴 점프 뒤/앞 | ↓ / ↑ |
| 처음/끝 | Home / End |
| B 오프셋 −1/+1 | `<` / `>` |
| 루프 시작/끝, 켜기/끄기, 해제 | `[` / `]`, Ctrl+L, Shift+L |
| 보기 모드 순환 | V |
| 좌우 ⇄ 위아래 / 와이프 / A·B 전환(전환할 때 화면 가운데에 A·B 표시) / 차이 | S / W / Tab 또는 `` ` `` / D |
| 차이 배율 ×2 / ÷2 | G / Shift+G |
| A ↔ B 교체 | X |
| 화면 맞춤 / 1:1 / 확대·축소 | 0 / 1 / `=` `-` (휠: 커서 기준) |
| 화면 맞춤 등 소수 배율에서 부드럽게 ⇄ 정확한 픽셀 | N |
| 픽셀 값 표시 / RGB ⇄ 원본 코드 | P / Y |
| A/B 범례 (영상마다 A·B 표시와 파일 이름) / 범례 글자 입력 | L / Ctrl+Shift+L |
| 정보 오버레이 | I |
| 전체화면 / 해제 | F, F11, 더블클릭 / Esc |
| 열기 A / B | Ctrl+O / Ctrl+Shift+O |
| 픽셀 값 복사 | Ctrl+C |

키는 입력된 문자가 아니라 키 코드로 처리하므로 한글 입력 상태에서도 동작합니다. 키를 누르고 있으면 이전 요청이 화면에 나온 뒤에만 다음 반복을 받아서, 키를 떼면 밀린 작업 없이 바로 멈춥니다.

마우스: 휠 = 커서 기준 확대/축소, 드래그 = 이동, 와이프 분할선 드래그 = 분할 위치, 터치패드 스크롤 = 이동, 핀치 = 확대.

## 설정

File → Settings: 디코딩(GPU 우선 / CPU / 장치 지정), 메모리 캐시(기본: 물리 메모리 25%), VRAM 캐시(기본: 총 VRAM 40%, 사용 가능한 양으로 제한), 점프 간격과 단위, 키맵. 설정 창에 현재 영상 기준으로 캐시에 들어가는 프레임/GOP 수가 표시됩니다.

## HiDPI 주의

픽셀 값을 볼 때 "1:1"은 장치 픽셀 기준입니다. GNOME X11의 분수 배율(예: 125%)처럼 OS가 화면 전체를 다시 확대하는 환경에서는 어떤 배율에서도 화면 픽셀이 영상 픽셀과 정확히 대응하지 않습니다. 픽셀 검사는 100% 또는 200% 배율에서 하세요.

## 테스트

```bash
tests/fixtures/gen_fixtures.sh   # 테스트 영상 + ffmpeg framemd5 기준값 생성 (x264/x265/libvpx/svtav1 필요, libaom은 있으면 사용)
ctest --preset dev
```

- 엔진: 전 fixture 순차/랜덤/역방향 디코딩이 `ffmpeg -f framemd5` 와 비트 단위로 일치하는지, 하드웨어 디코딩 결과, 캐시 예산, 스케줄러, 동기 재생 로직.
- 렌더: 오프스크린 GL로 색 변환·표시·비교 모드·숫자·픽셀 읽기를 CPU 기준값과 비교 (NVIDIA와 Mesa 양쪽).
- E2E: 실제 앱을 가상 디스플레이(Xvfb)에서 실행해 화면을 캡처·검사 (`scripts/xvfb-run.sh`). 예: 3프레임 잘린 영상을 오프셋 −3으로 비교하면 차이 모드가 완전히 검정.
- `cmake --preset asan` / `--preset tsan` 으로 sanitizer 빌드.
- 성능 측정: `build/dev/tools/scvp-bench video.mkv --scenario seq|random|backstep --hw cpu|gpu --json`.

## 알려진 제한

- zero-copy 표시는 NVIDIA(CUDA)만 구현되어 있고, 그 외 GPU는 하드웨어 디코딩 후 시스템 메모리를 거쳐 업로드합니다(기능은 같음).
- MIPI RAW10/12 packed 형식은 아직 지원하지 않습니다(16비트 Bayer로 변환해서 열 수 있음).
- HDR(PQ/HLG) 톤 매핑, 디인터레이스, 자막, 오디오는 범위 밖입니다.
