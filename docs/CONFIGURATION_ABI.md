# Startup hardware configuration ABI 1

`-M bes1503,hardware-config=/absolute/path/config.json` 在机器初始化阶段解析数据一次；BEST1502P 的现有机器入口也接受同一参数以加载通用软件补丁数据。BEST1700 继续使用既有启动属性，数据由外部编排器传入。

根对象含整数 `abi: 1`，以及可选的 `board`、`firmware`、`audio`、`binding_ui`、`binding_auth`。字段与范围的完整定义为公开的 `tools/runtime_schema.py`。生成器产生的 `bes_runtime_decode.inc` 是普通 C 解码器，不包含商品数据。`tools/synthetic_config.py` 生成可独立运行的完整示例。

`board` 描述显示几何、接线、模拟量、触摸、NAND 几何和电量曲线。`firmware` 描述引导向量、映射、存储分区和协议参数；指定它必须同时指定匹配的 board。音频和补丁段有固件 hash、配置 hash、地址、指令 guard 和受限数组。它们只是供公开适配器解释的数据；补丁功能仍会在加载实际镜像时检查 hash 和原指令。

所有出现的段必须包含完整字段；未知字段、错误类型、越界、长度/计数冲突和板固件绑定冲突拒绝启动。文件最多 256 KiB。启动参数只能设置一次，初始化后禁止通过 QMP 替换。配置内存由单实例 QEMU 进程持有，直至退出；热路径继续访问原 C 结构体。未提供配置时使用明确的合成默认板。

`board-profile` / `firmware-profile` 只选择已加载数据中的 ID，不再查找编译进二进制的商品表。应通过 hardware-config 选择配对数据，不依赖多个相互覆盖的 QOM 参数解析顺序。SoC/package 的内存容量和器件实现仍属于公开核心及必要构建材料。

配置解析器不识别型号包 ID、购买记录或网络授权。用户可自行创建配置，数据格式是公开的互操作接口。能够用外部 JSON 描述并不自动证明数据具有独立闭源许可依据；其来源和派生关系仍须审查。
