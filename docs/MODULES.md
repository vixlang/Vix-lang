# 模块与文件包含

本文描述当前实现。历史版本里有过另一套规则（`import module::symbol`、
`mod name`、包名展开），那些语法已删除，见文末的迁移说明。

---

## 一、两种机制，两个关键字

| 写法 | 做什么 | 命名空间 |
|------|--------|----------|
| `include "path.vix"` | 把目标文件处理后的源码**拼进当前文件** | 扁平，并入当前编译单元 |
| `use "path.vix" as name` | 加载模块，符号加前缀 | `name::item` |

### include

```vix
include "lib/math.vix"

fn main(): i32
{
    return add(3, 4)          // 扁平：直接调
}
```

### use

```vix
use "lib/math.vix" as math

fn main(): i32
{
    return math::add(3, 4)    // 限定名
}
```

**模块身份是路径，命名空间是别名。** 两者都不从对方推导：树里有 5 个
`mod.vix`、2 个 `codegen.vix`、2 个 `error.vix`，任何"从文件名猜命名空间"
的方案都必然撞。

同一文件可以用不同别名引入多次，只加载一次：

```vix
use "lib/math.vix" as math
use "lib/math.vix" as m2
```

`as` **必写**。省略是错误而不是猜测：

```
error[E1001]: expected token -- expected 'as', got 'fn'
```

---

## 二、路径解析

只有一条规则：

1. 取**声明所在文件**的目录
2. 拼上路径
3. 做词法规范化

路径在**读取之前**和**去重之前**都会规范化，所以同一文件的多种拼写只包含一次：

| 拼写 | 规范化 |
|------|--------|
| `./a.vix` | `a.vix` |
| `a//b.vix` | `a/b.vix` |
| `sub/../a.vix` | `a.vix` |

规范化是纯词法的，不要求路径存在——这正是它能作为导入身份的原因。

`use` 的路径除了 `x.vix` 之外还接受目录模块：`use "utils" as utils` 会依次找
`utils.vix` 和 `utils/mod.vix`。

**没有包管理器**：没有裸名展开、没有 `$VIX_HOME`、没有 `.vix/libs` 搜索。
这些在历史文档里写过，但从未实现，相关文档已删除。

---

## 三、一个账本

模块图是**唯一**决定"一个程序包含哪些文件"的地方。它在解析之前构建，并把已
认领的路径交给预处理器，因此 `include` 不会重复内联模块图已经拥有的文件。

原因：两套机制各自维护去重集合时，一个文件被 `mod` 和 `import` 同时引用就会
包含两次。函数因为合并时会加前缀而幸免，结构体不会：

```
error[E2006]: duplicate struct 'Thing'
```

回归测试：`tests/dual_mechanism/`。

---

## 四、接口文件 .vixi

每个被 `use` 加载的模块都会生成一个 JSON 接口文件，记录它的签名和
三个失效依据。编译成功后自动写出，不需要手工调用。

```json
{"vixi":1,"path":"dep.vix","source_hash":"3f1a2b04",
 "compiler":"vixc 0.5.1","interface_hash":"6db11259",
 "dep_paths":[],"dep_hashes":[],
 "functions":[{"name":"value","type_params":[],"param_names":[],
               "param_types":[],"ret_ty":"i32","is_var_arg":0}],"types":[]}
```

手工生成（调试用）：

```sh
vixc src/math.vix --emit-vixi        # 写出 src/math.vixi
```

### 失效的三路判断

| 字段 | 失效条件 |
|------|----------|
| `source_hash` | 模块自身的源码变了 |
| `compiler` | 换了编译器 |
| `dep_hashes` | 它依赖的某个模块**接口**变了 |

第三路是关键的区分：改一个模块的**函数体**不动它的接口，依赖方**不该**失效；
改它的**签名**，依赖方必须失效。少了第三路，被依赖方接口变了而依赖方文件
没动时会静默接受过期接口——那是错编译，不是性能问题。

### 缓存怎么被用

1. 编译前，模块加载器逐个校验 `.vixi`；有效的模块被记为「已检查」
2. 类型检查和语义检查**跳过这些模块的函数体**（环境仍然从所有函数建立，
   所以跨模块签名照常解析）
3. 全部检查通过后，为尚未有效的模块写出 `.vixi`，按逆序（先子后父）
   保证父模块记录到依赖的接口哈希

**这个顺序是正确性的关键**：`.vixi` 只在模块被真正检查过之后才写出，
所以「存在且有效」等价于「这个源码已被这个编译器检查通过」。先写后查会让缓
存变成错编译的来源。

`.vixi` 是**纯缓存**：删光它们，编译结果逐字节不变，只是变慢。
测试见 `tests/vixi_cache_e2e.py`。

### 尚未做到

接口文件目前只用于**跳过重复检查**，还没有被用来做跨模块名字解析——
合并 AST 仍然带上全部函数体（LLVM 后端从整颗合并 AST 生成代码）。

另外可见性没有进入接口：`pub` 尚未落到 AST 上，所以接口列出所有顶层
声明。
## 五、可见性

`pub` 目前**不强制**：解析器接受并丢弃，合并时不做可见性过滤。所以接口文件
列出所有顶层声明（诚实的超集）。要真正实现可见性，得先把 `is_pub` 加进
`AstFunction` / `AstStructDecl` / `AstAdtDecl`。

---

## 六、迁移

| 旧写法 | 新写法 |
|--------|--------|
| `import "x.vix"` | `include "x.vix"` |
| `mod name` | `use "name.vix" as name` |
| `mod "x.vix"` | `use "x.vix" as <别名>` |
| `pub mod name` | `use "name.vix" as name`（pub 尚未强制） |
| `import module::symbol` | 直接写限定名 `module::symbol` |

旧语法不是静默失效，而是给出迁移提示：

```
error[E1000]: 'mod' has been removed: declare the module with 'use <path> as <name>'
error[E1000]: 'import' has been removed: use 'include <path>' to splice a file,
              or 'use <path> as <name>' for a module
```
