# JCE 脚本 API 与多语言绑定

要求：**每一种在册语言都能作为脚本驱动一个完整应用**，
引擎侧提供扁平 C ABI 供其它语言 native 调用。

## 1. 两层真理源

| 层 | 文件 | 内容 |
|---|---|---|
| 结构 | `engine/include/jce/middleware/script/jce_script.h` | `JceScriptHost` —— 首位 `void *user` 之后是一串函数指针成员，脚本能看到的全部。**成员数不要写死**：见 `script_exposure.json` 的 `declared_totals.host_members` |
| 清单 | `engine/src/middleware/script/script_exposure.json` | 每个成员怎么暴露。分项数看它自己的 `declared_totals`（`expose` / `hand_written` / `constants`），别抄这里 |
| 对外契约 | `contracts/script-api.json` | 机器生成 + 机器自检（体积随成员数变化，不作判据） |
| ABI 散文 | `contracts/language-driver-abi.md` | 语言驱动 ABI 说明（**未机检**） |

**改脚本 API = 改这两层，不是去改某一种语言的绑定文件。**

## 2. 生成器就是检查器

```bash
python tools/scriptgen/gen_script_bindings.py      # 默认 --check
python tools/scriptgen/gen_script_c_abi.py         # 默认 --check
```

实测输出（本轮，全绿）：

```
script-bindings: OK - <N> host members, <A> generatable + <B> hand-written = <A+B> registered,
                 1 constant, 79 table keys; 70 members reached by generated bindings,
                 4 only by hand-written, 0 unclaimed
script-bindings: generated output verified — 71 functions, 1 constant,
                 16 artefacts byte-identical to a fresh emit
script-c-abi:    OK - 71 manifest entries + 3 meta = 74 exports,
                 7 hand-written entries excluded with a reason;
                 5 artefacts byte-identical to a fresh emit
```

要真正写文件时加 `--write`。

> **没有 `--check` 这个 flag。** 两个生成器的参数只有 `--write`（`gen_script_bindings.py`
> 另有 `--update`）；**不带任何参数跑就是校验模式**，只读、不写盘、exit 0。
> `tools/audit/run_architecture_audit.py` 正是以空参数列表调用它们的。
> 打 `--check` 会 `unrecognized arguments` 并 exit 2 —— 那不是「校验失败」，是命令打错了。

**校验模式（不带参数）是只读的。** 「我处于只读约束下所以没跑生成器」不构成豁免——
RED 基线里有人据此在**没跑过任何生成器**的情况下宣布
「≈0.95 的把握这个调用每种语言都已发货」。**禁止在未跑校验模式的前提下
声称某个脚本调用已存在或已发货。**

## 花名册：谁是权威，有几种（2026-08-28 实测）

**离线权威是 `engine/src/resource/jce_asset_ext.c` 的 `k_script_ext_table`**，
`tools/audit/check_script_language_catalog.py` 守它。当前 **8 行 / 7 种语言**
（java 占两行：源码与字节码）：

| 语言 | 扩展名 | 形态 |
|---|---|---|
| lua | `.lua` | SOURCE（**引擎内建**，不在 `scripting/` 下） |
| python | `.py` | SOURCE |
| java | `.java` / `.class` | SOURCE / BYTECODE |
| cpp | `.jcecpp` | REFERENCE |
| c | `.jcec` | REFERENCE |
| js | `.jcejs` | SOURCE（**不是 `.js`**——`.js`/`.ts` 已被编辑器归为项目侧 web 工具链，认领它会把每个构建脚本都变成可挂载的 gameplay 脚本并以明文发货） |
| csharp | `.cs` | REFERENCE |

emitter 实体：`tools/scriptgen/` 下的 `emit_lua.py`、`emit_python.py`、
`emit_java.py`、`emit_cpp.py`、`emit_js.py`、`emit_csharp.py`；
C 侧由 `gen_script_c_abi.py` 产出。

**别把数字写死。** 重测：

```bash
python tools/audit/check_script_language_catalog.py     # 报 catalog 行数 / 运行时声明 / 后端实现数，三者必须一致
ls -1d scripting/*/                                     # 后端目录（c_abi 与 cmake 是基础设施，不是语言）
```

## 花名册说「有」，不等于它会跑（2026-09-21 实测）

**上面那张表是*离线*花名册：一个扩展名能不能被认出来。**
它回答不了「挂上去之后会不会执行」——而这两件事在这棵树上**相差三种语言**，
且差的那几种**不报错**：脚本就是不运行，和「用户没写脚本」逐位相同。

用一个七种语言各驱动一个模块的贪吃蛇实测，同一个工程、同一批脚本：

| 语言 | 出货 exe | 编辑器 Play | 差在哪 |
|---|---|---|---|
| lua | 跑 | 跑 | —— |
| python | 跑 | 跑 | —— |
| java | 跑 | 跑 | —— |
| js | 跑 | 跑 | —— |
| c | 跑 | **不跑** | 原生模块只在「打开项目」那条路注册；无头/直接开场景到不了 |
| cpp | 跑 | **不跑** | 同上 |
| csharp | **不跑** | **不跑** | 见下 |

**编辑器自己印的那行是它*编译进了哪些后端*，不是*这一局会跑哪几种*。**
实测它打印 `Play can run: c lua cpp csharp java js python` —— 七个全在，
而同一局里 c / cpp / csharp 一个都没执行。**那行字来自构建期的宏，不是运行期的点名。**

判据（别读那行字，读装载日志）：

```bash
JCE_MAX_FRAMES=200 JCE_WINDOW_HIDDEN=1 <游戏>.exe 2>&1   | grep -o "script: loaded '[^']*' ([a-z]*)" | sed 's/.*(//' | sort -u
```

`grep` 出来的那一列才是这一局真的装载了的语言。**数量对不上就是缺，不是慢。**

### csharp 的那一条：hostfxr 的 ALC 隔离

`Vm.LoadAssembly` 曾经写 `AssemblyLoadContext.Default.LoadFromAssemblyPath`。
但 hostfxr 把 **`JceScript.dll` 自己**装进一个隔离 ALC，于是游戏程序集进 Default
之后**绑不上它自己引用的那个 `JceScript`**，`IndexNamedHandlers` 一遍历类型就抛
`ReflectionTypeLoadException`，而它被吞掉 ⟹ **这门语言安静地不存在**。

阳性对照很关键：**同一个 DLL 在 PowerShell 里单独加载是正常的**——
所以「程序集是好的」这条证据**不能**用来排除装载失败。
修法是装进桥自己所在的那个 ALC（`AssemblyLoadContext.GetLoadContext(typeof(Vm).Assembly)`）。

### 三个会咬人的不对称

1. **`log` 是 8 个 hand-written 绑定之一，⇒ 逐个 emitter 手写，覆盖天生不齐。**
   `contracts/script-api.json` 的 `hand_written` 是
   `line_set_points` `log` `asset_read_text` `asset_read_json` `play_sound`
   `start_coroutine` `wait_seconds` `stop_coroutine`——**这 8 个不由通用生成路径产出**，
   哪门语言有、哪门没有，取决于那门语言的 emitter 有没有替它写一遍。
   实测两端：**java 有**（在基类 `JceEntityScript.log(String)` 上，直通
   `JceScriptHost::log`）；**csharp 没有**（`Jce.g.cs` 101 个绑定，
   连同 `JceEntityScript.cs` 一起 grep `Log` **零命中**）。
   ⇒ **跨语言脚本不能靠打印来自证「我跑了」**：在没有 `log` 的那几门里，
   那根本不是一个存在的名字。要靠它**改了什么状态**来证明。
   动手前先数一遍那门语言自己的生成面，别假设。
2. **不要凭印象判断某门语言「绑定很少」——去数。**
   本节初稿在这里写过「java 只拿到 107 个里的 9 个、`find_by_name` 不在其中」，
   **实测是假的**：`JceScript.java` 上有 **103 个**公开方法，
   `findByName` / `findByPrefix` / `findWithTag` / `uiSetText` /
   `saveGame` / `loadGame` / `isKeyDown` 全都在，
   `log` 另在基类 `JceEntityScript` 上（`JceScriptHost::log` 的直通）。
   数它们的命令，别靠印象：

   ```bash
   python -c "import re,pathlib; s=pathlib.Path('scripting/java/src/main/java/com/jce/script/JceScript.java').read_text(encoding='utf-8'); print(len(set(re.findall(r'^\s{4}public (?:static )?(?:final )?[\w\[\]<>., ]+? (\w+)\(', s, re.M))))"
   ```

   总数的权威是 `contracts/script-api.json` 的 `declared_totals`
   （当前 `expose` 99 + `hand_written` 8 = **107**），**不是任何一门语言的文件**。
3. **`project.create` 的模板不调 `jce_script_enable()`** ⇒ 新工程默认只有 lua。
   多语言工程必须自己往 `CMakeLists.txt` 里加那一句，否则另外六种**连编都没编进去**。

### 同一个绑定，七种语言七种拼法——而且写错不报错

`find_by_name` 的 shape 是 **`first_and_count`**：它回答「第一个匹配」+「匹配了几个」。
**每门语言把这一对包成了不同的东西**，实测：

| 语言 | 返回 | 取实体 |
|---|---|---|
| lua | 多返回值 | `local e = jce.find_by_name(n)`（取第一个） |
| python | `tuple` `(first_or_None, count)` | `jce.find_by_name(n)[0]` |
| js | **`Array`** `[first_or_null, count]` | `jce.find_by_name(n)[0]` |
| java | `JceScript.FindByNameResult` | `.first`（`Long`，可能是 null） |
| csharp | `FindByNameResult` | `.First`（`uint?`）+ `.Count` |
| cpp | 结构体 | `.first`（`std::optional`） |
| c | 出参数组 + 返回计数 | `find_by_name(api, n, found, 2) > 0 ? found[0] : 0` |

**把那一对本身当实体传进去，不会抛错。** 后果逐语言不同，但都是安静的：

- **js**：`get_position([e,1])` 返回 **null** ⟹ 模块每帧 early-return，
  游戏永远停在菜单，**日志里一条错误都没有**。
- **python**：在 ctypes 深处炸成
  `argument 2: TypeError: 'tuple' object cannot be interpreted as an integer`，
  **既不点名这个调用、也不点名这个文件**，而且该实例的 `on_update` 会被
  **永久停用**（「hot-reload the script or respawn the entity to re-enable」）。

一次七语言改写里，**七个模块有三个第一遍就写错了这一处**。

⇒ **在一门你没写过的语言里第一次用某个绑定，先去看那门语言的生成面**
（`scripting/<lang>/` 或 `Jce.g.cs` / `JceScript.java` / `_generated.py`），
**不要照抄另一门语言的调用形状**。判据不是「能编译」——
上面两种都编译得过，一种连异常都没有。

### 两个原生模块进同一个二进制

`c` 与 `cpp` 各自会生成一个「本共享库的入口」。两个都链进**同一个 exe** 时符号撞车
（`LNK2005`）。开关是编译定义 **`JCE_SCRIPT_MODULE_NO_ENTRY`**：
带上它编译模块源码，入口不生成，改由宿主自己调各模块的 publish 函数。
单文件 exe（把两种原生语言都烘进去）**必须**走这条路。

## 加一门语言 = 加一个目录

**`scripting/<lang>/` 这个目录本身就是花名册**——没有另一张需要同步的清单。
配套的开关是 `jce_script_enable(<target> ...)`，定义在
`scripting/cmake/JCEScriptEnable.cmake`，**同一份源被两个世界 include**：
树内构建（`scripting/CMakeLists.txt`）与安装后的 SDK（`JCEScripting.cmake`
从自己旁边 include 它）。这一点不是整洁癖——它自己的注释写着
「两种拼写回答同一个问题，是一个项目最终得到一条只在其中一个世界跑过的分支的原因」，
这个文件的存在正是因为有项目撞上了那件事。

**消费方（一个项目要开脚本）现在是一句话：**

```cmake
if(COMMAND jce_script_enable)
    jce_script_enable(MyGame)          # 链上这个构建有的后端、烘进各自的注册 shim
endif()
```

它**做完全部四步并生成注册 shim**。项目只在有**自己的**原生模块（C/C++ 类）时
才需要额外加源文件并提供一个发布回调——那些类是项目的，不是引擎的。
（在树里可抄的调用方：`editor/CMakeLists.txt`。完整的「单 exe + 多语言脚本
游戏工程」样例曾是 elemental_serenity/，已于 2026-08-30 移出仓库，见
`<local-consumer-root>/` 与同目录的 source-only zip。）

`lua` 传给它会被**接受并忽略**（它在引擎内部，不是可开关的后端），
这样 `LANGUAGES lua python` 不会因为一个说得通的写法而报错。

⇒ 判断某语言是否在册，**读目录与 catalog，不要读散文里的数字**；
本文件里凡出现具体条数的地方都按「上一次的读数」对待。

### 校验模式没跑成时的输出形状

若校验模式因任何原因没跑成（权限拒绝、超时、环境缺失），**结论必须降级措辞**：

> 清单与生成物在本次读到的版本里一致，**但未经生成器校验**。

形式要求有三条，**靠语义自查而不是靠避开某几个词**（实测：黑名单上有「贯通」，
于是同义的「全链已接通」直接穿了过去——黑名单教会的是避词，不是降级断言）：

1. 每一条关于「已接通 / 已一致 / 已可用 / 全链打通 / 闭环 / 齐活」的陈述，
   都必须带同一个后缀：**「——本次读到的版本如此，未经生成器校验」**。
   同义替换按同一条处理。
2. `CONFIDENCE` 不得为「高」。
3. 给出可直接复制的放行请求，写明命令不带参数即校验模式、只读、不写盘。

**自检**：把结论里每个断言单独摘出来问一句——
「如果 `*.gen.*` 被人手改过，这句话还成立吗？」不成立的，必须带后缀。

放行请求形如：

> 需要放行 `python tools/scriptgen/gen_script_bindings.py`（不带参数即校验模式，只读、不写盘）

理由：校验模式唯一能捕捉的就是你**读不出来**的那类问题——
`*.gen.*` 被人手改过、或清单改了没重跑。
「我逐行读到了每种语言的符号」对这一类**完全免疫**，所以它不是校验模式的替代品，
读得再仔细也不是。

## 3. append-only —— 两层，都是硬的

**第一层：清单表的形状只增不改。**
**改名 = 新增一项 + 把旧项标 `deprecated`**，不是原地重命名。
原地改名会让每一个已发布的脚本在下一个版本静默失联。
返回形状与修饰符是**封闭集合**（七种形状、九个修饰符）。
需要一个新形状时，那是对生成器的改动，不是在清单里塞一个特例。

**第二层：`JceScriptHost` 的成员只能追加到末尾——这一层是按字节强制的。**

```
engine/src/middleware/script/jce_script.c:633   memcpy(&s->host, host, n);
scripting/c_abi/src/jce_script_api.gen.c:8      copies min(host_size, sizeof)
                                                over a zeroed table
```

兼容靠的是**字节布局**，不是名字。往结构体中间插一个函数指针，会把其后每一个
槽位下移；一个仍按旧布局编译的调用方，它的槽位 N 会被引擎当作新成员读取——
**一次通过签名不同的指针发出的间接调用，静默而不是崩溃**。

实测（2026-08-27）：一个提交把 `get_world_position` 和 `line_set_points` 放在
槽位 7/8（共 76 个成员）。修法是移到最后一个成员之后（74/75），
`script_api_version` 保持 1，既有槽位一个不动。

> 守这条的门是 `tools/audit/check_abi_snapshot.py` 的 **ORDERED-PREFIX 规则**，
> 它的 docstring 点名的例子就是 `JceScriptHost`。那道门在 `main` 上缺席期间，
> 这个缺陷从它上面穿了过去。**它不在 `run_all.py` 里**，见 `references/build-and-gate.md` §3。

**「插在语义相关的位置更好看」这个冲动，就是这个 P0 的来源。**

## 4. 每个脚本的语言由扩展名决定

扩展名 → 语言的映射通过 `jce_script_vm_register_extension(extension, language)`
在运行时注册（`engine/src/middleware/script/jce_script_vm.c`）。
**不要把这张映射硬编码进引擎文件**——注册点是唯一入口。

`JCE_SCRIPT_LANGUAGE` 是**进程级全局覆盖**，不是回退
（`engine/src/application/jce_rt_script.c` 的注释写明了这一点：
把它当「未认领扩展名的回退」会让 `JCE_SCRIPT_LANGUAGE=lua` 去加载 `turret.py`）。

没有隐式 Lua 回退。一个扩展名没被认领就是没被认领。

## 5. 已知陷阱

- **两个原生脚本模块进不了同一个二进制**：C 与 C++ 的模块结束宏都会无条件发射
  同一个外部符号，两者同时存在会 `LNK2005`。需要同时提供两者时，
  其中一个必须以「无入口」形态编译。
- **`jce_script_api` 是共享库**：Linux / macOS 上消费方要设动态库搜索路径。
- **C / C++ 脚本没有 `log`**：别在 C/C++ 脚本里指望 Lua/Python 那个日志函数。
- **`if(TARGET ...)` 的顺序陷阱**：脚本后端目标必须在被引用之前定义。
- **SDK 的 `VERSION.txt` 会列出它实际装了哪些后端**（`scripts:` 行）。
  `python scripts/jce.py smoke` 故意跑两个独立消费者（纯引擎 / 带脚本），
  这样一个「声称有脚本后端却没装」的 SDK 会变红。

## 6. 改动后的完整顺序

```bash
# ① 改 jce_script.h 的 JceScriptHost（新成员一律追加在末尾）+ script_exposure.json
# ①b 新增 API 时：先手工往 contracts/script-api.json 种一次条目（见下），否则 ② 跑不动
# ② 重新生成（真写）—— 顺序是 c_abi 先，因为 bindings 要求新函数已在 C ABI 头里声明
python tools/scriptgen/gen_script_c_abi.py --write
python tools/scriptgen/gen_script_bindings.py --write
python tools/scriptgen/gen_script_c_abi.py --write     # bindings 会重写契约，再收敛一轮
# ③ 校验（应当逐字节一致）
python tools/scriptgen/gen_script_bindings.py
python tools/scriptgen/gen_script_c_abi.py
# ④ 公共头变了 ⇒ 必须重装 SDK
python scripts/jce.py sdk
# ⑤ 门禁
python tools/lint/run_all.py
cmake --build build/desktop/windows-x64 --target jce_tests
ctest --test-dir build/desktop/windows-x64 -L unit -j 8
python scripts/jce.py smoke
```

第 ④ 步是 RED 基线里最常被漏掉的一步，见 `references/user-project-sdk.md` §0。

### 加一个新 API 会撞上生成器自举环

`gen_script_bindings` 拒绝生成，除非新函数**已在 C ABI 头里声明** →
那个头由 `gen_script_c_abi` 从 `contracts/script-api.json` 生成 →
而那份契约又是 `gen_script_bindings --write` 的产物（它的 `_generated_by` 就这么写着）。
`--write` 和 `--update` 是同一件事，**都被前面的 `core.validate()` 挡着**。

**更好的破环点（2026-09-20 实测，比下面的手工种条目干净）：让生成器自己写一次，
只把 `validate` 静音。** 两个环的根因是同一个——`core.validate()` 拿**新 manifest**
去比**它自己还没写出来的产物**——所以只要有一次运行里它不是闸门，两个环一起破：

```python
mod = load("gen_script_bindings")          # 从 tools/scriptgen/ 导入
real = mod.core.validate
mod.core.validate = lambda m, man, c: (suppressed.extend(real(m, man, c)), [])[1]
sys.argv = ["gen_script_bindings.py", "--write"]
mod.main()                                  # 19 份产物 + contracts/script-api.json
```

然后 `gen_script_c_abi.py --write`，再跑**两个生成器不带参数的 check 模式**——
它们必须都 exit 0 并报 "byte-identical to a fresh emit"。

**把静音掉的那几条打印出来**：它们是这次破环所欠的全部，收敛之后必须一条不剩。
这条路每一个字节都是生成器写的（不是我手打的 JSON），且**判据是我刚刚绕过的那个 check 模式**。

手工种条目的老办法仍然可用，记在下面；它要多维护一份 `declared_totals`：

打破点是**手工往 `contracts/script-api.json` 里种一次条目**：
- `expose` 条目抄同 `shape` 的邻居（`c_signature` / `params` / `out_params` 照抄），
  `vtable_index` 用头文件里的声明序；
- `hand_written` 条目**必须同时改 `declared_totals`**，否则报
  「`declared_totals.hand_written = 8 but the manifest carries 7`」。

种完按 **c_abi → bindings → c_abi** 收敛，直到两个都 exit 0、且报告
「artefacts byte-identical to a fresh emit」。之后 `--write` 会把种进去的条目规范化。

#### 还有**第二个环**（2026-09-01 实测，上面那段没记）

破完第一个环、`gen_script_c_abi --write` 成功之后，`gen_script_bindings` 仍会红：

```
manifest entry 'world_get_hour' is never registered in install_bindings()
... 每一条新条目一行
```

因为 `emit_lua.validate()` 的注册配对读的是**两个**翻译单元——
`engine/src/middleware/script/jce_script.c`（7 个永久手写的）**与**
`engine/src/middleware/script/jce_script_bindings.gen.c`（其余全部）。
而后者**正是这个生成器自己的产物**，且 `validate()` 在写之前跑。

⇒ 第二个破环点：**手工把注册行补进那个产物**，紧跟最后一条已有注册之后：

```c
    jce_script_register_binding(L, s, "<name>", l_jce_<name>);
```

下一次 `--write` 会整份重写它，手写内容不会留下——它只是让 `validate()` 能过。

**不要改去 `jce_script.c`**：那里是永久手写的 7 条所在，
把生成条目注册到那里会在重生成后变成**双重注册**。

#### 追加之后必然变红的黄金测试（实测 3 个）

| 测试 | 断言 | 修法 |
|---|---|---|
| `test_jce_script_table_shape` | 键数 + 整张按字典序排的键表 | 改计数、按字典序插入新键 |
| `test_jce_script_internal_header` | 键数 | 改计数 |
| `test_jce_script_api_abi` | **尾条目**符号名与其原型 | 改指新尾条目，按新原型重打 typedef 与调用 |

三个都会在失败信息里**点名自己的修法**。
**`cpp_differential` 会不会红，取决于新条目的形状**（2026-09-20 更正，
原文写的「不会红，它们是生成物」只对**标量**条目成立）：

| 新条目的形状 | cpp differential |
|---|---|
| 标量参数 / 标量返回 | 不红——`.gen.cpp` 随 `--write` 一起更新 |
| **多个 out 参数（`fallible_out` ≥ 2 个）** | **编译期红**，欠一个 `record()` overload |

多个 out 参数会让生成器发出一个**新的 POD 结果类型**（`GetParamResult`、
`GetTouchResult`…），而 `tests/scripting/cpp/jce_script_cpp_differential.hpp`
是**手写的**，里面按字段逐个 `record()` 的那几个 overload 也是手写的。
于是 `.gen.cpp` 调 `record(Slots&, const jce::script::XxxResult&)` 时：

```
error C2665: 'jce::diff::record': no overloaded function could convert all the argument types
```

**这是设计好的信号，不是缺陷**——那个头自己的注释就写着：
「A further entry of this shape is a COMPILE ERROR here, which is the correct
way to find out that a new overload is owed.」

修法是在那个 hpp 里按**字段声明顺序**加一个 overload（顺序就是被检查的东西，
所以必须手写字段名，不能让它从生成结构体那边推导）：

```cpp
inline void record(Slots &s, const jce::script::GetParamResult &p)
{
    record(s, p.out_kind);
    record(s, p.out_number);
    record(s, p.out_entity);
}
```

`python_differential` 不受影响（它的 mock 是纯生成物）。

#### 声明了槽位不等于填了它

`JceScriptHost` 的成员是函数指针，`jce_script.c` 对每个 NULL 槽都回落到一个
合理默认值（false / 0 / nil）——这既是短 host 的安全保证，也让**没人填的槽不可见**：
绑定被注册、可调用、永久返回默认值，七种语言全都如此。

运行时是**唯一**的填写者（没有 app 钩子），所以
「`engine/src/application/jce_rt_script.c` 里没有 `host.<name> =`」
＝「这条绑定对出货游戏里的每个脚本都是死的」。

2026-09-01 实测：90 个函数指针成员，89 个被赋值，**`is_key_down` 从写下起就没有**——
脚本层唯一的原始键盘原语一直返回 false。
现由 `tools/lint/check_script_host_writers.py` 守着（豁免必须带理由，
且对「已赋值却仍被豁免」也会红，因为陈旧豁免会掩盖回归）。

> `contracts/` 只在 `main` 上跟踪，在 `track-space-project` 上**整个目录都不存在**——
> 所以那条分支上 `gen_script_c_abi.py` 连跑都跑不起来，它提交的 C ABI 生成物
> 在任何已提交的输入下都不可复现。见 `references/git-and-worktrees.md`。

### 键数变化的连带

脚本 API 的**键数**是被多处硬编码断言的。加两个键（`declared_totals.table_keys` N → N+2）会同时打红：
5 个 ctest（`test_jce_script_table_shape` · `internal_header` · `api_abi` ·
`cpp_differential` · `python_differential`）和 9 个 `tools/audit/tests` 断言。
其中三类容易看漏：
- `test_a_short_host_is_never_read_past_its_end` 断言**清单尾项的名字**——追加成员会改变它；
- 差分 mock 的实体基值会**整体 +1**，手写契约测试对着旧值；
- `declared_totals` 的 `hand_written` / `table_keys` 两个数。

**这些测试文件不入库**，所以它们在别的机器上不会跟着走。

## 7. 跨语言差分测试的一个已知死角

「用多种语言写同一个脚本、比对输出」听起来是个好判据，**但沉默和沉默比起来相等**。
历史上一次跨语言差分 150/151 通过，而它其实是死的：两侧都没有输出。

**判据必须要求每种语言产生一个非平凡的、各自不同的可观测量**，
并且先跑一次**阳性对照**（故意让其中一种语言给出不同结果，确认差分能红）。

## 8. 红旗

| 念头 | 现实 |
|---|---|
| 「直接改 Lua 绑定文件更快」 | 绑定是生成的；手写绑定是缺陷 |
| 「这个调用应该已经有了」 | 先跑 `--check`，它是只读的 |
| 「把这个函数改个更好的名字」 | append-only：新增 + 标 deprecated |
| 「多语言差分测试全过了」 | 先确认它不是在比较沉默 |
| 「只改了脚本 API，不用重装 SDK」 | 改了 `engine/include/**` 就要重装 |
| 「`--check` 被拒了，但我逐行读过源码，可以说已发货」 | `--check` 抓的正是你读不出来的那类问题。降级措辞，见 §2 |
