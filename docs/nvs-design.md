# pico-nvs 设计文档

## 1. 目标

在 flash 上实现一个具备磨损均衡能力的 NVS（非易失键值存储），提供
`mount / read / write / delete / gc` 等能力，通过 flash 端口抽象层与具体
硬件解耦。

## 2. 设计前提与约束

- flash 以 **sector** 为擦除单位（例如 4KB），擦除后为全 `0xFF`。
- flash 写入只能把 `1` 写成 `0`，不能把 `0` 写回 `1`，因此**原地更新不可能**，
  删除只能标记，释放依赖后期擦除。
- 记录采用**定长槽位**方案：每个槽位固定 32 字节，一条记录可跨多个连续槽位。
- 全局只有一个 **ACTIVE** 扇区接收写入，写满后 GC 搬移到下一扇区，靠 `seq`
  递增轮转实现磨损均衡。

## 3. 总体结构

```
应用层
  |
nvs_fs (文件系统句柄，运行态状态)
  |
nvs.c (核心逻辑：mount/read/write/delete/gc/crc)
  |
nvs_port_flash (flash 抽象：read/write/erase 回调)
  |
底层 flash 驱动
```

## 4. 存储结构

### 4.1 Flash 分区

```
sector 0    sector 1    sector 2    ...    sector N-1
|----------|----------|----------|      |----------|
 sector_size sector_size sector_size       sector_size
```

扇区基地址：`sector_base(i) = flash_offset + i * sector_size`。

### 4.2 Sector 布局

```
offset 0                32                    64                    sector_size
|---- page header ----|---- entry bitmap ----|------ entry slots ------|
|      32 字节         |     32 字节          |       M × 32 字节       |
```

- `M = (sector_size - 64) / 32`，且 `M ≤ 128`（位图最多管理 128 槽）。
- 4KB 扇区时 `M = 126`。
- 槽位 `j` 地址：`sector_base + 64 + j * 32`。

### 4.3 页头（32 字节，对应 `nvs_page_header`）

```
| stat(1) | seq(1) | ver(1) | reserved(25) | crc32(4) |
```

- `stat`：扇区状态，见 5.1。
- `seq`：扇区序号，GC 时 +1，用于选取最新扇区。
  **注意：当前代码为 1 字节，256 次 GC 即回绕，应扩为 `uint32_t`。**
- `ver`：格式版本，便于未来迁移。
- `crc32`：覆盖 `stat + seq + ver`（不含 reserved 与 crc 自身）。

### 4.4 条目状态位图（32 字节，对应 `ENTRY_STAT_BITMAP`）

32 字节 = 256 bit = 128 槽 × 2bit。bit 排列固定为 **LSB-first**：
slot0 占 bit[1:0]，slot1 占 bit[3:2]，依次类推。

| 2bit 值 | 含义 |
|---------|------|
| `11` | unused（空闲槽，擦除态） |
| `10` | head（记录头槽） |
| `01` | continuation（记录续槽，纯数据） |
| `00` | deleted（已删除，待 GC 回收） |

位图更新只做“清位”，四种迁移均满足 flash 只能 1→0 的约束：

- `11→10`：提交 head（清低位）
- `11→01`：提交 continuation（清高位）
- `10→00`：删除 head（清高位）
- `01→00`：删除 continuation（清低位）

### 4.5 记录布局

#### 单槽记录（`size ≤ 24`）

占 1 个槽，位图为 `10`：

```
| key(2) | size(2) | crc32(4) | data(24) |
```

#### 多槽记录（`size > 24`）

```
slot 0  (head, 位图=10)        | key(2) | total_size(2) | crc32(4) | data[0..23]
slot 1  (continuation, 位图=01)| data[24..55]                          (32B 原始数据)
slot 2  (continuation, 位图=01)| data[56..87]                          (32B 原始数据)
...
slot K  (continuation, 位图=01)| data[...]                             (32B，末槽可不满)
```

- `K = ceil((total_size - 24) / 32)`；`total_size ≤ 24` 时 `K = 0`。
- 总槽位数 `S = 1 + K`。
- head 的 `size` = 整个 value 总长度。
- head 的 `crc32` 覆盖整个 value 的全部 `total_size` 字节（含所有续槽数据）。
- 续槽为**纯 32 字节数据**，不携带 key/size/crc；其身份由位图 `01` 表达，
  边界由 head 的 `total_size` 决定。

## 5. 状态编码

### 5.1 页头 `stat`（逐级清零）

| 值 | 状态 | 说明 |
|----|------|------|
| `0xFF` | ERASED | 擦除态 |
| `0xFE` | ACTIVE | 当前活动扇区，可写 |
| `0xFC` | FULL | 写满关闭，等待 GC |
| `0xF8` | GARBAGE | 半写/作废（实际直接擦除，不必写此值） |

扇区状态机：`ERASED → ACTIVE → FULL → (GC) → ERASED`。

### 5.2 条目位图

见 4.4。

## 6. 数据结构补全要点

`nvs_fs` 需要包含：

- `port`：flash 端口句柄
- `sector_size / sector_count / flash_offset`
- `active_sector`：当前活动扇区号
- `next_free_slot`：下一个空闲槽位
- 运行时位图缓存（可选，加速 read/delete）
- 各扇区擦除计数（可选，进阶均衡）

`nvs_page_header`：补齐为定长 32 字节（含 reserved 填充），`seq` 扩为 32 位。

`nvs_entry`：内存与 flash 表示统一，数据段为 `uint8_t data[24]`，不使用指针。

`ENTRY_STAT_BITMAP`：改为 `typedef uint8_t ENTRY_STAT_BITMAP[32];`。

## 7. 操作流程

### 7.1 mount

1. 校验 `fs` 非空、几何参数合法；否则返回 `-EINVAL`。
2. 逐扇区读页头并分类：
   - magic 合法且页头 CRC 合法 → 有效扇区（记录 `seq`、`stat`）。
   - 否则 → GARBAGE（半写头），加入待擦除列表。
3. 选 `seq` 最大的有效扇区为 ACTIVE：
   - `stat == ACTIVE` → 重建写位置。
   - `stat == FULL` → 标记需立即 GC。
   - 无有效扇区 → 首次格式化：写入初始页头（`seq=0`、`stat=ACTIVE`、`ver`）。
4. 擦除所有 GARBAGE 扇区与 seq 较旧的有效扇区。
   **必须先确定 ACTIVE，再擦其它。**
5. 重建运行时状态：扫位图得到 `next_free_slot`（第一个 `11`），
   可同时构建 head→槽位 索引缓存。

### 7.2 read

1. 校验 `fs` 已 mount、参数合法。
2. 顺序扫描位图，找出所有 `head(10)` 槽，读其 `key` 匹配目标。
3. 对匹配的 head 取**槽位号最大**的那个（最新值）。
4. 读 head 得 `total_size`，计算 `K`，顺序读取后续 `K` 个槽，
   要求每个均为 `continuation(01)`：
   - 中途遇 `11`/`10`/`00` → 记录不完整 → 视为损坏，返回错误。
5. 拼接 head 24B + 续槽 32B×K，截取 `total_size` 字节，校验整体 CRC；
   失败 → `-ENOENT`。
6. 拷贝 `min(total_size, buf_len)` 返回，返回实际 `total_size`。

### 7.3 write

1. 校验参数：`size ≤ 65535`（`uint16 size` 上限）。
2. 计算 `S = 1 + (size > 24 ? ceil((size-24)/32) : 0)`。
3. 找**连续 `S` 个 `unused(11)` 槽**（first-fit/next-fit）：
   - 找不到 → 触发 GC 紧凑化后重试 → 仍无 → `-ENOSPC`。
4. 把 head 与全部续槽数据写到对应槽位（此时位图仍为 `11`）。
5. 提交顺序（关键）：
   - 先把各续槽位图 `11→01`；
   - 最后把 head 位图 `11→10`，作为整条记录的提交点。
6. 覆盖旧值：先提交新记录，再删除旧记录（见 7.4）。
7. 更新 `next_free_slot` 为记录之后第一个 `11` 的位置。

### 7.4 delete

1. 找到目标 key 的最新 `head(10)`。
2. **先删 head：`10→00`（提交点）**，再删其 `K` 个续槽 `01→00`。
   - head 一旦为 `00`，记录即不可见；残留的 `01` 孤儿会被 read/GC 忽略。

### 7.5 GC

触发时机：write 时扇区写满，或 mount 发现 ACTIVE 扇区 `stat == FULL`。

1. 选目标扇区 `(active_sector + 1) % sector_count`；若目标非 ERASED，先擦除。
2. 顺序扫描旧 ACTIVE 扇区，**以 head 为边界重组记录**：
   - `deleted(00)` 槽：跳过。
   - 孤儿 `continuation(01)`（前面无有效 head）：跳过。
   - `head(10)` + 其后 `K` 个 `continuation(01)`：整块搬移。
3. 搬移按 write 提交顺序：写数据 → 提交续槽 `01` → 最后提交 head `10`。
4. 全部搬完 → 提交新扇区头（`seq+1`、`stat=ACTIVE`）→ 擦旧扇区 →
   更新 `active_sector / next_free_slot`。

GC 天然完成紧凑化：deleted 槽与孤儿续槽被丢弃，活记录连续排列，
扇区末尾留下连续空闲空间。

## 8. 掉电安全与原子性

- **写提交点**：head 位图 `11→10`。提交前掉电，head 仍 `11`，记录不可见，
  已提交的续槽成为孤儿，被 read/GC 忽略。
- **删提交点**：head 位图 `10→00`。提交后掉电，记录不可见，残留续槽为孤儿。
- **GC 中途掉电**：
  - 新扇区头未提交 → 判 GARBAGE 并擦除，旧扇区头仍有效，数据不丢。
  - 新头已提交、旧扇区未擦 → 两个有效扇区，选 `seq` 更大的新扇区，旧扇区擦除。
- **半写页头**：CRC 失败 → GARBAGE → 擦除。
- **首次格式化中断**：半写页头 → 下次 mount 判 GARBAGE 并重试。

## 9. 磨损均衡

- 一期采用 **seq 递增 + 写满才擦 + 轮转**：所有写入落在 ACTIVE 扇区，
  写满后 GC 到下一扇区，旧扇区擦除，`seq+1`。
- 进阶可选：记录每扇区擦除次数，GC 时选擦除次数最少的扇区作为目标。
- `seq` 必须为 32 位，避免 256 次 GC 后回绕。

## 10. CRC32

- 实现标准 CRC32（反射多项式 `0xEDB88320`），纯软实现，保持与平台解耦。
- 页头 CRC 覆盖 `stat + seq + ver`。
- 记录 CRC 覆盖整个 value（head 与所有续槽的数据）。

## 11. 边界与代价

- **小溢出浪费**：`size=25` 需 2 槽（64B 存 25B），定长槽位方案固有代价；
  `size=57` 需 3 槽。若不能接受，只能改用变长追加方案。
- **记录上限**：`uint16 total_size` 上限 65535B，实际受单扇区槽位数限制，
  需要定义并检查。
- **孤儿续槽**：write 半写、delete 中断都可能留下，read/GC 必须以“head 有效”
  为前提，忽略无主续槽。
- **位图 bit 序**：固定 LSB-first，否则 head/continuation 判定会乱。

## 12. 与当前代码的差异清单

| 项 | 现状 | 应改为 |
|----|------|--------|
| `seq` 宽度 | `uint8_t`（256 次 GC 即回绕） | `uint32_t` |
| `nvs_entry.data` | `uint8_t *` 指针 | `uint8_t data[24]` |
| `ENTRY_STAT_BITMAP` | 错误的宏定义 | `typedef uint8_t ENTRY_STAT_BITMAP[32];` |
| `nvs_page_header` | 缺 `reserved[25]`，实际 8B | 补齐为定长 32B |
| `nvs_fs` | 空 | 补几何信息 + 运行时状态字段 |
| 位图 `01` 状态 | 保留 | 定义为 continuation |
| `earse` | 拼写错误 | `erase` |
| 构建 | `add_executable` + `main()` | `add_library` + 去掉 `main()` |

## 13. 实施顺序

1. 定几何参数（sector 大小/数量）与状态机。
2. 补全 `nvs_port`、`nvs_fs`、`nvs_page_header`、`nvs_entry`、位图类型。
3. CRC32。
4. `nvs_mount` 扇区扫描。
5. `read / write / delete`（先不做 GC，写满即报错）。
6. GC + 磨损均衡轮转。
7. 掉电恢复路径。
8. 单元测试 + host 内存模拟 flash。
