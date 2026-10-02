# comparison-video-player

SCVP (Simple Comparison Video Player): 두 영상을 프레임 단위로 정확히 맞춰 재생·이동·확대하고, 픽셀 값까지 확인하는 크로스플랫폼(Linux / macOS / Windows) 비교 플레이어입니다.

- C++17 · Qt6 Widgets (≥ 6.4) · FFmpeg (≥ 6.1) · OpenGL 3.3 core (macOS 4.1)
- 정확한 프레임 단위 이동(앞/뒤), 설정 가능한 구간 점프, 메모리 선로딩 캐시, 구간 반복, 전체화면
- 두 영상 동기 재생(프레임 오프셋, 범위 밖 영상은 HOLD), 좌우/위아래·와이프·A/B 전환·차이 비교
- 픽셀 단위 확대와 RGB / YUV 값 표시, GPU 우선 디코딩

> 개발 진행 중입니다. 빌드 방법과 사용법은 구현이 진행되면서 채워집니다.
