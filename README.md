# LILYGO T-Embed 软件平台

这是一个面向 LILYGO T-Embed ESP32-S3 的 C++ 软件平台，目标板配备 16 MB Flash、8 MB OPI PSRAM 和 320 × 170 的 ST7789 屏幕。System 常驻固件，应用和 System 共用头文件、工具链与 ESP-IDF 配置。一次构建生成系统固件和各应用独立的 ELF，系统按需从 TF 卡将应用加载到 RAM。

**验证进度**：ESP-IDF 6.1.1 交叉编译、固件链接和主机端 ELF 加载测试已通过。开发板到货后再进行真机验证。

## 无板快速开始

激活 ESP-IDF 6.1.1 开发环境，从仓库根目录执行以下命令。首次构建由 IDF Component Manager 按 `src/dependencies.lock` 获取依赖。主机检查需要 C 编译器，默认使用 `cc`，也可用 `HOST_CC` 指定。

```sh
cd src
idf.py build
python3 tools/check_packages.py
```

| 构建产物（相对于 `src/`） | 用途 |
| --- | --- |
| `build/kira-lilygo.bin` | 系统固件，包含首页、应用选择器和设置 |
| `build/sdcard/apps/scope/` | 麦克风波形示例，包含 `app.elf` 和 `manifest.json` |
| `build/sdcard/apps/tone/` | 440 Hz 测试音示例，可切到后台播放 |

主机检查会比对应用清单与固件的 SHA-256，校验两个 ELF 的结构，并用乐鑫加载器同时装入两个应用，重复装卸 100 次。它也检查截断文件、缺失导入符号和内存分配失败时的返回结果与资源回收。板上的 PSRAM 指令执行、屏幕显示和实际录放音列入真机验收。

## 系统与应用如何协作

- **共享资源**由 System 初始化。前后台应用可直接使用 socket 和 POSIX 文件接口。麦克风向每个读者提供独立的 PCM 读游标，扬声器把各应用的播放流混音输出。
- **前台资源**包括屏幕、旋钮、BLE、灯环和可用扩展引脚。前台应用可创建 LVGL 控件或提交 RGB565 像素块。首页短按旋钮进入选择器，旋转选择、短按启动或恢复应用，长按回首页。
- **系统界面**显示时间、日期、电池电压、后台应用数、内存和 TF 卡占用。设置随固件编译，提供 Wi-Fi、BLE、文件管理、TF 卡格式化、音量和亮度控制。

## 开发一个 ELF 应用

1. 将 `src/elf_app/tone/` 复制为 `src/elf_app/<app_id>/`。在 `manifest.json` 中将 `id` 设为目录名，用 `name` 设置显示名称，用 `background` 声明是否支持后台运行。
2. 在应用的 `CMakeLists.txt` 中调用 `t_embed_app(<app_id> main.cpp)`，并在上级 `src/elf_app/CMakeLists.txt` 中登记 `add_subdirectory(<app_id>)`。
3. 在 `app_main` 中填写 `embed::App` 的生命周期回调。前台回调可使用应用自己的 LVGL 根容器。应用自行创建的任务、文件和 socket 由停止回调清理。可参考[示例入口](src/elf_app/tone/main.cpp)和[System 接口](src/components/platform/include/platform.hpp)。
4. 在 `src/` 下重新执行 `idf.py build` 和 `python3 tools/check_packages.py`。新应用包位于 `build/sdcard/apps/<app_id>/`。

ELF 是与 System 共享地址空间的可信原生代码。发布时将固件与同一次构建的 `build/sdcard/` 内容配套使用。应用加载器会检查清单中的固件标识，拒绝不匹配的应用包。

## 进一步阅读

- 板卡接口、外设与 GPIO 分配见[硬件定义](docs/Hardware.md)。
- 驱动选型、架构图、真机部署步骤与验收清单见[软件平台设计](docs/SoftwarePlatform.md)。
