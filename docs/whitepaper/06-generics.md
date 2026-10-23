# 6. 泛型与接口

clux 的泛型是编译期单态化：每个具体实参组合生成独立实体，运行期不保留类型参数。

## 6.1 泛型参数

```clux
func id[T](x: T): T { return x; }

struct Box[T] {
    value: T;
}

type Buffer[T] = []T;
```

类型参数在方括号中声明，使用逗号分隔。

## 6.2 默认值

```clux
func size_of[T = i32](): u64 {
    return sizeof(T);
}

func f[T, U = i32](x: T): T { return x; }
```

默认值从右向左省略；后面的参数可以有默认值，前面的不能依赖后面的参数。

## 6.3 编译期值参数

```clux
func buffer[N: comptime usize](value: i32): [N]i32 {
    return .[N]i32{ <value, N> };
}
```

值参数必须显式写 `: comptime <type>`。它在编译期绑定为常量。

## 6.4 参数包

```clux
func first[...Args](args: ...Args): void { }
```

- 参数包必须放在参数列表最后。
- 参数包可以表示一组类型或一组编译期值。
- 包在实例化时展开。

## 6.5 约束

```clux
func write[W extends IWriter[W]](w: ref *W): void { ... }
```

`extends` 后是类型模式或接口约束；约束在实例化时检查。

## 6.6 显式实例化

```clux
id[i32](1)
Box[str]{ .value = "a" }
Buffer[u8]
```

类型实参用方括号；类型上下文不做隐式推导。

## 6.7 调用点推导

```clux
func pair[T](a: T, b: T): T { return a; }
var x = pair(1, 2);
```

当类型实参未显式写出时，调用点根据实参类型推导。

## 6.8 泛型方法与静态方法

```clux
struct Box[T] {
    value: T;

    func map[U](self: ref *Box[T], f: func(T) -> U): Box[U] {
        return .Box[U]{ .value = f(self.value) };
    }
}

var b = .Box[i32]{ .value = 1 };
var s = b.map[str](func(x: i32): str { return "x"; });
```

## 6.9 泛型接口

```clux
interface IWriter[W] {
    func write(self: ref *W, data: ref []u8): void;
}

struct File implements IWriter[File] {
    func write(self: ref *File, data: ref []u8): void { ... }
}
```

接口只做编译期结构检查，不产生运行期虚表。

## 6.10 实例化语义

- 每个具体类型组合产生一个独立实例。
- 泛型实例的类型元信息和类型装饰器按实例执行。
- 递归实例化必须收敛；实例化深度有上限。
- 实例化后的函数/类型可以像普通函数/类型一样使用。

## 6.11 泛型与元信息

```clux
[[metadata("size", sizeof(T))]]
struct Box[T] { value: T; }

var n: usize = get_metadata[Box[i32]]("size");
```

泛型类型元信息属于具体实例，而不是泛型定义本身。
