# Cold Brew

Cold Brew는 Aroma용 Wii U 플러그인입니다. Wii U를 부팅할 때 표시되는 사용자 선택 화면에서 지정한 계정을 숨깁니다.

계정을 삭제하거나 계정 파일을 수정하지 않습니다. Wii U Menu 프로세스가 계정 목록을 확인할 때 반환되는 표시 정보만 필터링합니다. 게임과 다른 앱의 계정 API에는 후킹을 적용하지 않습니다.

## 기능

- Aroma 설정 메뉴에서 각 계정을 Mii 이름, NNID, 슬롯 번호로 구분
- 계정별 `Visible` / `Hidden` 설정
- 설정을 SD 카드의 Aroma 플러그인 저장소에 숨김 비트마스크로 저장
- 모든 계정을 동시에 숨기지 못하도록 보호
- Wii U Menu 프로세스의 슬롯 표시 확인에만 후킹 적용

## 설치 및 설정

1. `cold_brew.wps`를 SD 카드의 `sd:/wiiu/environments/aroma/plugins/`에 복사합니다.
2. Wii U를 Aroma로 부팅합니다.
3. `L + 십자키 아래 + MINUS(-)`를 눌러 Aroma 플러그인 설정을 엽니다.
4. **Cold Brew**에서 숨길 계정을 `Hidden`으로 바꿉니다.
5. 콘솔을 재부팅합니다.

설정 화면에서 마지막으로 표시되는 계정까지 숨기려고 하면 해당 계정은 자동으로 `Visible` 상태를 유지합니다.

> 계정의 자동 로그인 기능이 켜져 있으면 Wii U의 사용자 선택 화면 자체가 생략될 수 있습니다. Cold Brew는 자동 로그인 설정을 변경하지 않습니다.

## 빌드

Docker가 설치되어 있다면:

```sh
docker build -t cold-brew-builder .
docker run --rm -v "${PWD}:/project" cold-brew-builder make
```

또는 devkitPPC, wut, WiiUPluginSystem이 설치된 devkitPro 환경에서:

```sh
make
```

출력 파일은 `cold_brew.wps`입니다.

## 구현 참고

Cold Brew는 `nn::act::IsSlotOccupied`를 Wii U Menu 프로세스에서만 교체합니다. 실제 계정 수는 변경하지 않고, 원본 슬롯 점유 결과에서 설정된 슬롯만 표시 대상에서 제외합니다. 계정 생성, 삭제, 이동 또는 NAND 저장 API는 호출하지 않습니다.

`v0.1.1`부터 `GetNumOfAccounts`는 후킹하지 않습니다. 실제 계정 수를 줄이면 슬롯 번호가 듬성듬성해질 때 Switch Account 화면의 열거 범위와 내부 상태가 어긋날 수 있기 때문입니다.

지원 슬롯은 Wii U의 사용자 슬롯 1–12입니다.

## 라이선스

[MIT](LICENSE)
