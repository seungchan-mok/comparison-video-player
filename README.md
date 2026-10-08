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
| 픽셀 값 | 충분히 확대하면 각 픽셀에 RGB 값(원본 비트 심도) 또는 원본 코드값(YUV 등)을 숫자로 표시, 차이 모드는 부호 있는 A−B. 커서 아래 픽셀은 두 영상 모두 테두리 표시 + 하단에 값 표시(Ctrl+Shift+C 복사) |
| 색 해석 | 영상별 YUV 행렬(BT.601/709/2020)·범위(limited/full) 재지정, 크로마 최근접(기본, 값 정확)/bilinear |
| GPU | GPU 우선 하드웨어 디코딩(NVDEC/VAAPI/D3D11VA/VideoToolbox, 비트 정확한 코덱만. 처음 한 번 소프트웨어 결과와 비교해 다르면 자동으로 소프트웨어), NVIDIA는 CUDA-GL interop zero-copy 표시. 색 변환·확대·비교·숫자·디모자이크는 모두 셰이더 |
| 화면 복사 | Ctrl+C: 보이는 그대로(확대·이동, 비교 모드, 픽셀 숫자, 범례·정보 오버레이) 클립보드에 무손실 PNG로 복사. 화면에서 영상이 축소돼 보이면 영상이 1:1이 되는 해상도로 GPU에서 다시 그려 화면보다 선명하게 담음(최대 4천만 화소) |
| 비교 영상 내보내기 | Ctrl+E: 구간(전체·루프·지정)을 비교 화면 그대로 영상으로. 전체 프레임(영상 1:1, 비교 모드 유지) 또는 화면 그대로, H.264/HEVC MP4(GPU 인코더 우선)·무손실 FFV1 MKV·PNG 시퀀스, 범례·프레임 번호 선택. 재생과 별개로 백그라운드에서 진행 |
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
scripts/package-linux.sh      # Linux 릴리즈 번들 (docker만 필요, 아래 참고)
cmake --preset release && cmake --build --preset release
cd build/release && cpack     # Windows: zip (Qt·FFmpeg DLL·VC++ 런타임 포함), macOS: dmg
```

CI(`.github/workflows/build.yml`)가 세 OS에서 빌드·테스트·패키지를 만들고, 패키지가 다른 설치 없이 실행되는지 확인합니다(Linux: 여러 배포판의 깨끗한 컨테이너, Windows: Windows만 PATH에 둔 실행과 설치·삭제).

`scripts/package-linux.sh` 는 glibc 2.28 컨테이너([packaging/linux/Dockerfile](packaging/linux/Dockerfile): Qt 6.8, 거기서 빌드하는 LGPL FFmpeg)에서 번들을 만들고, 시스템 기본 라이브러리(glibc, OpenGL, X11, fontconfig 등) 외에는 모두 넣습니다. `scripts/smoke-linux-bundle.sh <이미지> <tar.gz> <클립>` 으로 원하는 배포판에서 확인할 수 있습니다.

### Linux 설치 (터미널 없이 실행, 기본 프로그램)

릴리즈의 `scvp-<버전>-linux-x86_64.tar.gz` 는 Ubuntu 20.04 이상, Debian 12 이상, Fedora, RHEL/Rocky 8 이상(x86_64; glibc 2.28 이상인 다른 배포판도 대개 동작)에서 따로 설치할 것 없이 실행됩니다. Qt와 FFmpeg가 들어 있습니다(시스템 FFmpeg 불필요).

```bash
tar xzf scvp-*-linux-x86_64.tar.gz
scvp-*-linux-x86_64/install.sh                # 현재 사용자에게 설치 (root 불필요)
scvp-*-linux-x86_64/scvp a.mp4 b.mp4          # 또는 설치 없이 바로 실행
```

- 프로그램은 `~/.local/opt/scvp`, 실행 링크는 `~/.local/bin/scvp` 에 설치되고, 애플리케이션 메뉴(활동 → "SCVP" 검색, 독에 고정 가능), 아이콘, 파일 형식이 등록됩니다.
- 영상 파일을 오른쪽 클릭 → **다른 애플리케이션으로 열기 → SCVP**. 두 파일을 선택해 열면 A/B 비교로 열립니다.
- 그냥 설치하면 기존 기본 프로그램은 바뀌지 않습니다. 다만 다른 프로그램이 열지 못하던 raw 형식(.h264/.h265/.yuv/.y4m/.ivf 등)은 SCVP로 열립니다.
- 기본 프로그램으로 쓰려면: 앱 메뉴 **File → Make SCVP the Default Video Player...**, 또는 `install.sh --set-default`, 또는 설정 → 앱 → 기본 앱 → 비디오. 되돌릴 때도 설정 → 기본 앱에서 고르면 됩니다.
- 삭제: `~/.local/opt/scvp/install.sh --uninstall` (SCVP를 가리키는 기본 프로그램 설정도 함께 지웁니다).
- 하드웨어 디코딩: NVIDIA(NVDEC, 드라이버 470 이상)는 그대로, Intel/AMD(VA-API)는 시스템의 VA-API 드라이버가 있으면 씁니다.
- 영상 내보내기 인코더: NVIDIA GPU가 있으면 NVENC(H.264/HEVC), 없으면 H.264는 OpenH264, 그 밖에 FFV1과 PNG. 번들의 FFmpeg는 LGPL 빌드라 x264/x265는 들어 있지 않습니다.

### Windows 설치

릴리즈의 `scvp-<버전>-win64-setup.exe` 를 실행합니다. 관리자 권한 없이 현재 사용자에게 설치되고(`%LOCALAPPDATA%\Programs\SCVP`), 따로 설치할 것(VC++ 재배포 패키지, FFmpeg 등)이 없습니다.

- 시작 메뉴에 "SCVP"가 추가되고, 영상 파일 오른쪽 클릭 → **연결 프로그램 → SCVP** 로 열 수 있습니다.
- 기본 플레이어로 쓰려면 설치 마지막 화면의 "기본 비디오 플레이어로 선택"을 켜거나 설정 → 앱 → 기본 앱 → SCVP 에서 고릅니다(Windows는 프로그램이 스스로 기본 앱이 되는 것을 허용하지 않습니다).
- 삭제: 설정 → 앱 → 설치된 앱 → SCVP → 제거.
- 설치 없이 쓰려면 `scvp-<버전>-win64.zip` 을 풀고 `bin\scvp.exe` 를 실행합니다.

## 사용법

```
scvp [옵션] [A] [B]
```

명령줄 옵션으로 비교 화면을 준비한 상태로 띄울 수 있습니다(스크립트나 Claude Code 스킬에서 호출하기 좋게). 옵션은 **이번 실행에만** 적용되고 저장된 설정은 바꾸지 않습니다. 전체 설명은 `scvp --help` 가 출력합니다(구역별 옵션, 예시, 현재 키맵, 마우스; 디스플레이 없는 셸에서도 동작).

| 구역 | 옵션 |
|---|---|
| 파일 | `--raw SPEC`(A·B 모두), `--raw-a SPEC`, `--raw-b SPEC` — raw 영상으로 열기. SPEC = `WxH:pix_fmt[:fps[:header 바이트]]` (예: `1920x1080:nv12:30`, `3840x2160:bayer_rggb16le`) |
| 위치·재생 | `--frame N`(0부터, 표시 순서), `--play`, `--speed X`(1/16–16), `--loop FIRST:LAST` |
| 두 영상 | `--offset N`(B는 A 프레임 + N 을 표시), `--sync frame\|time`(프레임 번호 / 시간으로 맞춤) |
| 비교 보기 | `--view split\|sbs\|tb\|wipe\|flip\|diff`, `--wipe POS`(0–1), `--diff-gain G`(1–256). `--wipe`/`--diff-gain` 은 `--view` 가 없으면 그 모드를 고름 |
| 보기 | `--zoom fit\|1:1\|N`(영상 1픽셀당 화면 N픽셀, 약 27 이상에서 픽셀 값 숫자 표시), `--center X,Y`(이 영상 픽셀을 가운데로, 기본 1:1), `--pixel-mode rgb\|codes`, `--legend`, `--legend-a 글자`, `--legend-b 글자`, `--info`, `--fullscreen` |
| 색 해석 | `--matrix auto\|601\|709\|2020`, `--range auto\|limited\|full` (A·B 모두), `--matrix-a/-b`, `--range-a/-b`(한쪽만, 공통 값보다 우선) |
| 디코딩 | `--hwdec gpu\|cpu\|cuda\|vaapi\|d3d11va\|videotoolbox`, `--cache-mb M`, `--vram-mb M`(0: 사용 안 함) |

```bash
scvp ref.mp4 enc.mp4 --view diff --diff-gain 8 --frame 120            # 차이를 8배로
scvp src.yuv enc.mp4 --raw-a 1920x1080:yuv420p:30 --wipe 0.3           # raw 원본 대 인코딩
scvp a.mp4 b.mp4 --view sbs --zoom 32 --center 960,540 --pixel-mode codes   # 한 픽셀 주변의 YUV 코드값
scvp a.mkv b.mkv --legend-a "x264 crf 20" --legend-b "x265 crf 22" --play --loop 0:99
```

- 잘못된 값은 창을 띄우지 않고 이유를 stderr(`scvp: ...`)에 쓰고 종료 코드 2로 끝납니다. 열 수 없는 파일은 대화상자와 함께 stderr에도 알립니다. 정상 종료(창을 닫음)는 0.
- `--zoom`/`--center` 는 두 영상이 첫 프레임을 보여 준 뒤 적용됩니다. N은 장치 픽셀 기준(1 키와 같음)입니다.

- 파일을 창의 왼쪽(위쪽) 절반에 놓으면 A, 오른쪽(아래쪽) 절반에 놓으면 B, 두 개를 함께 놓으면 A와 B로 열립니다. 폴더를 놓으면 이미지 시퀀스로 엽니다.
- raw 영상은 파일 이름에서 크기·형식·fps를 읽습니다: `clip_1920x1080_nv12.yuv`, `cam.3840x2160.p010le.60fps.raw`, `sensor_1920x1080_bayer_rggb16le.raw`. 알 수 없으면 입력 창이 뜨고, 파일 크기로 프레임 수를 바로 확인할 수 있습니다.
- 범례에는 파일 이름 대신 직접 입력한 글자(방법·설정 이름 등)를 쓸 수 있습니다: **View → Legend text...**(Ctrl+Shift+L) 또는 `--legend-a`/`--legend-b`. 비워 두면 파일 이름으로 돌아갑니다. 입력한 글자는 A ↔ B 교체 때 영상을 따라가고, 그 슬롯에 다른 파일을 열면 지워지며, 저장되지 않습니다(이번 실행에만 적용). 대화상자에서 이름을 붙이면 범례 켜짐이 저장되지만, 명령줄로 붙이면 이번 실행에만 켜집니다.
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
| 현재 화면을 이미지로 복사 / 픽셀 값 복사 | Ctrl+C / Ctrl+Shift+C |
| 비교 영상 내보내기 | Ctrl+E |

화면 복사(Ctrl+C)는 영상 영역만 담고(메뉴·컨트롤 바 제외), 마우스가 영상 위에 있으면 커서 픽셀 테두리와 하단 픽셀 값 줄도 화면처럼 들어갑니다. "복사 중"/"복사됨" 알림, 디코딩 대기 표시, A·B 전환 표시는 들어가지 않습니다. PNG는 별도 스레드에서 만들어 두므로 큰 이미지를 붙여 넣어도 플레이어가 멈추지 않습니다. 이미지에는 화면에서의 크기가 해상도(DPI)로 기록됩니다(이 값으로 그림 크기를 정하는 프로그램에서는 화면과 같은 크기로 들어감).

키는 입력된 문자가 아니라 키 코드로 처리하므로 한글 입력 상태에서도 동작합니다. 키를 누르고 있으면 이전 요청이 화면에 나온 뒤에만 다음 반복을 받아서, 키를 떼면 밀린 작업 없이 바로 멈춥니다.

마우스: 휠 = 커서 기준 확대/축소, 드래그 = 이동, 와이프 분할선 드래그 = 분할 위치, 터치패드 스크롤 = 이동, 핀치 = 확대.

### 비교 영상 내보내기

**File → Export Comparison Video...**(Ctrl+E)는 타임라인 구간을 지금 보는 비교 그대로 영상 파일로 만듭니다.

- **범위**: 전체 타임라인, 루프 구간(지정돼 있으면 기본), 또는 프레임 번호로 지정. 영상의 색인이 끝나야 시작할 수 있습니다(그 전의 프레임 번호는 확정이 아님).
- **그림**:
  - 전체 프레임: 지금의 비교 모드(분할 방향, 와이프 위치, 차이 배율, 전환으로 보이는 영상)·오프셋·색 해석을 쓰되 확대·이동은 무시하고 영상을 1:1로 그립니다. 분할은 두 영상과 그 사이 구분선(2픽셀)이고, 방향이 자동이면 좌우입니다.
  - 화면 그대로: Ctrl+C처럼 보이는 영역을, 화면에서 축소된 영상이 1:1이 되는 해상도로 그립니다(픽셀 숫자 포함, 커서 테두리 제외).
  - 크기는 둘 다 100/75/50/25 %에서 고릅니다.
- **오버레이**:
  - 영상 이름(기본 켬): 범례가 켜져 있으면 범례, 아니면 A·B 라벨이고 HOLD 표시를 포함합니다.
  - 프레임 번호와 시간(왼쪽 아래, 기본 끔).
  - 글자 크기는 화면 그대로일 때 복사와 같고, 전체 프레임일 때 1080줄 화면 기준으로 커집니다.

| 형식 | 인코더 | 내용 |
|---|---|---|
| H.264 (MP4) | 자동: NVENC → VideoToolbox → Quick Sync → AMF → Media Foundation → x264 → OpenH264 중 이 그림 크기를 받는 첫 번째 | BT.709 limited 4:2:0, 품질 높음/보통/작게(x264 CRF 16/20/26, NVENC CQ 18/23/28), 약 1초마다 키 프레임 |
| HEVC (MP4) | 자동: NVENC → ... → x265 | 위와 같음, Apple 호환 `hvc1` 태그 |
| FFV1 (MKV) | ffv1 | 무손실 RGB: 그린 그림과 비트 단위로 같음 |
| PNG 시퀀스 | png | `이름_000123.png`, 번호는 타임라인 위치 |

- 자동이 아니라 인코더를 직접 고를 수도 있습니다. 처음 대화상자를 열 때 실제로 열리는 인코더만 목록에 넣습니다. Windows 빌드(LGPL FFmpeg)에는 x264/x265가 없어 GPU 인코더나 Media Foundation을 씁니다.
- **백그라운드 처리**:
  - 세 단계가 동시에 진행되고, 플레이어는 그동안 계속 쓸 수 있습니다.
    - 디코딩: 영상마다 스레드 하나. 하드웨어 디코딩 설정을 그대로 쓰고, NVIDIA는 GPU 프레임을 복사 없이 그립니다.
    - 그리기: 화면과 같은 렌더러로, 전용 GL 컨텍스트에서 그립니다. H.264/HEVC용 YUV 변환도 GPU가 합니다(CPU 변환과 비트 단위로 같은 결과).
  - NVENC를 쓰면 디코딩부터 인코딩까지 GPU에서 처리하므로 CPU는 한 코어도 다 쓰지 않습니다(RTX 3080, 4K 두 개 좌우 7682×2160 HEVC 약 32 fps, 1080p 두 개 약 120 fps).
    - 인코딩.
  - 진행 창에서 속도·남은 시간을 보고 취소할 수 있습니다. 창을 닫아도 내보내기는 계속되고, Ctrl+E로 다시 엽니다.
  - 내보내기 중에 영상을 닫아도 그 영상은 끝까지 내보냅니다.
- 취소하거나 실패하면 쓰던 파일은 지웁니다. 같은 이름의 기존 파일은 완료되는 순간에만 바뀝니다(`.part` 로 쓰고 이름 변경).
- 4:2:0 형식에서 그림 크기가 홀수이면 마지막 열/행을 복제해 1픽셀 크게 인코딩합니다.
- 디코딩할 수 없는 프레임은 옆 프레임으로 대신하고 완료 메시지에 개수를 알립니다.

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
- 내보내기: 인코더별 왕복(무손실은 비트 단위, H.264/HEVC는 BT.709 값과 PSNR), FFV1로 내보낸 각 프레임이 같은 프레임 쌍을 렌더러로 그린 결과와 비트 단위로 같은지(소프트웨어·하드웨어 디코딩), 취소 시 파일 없음.
- E2E: 실제 앱을 가상 디스플레이(Xvfb)에서 실행해 화면을 캡처·검사 (`scripts/xvfb-run.sh`). 예: 3프레임 잘린 영상을 오프셋 −3으로 비교하면 차이 모드가 완전히 검정.
- `cmake --preset asan` / `--preset tsan` 으로 sanitizer 빌드.
- 성능 측정: `build/dev/tools/scvp-bench video.mkv --scenario seq|random|backstep --hw cpu|gpu --json`.

## 알려진 제한

- zero-copy 표시는 NVIDIA(CUDA)만 구현되어 있고, 그 외 GPU는 하드웨어 디코딩 후 시스템 메모리를 거쳐 업로드합니다(기능은 같음).
- MIPI RAW10/12 packed 형식은 아직 지원하지 않습니다(16비트 Bayer로 변환해서 열 수 있음).
- HDR(PQ/HLG) 톤 매핑, 디인터레이스, 자막, 오디오는 범위 밖입니다.
- 내보내기는 화면과 같은 8비트 RGB로 그린 결과입니다. 10비트 영상도 10비트로 내보내지 않습니다. 프레임률은 기준 영상(A, 없으면 B)의 고정 프레임률이고 소리는 없습니다.
