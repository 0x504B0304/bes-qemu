# MiBand QEMU public core

通用 BES SoC、IP、板级器件及进程内桥接实现。无需 Studio、商品型号包、授权文件或厂商固件即可构建和运行合成客体。

```sh
# Linux / WSL: clang-23, Meson, Ninja, GLib and pixman development packages
MIBAND_QEMU_SRC=/tmp/bes-public-qemu bash build.sh
python3 tools/synthetic_config.py /tmp/bes-synthetic
/tmp/bes-public-qemu/build/qemu-system-arm \
  -M bes1503,hardware-config=/tmp/bes-synthetic/hardware.json \
  -kernel /tmp/bes-synthetic/guest.bin -display none -serial null -monitor stdio
# 在 monitor 输入 quit 退出；客体为自包含 Thumb 循环。
python3 tools/check_project.py --qemu /tmp/bes-public-qemu/build/qemu-system-arm
```

构建固定上游 commit `f671ce11d2a15a58ab752d7cf0b2890c9022c7ba`，包含七组公开补丁。完整输入为 `qemu/machine/sources.json`、SoC/package 配置、`tools/runtime_schema.py` 及公开构建工具。型号包不参与编译。不要覆盖带有其他修改的 QEMU checkout。

`hardware-config` 是公开的启动配置 ABI 1；它不加载代码，也不读取商品许可证。解析器、字段校验、合成示例都在本项目。用户可依据适用 GPL 权利自行编译、修改和适配任意硬件。商品型号授权只用于独立 Studio 产品。

`qemu/` 保留设备和热路径；`tests/` 保留通用与合成回归；`debugger/` 保留公开调试支持。商品目录、固件识别及产品回归位于独立 Studio 型号包。来源存疑的历史/现行支持源文件另保留为公开审查材料，不将其视为闭源资产。

参阅 [配置 ABI](docs/CONFIGURATION_ABI.md)、[许可边界](docs/LICENSING_BOUNDARIES.md) 和 [验证范围](docs/VALIDATION.md)。本次没有执行发布或重写 Git 历史。原文件声明优先，未确认贡献者权利仍是商业发布前置条件。
